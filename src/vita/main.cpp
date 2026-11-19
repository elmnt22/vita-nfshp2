// PS Vita host for the native PAL engine (spike).
// Stage 1: CPU-only host. SubmitFrame is a null renderer so the engine's own
// render.timing_window / render.phase_window profile measures the CPU side
// (logic + capture) on the Cortex-A9 before any GPU backend exists.
#include <psp2/ctrl.h>
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
unsigned int _newlib_heap_size_user = 330u * 1024u * 1024u;
unsigned int sceUserMainThreadStackSize = 4u * 1024u * 1024u;
}

namespace hp2 { extern int debugQuickRaceTrack, debugQuickRaceReverse; }

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

class VitaLog final {
public:
    explicit VitaLog(const std::string& path) { file_ = std::fopen(path.c_str(), "w"); }
    ~VitaLog() { if (file_) std::fclose(file_); }
    void Line(const char* event, const char* detail) noexcept
    {
        if (!file_) return;
        std::lock_guard<std::mutex> lock(mutex_); // render + application threads
        const auto ms = static_cast<unsigned long long>(sceKernelGetProcessTimeWide() / 1000u);
        std::fprintf(file_, "[%llu] %s%s%s\n", ms, event, detail ? " " : "", detail ? detail : "");
        std::fflush(file_);
    }
private:
    std::FILE* file_{};
    std::mutex mutex_;
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
    WorldAudioResult PumpAudio(bSound&) override
    {
        if (!audioEnabled_) return WORLD_AUDIO_OK;
        const auto result = audio_.Pump();
        if (result != WORLD_AUDIO_OK) log_.Line("audio.failure", audio_.Error().c_str());
        if (++audioPumps_ % 300u == 0) {
            const auto audio = audio_.TakeStatistics();
            char detail[200];
            std::snprintf(detail, sizeof(detail),
                "pumps=300 decode_ms=%.3f commands_ms=%.3f mix_ms_total=%.1f buffers=%llu voices_peak=%u",
                audio.decodeMilliseconds / 300.0, audio.commandMilliseconds / 300.0, audio.mixMilliseconds,
                static_cast<unsigned long long>(audio.buffers), audio.voicesPeak);
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
    vita_crash::Init(kDataDirectory, [](const char* event, const char* detail) { if (g_log) g_log->Line(event, detail); });
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
    sceKernelExitProcess(0);
    return 0;
}
