// PS Vita host for the native PAL engine (spike).
// Stage 1: CPU-only host. SubmitFrame is a null renderer so the engine's own
// render.timing_window / render.phase_window profile measures the CPU side
// (logic + capture) on the Cortex-A9 before any GPU backend exists.
#include <psp2/ctrl.h>
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/appmgr.h>
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
#include "alloccensus.hpp"
namespace vita_clock { extern bool census; void Note(const void*) noexcept; void Dump(char*, std::size_t) noexcept; }
extern int bMatrixNeonEnabled;
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
// newlib serves blocks under 1 MB; bigger ones come from kernel memory blocks
// (src/vita/alloccensus.cpp) so a fragmented arena can't refuse them. The
// 120 MB moved out of newlib stays free for those blocks: vitaGL's RAM
// threshold below grows by the same amount, so its pool is unchanged.
constexpr unsigned kBigBlockBudget = 120u * 1024u * 1024u;
unsigned int _newlib_heap_size_user = 280u * 1024u * 1024u - kBigBlockBudget;
unsigned int sceUserMainThreadStackSize = 4u * 1024u * 1024u;
}

namespace hp2 { extern int nativeRaceStartDrainFrames; extern int nativeSpeedTrapSettle; }
namespace hp2 { extern int ecstasyHalfCompositeBounds; extern int ecstasyHalfDepthPolicy; extern int debugQuickRaceTrack, debugQuickRaceReverse; extern int debugDemoTrack; extern int debugBenchSeed; extern std::atomic<int> debugDemoStarted; extern int debugRaceInputStep; extern int debugCarSpeedLog; namespace ai { extern int retailThinkTurns; } extern int debugBenchMirror; }
extern int collisionWitnessWarmProbe;
extern int collisionQueuePrediction;
extern uint32_t (*collisionWitnessProbeClock)();
extern uint32_t (*collisionWitnessClockMicros)();
extern int worldParallelCapture;
extern int worldParallelResolve;
namespace hp2::frontend { extern int flailerDiscardEarlyOut; }
namespace hp2 { extern int ecstasyAsyncLateJoin; extern int ecstasyAsyncParticleSplit; extern int ecstasyPosedStream; extern int ecstasyDiagNoDraw; extern int ecstasyAttachPrefetch; extern int ecstasySceneryLeafFast; extern int ecstasySceneryLeafVerify; extern int ecstasySceneryPrefetch; extern int ecstasySceneryFirstFast; extern int ecstasyReplayFast; extern int ecstasySceneryRunReplay; extern int ecstasyGatherDetail; extern double ecstasyGatherDetailMs; extern float ecstasyParticleNearFadeStart, ecstasyParticleNearFadeEnd; extern int ecstasyParticleMirrorCull; extern int ecstasySceneryReuseCensus; extern int ecstasyFastSceneryCapture; extern int ecstasySceneryRuns; extern int ecstasySceneryStripCull; extern int ecstasyFragmentMediump; extern int ecstasyForceNoVsync; extern int ecstasyVsyncInterval; extern std::uint32_t ecstasyGeometryPrewarmMax; extern std::uint32_t ecstasyVictimPinScan; extern std::uint32_t ecstasyPrewarmVictimIdle; extern std::uint32_t ecstasyGeometryPrewarmBurst; extern std::uint32_t ecstasyGeometryPrewarmWindow; extern std::uint32_t ecstasyGeometryPrewarmLookahead; extern int ecstasyGpuDiag; extern int ecstasyModelRunCache; extern int ecstasyTexturePrewarm; extern int ecstasyGeometryPrewarm; extern int ecstasyScissorElide; extern int ecstasySunQuery; extern int ecstasyInstancing; extern int ecstasyGxmDirect; extern int ecstasyGxmParity; extern int ecstasyGxmFlushDraws; extern int ecstasyGxmProf; extern int ecstasyDeferOrder; extern int ecstasyGpuTimeline; extern int ecstasyGatherSampler; void StartGatherSampler(); void StartVitaPgo(); extern int ecstasyHalfFirstRun; extern int ecstasyRetireStamp; extern int ecstasyTexturePrewarmFair; extern int ecstasyGpuProbe; extern int ecstasyGpuHeavyShots; extern int ecstasyResidentVram; extern int ecstasyAlphaElide; extern int ecstasyDeferResolve; extern int ecstasyAlphaCheck; extern int ecstasySceneWorkerPriority; extern int ecstasyParallelSceneryPrepare; extern int ecstasyHalfResSkipAlphaTest; extern int ecstasyHalfResHardwareDepth; extern int ecstasySceneJobsSerial; extern int ecstasySceneClaimPool; extern int ecstasyAsyncMirrorScenery; extern int ecstasyAsyncParticles; extern int ecstasyPrepareCache; extern int ecstasyRigidSceneryBatch; extern int ecstasyShadingResponseTables; extern std::atomic<int> ecstasyAsyncMirrorPhase; extern int ecstasyAffineUV; extern int ecstasySceneryFastMerge; extern int ecstasyParallelSceneryEmit; extern int ecstasyParallelParticles; extern int ecstasyParticleJobsPooled; extern int ecstasyParallelModelStrips; extern int ecstasyParallelSkinStrips; extern int ecstasyHalfResParticles; extern double ecstasyHalfResMinScreens; extern double ecstasyQuarterResMinScreens; extern int ecstasyHalfResDepthFlip; extern int ecstasyHalfResSync; extern int ecstasyHalfResCompositeFlip; extern std::vector<std::uint64_t> ecstasyScreenshotFrames; extern std::vector<std::uint64_t> ecstasyDisplayShotFrames; }
namespace hp2 { extern std::array<int,2> ecstasySceneWorkerCores; extern int ecstasyDiscardSink; }
extern int collisionSpeculationMode;
extern uint64_t collisionSpeculationVerifyBudget;

namespace {
class VitaLog;
VitaLog* g_log{};
int mainThreadPriority = -1;
}
int vitaWatchedThreads[5]{-1, -1, -1, -1, -1}; // main, render, GL, audio out, audio worker (thread_cpu.txt)
extern int collisionHelperThreadIds[2];
namespace {
// thread_cpu.txt diagnostic: CPU share of the main, render, GL and helper
// threads (kernel runClocks deltas) every 5 s (vita.thread_cpu).
void IdleLogLine(const char* text);
bool g_allocProfile = false;
void StartThreadCpuProbe()
{
    const SceUID logger = sceKernelCreateThread("hp2_cpu_log", [](SceSize, void*) -> int {
        static const char* const names[7] = {"main", "render", "gl", "helper0", "helper1", "audio", "audio_worker"};
        SceUInt64 before[7]{};
        SceUInt64 start = sceKernelGetSystemTimeWide();
        for (;;) {
            sceKernelDelayThread(5000000);
            const SceUInt64 now = sceKernelGetSystemTimeWide();
            const double wall = static_cast<double>(now - start); start = now;
            const int ids[7] = {vitaWatchedThreads[0], vitaWatchedThreads[1], vitaWatchedThreads[2],
                collisionHelperThreadIds[0], collisionHelperThreadIds[1], vitaWatchedThreads[3], vitaWatchedThreads[4]};
            char text[320]; int at = 0;
            for (int k = 0; k < 7; ++k) {
                SceKernelThreadInfo info{}; info.size = sizeof(info);
                if (ids[k] < 0 || sceKernelGetThreadInfo(ids[k], &info) < 0) continue;
                const SceUInt64 clocks = info.runClocks;
                at += std::snprintf(text + at, sizeof(text) - at, "%s=%.1f/c%d ", names[k],
                    before[k] ? 100.0 * static_cast<double>(clocks - before[k]) / wall : 0.0, static_cast<int>(info.lastExecutedCpuId));
                before[k] = clocks;
                if (at >= static_cast<int>(sizeof(text))) break;
            }
            IdleLogLine(text);
            if (g_allocProfile) {char prof[320]; vita_census::ProfileLine(prof, sizeof(prof)); IdleLogLine(prof); vita_census::ProfileStacks(IdleLogLine);}
        }
        return 0;
    }, 64, 0x2000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (logger >= 0) sceKernelStartThread(logger, 0, nullptr);
}
// Display truth (always on, ~free): the vitaGL display-queue callback stamps
// each flip with the vblank counter; a background thread logs the histogram
// of vblanks between flips every 5 s (vita.flips: d2 = on time at 30 fps,
// d3+ = a frame shown late). in_game counts flips while a race is running.
std::atomic<std::uint32_t> g_flipLastVcount{0};
std::array<std::atomic<std::uint32_t>, 6> g_flipIntervals{};
std::atomic<int> g_flipInGame{0};
std::array<std::uint64_t, 12> g_flipGaps{};
std::atomic<std::uint32_t> g_flipGapCount{0};
void FlipCallback(void*)
{
    const std::uint32_t now = static_cast<std::uint32_t>(sceDisplayGetVcount());
    const std::uint32_t last = g_flipLastVcount.exchange(now, std::memory_order_relaxed);
    if (!last) return;
    std::uint32_t delta = now - last;
    if (delta >= 3 && g_flipInGame.load(std::memory_order_relaxed)) {
        // Late in-game flip: keep (vblanks, process ms) for the log thread.
        const auto at = g_flipGapCount.fetch_add(1, std::memory_order_relaxed);
        if (at < g_flipGaps.size())
            g_flipGaps[at] = (static_cast<std::uint64_t>(delta) << 40) | (sceKernelGetProcessTimeWide() / 1000u);
    }
    if (delta > 5) delta = 5;
    g_flipIntervals[delta].fetch_add(1, std::memory_order_relaxed);
}
void LogFlipLine(const char* detail);
void LogFlipGaps(const char* detail);
void* FlipLogThread(void*)
{
    for (;;) {
        sceKernelDelayThread(5u * 1000u * 1000u);
        std::uint32_t counts[6];
        for (int i = 0; i < 6; ++i) counts[i] = g_flipIntervals[i].exchange(0, std::memory_order_relaxed);
        char detail[128];
        std::snprintf(detail, sizeof(detail), "in_game=%d d1=%u d2=%u d3=%u d4=%u d5plus=%u",
            g_flipInGame.load(std::memory_order_relaxed), counts[1], counts[2], counts[3], counts[4], counts[5]);
        LogFlipLine(detail);
        const auto gaps = std::min<std::uint32_t>(g_flipGapCount.load(std::memory_order_relaxed), g_flipGaps.size());
        if (gaps) {
            char text[400]; int used = std::snprintf(text, sizeof(text), "gaps=%u", g_flipGapCount.load(std::memory_order_relaxed));
            for (std::uint32_t i = 0; i < gaps && used < static_cast<int>(sizeof(text)) - 24; ++i)
                used += std::snprintf(text + used, sizeof(text) - used, " %ux@%llu", static_cast<unsigned>(g_flipGaps[i] >> 40),
                    static_cast<unsigned long long>(g_flipGaps[i] & ((1ull << 40) - 1)));
            g_flipGapCount.store(0, std::memory_order_relaxed);
            LogFlipGaps(text);
        }
    }
    return nullptr;
}
class VitaApplicationHost;
VitaApplicationHost* g_host{};
// Watchdog diagnostics: main-thread stage and completed-frame counter.
std::atomic<int> g_mainStage{0};
std::atomic<unsigned> g_framesDone{0};
constexpr const char* kDataDirectory = "ux0:data/NFSHP2";
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
                    // Autopad race bench: the demo bench's fixed seed, so
                    // the same inputs replay the same race (A/B timing).
                    ::hp2::debugBenchSeed = 1;
                    std::snprintf(detail, sizeof(detail), "track=%d reverse=%d fixed_seed=1", track, reverse);
                    log_.Line("vita.autopad_track", detail);
                }
            }
            std::fclose(file);
        }
        // Race pad record/replay bench (with autopad.txt "<p> <h> <until> <track>"
        // for the menus and the fixed seed; autopad stops at the race): padrec.txt
        // records the raw pad per race input step to padrec.bin, padplay.txt
        // replays padrec.bin instead of the physical pad.
        {
            SceIoStat probe{};
            if (sceIoGetstat((std::string(kDataDirectory) + "/padplay.txt").c_str(), &probe) >= 0) {
                if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/padrec.bin").c_str(), "rb")) {
                    PadRecord record{};
                    while (std::fread(&record, sizeof(record), 1, file) == 1) padTrack_.push_back(record);
                    std::fclose(file);
                }
                padMode_ = 2;
                char detail[64];
                std::snprintf(detail, sizeof(detail), "replay steps=%u", static_cast<unsigned>(padTrack_.size()));
                log_.Line("vita.padrec", detail);
            } else if (sceIoGetstat((std::string(kDataDirectory) + "/padrec.txt").c_str(), &probe) >= 0) {
                padMode_ = 1;
                if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/padrec.bin").c_str(), "wb")) std::fclose(file);
                log_.Line("vita.padrec", "record");
            }
        }
        {
            SceIoStat probe{};
            if (sceIoGetstat((std::string(kDataDirectory) + "/car_speed_log.txt").c_str(), &probe) >= 0) ::hp2::debugCarSpeedLog = 1;
            // ai_think_loops.txt: old per-world-loop AI think turns (2x retail rate).
            if (sceIoGetstat((std::string(kDataDirectory) + "/ai_think_loops.txt").c_str(), &probe) >= 0) ::hp2::ai::retailThinkTurns = 0;
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
        const int raceStep = ::hp2::debugRaceInputStep;
        if (padMode_ && raceStep >= 0) {
            autoPeriod_ = 0; // the menus are behind; the race is recorded/replayed input only
            if (padMode_ == 2) {
                const auto at = static_cast<std::size_t>(raceStep);
                const PadRecord record = at < padTrack_.size() ? padTrack_[at] : PadRecord{0, {128, 128, 128, 128}};
                pad.buttons = record.buttons;
                pad.lx = record.sticks[0]; pad.ly = record.sticks[1]; pad.rx = record.sticks[2]; pad.ry = record.sticks[3];
            } else {
                padTrack_.push_back({pad.buttons, {pad.lx, pad.ly, pad.rx, pad.ry}});
                if (padTrack_.size() >= 600) FlushPadRecording();
            }
        }
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
    ~VitaApplicationHost() override { FlushPadRecording(); StopRenderThread(); }
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
        g_flipInGame.store(enabled ? 1 : 0, std::memory_order_relaxed);
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
            char detail[512];
            std::snprintf(detail, sizeof(detail),
                "pumps=300 decode_ms=%.3f commands_ms=%.3f pump_peak_ms=%.3f apply_peak_ms=%.3f apply_peak_kind=%d "
                "inner_peak_ms=%.3f inner_peak_music_ms=%.3f inner_peak_install_ms=%.3f inner_peak_commands_ms=%.3f inner_peak_requests=%u inner_peak_lock_ms=%.3f inner_peak_apply_ms=%.3f "
                "mix_ms_total=%.1f buffers=%llu voices_peak=%u reverb_buffers=%llu wet_voice_buffers=%llu reverb_mode=%d/%d reverb_depth=%d/%d",
                audio.decodeMilliseconds / 300.0, audio.commandMilliseconds / 300.0, pumpPeakUs_ / 1000.0,
                audio.applyPeakMilliseconds, audio.applyPeakKind,
                audio.pumpPeakMilliseconds, audio.pumpPeakMusicMs, audio.pumpPeakInstallMs, audio.pumpPeakCommandsMs, audio.pumpPeakRequests,audio.pumpPeakLockWaitMs,audio.pumpPeakApplyMs,
                audio.mixMilliseconds, static_cast<unsigned long long>(audio.buffers), audio.voicesPeak,
                static_cast<unsigned long long>(audio.reverbBuffers), static_cast<unsigned long long>(audio.wetVoiceBuffers),
                audio.reverbPreset[0], audio.reverbPreset[1], audio.reverbVolume[0], audio.reverbVolume[1]);
            pumpPeakUs_ = 0;
            log_.Line("audio.backend_window", detail);
            {
                // Top request kinds by apply time (per pump averages).
                int order[40];for (int k = 0; k < 40; ++k) order[k] = k;
                std::sort(order, order + 40, [&](int a, int b) {return audio.kindMilliseconds[a] > audio.kindMilliseconds[b];});
                int used = std::snprintf(detail, sizeof(detail), "requests=%.2f predecode_ms=%.3f apply_ms=%.3f lock_ms=%.3f",
                    audio.requests / 300.0, audio.preDecodeMilliseconds / 300.0, audio.applyMilliseconds / 300.0, audio.lockWaitMilliseconds / 300.0);
                for (int i = 0; i < 6 && used > 0 && used < static_cast<int>(sizeof(detail)); ++i) {
                    const int k = order[i];if (!audio.kindCounts[k]) break;
                    used += std::snprintf(detail + used, sizeof(detail) - used, " k%d=%u/%.3fms", k, audio.kindCounts[k], audio.kindMilliseconds[k] / 300.0);
                }
                log_.Line("audio.request_kinds", detail);
            }
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
        vitaWatchedThreads[1] = sceKernelGetThreadId();
        hp2::ecstasySceneJobsLane = 1;
        {
            std::uint32_t fpscr = 0;
            asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
            char detail[80]; std::snprintf(detail, sizeof(detail), "fpscr=0x%08x priority=%d main_priority=%d", static_cast<unsigned>(fpscr),
                sceKernelGetThreadCurrentPriority(), mainThreadPriority);
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
    struct PadRecord { std::uint32_t buttons; std::uint8_t sticks[4]; };
    int padMode_{}; // 1 record, 2 replay
    std::vector<PadRecord> padTrack_;
    void FlushPadRecording()
    {
        if (padMode_ != 1 || padTrack_.empty()) return;
        if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/padrec.bin").c_str(), "ab")) {
            std::fwrite(padTrack_.data(), sizeof(PadRecord), padTrack_.size(), file);
            std::fclose(file);
        }
        padTrack_.clear();
    }
    bool demoBench_{};
#ifdef HP2_ENABLE_VITAGL
    hp2::EcstasyVulkanDevice* graphics_{};
#endif
};
void LogFlipLine(const char* detail) { if (g_log) g_log->Line("vita.flips", detail); }
void LogFlipGaps(const char* detail) { if (g_log) g_log->Line("vita.flip_gaps", detail); }
}

namespace {
void IdleLogLine(const char* text) {if (g_log) g_log->Line("vita.thread_cpu", text);}
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
    mainThreadPriority = sceKernelGetThreadCurrentPriority();
    vitaWatchedThreads[0] = sceKernelGetThreadId();

    const std::filesystem::path base = kDataDirectory;
    const auto saves = base / "Saves";
    std::error_code ignored;
    std::filesystem::create_directories(saves, ignored);
    {   // After an out-of-memory relaunch keep the previous session's log.
        const std::string dir = kDataDirectory;
        SceIoStat relaunched{};
        if (sceIoGetstat((dir + "/relaunched.txt").c_str(), &relaunched) >= 0) {
            sceIoRemove((dir + "/relaunched.txt").c_str());
            sceIoRemove((dir + "/NFSHP2_log_prev.txt").c_str());
            sceIoRename((dir + "/NFSHP2_log.txt").c_str(), (dir + "/NFSHP2_log_prev.txt").c_str());
        }
    }
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
    {
        // Diagnostic A/B: scalar VFP matrix paths (identical results under FZ).
        SceIoStat neonProbe{};
        if (sceIoGetstat((std::string(kDataDirectory) + "/neon_matrix_off.txt").c_str(), &neonProbe) >= 0) {
            bMatrixNeonEnabled = 0;
            if (g_log) g_log->Line("vita.diag", "neon matrix off");
        }
    }
    hp2::platform::BindSessionSink([](const char* event, const char* detail) noexcept {
        if (g_log && std::strcmp(event, "file.open") != 0) g_log->Line(event, detail);
    });
    {
        SceIoStat census{};
        if (sceIoGetstat((std::string(kDataDirectory) + "/alloc_census.txt").c_str(), &census) >= 0) {
            vita_census::Enable();
            log.Line("vita.diag", "alloc census on");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/alloc_prof.txt").c_str(), &census) >= 0) {
            vita_census::EnableProfile(); g_allocProfile = true;
            log.Line("vita.diag", "heap call profile on (logged with thread_cpu.txt)");
        }
    }
    hp2::platform::BindHeapCheckpointHook([](const char* tag) noexcept {
        if (!tag || std::strncmp(tag, "enter:", 6) && std::strcmp(tag, "decode.failed")) return;
#ifdef HP2_ENABLE_VITAGL
        if (g_log) {
            char detail[160];
            std::snprintf(detail, sizeof(detail), "tag=%s vram_free_kb=%u ram_free_kb=%u phycont_free_kb=%u budget_free_kb=%u",
                tag, static_cast<unsigned>(vglMemFree(VGL_MEM_VRAM) / 1024), static_cast<unsigned>(vglMemFree(VGL_MEM_RAM) / 1024),
                static_cast<unsigned>(vglMemFree(VGL_MEM_PHYCONT) / 1024), static_cast<unsigned>(vglMemFree(VGL_MEM_BUDGET) / 1024));
            g_log->Line("vita.gpu_memory", detail);
            char heap[192]; vita_heap::Stats(heap, sizeof(heap));
            g_log->Line("vita.heap", heap);
        }
#endif
        if (!std::strcmp(tag, "enter:GAMEFLOW_STATE_LOADING_TRACK_COMMON")) vita_census::NextEpoch();
        if (!std::strcmp(tag, "enter:GAMEFLOW_STATE_LOADING_FRONTEND") || !std::strcmp(tag, "enter:GAMEFLOW_STATE_LOADING_TRACK_SPECIFIC") ||
            !std::strcmp(tag, "decode.failed"))
            vita_census::Dump(tag, [](const char* event, const char* detail) { if (g_log) g_log->Line(event, detail); });
    });
    // Out of memory while loading (heap fragmented after a long race): relaunch
    // the app instead of crashing. A fresh process returns the whole heap; the
    // game boots back to the menus. The log is kept as NFSHP2_log_prev.txt.
    hp2::platform::BindOutOfMemoryHook([](const char* what) noexcept {
        if (g_log) {
            g_log->Line("vita.out_of_memory", what ? what : "");
            g_log->Line("vita.out_of_memory", "relaunching the app to recover the heap");
            g_log->Flush();
        }
        const SceUID marker = sceIoOpen((std::string(kDataDirectory) + "/relaunched.txt").c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (marker >= 0) sceIoClose(marker);
        sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr);
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
        vglInitExtended(4 * 1024 * 1024, kWidth, kHeight, 12 * 1024 * 1024 + static_cast<int>(kBigBlockBudget), SCE_GXM_MULTISAMPLE_NONE);
        vglSetDisplayCallback(FlipCallback);
        {
            pthread_t flipThread; pthread_attr_t attributes; pthread_attr_init(&attributes);
            pthread_attr_setstacksize(&attributes, 32 * 1024);
            if (pthread_create(&flipThread, &attributes, FlipLogThread, nullptr) == 0) pthread_detach(flipThread);
            pthread_attr_destroy(&attributes);
        }
        {
            char heap[192]; vita_heap::Stats(heap, sizeof(heap));
            log.Line("vita.heap", heap);
        }
        vglWaitVblankStart(GL_FALSE);
        log.Line("vita.gl", "vitaGL initialized");
        hp2::EcstasyVulkanOptions graphicsOptions;
        graphicsOptions.width = kWidth; graphicsOptions.height = kHeight;
        graphicsOptions.logicalWidth = kWidth; graphicsOptions.logicalHeight = kHeight;
        graphicsOptions.fixedRenderExtent = true;
        graphicsOptions.vsync = false;
        auto graphics = std::make_unique<hp2::EcstasyVulkanDevice>(graphicsOptions);
        // Pipelined rendering is on unless ux0:data/NFSHP2/nopipeline.txt exists.
        SceIoStat noPipeline{};
        const bool pipeline = sceIoGetstat((std::string(kDataDirectory) + "/nopipeline.txt").c_str(), &noPipeline) < 0;
        log.Line("vita.gl", pipeline ? "render thread pipelining on" : "render thread pipelining off");
        host.AttachGraphics(*graphics, pipeline);
        hp2::nativeRenderDrainHook = [] { if (g_host) g_host->FinishFrames(); };
        g_host = &host;
#endif
        // Audio is on unless ux0:data/NFSHP2/noaudio.txt exists (then music
        // must be off too: the jukebox needs the audio device as its owner).
        SceIoStat noAudio{};
        if (sceIoGetstat((std::string(kDataDirectory) + "/noaudio.txt").c_str(), &noAudio) >= 0) {
            host.DisableAudio();
            applicationLaunchOptions.musicEnabled = 0;
            log.Line("vita.audio", "disabled by noaudio.txt");
        }
        SceIoStat probe{};
        collisionWitnessClockMicros = []{ extern uint32_t diagClockReads; ++diagClockReads;
            if (vita_clock::census) vita_clock::Note(__builtin_return_address(0));
            return (uint32_t)sceKernelGetProcessTimeLow(); };
        if (sceIoGetstat((std::string(kDataDirectory) + "/clock_census.txt").c_str(), &probe) >= 0) {
            vita_clock::census = true;
            extern void (*diagClockCensusDump)(char*, std::size_t);
            diagClockCensusDump = vita_clock::Dump;
            char base[96];
            std::snprintf(base, sizeof(base), "clock census on note_addr=%p", reinterpret_cast<const void*>(&vita_clock::Note));
            log.Line("vita.diag", base);
        }
        {
            // Diagnostic: cost of one timer read (the split timers are hot).
            const uint64_t started = sceKernelGetProcessTimeWide();
            volatile uint32_t sink = 0;
            for (int index = 0; index < 1000; ++index) sink += collisionWitnessClockMicros();
            (void)sink;
            char detail[64];
            std::snprintf(detail, sizeof(detail), "ns_per_read=%llu",
                (unsigned long long)(sceKernelGetProcessTimeWide() - started));
            log.Line("vita.clock", detail);
        }
        // Exact speculative collision separation on cores 1/2 (exact commit
        // check per query). Serial verification of the first separations is
        // off by default: it ran at the first race start and is a test, not
        // a safeguard (PC HP2_COLLISION_SPEC=2 verifies). collision_spec.txt
        // "3 2000" restores it.
        collisionSpeculationMode = 3; collisionSpeculationVerifyBudget = 0;
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
        {
            // Calibration: cost of the steady_clock reads used by the timers,
            // and of a 64 KB memcpy (cache-resident and streaming).
            const auto start = std::chrono::steady_clock::now();
            auto last = start;
            for (int i = 0; i < 1000; ++i) last = std::chrono::steady_clock::now();
            const double clockUs = std::chrono::duration<double, std::micro>(last - start).count() / 1000.0;
            std::vector<unsigned char> a(4u << 20), b(4u << 20);
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 64; ++i) std::memcpy(b.data(), a.data() + (i & 1) * 64, 65536);
            auto t1 = std::chrono::steady_clock::now();
            std::memcpy(b.data(), a.data(), a.size());
            auto t2 = std::chrono::steady_clock::now();
            char detail[160];
            std::snprintf(detail, sizeof(detail), "calibrate clock_us=%.3f memcpy64k_hot_mbps=%.0f memcpy4m_mbps=%.0f", clockUs,
                64.0 * 65536.0 / std::chrono::duration<double, std::micro>(t1 - t0).count(),
                double(a.size()) / std::chrono::duration<double, std::micro>(t2 - t1).count());
            log.Line("vita.diag", detail);
        }
        if (FILE* f = std::fopen((std::string(kDataDirectory) + "/diag_nodraw.txt").c_str(), "rb")) {
            int mode = 1; if (std::fscanf(f, "%d", &mode) != 1) mode = 1; std::fclose(f);
            hp2::ecstasyDiagNoDraw = mode;
            char detail[48]; std::snprintf(detail, sizeof(detail), "DIAG: GPU draws skipped mode=%d", mode);
            log.Line("vita.diag", detail);
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/diag_nocars.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyDiagSkipCars = true;
            log.Line("vita.diag", "DIAG: car rendering skipped");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/diag_noscenery.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyDiagSkipScenery = true;
            log.Line("vita.diag", "DIAG: scenery geometry skipped");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/model_runs_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyModelRuns = 0;
            log.Line("vita.diag", "model strip runs off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_fast_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyFastSceneryCapture = 0;
            log.Line("vita.diag", "scenery fast capture off");
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
        if (sceIoGetstat((std::string(kDataDirectory) + "/half_firstrun.txt").c_str(), &probe) >= 0) hp2::ecstasyHalfFirstRun = 1;
        if (sceIoGetstat((std::string(kDataDirectory) + "/retire_old.txt").c_str(), &probe) >= 0) hp2::ecstasyRetireStamp = 0;
        if (sceIoGetstat((std::string(kDataDirectory) + "/retire_verify.txt").c_str(), &probe) >= 0) hp2::ecstasyRetireStamp = 2;
        if (sceIoGetstat((std::string(kDataDirectory) + "/prewarm_unfair.txt").c_str(), &probe) >= 0) hp2::ecstasyTexturePrewarmFair = 0;
        if (sceIoGetstat((std::string(kDataDirectory) + "/gather_sampler.txt").c_str(), &probe) >= 0) {hp2::ecstasyGatherSampler = 1; hp2::StartGatherSampler();}
        if (sceIoGetstat((std::string(kDataDirectory) + "/thread_cpu.txt").c_str(), &probe) >= 0) StartThreadCpuProbe();
        hp2::StartVitaPgo(); // HP2_VITA_PGO=generate builds only
        if (sceIoGetstat((std::string(kDataDirectory) + "/gpu_probe.txt").c_str(), &probe) >= 0) hp2::ecstasyGpuProbe = 1;
        if (sceIoGetstat((std::string(kDataDirectory) + "/halfres_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyHalfResParticles = 0;
            log.Line("vita.diag", "half-resolution particles off");
        }
        if (FILE* fade = std::fopen((std::string(kDataDirectory) + "/particle_fade.txt").c_str(), "r")) {
            float start = 0.0f, end = 0.0f;
            if (std::fscanf(fade, "%f %f", &start, &end) == 2) {
                hp2::ecstasyParticleNearFadeStart = start;hp2::ecstasyParticleNearFadeEnd = end;
            }
            std::fclose(fade);
            log.Line("vita.diag", ("particle near fade " + std::to_string(hp2::ecstasyParticleNearFadeStart) + " " +
                std::to_string(hp2::ecstasyParticleNearFadeEnd)).c_str());
        }
        hp2::nativeRaceStartDrainFrames = 3;
        if (FILE* drain = std::fopen((std::string(kDataDirectory) + "/race_start_drain.txt").c_str(), "r")) {
            int frames = 3;
            if (std::fscanf(drain, "%d", &frames) == 1 && frames >= 0 && frames <= 60) hp2::nativeRaceStartDrainFrames = frames;
            std::fclose(drain);
        }
        log.Line("vita.diag", ("race start drain frames " + std::to_string(hp2::nativeRaceStartDrainFrames)).c_str());
        if (sceIoGetstat((std::string(kDataDirectory) + "/speedtrap_retail.txt").c_str(), &probe) >= 0) {
            hp2::nativeSpeedTrapSettle = 0;
            log.Line("vita.diag", "speed trap: retail snap-back");
        }
        if (FILE* detail = std::fopen((std::string(kDataDirectory) + "/gather_detail.txt").c_str(), "r")) {
            double value = 26.0;
            if (std::fscanf(detail, "%lf", &value) == 1) hp2::ecstasyGatherDetailMs = value;
            std::fclose(detail);hp2::ecstasyGatherDetail = 1;
            log.Line("vita.diag", ("gather detail over ms " + std::to_string(hp2::ecstasyGatherDetailMs)).c_str());
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/particle_mirror_on.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyParticleMirrorCull = 0;
            log.Line("vita.diag", "particles drawn in mirror");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/half_bounds_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyHalfCompositeBounds = 0;
            log.Line("vita.diag", "half composite bounds off");
        }
        if (FILE* policy = std::fopen((std::string(kDataDirectory) + "/half_depth_policy.txt").c_str(), "r")) {
            int value = 2;
            if (std::fscanf(policy, "%d", &value) == 1) hp2::ecstasyHalfDepthPolicy = value;
            std::fclose(policy);
            log.Line("vita.diag", ("half depth policy " + std::to_string(hp2::ecstasyHalfDepthPolicy)).c_str());
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
        if (FILE* gate = std::fopen((std::string(kDataDirectory) + "/quarterres_min.txt").c_str(), "r")) {
            double value = 0.0;
            if (std::fscanf(gate, "%lf", &value) == 1) hp2::ecstasyQuarterResMinScreens = value;
            std::fclose(gate);
            log.Line("vita.diag", ("quarter-res min screens " + std::to_string(hp2::ecstasyQuarterResMinScreens)).c_str());
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
        if (sceIoGetstat((std::string(kDataDirectory) + "/particles_jobs_serial.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyParticleJobsPooled = 0;
            log.Line("vita.diag", "particle jobs inline");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/particles_serial.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyParallelParticles = 0;
            log.Line("vita.diag", "particle parallel build off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/discard_sink_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyDiscardSink = 0;
            log.Line("vita.diag", "discarded capture sink off");
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
        if (sceIoGetstat((std::string(kDataDirectory) + "/alpha_check.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyAlphaCheck = 1;
            if (FILE* check = std::fopen((std::string(kDataDirectory) + "/alpha_check.txt").c_str(), "r")) {
                int mode = 1;
                if (std::fscanf(check, "%d", &mode) == 1 && mode == 2) hp2::ecstasyAlphaCheck = 2;
                std::fclose(check);
            }
            log.Line("vita.diag", "alpha check on");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/parallel_capture_off.txt").c_str(), &probe) >= 0) {
            ::worldParallelCapture = 0;
            log.Line("vita.diag", "parallel participant capture off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/attach_prefetch_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyAttachPrefetch = 0;
            log.Line("vita.diag", "attach prefetch off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_leaf_fast_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneryLeafFast = 0;
            log.Line("vita.diag", "scenery leaf fast path off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_leaf_verify.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneryLeafVerify = 1;
            log.Line("vita.diag", "scenery leaf verify");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/defer_resolve_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyDeferResolve = 0;
            log.Line("vita.diag", "deferred index resolve off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/alpha_elide_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyAlphaElide = 0;
            log.Line("vita.diag", "alpha elide off");
        }
        if (FILE* timeline = std::fopen((std::string(kDataDirectory) + "/gpu_timeline.txt").c_str(), "r")) {
            hp2::ecstasyGpuTimeline = 1;
            int mode = 0, shots = 0;
            if (std::fscanf(timeline, "%d %d", &mode, &shots) == 2 && shots > 0) hp2::ecstasyGpuHeavyShots = shots;
            std::fclose(timeline);
            log.Line("vita.diag", "DIAGNOSTIC gpu timeline");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/resident_vram.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyResidentVram = 1;
            log.Line("vita.diag", "resident geometry in CDRAM");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/defer_order_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyDeferOrder = 0;
            log.Line("vita.diag", "defer order off");
        }
        if (FILE* flush = std::fopen((std::string(kDataDirectory) + "/gxm_flush.txt").c_str(), "r")) {
            int draws = 0;
            if (std::fscanf(flush, "%d", &draws) == 1 && draws > 0) hp2::ecstasyGxmFlushDraws = draws;
            std::fclose(flush);
            log.Line("vita.diag", ("gxm midscene flush every " + std::to_string(hp2::ecstasyGxmFlushDraws) + " draws").c_str());
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/gxm_prof.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyGxmProf = 1;
            log.Line("vita.diag", "gxm pass profile");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/gxm_parity.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyGxmParity = 1;
            log.Line("vita.diag", "gxm parity shots");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/gxm_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyGxmDirect = 0;
            log.Line("vita.diag", "direct gxm off (vitaGL draws)");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/instance_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyInstancing = 0;
            log.Line("vita.diag", "instancing off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_reuse.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneryReuseCensus = 1;
            log.Line("vita.diag", "scenery reuse census on");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_first_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneryFirstFast = 0;
            log.Line("vita.diag", "scenery first-strip record off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_prefetch_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneryPrefetch = 0;
            log.Line("vita.diag", "scenery walk prefetch off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/replay_fast_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyReplayFast = 0;
            log.Line("vita.diag", "epoch replay fast path off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scenery_replay_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneryRunReplay = 0;
            log.Line("vita.diag", "scenery run replay off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/sun_query_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySunQuery = 0;
            log.Line("vita.diag", "sun query off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scissor_elide_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyScissorElide = 0;
            log.Line("vita.diag", "scissor elide off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/texture_prewarm_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyTexturePrewarm = 0;
            log.Line("vita.diag", "texture prewarm off");
        }
        // Geometry prewarm, timed: strips are listed with the seal of their
        // first use in a world (geometry_prewarm2.txt). Those first used in
        // the first 900 seals are prepared behind the loading screen (burst
        // 4000), later ones 300 seals ahead of their recorded first use. The
        // old untimed list thrashed the 24 MB cache on the larger tracks;
        // the timed one holds only the near future. geometry_prewarm_off.txt.
        // 2026-11-24: off by default. On the demo bench (tracks 45/21) the
        // prewarmed strips evicted strips in view: 1000-strip reveals mid-
        // race stalled Gather 150-600 ms (late flips 7.7% -> 5.8-6.3% off).
        // geometry_prewarm_on.txt restores it (gentle eviction, bounded scan).
        hp2::ecstasyGeometryPrewarm = sceIoGetstat((std::string(kDataDirectory) + "/geometry_prewarm_on.txt").c_str(), &probe) >= 0 ? 1 : 0;
        if (hp2::ecstasyGeometryPrewarm) log.Line("vita.diag", "geometry prewarm on");
        hp2::ecstasyGeometryPrewarmWindow = 900;
        hp2::ecstasyGeometryPrewarmBurst = 4000;
        if (sceIoGetstat((std::string(kDataDirectory) + "/async_join_early.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyAsyncLateJoin = 0;
            log.Line("vita.diag", "async mirror joins all tasks at view 3");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/async_particles_whole.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyAsyncParticleSplit = 0;
            log.Line("vita.diag", "async particle prepare as one task");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/posed_stream_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyPosedStream = 0;
            log.Line("vita.diag", "posed strips resident (posed stream off)");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/prewarm_evict_any.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyPrewarmVictimIdle = 0;
            log.Line("vita.diag", "geometry prewarm may evict any idle entry");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/music_main.txt").c_str(), &probe) >= 0) {
            extern int vitaAudioMusicOnWorker;
            vitaAudioMusicOnWorker = 0;
            log.Line("vita.diag", "music refill on the game thread");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/shading_tables_on.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyShadingResponseTables = 1;
            log.Line("vita.diag", "shading records copy the specular response tables");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/worker_boost_off.txt").c_str(), &probe) >= 0) {
            extern int vitaAudioWorkerBoostLocks;
            vitaAudioWorkerBoostLocks = 0;
            log.Line("vita.diag", "audio worker locks at its own priority");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/victim_scan_full.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyVictimPinScan = 0;
            log.Line("vita.diag", "resident victim scan unbounded");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/geometry_prewarm_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyGeometryPrewarm = 0;
            log.Line("vita.diag", "geometry prewarm off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/model_run_cache_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyModelRunCache = 0;
            log.Line("vita.diag", "model run cache off");
        }
        if (FILE* diag = std::fopen((std::string(kDataDirectory) + "/gpu_diag.txt").c_str(), "r")) {
            int mode = 0;
            if (std::fscanf(diag, "%d", &mode) == 1) hp2::ecstasyGpuDiag = mode;
            std::fclose(diag);
            char text[48];std::snprintf(text, sizeof(text), "gpu diagnostic mode %d", hp2::ecstasyGpuDiag);
            log.Line("vita.diag", text);
        }
        if (FILE* file = std::fopen((std::string(kDataDirectory) + "/geometry_prewarm_window.txt").c_str(), "r")) {
            unsigned value = 0;
            if (std::fscanf(file, "%u", &value) == 1) hp2::ecstasyGeometryPrewarmWindow = value;
            std::fclose(file);
        }
        if (FILE* file = std::fopen((std::string(kDataDirectory) + "/geometry_prewarm_lookahead.txt").c_str(), "r")) {
            unsigned value = 0;
            if (std::fscanf(file, "%u", &value) == 1) hp2::ecstasyGeometryPrewarmLookahead = value;
            std::fclose(file);
        }
        if (FILE* file = std::fopen((std::string(kDataDirectory) + "/geometry_prewarm_burst.txt").c_str(), "r")) {
            unsigned value = 0;
            if (std::fscanf(file, "%u", &value) == 1) hp2::ecstasyGeometryPrewarmBurst = value;
            std::fclose(file);
        }
        if (FILE* file = std::fopen((std::string(kDataDirectory) + "/geometry_prewarm_max.txt").c_str(), "r")) {
            unsigned value = 0;
            if (std::fscanf(file, "%u", &value) == 1) hp2::ecstasyGeometryPrewarmMax = value;
            std::fclose(file);
            char detail[48];
            std::snprintf(detail, sizeof(detail), "geometry prewarm max %u", hp2::ecstasyGeometryPrewarmMax);
            log.Line("vita.diag", detail);
        }
        // 30 fps lock by default (present every 2nd vblank); fps_uncapped.txt
        // restores vitaGL's one-vblank interval.
        hp2::ecstasyVsyncInterval = 2;
        if (sceIoGetstat((std::string(kDataDirectory) + "/fps_uncapped.txt").c_str(), &probe) >= 0)
            hp2::ecstasyVsyncInterval = 0;
        log.Line("vita.diag", hp2::ecstasyVsyncInterval ? "30 fps lock (vsync interval 2)" : "uncapped (vsync interval 1)");
        if (sceIoGetstat((std::string(kDataDirectory) + "/vsync_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyForceNoVsync = 1;
            log.Line("vita.diag", "vsync forced off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/frag_mediump.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyFragmentMediump = 1;
            log.Line("vita.diag", "fragment precision mediump");
        }
        // Scene worker jobs share cores 1/2 with the gather and GL threads;
        // inline jobs measured faster on heavy tracks (track 64 traverse
        // 5.5 -> 1.4 ms/view). scene_jobs_parallel.txt restores the pool.
        hp2::ecstasySceneJobsSerial = 3;
        if (sceIoGetstat((std::string(kDataDirectory) + "/scene_claim_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneClaimPool = 0;
            log.Line("vita.diag", "scene claim pool off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/scene_jobs_parallel.txt").c_str(), &probe) >= 0) {
            hp2::ecstasySceneJobsSerial = 0;
            log.Line("vita.diag", "scene jobs parallel");
        }
        // scene_jobs.txt "<serial mask> <app worker cores> <render worker cores>":
        // mask bit0 application thread, bit1 render thread inline; cores
        // bit0 core 1, bit1 core 2 (A/B of per-thread scene job pools).
        if (FILE* jobs = std::fopen((std::string(kDataDirectory) + "/scene_jobs.txt").c_str(), "r")) {
            int mask = 3, app = 3, render = 3;
            const int read = std::fscanf(jobs, "%d %d %d", &mask, &app, &render);
            std::fclose(jobs);
            if (read >= 1) hp2::ecstasySceneJobsSerial = mask & 3;
            if (read >= 2) hp2::ecstasySceneWorkerCores[0] = app & 3;
            if (read >= 3) hp2::ecstasySceneWorkerCores[1] = render & 3;
            char text[96];
            std::snprintf(text, sizeof(text), "scene jobs serial_mask=%d app_cores=%d render_cores=%d",
                hp2::ecstasySceneJobsSerial, hp2::ecstasySceneWorkerCores[0], hp2::ecstasySceneWorkerCores[1]);
            log.Line("vita.diag", text);
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
        if (sceIoGetstat((std::string(kDataDirectory) + "/emit_twice.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyDiagEmitTwice = true;
            log.Line("vita.diag", "scenery emit twice (diagnostic)");
        }
        if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/pool_hot.txt").c_str(), "r")) {
            extern uint32_t collisionPoolHotMicros;
            unsigned value = 0;
            if (std::fscanf(file, "%u", &value) == 1) collisionPoolHotMicros = value;
            std::fclose(file);
            char text[64]; std::snprintf(text, sizeof(text), "collision pool hot polling %u us", value);
            log.Line("vita.diag", text);
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/helper_float.txt").c_str(), &probe) >= 0) {
            extern int collisionHelperFloat;
            collisionHelperFloat = 1;
            log.Line("vita.diag", "collision helpers float over cores 1-2");
        }
        if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/helper_wake.txt").c_str(), "r")) {
            extern int collisionHelperWakePriority;
            int value = 185;
            if (std::fscanf(file, "%d", &value) == 1 && value >= 64 && value <= 191) collisionHelperWakePriority = value;
            std::fclose(file);
            char text[64]; std::snprintf(text, sizeof(text), "collision helper wake priority %d", collisionHelperWakePriority);
            log.Line("vita.diag", text);
        }
        if (std::FILE* file = std::fopen((std::string(kDataDirectory) + "/parallel_order.txt").c_str(), "r")) {
            extern int worldParallelOrder;
            int value = 1;
            if (std::fscanf(file, "%d", &value) == 1) worldParallelOrder = value;
            std::fclose(file);
            char text[64]; std::snprintf(text, sizeof(text), "parallel capture order %d", worldParallelOrder);
            log.Line("vita.diag", text);
        }
        if (FILE* file = std::fopen((std::string(kDataDirectory) + "/walk_prefetch.txt").c_str(), "r")) {
            extern int collisionWalkPrefetch;
            int value = 0;
            if (std::fscanf(file, "%d", &value) == 1) collisionWalkPrefetch = value;
            std::fclose(file);
            char text[64]; std::snprintf(text, sizeof(text), "collision walk prefetch %d", value);
            log.Line("vita.diag", text);
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/witness_bench.txt").c_str(), &probe) >= 0) {
            extern int collisionWitnessBench; extern void (*collisionWitnessBenchLog)(const char*);
            static auto* benchLog = &log;
            collisionWitnessBench = 1;
            collisionWitnessBenchLog = [](const char* text) { benchLog->Line("vita.witness_bench", text); };
            log.Line("vita.diag", "witness bench armed");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/contact_pipe_off.txt").c_str(), &probe) >= 0) {
            extern int collisionContactPipeline;
            collisionContactPipeline = 0;
            log.Line("vita.diag", "contact pipeline off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/spec_abort_off.txt").c_str(), &probe) >= 0) {
            extern int collisionSpeculativeAbort;
            collisionSpeculativeAbort = 0;
            log.Line("vita.diag", "speculative abort off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/async_mirror.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyAsyncMirrorScenery = 1;
            log.Line("vita.diag", "async mirror scenery on");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/async_mirror_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyAsyncMirrorScenery = 0;
            log.Line("vita.diag", "async mirror scenery off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/resolve_parallel_off.txt").c_str(), &probe) >= 0) {
            worldParallelResolve = 0;
            log.Line("vita.diag", "parallel resolve captures off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/rigid_batch_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyRigidSceneryBatch = 0;
            log.Line("vita.diag", "rigid-animated scenery batch off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/prepare_cache_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyPrepareCache = 0;
            log.Line("vita.diag", "scenery prepare cache off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/hud_discard_full.txt").c_str(), &probe) >= 0) {
            ::hp2::frontend::flailerDiscardEarlyOut = 0;
            log.Line("vita.diag", "hud discard early-out off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/munition_prefix_off.txt").c_str(), &probe) >= 0) {
            extern int worldParallelMunitionPrefix;
            worldParallelMunitionPrefix = 0;
            log.Line("vita.diag", "parallel capture munition prefix off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/async_particles_off.txt").c_str(), &probe) >= 0) {
            hp2::ecstasyAsyncParticles = 0;
            log.Line("vita.diag", "async particles off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/show_fps.txt").c_str(), &probe) >= 0) {
            extern bool applicationShowFrameRate;
            applicationShowFrameRate = true;
            log.Line("vita.diag", "fps counter on");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/walk_memo_small.txt").c_str(), &probe) >= 0) {
            extern uint32_t collisionWitnessMemoEntries;
            collisionWitnessMemoEntries = 65536u;
            log.Line("vita.diag", "walk memo 65536 entries");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/walk_memo_off.txt").c_str(), &probe) >= 0) {
            extern int collisionWitnessMemo;
            collisionWitnessMemo = 0;
            log.Line("vita.diag", "walk memo off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/witness_cache_off.txt").c_str(), &probe) >= 0) {
            extern int collisionWitnessTransformCache;
            collisionWitnessTransformCache = 0;
            log.Line("vita.diag", "witness transform cache off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/requeue_pred_off.txt").c_str(), &probe) >= 0) {
            extern int collisionRequeuePrediction;
            collisionRequeuePrediction = 0;
            log.Line("vita.diag", "requeue prediction off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/sep_pipe_off.txt").c_str(), &probe) >= 0) {
            extern int collisionSeparatePipelined;
            collisionSeparatePipelined = 0;
            log.Line("vita.diag", "pipelined separation off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/memo_gate_off.txt").c_str(), &probe) >= 0) {
            extern int collisionMemoGate;
            collisionMemoGate = 0;
            log.Line("vita.diag", "walk memo gate off");
        }
        if (FILE* file = std::fopen((std::string(kDataDirectory) + "/pred_min.txt").c_str(), "r")) {
            extern int collisionPredictMinimum;
            int value = 0;
            if (std::fscanf(file, "%d", &value) == 1 && value >= 2) collisionPredictMinimum = value;
            std::fclose(file);
            char detail[48];
            std::snprintf(detail, sizeof(detail), "prediction minimum %d", collisionPredictMinimum);
            log.Line("vita.diag", detail);
        }
        {
            extern int diagFineTimers;
            diagFineTimers = sceIoGetstat((std::string(kDataDirectory) + "/diag_fine.txt").c_str(), &probe) >= 0;
            log.Line("vita.diag", diagFineTimers ? "fine timers on" : "fine timers off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/queue_pred_off.txt").c_str(), &probe) >= 0) {
            collisionQueuePrediction = 0;
            log.Line("vita.diag", "collision queue prediction off");
        }
        if (sceIoGetstat((std::string(kDataDirectory) + "/witness_kinds.txt").c_str(), &probe) >= 0) {
            collisionWitnessProbeClock = [] { return static_cast<uint32_t>(sceKernelGetProcessTimeLow()); };
            log.Line("vita.diag", "witness kind probe on");
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
                    std::snprintf(detail, sizeof(detail), "frames=%u main_stage=%d render_stage=%d quiet_s=%d mirror=%d",
                        now, g_mainStage.load(), hp2::EcstasyVulkanDevice::RenderStage(), quiet, hp2::ecstasyAsyncMirrorPhase.load());
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
