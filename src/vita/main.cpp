// PS Vita host for the native PAL engine (spike).
// Stage 1: CPU-only host. SubmitFrame is a null renderer so the engine's own
// render.timing_window / render.phase_window profile measures the CPU side
// (logic + capture) on the Cortex-A9 before any GPU backend exists.
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/kernel/sysmem.h>

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
#include <fstream>
#include <stdexcept>
#include <string>

extern "C" {
// Engine heap: newlib malloc arena. The rest of user memory stays for the
// kernel/GPU. Tunable once the real footprint is measured.
unsigned int _newlib_heap_size_user = 330u * 1024u * 1024u;
unsigned int sceUserMainThreadStackSize = 4u * 1024u * 1024u;
}

namespace {
class VitaLog;
VitaLog* g_log{};
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
        const auto ms = static_cast<unsigned long long>(sceKernelGetProcessTimeWide() / 1000u);
        std::fprintf(file_, "[%llu] %s%s%s\n", ms, event, detail ? " " : "", detail ? detail : "");
        std::fflush(file_);
    }
private:
    std::FILE* file_{};
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
        if (std::strncmp(event, "render.slow_", 12) == 0) return;
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
    }
    int32_t ReadInput(int32_t port, JoystickControls& destination) override
    {
        hp2::platform::ControllerState state = port == 0 ? state_ : hp2::platform::ControllerState{};
        destination = hp2::platform::NativeScannerControls(state);
        return state.connected ? 1 : 0;
    }
    void SetRumble(int32_t, uint8_t, uint8_t) override {}
    void SubmitFrame(hp2::EcstasyFrame& frame) override
    {
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
    bool SupportsAudio() const noexcept override { return false; }
    WorldAudioResult OpenAudio(bSound&) override { return WORLD_AUDIO_OK; }
    WorldAudioResult PumpAudio(bSound&) override { return WORLD_AUDIO_OK; }
    WorldAudioResult CloseAudio(bSound&) override { return WORLD_AUDIO_OK; }
    int Width() const override { return kWidth; }
    int Height() const override { return kHeight; }
    std::size_t AvailableMemory() const override
    {
        SceKernelFreeMemorySizeInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetFreeMemorySize(&info) < 0) return 0;
        return static_cast<std::size_t>(info.size_user);
    }
    void ApplyDisplaySettings(int32_t, int32_t, bool, bool) override {}

private:
    VitaLog& log_;
    const GameTime* clock_{};
    hp2::NativeRefreshClock refreshClock_;
    hp2::platform::ControllerState state_{};
    std::uint64_t frames_{}, commands_{};
    std::uint32_t polls_{};
};
}

int main(int, char**)
{
    // Max clocks: the engine is CPU bound on the Cortex-A9.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    const std::filesystem::path base = kDataDirectory;
    const auto saves = base / "Saves";
    std::error_code ignored;
    std::filesystem::create_directories(saves, ignored);
    VitaLog log((base / "NFSHP2_log.txt").string());
    g_log = &log;
    hp2::platform::BindSessionSink([](const char* event, const char* detail) noexcept {
        if (g_log && std::strcmp(event, "file.open") != 0) g_log->Line(event, detail);
    });
    try {
        log.Line("vita.start", "native PAL engine, stage1 null renderer");
        hp2::GameResources files(base / "ZZDATA.BIN");
        log.Line("vita.resources", "opened");
        hp2::configureNativeRenderSettings({});
        VitaApplicationHost host(log);
        hp2::ApplicationRuntime application(files, host, saves);
        host.BindClock(application.Clock());
        log.Line("vita.init", "begin");
        application.Initialize();
        log.Line("vita.init", "done");
        while (application.Tick()) {}
        application.Shutdown();
    } catch (const std::exception& error) {
        log.Line("vita.fatal", error.what());
        sceKernelDelayThread(1000000);
    }
    sceKernelExitProcess(0);
    return 0;
}
