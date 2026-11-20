#include <switch.h>

#include "audio.hpp"
#include "display.hpp"
#include "platform.hpp"
#include "async_log.hpp"
#include "control_layout.hpp"
#include "controller_session.hpp"
#include "config.hpp"
#include "pgo.hpp"
#include "pipeline_cache.hpp"

#include "bware/inc/bFile.hpp"
#include "bware/inc/bWare.hpp"
#include "indep/inc/CarDriver.hpp"
#include "indep/inc/Ecstasy.hpp"
#include "indep/inc/Flags.hpp"
#include "indep/inc/Timer.hpp"
#include "indep/inc/NativeRenderSettings.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

#ifndef __SWITCH2__
extern "C" {
// libnx applies this APM configuration in handheld mode. Docked uses HOS defaults.
u32 __nx_applet_PerformanceConfiguration[2]={0x92220008,0};
}
#endif

namespace {
constexpr int width=hp2::nx::renderWidth;
constexpr int height=hp2::nx::renderHeight;
using hp2::nx::installDirectory;
std::ostream* languageJournal{};
using hp2::platform::Ps2Button;

constexpr std::pair<HidNpadButton,Ps2Button> buttonMap[]={
    {HidNpadButton_Minus,Ps2Button::Select}, {HidNpadButton_Plus,Ps2Button::Start},
    {HidNpadButton_StickL,Ps2Button::L3}, {HidNpadButton_StickR,Ps2Button::R3},
    {HidNpadButton_Up,Ps2Button::Up}, {HidNpadButton_Right,Ps2Button::Right},
    {HidNpadButton_Down,Ps2Button::Down}, {HidNpadButton_Left,Ps2Button::Left},
    {HidNpadButton_ZL,Ps2Button::L2}, {HidNpadButton_ZR,Ps2Button::R2},
    {HidNpadButton_L,Ps2Button::L1}, {HidNpadButton_R,Ps2Button::R1},
    {HidNpadButton_X,Ps2Button::Triangle}, {HidNpadButton_B,Ps2Button::Circle},
    {HidNpadButton_A,Ps2Button::Cross}, {HidNpadButton_Y,Ps2Button::Square},
};

std::uint8_t axis(int value) noexcept
{
    return hp2::platform::NormalizeAxis(static_cast<std::int16_t>(std::clamp(value,-32768,32767)));
}

hp2::EcstasyVulkanOptions graphicsOptions()
{
    auto* window=nwindowGetDefault();
    if(!window)throw std::runtime_error("libnx did not provide a default display window");
    hp2::nx::ConfigureDisplayWindow();
    hp2::EcstasyVulkanOptions options;
    options.width=width;
    options.height=height;
    options.logicalWidth=width;
    options.logicalHeight=height;
    options.nativeWindow=window;
    options.fixedRenderExtent=true;
    options.asyncPipelines=true;
    options.streamTransformBatching=true;
    options.pipelineCachePath=std::string(installDirectory)+"/Saves/VulkanPipelineCache.bin";
    const auto seed=hp2::nx::BundledPipelineCache();
    options.pipelineCacheSeedData=seed.data;options.pipelineCacheSeedSize=seed.size;
    return options;
}

class SwitchApplicationHost final:public hp2::NativeApplicationHost {
public:
    SwitchApplicationHost(std::ostream& log,hp2::nx::PgoSession& profile)
        :log_(log),profile_(profile),graphics_(graphicsOptions())
    {
        padConfigureInput(8,HidNpadStyleSet_NpadStandard);
        for(std::size_t i=0;i<8;++i)padInitialize(&pads_[i],static_cast<HidNpadIdType>(i));
        padInitialize(&pads_[8],HidNpadIdType_Handheld);
        languageJournal=&log_;
        hp2::platform::BindSessionSink([](const char* event,const char* detail) noexcept {
            // Keep this diagnostic sparse: resource/movie journals can be
            // very busy, while language changes occur only on user actions.
            if(std::strncmp(event,"language.",9)!=0 || !languageJournal)return;
            *languageJournal<<event;if(detail)*languageJournal<<' '<<detail;
            *languageJournal<<std::endl;
        });
        hp2::nx::controllerSupportContext=this;
        hp2::nx::controllerSupport=[](void* context) {return static_cast<SwitchApplicationHost*>(context)->ConnectTwoPlayers();};
    }

    ~SwitchApplicationHost() override
    {
        FlushSlowFrames();
        hp2::platform::BindSessionSink(nullptr);languageJournal=nullptr;
        hp2::nx::controllerSupport=nullptr;hp2::nx::controllerSupportContext=nullptr;
        StopRumble();
    }

    void BindClock(const GameTime& clock) noexcept {clock_=&clock;}
    bool PollEvents() override
    {
        const int limit=hp2::nx::gameplayPresentation ? hp2::nx::userSettings.frameLimit:0;
        if(limit!=presentationLimit_.Limit()) {presentationLimit_.SetLimit(limit);presentationLimit_.Reset(FrameTime());}
        if(!appletMainLoop())return false;
        hp2::nx::UpdateDisplayRefresh();
        if(appletGetFocusState()!=AppletFocusState_InFocus) {
            StopRumble();
            profile_.Checkpoint(true);
            do {
                svcSleepThread(16'000'000);
                if(!appletMainLoop())return false;
            } while(appletGetFocusState()!=AppletFocusState_InFocus);
            hp2::nx::UpdateDisplayRefresh(true);
            ++timingEpoch_;
            refreshClock_={};presentationLimit_.Reset(FrameTime());
        }
        if(clock_) {
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
                FrameTime().time_since_epoch()).count();
            const auto refreshes=refreshClock_.Observe(ns,1'000'000'000,gameTimeGetVideoMode(clock_));
            for(std::uint64_t i=0;i<refreshes;++i) {
                graphicsNotifyPresentationComplete();
                platformNotifyVideoBlank();
            }
        }
        return true;
    }
    std::uint64_t PresentationTimingEpoch() const noexcept override {return timingEpoch_;}
    bool PresentationDue(hp2::NativeFrameTiming::Clock::time_point now) const noexcept override {return presentationLimit_.Due(now);}
    hp2::NativeFrameTiming::Clock::time_point PresentationDeadline() const noexcept override {return presentationLimit_.Deadline();}
    void WaitUntil(hp2::NativeFrameTiming::Clock::time_point deadline) override
    {
        const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-FrameTime()).count();
        if(remaining>0)svcSleepThread(std::min<std::int64_t>(remaining,16'000'000));
    }
    void RecordSessionEvent(const char* event,const char* detail) noexcept override
    {
        if constexpr(!hp2::platform::nativeDiagnosticsEnabled)return;
        if(std::strncmp(event,"render.slow_",12)==0) {
            // Keep frame logging in RAM until the existing summary flush.
            // A bounded fallback also handles a run where every frame is slow.
            if(slowFrameJournal_.size()>65536)FlushSlowFrames();
            slowFrameJournal_+=event;slowFrameJournal_+=' ';
            if(detail)slowFrameJournal_+=detail;slowFrameJournal_+='\n';return;
        }
        if(std::strcmp(event,"render.detail_window")==0 || std::strncmp(event,"shutdown.",9)==0)FlushSlowFrames();
        log_<<event;
        if(detail)log_<<' '<<detail;
        log_<<std::endl;
        if(std::strcmp(event,"render.detail_window")==0) {
            log_<<"render.capture_window submitted_captures="<<captureFrames_
                <<" commands="<<captureCommands_<<" instances="<<captureInstances_
                <<" projections="<<captureProjections_<<std::endl;
            log_<<"render.scenery_packets hits="<<sceneryPacketHits_
                <<" misses="<<sceneryPacketMisses_<<" fallbacks="<<sceneryPacketFallbacks_<<std::endl;
            const auto audio=audio_.TakeStatistics();
            const double frames=static_cast<double>(std::max<std::uint64_t>(captureFrames_,1));
            log_<<"audio.backend_window decode_ms="<<audio.decodeMilliseconds/frames
                <<" commands_ms="<<audio.commandMilliseconds/frames<<" mix_ms="<<audio.mixMilliseconds/frames
                <<" output_ms="<<audio.outputMilliseconds/frames<<" buffers="<<audio.buffers
                <<" voices_peak="<<audio.voicesPeak<<" voice_buffers="<<audio.voiceBuffers<<std::endl;
            captureFrames_=0;captureCommands_=0;captureInstances_=0;captureProjections_=0;
            sceneryPacketHits_=0;sceneryPacketMisses_=0;sceneryPacketFallbacks_=0;
        }
    }
    void BeginInputStep(hp2::NativeFrameTiming::Clock::time_point) override
    {
        if(routingSplit_!=hp2::nx::separatePlayers) {StopRumble();routingSplit_=hp2::nx::separatePlayers;}
        const auto previousPhysical=activePhysical_;
        for(std::size_t i=0;i<pads_.size();++i) {
            padUpdate(&pads_[i]);blockedButtons_[i]&=padGetButtons(&pads_[i]);
            sources_[i]=ReadPhysical(i);
            if(sources_[i].connected && (!sources_[activePhysical_].connected || sources_[i].held ||
                std::any_of(sources_[i].sticks.begin(),sources_[i].sticks.end(),[](auto value){return std::abs(int(value)-128)>24;})))activePhysical_=i;
        }
        if(!hp2::nx::separatePlayers && previousPhysical!=activePhysical_)StopRumble();
    }
    int32_t ReadInput(int32_t port,JoystickControls& destination) override
    {
        const auto state=hp2::nx::RoutedController(sources_,port,hp2::nx::separatePlayers);
        destination=hp2::platform::NativeScannerControls(state);
        return state.connected?1:0;
    }
    void SetRumble(int32_t port,uint8_t small,uint8_t large) override
    {
        if(port<0 || port>=JOYSTICK_PORT_COUNT)return;
        const auto index=static_cast<std::size_t>(port);
        const auto physical=hp2::nx::separatePlayers ? index:activePhysical_;
        const auto& pad=pads_[physical];
        auto& vibration=vibration_[index];
        if(!padIsConnected(&pad)) {
            vibration={};
            return;
        }
        const auto id=physical==8 ? HidNpadIdType_Handheld:static_cast<HidNpadIdType>(physical);
        const auto style=hidGetNpadStyleSet(id)&HidNpadStyleSet_NpadFullCtrl;
        if(!style)return;
        if(style!=vibration.style || id!=vibration.id) {
            if(vibration.style) {
                constexpr HidVibrationValue silent[]={{0,160,0,320},{0,160,0,320}};
                (void)hidSendVibrationValues(vibration.handles.data(),silent,2);
            }
            if(R_FAILED(hidInitializeVibrationDevices(vibration.handles.data(),2,id,
                static_cast<HidNpadStyleTag>(style))))return;
            vibration.style=style;
            vibration.id=id;
            vibration.small=0;
            vibration.large=0;
        }
        if(small==vibration.small && large==vibration.large)return;
        const HidVibrationValue value{large/255.0f,160.0f,small?1.0f:0.0f,320.0f};
        const HidVibrationValue values[]{value,value};
        if(R_SUCCEEDED(hidSendVibrationValues(vibration.handles.data(),values,2))) {
            vibration.small=small;
            vibration.large=large;
        }
    }
    void BeginSceneCapture() override {captureStarted_=FrameTime();}
    void SubmitFrame(hp2::EcstasyFrame& frame) override
    {
        ++captureFrames_;
        captureCommands_+=frame.Commands().size();
        captureInstances_+=frame.CapturedInstanceCount();
        captureProjections_+=frame.CapturedProjectionCount();
        const auto started=FrameTime();
        graphics_.Render(frame);
        if(graphics_.LastFrameStatistics().presentAccepted)Presented(started);
        const auto& statistics=graphics_.LastFrameStatistics();
        sceneryPacketHits_+=statistics.sceneryPacketHits;
        sceneryPacketMisses_+=statistics.sceneryPacketMisses;
        sceneryPacketFallbacks_+=statistics.sceneryPacketFallbacks;
    }
    bool TrySubmitPreparedFrame() override {
        const auto started=FrameTime();
        if(!graphics_.CanRenderPrepared() || !graphics_.RenderPrepared())return false;
        if(graphics_.LastFrameStatistics().presentAccepted) {captureStarted_=started;Presented(started);}
        return true;
    }
    void InvalidatePreparedFrame() noexcept override {graphics_.InvalidatePrepared();}
    bool ReadRenderStatistics(hp2::EcstasyVulkanFrameStatistics& result) const noexcept override
    {
        result=graphics_.LastFrameStatistics();
        return true;
    }
    WorldAudioResult OpenAudio(bSound& sound) override
    {
        const auto result=audio_.Open(sound);
        RecordSessionEvent("audio.open",result==WORLD_AUDIO_OK?"libnx audout":audio_.Error().c_str());
        return result;
    }
    WorldAudioResult PumpAudio(bSound&) override
    {
        const auto result=audio_.Pump();
        if(result!=WORLD_AUDIO_OK)RecordSessionEvent("audio.failure",audio_.Error().c_str());
        return result;
    }
    WorldAudioResult CloseAudio(bSound&) override {return audio_.Close();}
    int Width() const override {return width;}
    int Height() const override {return height;}
    std::size_t AvailableMemory() const override
    {
        u64 total{},used{};
        if(R_FAILED(svcGetInfo(&total,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0)) ||
           R_FAILED(svcGetInfo(&used,InfoType_UsedMemorySize,CUR_PROCESS_HANDLE,0)) || used>total)
            return 0;
        return static_cast<std::size_t>(total-used);
    }
    void ApplyDisplaySettings(int32_t,int32_t,bool,bool vsync) override
    {
        // Camera targets already use the fixed console render extent. A PC
        // profile's 1280x720 preference must not enlarge a 1080p viewport again.
        graphics_.SetLogicalSize(width,height);
        graphics_.ConfigurePresentation(vsync);
    }

private:
    hp2::platform::ControllerState ReadPhysical(std::size_t index) const {
        hp2::platform::ControllerState state;
        const auto& pad=pads_[index];state.connected=padIsConnected(&pad);
        auto held=padGetButtons(&pad)&~blockedButtons_[index];
        auto left=padGetStickPos(&pad,0);auto right=padGetStickPos(&pad,1);
        const auto style=padGetStyleSet(&pad);
        // Single Joy-Con controllers are held horizontally. Convert their
        // physical face cluster/stick to the same layout as a full gamepad.
        if(style==HidNpadStyleTag_NpadJoyLeft || style==HidNpadStyleTag_NpadJoyRight) {
            const auto source=held;
            held&=~(HidNpadButton_A|HidNpadButton_B|HidNpadButton_X|HidNpadButton_Y|
                HidNpadButton_Up|HidNpadButton_Down|HidNpadButton_Left|HidNpadButton_Right);
            const auto button=[&](HidNpadButton from,HidNpadButton to) {if(source&from)held|=to;};
            if(style==HidNpadStyleTag_NpadJoyLeft) {
                button(HidNpadButton_Down,HidNpadButton_A);button(HidNpadButton_Left,HidNpadButton_B);
                button(HidNpadButton_Right,HidNpadButton_X);button(HidNpadButton_Up,HidNpadButton_Y);
                button(HidNpadButton_Minus,HidNpadButton_Plus);
                button(HidNpadButton_LeftSL,HidNpadButton_L);button(HidNpadButton_LeftSR,HidNpadButton_R);
                left={-left.y,left.x};
            } else {
                button(HidNpadButton_X,HidNpadButton_A);button(HidNpadButton_A,HidNpadButton_B);
                button(HidNpadButton_Y,HidNpadButton_X);button(HidNpadButton_B,HidNpadButton_Y);
                button(HidNpadButton_RightSL,HidNpadButton_L);button(HidNpadButton_RightSR,HidNpadButton_R);
                left={right.y,-right.x};
            }
            right={};
        }
        for(const auto& [physical,logical]:buttonMap)
            hp2::platform::SetButton(state,hp2::nx::ContextButton(logical,hp2::nx::drivingControls),(held&physical)!=0);
        state.sticks={axis(right.x),axis(-right.y),axis(left.x),axis(-left.y)};
        return state;
    }
    bool ConnectTwoPlayers() {
        StopRumble();profile_.Checkpoint(true);
        HidLaControllerSupportArg argument;hidLaCreateControllerSupportArg(&argument);
        argument.hdr.player_count_min=2;argument.hdr.player_count_max=2;
        argument.hdr.enable_take_over_connection=1;argument.hdr.enable_left_justify=1;
        argument.hdr.enable_permit_joy_dual=1;argument.hdr.enable_single_mode=0;
        HidLaControllerSupportResultInfo result{};
        const auto status=hidLaShowControllerSupportForSystem(&result,&argument,false);
        ++timingEpoch_;refreshClock_={};presentationLimit_.Reset(FrameTime());
        for(std::size_t i=0;i<pads_.size();++i) {padUpdate(&pads_[i]);blockedButtons_[i]=padGetButtons(&pads_[i]);sources_[i]=ReadPhysical(i);}
        const bool ready=R_SUCCEEDED(status) && result.player_count==2 && padIsConnected(&pads_[0]) && padIsConnected(&pads_[1]);
        log_<<"input.controller_support result="<<status<<" players="<<int(result.player_count)<<" accepted="<<ready<<std::endl;
        return ready;
    }
    void FlushSlowFrames() {if(!slowFrameJournal_.empty()){log_<<slowFrameJournal_;slowFrameJournal_.clear();}}
    void Presented(hp2::NativeFrameTiming::Clock::time_point started) {
        // Include CPU capture in the capped interval. Starting the timer
        // after capture would add that cost to every 33 ms interval.
        presentationLimit_.Presented(captureStarted_);
        captureStarted_=started;

    }
    struct Vibration {
        std::array<HidVibrationDeviceHandle,2> handles{};
        HidNpadIdType id{};
        u32 style{};
        uint8_t small{},large{};
    };

    void StopRumble()
    {
        for(auto& vibration:vibration_) {
            if(vibration.style && (vibration.small || vibration.large)) {
                constexpr HidVibrationValue silent[]{ {0.0f,160.0f,0.0f,320.0f},
                                                     {0.0f,160.0f,0.0f,320.0f} };
                (void)hidSendVibrationValues(vibration.handles.data(),silent,2);
            }
            vibration={};
        }
    }

    std::ostream& log_;
    hp2::nx::PgoSession& profile_;
    hp2::EcstasyVulkanDevice graphics_;
    hp2::nx::AudioDevice audio_;
    std::array<PadState,9> pads_{};
    std::array<hp2::platform::ControllerState,9> sources_{};
    std::array<u64,9> blockedButtons_{};
    std::size_t activePhysical_{};
    bool routingSplit_{};
    std::array<Vibration,JOYSTICK_PORT_COUNT> vibration_{};
    const GameTime* clock_{};
    hp2::NativeRefreshClock refreshClock_;
    hp2::NativePresentationClock presentationLimit_;
    hp2::NativeFrameTiming::Clock::time_point captureStarted_{};
    std::string slowFrameJournal_;
    std::uint64_t timingEpoch_{};
    std::uint64_t captureFrames_{},captureCommands_{},captureInstances_{},captureProjections_{};
    std::uint64_t sceneryPacketHits_{},sceneryPacketMisses_{},sceneryPacketFallbacks_{};
};
}

int main(int argc,char** argv)
{
    const std::filesystem::path base=installDirectory;
    const auto saves=base/"Saves";
#ifdef HP2_DISABLE_DIAGNOSTICS
    std::ostream log(nullptr);
#else
    hp2::nx::AsyncLog log;
#endif
    try {
        std::filesystem::create_directories(saves);
#ifndef HP2_DISABLE_DIAGNOSTICS
        log.open(saves/"NFSHP2_log.txt",std::ios::trunc);
        log<<"log.backend asynchronous queue_bytes=262144"<<std::endl;
#endif
        auto configDirectory=base;
#ifndef __SWITCH2__
        if(argc>0 && argv[0] && std::filesystem::path(argv[0]).extension()==".nro" &&
            std::filesystem::path(argv[0]).has_parent_path())configDirectory=std::filesystem::path(argv[0]).parent_path();
#endif
        hp2::nx::LoadUserSettings(configDirectory,log);
        applicationShowFrameRate=hp2::nx::userSettings.showFps;
        hp2::nx::PgoSession profile(saves,log);
        log<<"build.optimization "<<hp2::nx::PgoSession::BuildMode()<<std::endl;
        log<<"render.capture_layout command_bytes="<<sizeof(hp2::EcstasyDrawCommand)
           <<" instance_bytes="<<sizeof(hp2::EcstasyDrawInstance)
           <<" projection_bytes="<<sizeof(hp2::EcstasyDrawProjection)<<std::endl;
        log<<"Opening PAL game data"<<std::endl;
        hp2::GameResources files(argc>1?std::filesystem::path(argv[1]):base);
        log<<"Creating Switch Vulkan host "<<width<<'x'<<height<<", CPU workers="
           <<hp2::NativeSceneEffectiveHardwareConcurrency()<<std::endl;
        hp2::configureNativeRenderSettings({});
        hp2::configureNativeFrameLimitQuality(hp2::nx::userSettings.frameLimit);
        log<<"render.lod_settings scenery_multiplier="<<hp2::nativeSceneQuality().sceneryLod
           <<" vehicle_multiplier="<<hp2::nativeSceneQuality().vehicleLod
           <<" near_detail_enter="<<hp2::nativeSceneQuality().nearDetailEnter
           <<" near_detail_exit="<<hp2::nativeSceneQuality().nearDetailExit
           <<" draw_distance="<<hp2::nativeRenderSettings().drawDistanceScale
           <<" mirror_distance="<<hp2::nativeRenderSettings().mirrorDistanceScale
           <<" gpu_scenery_clip_max_strips="<<hp2::nativeGpuSceneryClipMaximumStrips<<std::endl;
        SwitchApplicationHost host(log,profile);
        hp2::ApplicationRuntime application(files,host,saves);
        host.BindClock(application.Clock());
        log<<"Initializing game"<<std::endl;
        application.Initialize();
        profile.Checkpoint(true);
        while(application.Tick())profile.Checkpoint();
        profile.Checkpoint(true);
        application.Shutdown();
        return 0;
    } catch(const std::exception& error) {
        log<<error.what()<<std::endl;
        std::fprintf(stderr,"NFSHP2: %s\n",error.what());
        return 1;
    }
}
