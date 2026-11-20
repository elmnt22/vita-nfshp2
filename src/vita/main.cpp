// PS Vita host for the native PAL engine (spike).
// Stage 1: CPU-only host. SubmitFrame is a null renderer so the engine's own
// render.timing_window / render.phase_window profile measures the CPU side
// (logic + capture) on the Cortex-A9 before any GPU backend exists.
#include <psp2/ctrl.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#ifdef HP2_ENABLE_VITAGL
#include <vitaGL.h>
#endif

#include "bware/inc/bFile.hpp"
#include "bware/inc/bWare.hpp"
#include "indep/inc/CarDriver.hpp"
#include "indep/inc/Ecstasy.hpp"
#include "indep/inc/Flags.hpp"
#include "indep/inc/Timer.hpp"
#include "indep/inc/NativeRenderSettings.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <pthread.h>
#include "crash.hpp"
#include "audio.hpp"
namespace vita_selftest { void Run(void (*log)(const char*, const char*)); }
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <string>

extern "C" {
// Engine heap: newlib malloc arena. The rest of user memory stays for the
// kernel/GPU. Tunable once the real footprint is measured.
// Peak newlib use on heavy tracks ~215 MB (renderer.vitagl.memory_use); the
// rest of user RAM goes to vitaGL, which otherwise runs out of GPU memory.
unsigned int _newlib_heap_size_user = 280u * 1024u * 1024u;
unsigned int sceUserMainThreadStackSize = 4u * 1024u * 1024u;
}

namespace hp2 { extern int debugQuickRaceTrack, debugQuickRaceReverse; extern int debugDemoTrack; extern std::atomic<int> debugDemoStarted; extern int debugBenchMirror; }
extern int collisionWitnessWarmProbe;
extern uint32_t (*collisionWitnessProbeClock)();
extern uint32_t (*collisionWitnessClockMicros)();
namespace hp2 { extern int ecstasySceneryRuns; extern int ecstasySceneryStripCull; extern int ecstasyFragmentMediump; extern int ecstasySceneWorkerPriority; extern int ecstasyParallelSceneryPrepare; extern int ecstasyHalfResSkipAlphaTest; extern int ecstasyHalfResHardwareDepth; extern int ecstasySceneJobsSerial; extern int ecstasyAffineUV; extern int ecstasySceneryFastMerge; extern int ecstasyParallelSceneryEmit; extern int ecstasyParallelParticles; extern int ecstasyParallelModelStrips; extern int ecstasyParallelSkinStrips; extern int ecstasyHalfResParticles; extern double ecstasyHalfResMinScreens; extern double ecstasyQuarterResMinScreens; extern int ecstasyHalfResDepthFlip; extern int ecstasyHalfResSync; extern int ecstasyHalfResCompositeFlip; extern std::vector<std::uint64_t> ecstasyScreenshotFrames; extern std::vector<std::uint64_t> ecstasyDisplayShotFrames; }
extern int collisionSpeculationMode;
extern uint64_t collisionSpeculationVerifyBudget;

namespace {
class VitaLog;
VitaLog* g_log{};
class VitaApplicationHost;
VitaApplicationHost* g_host{};
// Watchdog diagnostics: main-thread stage and completed-frame counter.
std::atomic<int> g_mainStage{0};
std::atomic<unsigned> g_framesDone{0};
constexpr const char* kDataDirectory = "ux0:data/NFSHP2N";
constexpr int kWidth = 960;
constexpr int kHeight = 544;

// Lines are formatted on the caller and written by a low-priority thread:
// a memory-card fflush costs ~3-4 ms, so the 5 s diagnostics report used to
// stall the game thread ~75 ms. Flush() writes synchronously (crash paths).
class VitaLog final {
public:
    explicit VitaLog(const std::string& path)
    {
        file_ = std::fopen(path.c_str(), "w");
        if (!file_) return;
        sema_ = sceKernelCreateSema("hp2_log", 0, 0, 1 << 20, nullptr);
        if (sema_ < 0) return;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 64 * 1024);
        writing_ = pthread_create(&writer_, &attributes, [](void* self) -> void* {
            static_cast<VitaLog*>(self)->Run();
            return nullptr;
        }, this) == 0;
        pthread_attr_destroy(&attributes);
    }
    ~VitaLog()
    {
        if (writing_) {
            stopping_.store(true, std::memory_order_release);
            sceKernelSignalSema(sema_, 1);
            pthread_join(writer_, nullptr);
        }
        if (sema_ >= 0) sceKernelDeleteSema(sema_);
        Flush();
        if (file_) std::fclose(file_);
    }
    void Line(const char* event, const char* detail) noexcept
    {
        if (!file_) return;
        char text[1600];
        const auto ms = static_cast<unsigned long long>(sceKernelGetProcessTimeWide() / 1000u);
        int length = std::snprintf(text, sizeof(text), "[%llu] %s%s%s\n", ms, event, detail ? " " : "", detail ? detail : "");
        if (length < 0) return;
        if (static_cast<std::size_t>(length) >= sizeof(text)) {length = sizeof(text) - 1; text[length - 1] = '\n';}
        {
            std::lock_guard<std::mutex> lock(mutex_); // render + application threads
            try {pending_.append(text, static_cast<std::size_t>(length));} catch (...) {return;}
        }
        if (writing_) sceKernelSignalSema(sema_, 1);
        else Flush();
    }
    // Synchronous drain on the caller (crash/watchdog paths, shutdown).
    void Flush() noexcept
    {
        if (!file_) return;
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return;
        std::lock_guard<std::mutex> fileLock(fileMutex_);
        if (!pending_.empty()) {std::fwrite(pending_.data(), 1, pending_.size(), file_); pending_.clear();}
        std::fflush(file_);
    }
private:
    void Run()
    {
        sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
        sceKernelChangeThreadPriority(0, 190);
        std::string batch;
        for (;;) {
            sceKernelWaitSema(sema_, 1, nullptr);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                batch.swap(pending_);
            }
            if (!batch.empty()) {
                std::lock_guard<std::mutex> fileLock(fileMutex_);
                std::fwrite(batch.data(), 1, batch.size(), file_);
                std::fflush(file_);
                batch.clear();
            }
            if (stopping_.load(std::memory_order_acquire)) return;
        }
    }
    std::FILE* file_{};
    std::mutex mutex_, fileMutex_;
    std::string pending_;
    SceUID sema_{-1};
    pthread_t writer_{};
    bool writing_{};
    std::atomic<bool> stopping_{false};
};

using hp2::platform::Ps2Button;
constexpr std::pair<unsigned, Ps2Button> kButtons[] = {
    {SCE_CTRL_SELECT, Ps2Button::Select}, {SCE_CTRL_START, Ps2Button::Start},
    {SCE_CTRL_UP, Ps2Button::Up}, {SCE_CTRL_RIGHT, Ps2Button::Right},
    {SCE_CTRL_DOWN, Ps2Button::Down}, {SCE_CTRL_LEFT, Ps2Button::Left},
    {SCE_CTRL_LTRIGGER, Ps2Button::L1}, {SCE_CTRL_RTRIGGER, Ps2Button::R1},
    {SCE_CTRL_TRIANGLE, Ps2Button::Triangle}, {SCE_CTRL_CIRCLE, Ps2Button::Circle},
    {SCE_CTRL_CROSS, Ps2Button::Cross}, {SCE_CTRL_SQUARE, Ps2Button::Square},
};

class VitaApplicationHost final : public hp2::NativeApplicationHost {
public:
    explicit VitaApplicationHost(VitaLog& log) : log_(log)
    {
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
        // Optional unattended navigation: "period hold until" in logical
        // 60 Hz input steps, e.g. "90 10 6000" taps Cross every 1.5 s.
        if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/autopad.txt").c_str(), "r")) {
            unsigned period = 0, hold = 0, until = 0;
            if (std::fscanf(file, "%u %u %u", &period, &hold, &until) == 3 && period && hold < period) {
                autoPeriod_ = period; autoHold_ = hold; autoUntil_ = until;
                char detail[96];
                std::snprintf(detail, sizeof(detail), "cross period=%u hold=%u until=%u", period, hold, until);
                log_.Line("vita.autopad", detail);
                // Optional 4th/5th fields: quick-race track number and direction.
                int track = -1, reverse = 0;
                if (std::fscanf(file, "%d %d", &track, &reverse) >= 1 && track >= 0) {
                    ::hp2::debugQuickRaceTrack = track; ::hp2::debugQuickRaceReverse = reverse;
                    std::snprintf(detail, sizeof(detail), "track=%d reverse=%d", track, reverse);
                    log_.Line("vita.autopad_track", detail);
                }
            }
            std::fclose(file);
        }
        // Testing: quickrace_track.txt "<track> [reverse]" forces Quick Race
        // onto that track (normal menus and input; no autopad taps).
        if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/quickrace_track.txt").c_str(), "r")) {
            int track = -1, reverse = 0;
            if (std::fscanf(file, "%d %d", &track, &reverse) >= 1 && track >= 0) {
                ::hp2::debugQuickRaceTrack = track; ::hp2::debugQuickRaceReverse = reverse;
                char detail[64];
                std::snprintf(detail, sizeof(detail), "track=%d reverse=%d", track, reverse);
                log_.Line("vita.quickrace_track", detail);
            }
            std::fclose(file);
        }
        // Benchmark: demo_bench.txt "<track>" runs the retail AI attract demo
        // (live pursuit: AI player car, 4 cops, helicopter, traffic) on that
        // track. Autopad taps reach the main menu, the demo starts after 1 s
        // idle and lasts an hour; taps stop once it starts (input ends it).
        if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/demo_bench.txt").c_str(), "r")) {
            int track = -1, mirror = 0;
            if (std::fscanf(file, "%d %d", &track, &mirror) >= 1 && track > 0) {
                ::hp2::debugDemoTrack = track;
                ::hp2::debugBenchMirror = mirror != 0;
                applicationLaunchOptions.beginDemoSeconds = 1.0f;
                applicationLaunchOptions.demoDurationSeconds = 3600.0f;
                if (!autoPeriod_) {autoPeriod_ = 90; autoHold_ = 10; autoUntil_ = 100000;}
                demoBench_ = true;
                char detail[64];
                std::snprintf(detail, sizeof(detail), "track=%d mirror=%d", track, mirror);
                log_.Line("vita.demo_bench", detail);
            }
            std::fclose(file);
        }
    }
    void BindClock(const GameTime& clock) noexcept { clock_ = &clock; }

    bool PollEvents() override
    {
        if (clock_) {
            const auto refreshes = refreshClock_.Observe(sceKernelGetProcessTimeWide(), 1000000u,
                gameTimeGetVideoMode(clock_));
            for (std::uint64_t i = 0; i < refreshes; ++i) {
                graphicsNotifyPresentationComplete();
                platformNotifyVideoBlank();
            }
        }
        // Keep the screen from dimming during unattended timing runs.
        if ((++polls_ & 63u) == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        return true;
    }
    void WaitUntil(hp2::NativeFrameTiming::Clock::time_point deadline) override
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - FrameTime()).count();
        if (remaining > 0) sceKernelDelayThread(static_cast<SceUInt>(std::min<std::int64_t>(remaining, 16000)));
    }
    void RecordSessionEvent(const char* event, const char* detail) noexcept override
    {
        if (std::strncmp(event, "render.slow_", 12) == 0) {
            // Sample: at most one slow-frame breakdown per second.
            const auto now = sceKernelGetProcessTimeWide();
            if (now - lastSlowFrame_ < 1000000u) return;
            lastSlowFrame_ = now;
        }
        if (std::strncmp(event, "file.", 5) == 0 && std::strncmp(event, "file.read.", 10) != 0) return;
        log_.Line(event, detail);
    }
    void BeginInputStep(hp2::NativeFrameTiming::Clock::time_point) override
    {
        SceCtrlData pad{};
        if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) { state_ = {}; return; }
        state_ = {};
        state_.connected = true;
        for (const auto& [physical, logical] : kButtons)
            hp2::platform::SetButton(state_, logical, (pad.buttons & physical) != 0);
        state_.sticks = {pad.rx, pad.ry, pad.lx, pad.ly};
        ++inputSteps_;
        if (demoBench_ && ::hp2::debugDemoStarted.load(std::memory_order_relaxed)) autoPeriod_ = 0;
        if (autoPeriod_ && inputSteps_ < autoUntil_ && (inputSteps_ % autoPeriod_) < autoHold_)
            hp2::platform::SetButton(state_, Ps2Button::Cross, true);
    }
    int32_t ReadInput(int32_t port, JoystickControls& destination) override
    {
        hp2::platform::ControllerState state = port == 0 ? state_ : hp2::platform::ControllerState{};
        destination = hp2::platform::NativeScannerControls(state);
        return state.connected ? 1 : 0;
    }
    void SetRumble(int32_t, uint8_t, uint8_t) override {}
#ifdef HP2_ENABLE_VITAGL
    ~VitaApplicationHost() override { StopRenderThread(); }
    void AttachGraphics(hp2::EcstasyVulkanDevice& graphics, bool pipeline)
    {
        graphics_ = &graphics;
        pipelineAllowed_ = pipeline;
        if (!pipeline) return;
        // Gather uses large stack buffers (32 KB+ arrays); the default Vita
        // pthread stack is far too small, so size the render thread explicitly.
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 2u * 1024u * 1024u);
        if (pthread_create(&renderThread_, &attributes,
                [](void* self) -> void* { static_cast<VitaApplicationHost*>(self)->RenderThread(); return nullptr; },
                this) == 0)
            renderThreadStarted_ = true;
        else pipelineAllowed_ = false;
        pthread_attr_destroy(&attributes);
    }
    void StopRenderThread()
    {
        if (!renderThreadStarted_) return;
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        wake_.notify_all();
        pthread_join(renderThread_, nullptr);
        renderThreadStarted_ = false;
    }
    bool ReadRenderStatistics(hp2::EcstasyVulkanFrameStatistics& result) const noexcept override
    {
        if (!graphics_) return false;
        // The render thread owns the live statistics; report the last
        // completed frame's copy taken on the application thread.
        result = pipelineAllowed_ ? lastStatistics_ : graphics_->LastFrameStatistics();
        return true;
    }
    void SetFramePipelining(bool enabled) override
    {
        const bool want = enabled && pipelineAllowed_;
        if (want == pipelining_) return;
        FinishFrames();
        pipelining_ = want;
    }
    bool FrameInFlight() const noexcept override { return inFlight_ != nullptr; }
    // Application thread only. Waits for the render thread and retires the
    // frame's source borrowers here, never on the render thread.
    void FinishFrames() override
    {
        if (!inFlight_) return;
        g_mainStage = 2;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            done_.wait(lock, [this] { return job_ == nullptr; });
        }
        if (renderError_) { auto error = renderError_; renderError_ = nullptr; inFlight_ = nullptr; std::rethrow_exception(error); }
        lastStatistics_ = graphics_->LastFrameStatistics();
        g_mainStage = 3;
        inFlight_->Complete();
        inFlight_ = nullptr;
        g_mainStage = 0;
    }
#endif
    void SubmitFrame(hp2::EcstasyFrame& frame) override
    {
#ifdef HP2_ENABLE_VITAGL
        if (graphics_ && pipelining_) {
            FinishFrames();
            inFlight_ = &frame;
            { std::lock_guard<std::mutex> lock(mutex_); job_ = &frame; }
            wake_.notify_all();
            return;
        }
        if (graphics_) {
            graphics_->SetDeferredRetire(false);
            g_mainStage = 4;
            graphics_->Render(frame);
            g_mainStage = 0;
            ++g_framesDone;
            lastStatistics_ = graphics_->LastFrameStatistics();
            return;
        }
#endif
        // Null renderer (stage 1). Count the work the GPU backend will see.
        ++frames_;
        commands_ += frame.Commands().size();
        if ((frames_ % 300u) == 0) {
            char detail[160];
            std::snprintf(detail, sizeof(detail), "frames=%llu avg_commands=%llu",
                static_cast<unsigned long long>(frames_),
                static_cast<unsigned long long>(commands_ / 300u));
            log_.Line("vita.null_render", detail);
            commands_ = 0;
        }
    }
    bool SupportsAudio() const noexcept override { return audioEnabled_; }
    WorldAudioResult OpenAudio(bSound& sound) override
    {
        if (!audioEnabled_) return WORLD_AUDIO_OK;
        const auto result = audio_.Open(sound);
        log_.Line("audio.open", result == WORLD_AUDIO_OK ? "sceAudioOut BGM 48 kHz stereo" : audio_.Error().c_str());
        return result;
    }
    std::uint64_t pumpPeakUs_ = 0;
    WorldAudioResult PumpAudio(bSound&) override
    {
        if (!audioEnabled_) return WORLD_AUDIO_OK;
        const auto pumpStarted = sceKernelGetProcessTimeWide();
        const auto result = audio_.Pump();
        pumpPeakUs_ = std::max<std::uint64_t>(pumpPeakUs_, sceKernelGetProcessTimeWide() - pumpStarted);
        if (result != WORLD_AUDIO_OK) log_.Line("audio.failure", audio_.Error().c_str());
        if (++audioPumps_ % 300u == 0) {
            const auto audio = audio_.TakeStatistics();
            char detail[384];
            std::snprintf(detail, sizeof(detail),
                "pumps=300 decode_ms=%.3f commands_ms=%.3f pump_peak_ms=%.3f apply_peak_ms=%.3f apply_peak_kind=%d "
                "inner_peak_ms=%.3f inner_peak_music_ms=%.3f inner_peak_install_ms=%.3f inner_peak_commands_ms=%.3f inner_peak_requests=%u inner_peak_lock_ms=%.3f inner_peak_apply_ms=%.3f "
                "mix_ms_total=%.1f buffers=%llu voices_peak=%u",
                audio.decodeMilliseconds / 300.0, audio.commandMilliseconds / 300.0, pumpPeakUs_ / 1000.0,
                audio.applyPeakMilliseconds, audio.applyPeakKind,
                audio.pumpPeakMilliseconds, audio.pumpPeakMusicMs, audio.pumpPeakInstallMs, audio.pumpPeakCommandsMs, audio.pumpPeakRequests,audio.pumpPeakLockWaitMs,audio.pumpPeakApplyMs,
                audio.mixMilliseconds, static_cast<unsigned long long>(audio.buffers), audio.voicesPeak);
            pumpPeakUs_ = 0;
            log_.Line("audio.backend_window", detail);
        }
        return result;
    }
    WorldAudioResult CloseAudio(bSound&) override { return audioEnabled_ ? audio_.Close() : WORLD_AUDIO_OK; }
    void DisableAudio() noexcept { audioEnabled_ = false; }
    int Width() const override { return kWidth; }
    int Height() const override { return kHeight; }
    std::size_t AvailableMemory() const override
    {
        SceKernelFreeMemorySizeInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetFreeMemorySize(&info) < 0) return 0;
        return static_cast<std::size_t>(info.size_user);
    }
    void ApplyDisplaySettings(int32_t, int32_t, bool, bool vsync) override
    {
#ifdef HP2_ENABLE_VITAGL
        // Like the Switch host: camera targets use the fixed console extent.
        if (graphics_) {
            graphics_->SetLogicalSize(kWidth, kHeight);
            graphics_->ConfigurePresentation(vsync);
        }
#else
        (void)vsync;
#endif
    }

public:
#ifdef HP2_ENABLE_VITAGL
    void RenderThread()
    {
        // Core 1: the application thread owns core 0.
        sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
        {
            std::uint32_t fpscr = 0;
            asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
            char detail[48]; std::snprintf(detail, sizeof(detail), "fpscr=0x%08x", static_cast<unsigned>(fpscr));
            if (g_log) g_log->Line("vita.render_thread", detail);
        }
        for (;;) {
            hp2::EcstasyFrame* frame{};
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || job_ != nullptr; });
                if (stopping_ && !job_) return;
                frame = job_;
            }
            try {
                graphics_->SetDeferredRetire(true);
                graphics_->Render(*frame);
            } catch (...) {
                renderError_ = std::current_exception();
            }
            { std::lock_guard<std::mutex> lock(mutex_); job_ = nullptr; }
            ++g_framesDone;
            done_.notify_all();
        }
    }
    pthread_t renderThread_{};
    bool renderThreadStarted_{};
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    hp2::EcstasyFrame* job_{};
    hp2::EcstasyFrame* inFlight_{};
    std::exception_ptr renderError_;
    hp2::EcstasyVulkanFrameStatistics lastStatistics_{};
    bool stopping_{}, pipelining_{}, pipelineAllowed_{};
#endif
    SceUInt64 lastSlowFrame_{};
    hp2::vita::AudioDevice audio_;
    unsigned audioPumps_{};
    bool audioEnabled_{true};
    VitaLog& log_;
    const GameTime* clock_{};
    hp2::NativeRefreshClock refreshClock_;
    hp2::platform::ControllerState state_{};
    std::uint64_t frames_{}, commands_{};
    std::uint32_t polls_{};
    std::uint64_t inputSteps_{};
    unsigned autoPeriod_{}, autoHold_{}, autoUntil_{};
    bool demoBench_{};
#ifdef HP2_ENABLE_VITAGL
    hp2::EcstasyVulkanDevice* graphics_{};
#endif
};
}

int main(int, char**)
{
    // Max clocks: the engine is CPU bound on the Cortex-A9.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    // Main/game thread owns core 0; engine scene workers use cores 1-2.
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);

    const std::filesystem::path base = kDataDirectory;
    const auto saves = base / "Saves";
    std::error_code ignored;
    std::filesystem::create_directories(saves, ignored);
    VitaLog log((base / "NFSHP2_log.txt").string());
    g_log = &log;
    // Faults and uncaught exceptions write crash.txt and exit (no popup).
    vita_crash::Init(kDataDirectory, [](const char* event, const char* detail) { if (g_log) {g_log->Line(event, detail); g_log->Flush();} });
    {   // Test switch: crash_test.txt makes the boot fault on purpose (handler check).
        SceIoStat test{};
        if (sceIoGetstat((std::string(kDataDirectory) + "/crash_test.txt").c_str(), &test) >= 0) {
            log.Line("vita.crash", "crash_test.txt: data abort now");
            static volatile std::uintptr_t bad = 8;
            *reinterpret_cast<volatile std::uint32_t*>(bad) = 0xdeadbeefu;
        }
    }
    vita_selftest::Run([](const char* event, const char* detail) { if (g_log) g_log->Line(event, detail); });
    hp2::platform::BindSessionSink([](const char* event, const char* detail) noexcept {
        if (g_log && std::strcmp(event, "file.open") != 0) g_log->Line(event, detail);
    });
    try {
        #ifdef HP2_ENABLE_VITAGL
        log.Line("vita.start", "native PAL engine, vitaGL renderer");
#else
        log.Line("vita.start", "native PAL engine, stage1 null renderer");
#endif
        hp2::GameResources files(base / "ZZDATA.BIN");
        log.Line("vita.resources", "opened");
        hp2::configureNativeRenderSettings({});
        VitaApplicationHost host(log);
#ifdef HP2_ENABLE_VITAGL
        SceIoStat shacccg{};
        if (sceIoGetstat("ur0:data/libshacccg.suprx", &shacccg) < 0)
            log.Line("vita.gl", "WARNING ur0:data/libshacccg.suprx missing (run ShaRKF00D)");
        vglUseExtraMem(GL_FALSE);
        {
            // A heavy scene (large smoke/dust quads over many tiles plus ~2k
            // draws) can overflow the default 16 MB parameter buffer, which
            // makes GXM split the scene into partial renders. pbuf16.txt
            // restores the default for comparison.
            SceIoStat pbuf{};
            const bool small = sceIoGetstat((std::string(kDataDirectory) + "/pbuf16.txt").c_str(), &pbuf) >= 0;
            vglSetParamBufferSize((small ? 16u : 32u) * 1024u * 1024u);
            log.Line("vita.gl", small ? "parameter buffer 16 MB" : "parameter buffer 32 MB");
        }
        vglInitExtended(4 * 1024 * 1024, kWidth, kHeight, 12 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
        vglWaitVblankStart(GL_FALSE);
        log.Line("vita.gl", "vitaGL initialized");
        hp2::EcstasyVulkanOptions graphicsOptions;
        graphicsOptions.width = kWidth; graphicsOptions.height = kHeight;
        graphicsOptions.logicalWidth = kWidth; graphicsOptions.logicalHeight = kHeight;
        graphicsOptions.fixedRenderExtent = true;
        graphicsOptions.vsync = false;
        auto graphics = std::make_unique<hp2::EcstasyVulkanDevice>(graphicsOptions);
        // Pipelined rendering is on unless ux0:data/NFSHP2N/nopipeline.txt exists.
        SceIoStat noPipeline{};
        const bool pipeline = sceIoGetstat((std::string(kDataDirectory) + "/nopipeline.txt").c_str(), &noPipeline) < 0;
        log.Line("vita.gl", pipeline ? "render thread pipelining on" : "render thread pipelining off");
        host.AttachGraphics(*graphics, pipeline);
        hp2::nativeRenderDrainHook = [] { if (g_host) g_host->FinishFrames(); };
        g_host = &host;
#endif
        // Audio is on unless ux0:data/NFSHP2N/noaudio.txt exists (then music
        // must be off too: the jukebox needs the audio device as its owner).
        SceIoStat noAudio{};
        if (sceIoGetstat((std::string(kDataDirectory) + "/noaudio.txt").c_str(), &noAudio) >= 0) {
            host.DisableAudio();
            applicationLaunchOptions.musicEnabled = 0;
            log.Line("vita.audio", "disabled by noaudio.txt");
        }
        SceIoStat probe{};
        collisionWitnessClockMicros = []{ return (uint32_t)sceKernelGetProcessTimeLow(); };
        // Exact speculative collision separation on cores 1/2: verify the
        // first separations against the serial path, then run speculatively.
        collisionSpeculationMode = 3; collisionSpeculationVerifyBudget = 2000;
        if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/collision_spec.txt").c_str(), "r")) {
            int mode = 0; unsigned long long budget = 0;
            if (std::fscanf(file, "%d %llu", &mode, &budget) >= 1) { collisionSpeculationMode = mode; collisionSpeculationVerifyBudget = budget; }
            std::fclose(file);
        }
        {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "collision_speculation mode=%d verify_budget=%llu", collisionSpeculationMode,
                static_cast<unsigned long long>(collisionSpeculationVerifyBudget));
            log.Line("vita.diag", detail);
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_runs_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneryRuns = 0;
            log.Line("vita.diag", "scenery strip runs off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_stripcull_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneryStripCull = 0;
            log.Line("vita.diag", "scenery strip cull off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/skin_serial.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyParallelSkinStrips = 0;
            log.Line("vita.diag", "skin parallel off");
        }
        for (const int threshold : {64, 128, 256}) {
            if (sceIoGetstat((std::string(kDataDirectory) + "/model_par" + std::to_string(threshold) + ".txt").c_str(), &probe) >= 0) {
                hp2::ecstasyParallelModelStrips = threshold;
                log.Line("vita.diag", ("model parallel strips " + std::to_string(threshold)).c_str());
            }
        }
        // Half-resolution smoke/dust (user-approved 2026-11-19) is the Vita
        // default; halfres_off.txt restores full resolution.
        hp2::ecstasyHalfResParticles = 1;
        if (sceIoGetstat((std::string(kDataDirectory) + "/quarterres_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyQuarterResMinScreens = 0.0;
            log.Line("vita.diag", "quarter-resolution particles off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/halfres_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyHalfResParticles = 0;
            log.Line("vita.diag", "half-resolution particles off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/halfres_sync.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyHalfResSync = 1;
            log.Line("vita.diag", "DIAGNOSTIC half-res GPU sync before depth read");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/halfres_dflip.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyHalfResDepthFlip = 1;
            log.Line("vita.diag", "half-res depth flip");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/halfres_cflip.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyHalfResCompositeFlip = 1;
            log.Line("vita.diag", "half-res composite flip");
        }
        if (FILE* gate = std::fopen((std::string(kDataDirectory) + "/halfres_min.txt").c_str(), "r")) {
            double value = 0.0;
            if (std::fscanf(gate, "%lf", &value) == 1) hp2::ecstasyHalfResMinScreens = value;
            std::fclose(gate);
            log.Line("vita.diag", ("half-res min screens " + std::to_string(hp2::ecstasyHalfResMinScreens)).c_str());
        }
        if (FILE* shots = std::fopen((std::string(kDataDirectory) + "/dshots.txt").c_str(), "r")) {
            unsigned long long frame = 0;
            while (std::fscanf(shots, "%llu", &frame) == 1) hp2::ecstasyDisplayShotFrames.push_back(frame);
            std::fclose(shots);
            log.Line("vita.diag", ("display shots " + std::to_string(hp2::ecstasyDisplayShotFrames.size())).c_str());
        }
        if (FILE* shots = std::fopen((std::string(kDataDirectory) + "/shots.txt").c_str(), "r")) {
            unsigned long long frame = 0;
            while (std::fscanf(shots, "%llu", &frame) == 1) hp2::ecstasyScreenshotFrames.push_back(frame);
            std::fclose(shots);
            log.Line("vita.diag", ("screenshots " + std::to_string(hp2::ecstasyScreenshotFrames.size())).c_str());
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/particles_serial.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyParallelParticles = 0;
            log.Line("vita.diag", "particle parallel build off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/emit_serial.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyParallelSceneryEmit = 0;
            log.Line("vita.diag", "scenery parallel emit off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/fastmerge_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneryFastMerge = 0;
            log.Line("vita.diag", "scenery fast merge off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/affine_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyAffineUV = 0;
            log.Line("vita.diag", "affine uv off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/frag_mediump.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyFragmentMediump = 1;
            log.Line("vita.diag", "fragment precision mediump");
        }
        // Scene worker jobs share cores 1/2 with the gather and GL threads;
        // inline jobs measured faster on heavy tracks (track 64 traverse
        // 5.5 -> 1.4 ms/view). scene_jobs_parallel.txt restores the pool.
        hp2::ecstasySceneJobsSerial = 1;
        if (sceIoGetstat((std::string(kDataDirectory) + "/scene_jobs_parallel.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneJobsSerial = 0;
            log.Line("vita.diag", "scene jobs parallel");
        }
        // Hardware-depth half pass: no measured gain over the depth-texture
        // discard (track 64 smoke 17.6 vs 17.8 fps); opt-in only.
        hp2::ecstasyHalfResHardwareDepth = 0;
        if (sceIoGetstat((std::string(kDataDirectory) + "/halfres_hwdepth.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyHalfResHardwareDepth = 1;
            log.Line("vita.diag", "half-res hardware depth");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/halfres_atest.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyHalfResSkipAlphaTest = 0;
            log.Line("vita.diag", "half-res keeps alpha test");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_prep_serial.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyParallelSceneryPrepare = 0;
            log.Line("vita.diag", "scenery prepare serial");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/worker_prio.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneWorkerPriority = 140;
            log.Line("vita.diag", "scene worker priority 140");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/witness_probe.txt").c_str(), &probe) >= 0) {
            collisionWitnessWarmProbe = 1;
            collisionWitnessProbeClock = [] { return static_cast<uint32_t>(sceKernelGetProcessTimeLow()); };
            log.Line("vita.diag", "witness warm probe on");
        }
        hp2::ApplicationRuntime application(files, host, saves);
        host.BindClock(application.Clock());
        // Watchdog: when no frame completes for 3 s, log both threads' stages.
        static std::atomic<bool> watchdogStop{false};
        std::thread watchdog([&log] {
            unsigned last = g_framesDone.load(); int quiet = 0; unsigned recoveries = 0;
            while (!watchdogStop.load()) {
                sceKernelDelayThread(1000000);
                if (const unsigned r = hp2::nativeScenePoolRecoveries.load(); r != recoveries) {
                    recoveries = r;
                    char detail[64]; std::snprintf(detail, sizeof(detail), "lost_wakeups_recovered=%u", r);
                    log.Line("vita.pool", detail);
                }
                const unsigned now = g_framesDone.load();
                if (now != last) { last = now; quiet = 0; continue; }
                if (++quiet >= 3) {
                    char detail[128];
                    std::snprintf(detail, sizeof(detail), "frames=%u main_stage=%d render_stage=%d quiet_s=%d",
                        now, g_mainStage.load(), hp2::EcstasyVulkanDevice::RenderStage(), quiet);
                    log.Line("vita.watchdog", detail);
                    // After the first frames, a long stall: record it and exit
                    // (watchdog_dump.txt: a full system core dump instead).
                    if (now > 100 && quiet >= 15) {
                        log.Line("vita.watchdog", "stalled frame pipeline: exiting");
                        log.Flush();
                        vita_crash::Stalled(detail);
                    }
                }
            }
        });
        log.Line("vita.init", "begin");
        application.Initialize();
        log.Line("vita.init", "done");
        applicationLaunchOptions.skipMovies = 1; // stage 1: no video path yet
        application.SetDisplayOptions(kWidth, kHeight, true, true);
        while (application.Tick()) {}
        application.Shutdown();
#ifdef HP2_ENABLE_VITAGL
        host.FinishFrames();
        hp2::nativeRenderDrainHook = nullptr; g_host = nullptr;
        host.StopRenderThread();
#endif
        watchdogStop = true; watchdog.join();
    } catch (const std::exception& error) {
        log.Line("vita.fatal", error.what());
        sceKernelDelayThread(1000000);
    }
    log.Flush();
    sceKernelExitProcess(0);
    return 0;
}
