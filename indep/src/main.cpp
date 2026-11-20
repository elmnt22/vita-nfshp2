#include <exception>
#include "Flags.hpp"
#ifdef __SWITCH__
#include "src/nx/control_layout.hpp"
#include "src/nx/controller_session.hpp"
#include "src/nx/config.hpp"
#endif
#include "bware/inc/bWare.hpp"
#include "DemoDisc.hpp"
#include "bware/inc/bMath.hpp"
#include "bware/inc/bWare.hpp"
#include "bware/inc/bChunk.hpp"
#include "bware/inc/bFile.hpp"
#include "indep/inc/AcidFX.hpp"
#include "indep/inc/CarInfo.hpp"
#include "indep/inc/CameraCoordinator.hpp"
#include "indep/inc/Ecstasy.hpp"
#include "indep/inc/eEnvMap.hpp"
#include "indep/inc/FlailerDB.hpp"
#include "indep/inc/FMenus.hpp"
#include "indep/inc/GameFlow.hpp"
#include "indep/inc/World.hpp"
#include "bware/inc/bWare.hpp"
#include <fstream>
#include <stdexcept>
#include <string>
#include "indep/inc/Fred.hpp"
#include "FMenus.hpp"
#include "indep/inc/Language.hpp"
#include "indep/inc/DemoModeReplay.hpp"
#include "indep/inc/CarLoader.hpp"
#include "indep/inc/RealDriver.hpp"
#include "indep/inc/AICarController.hpp"
#include "indep/inc/TrafficDriver.hpp"
#include "indep/inc/AIPlan.hpp"
#include "indep/inc/Player.hpp"
#include "indep/inc/Collision.hpp"
#include "indep/inc/Smokeable.hpp"
#include "indep/inc/Sound3dSample.hpp"
#include "indep/inc/SpeechNoise.hpp"
#include "indep/inc/WeatherMan.hpp"
#include "indep/inc/EasterEggs.hpp"

#include "indep/inc/eMath.hpp"
#include "indep/src/MoviePlayer/MV3Codec.h"
#include "indep/src/speech/mgr.hpp"
#include "bsound/include/bSound.h"
#include "bsound/include/bJukebox.h"
#include "indep/inc/CameraMover.hpp"
#include "indep/inc/CarDriver.hpp"
#include "indep/inc/AnimatedDriver.hpp"
#include <array>
#include <cstring>
#include <cstdio>
#include <thread>

// Native terminal lifetime API in the proven weatherman.cpp definition path.
// WeatherMan.hpp retains its inherited scope; no new header identity is claimed.
extern "C" WeatherManResult weatherManUnbindClock(const GameTime* clock);
namespace hp2 { void InitDisplaySystem(const GameResources& files,GameTime& clock); }

ApplicationBootState applicationBootState={-1};
ApplicationFrameTiming applicationFrameTiming{};
float applicationPreviousCpuFrameRate=0.0f;
float applicationPreviousGpuFrameRate=0.0f;
bool applicationGpuTimingAvailable=false;

// PAL12EE30..12EE94; same process vector RNG. vectorRandomSeed performs
// its actual initial bound1 draw; assigning a seed directly would differ.
void applicationSeedRandomNumber(ReplayIntegerLog& log)
{
    if (!log.clock || (log.replaying && !log.stream))
        throw std::invalid_argument("Application seed has no canonical clock/Joylog binding");
    uint32_t seed;
    if (log.replaying)seed=replayIntegerLogRead(&log,32,0);
    else {
        seed=hp2NativeTickerSource()+UINT32_C(0x12345678);
        replayIntegerLogWrite(&log,seed,32,0);
    }
    (void)vectorRandomSeed(seed,&vectorDefaultRandomState);
}

// PAL12EDA0..12EE2C. The PC file adapter accepts readable config.ini and
// read-only legacy config.bin. Its one selected word and Joylog order are
// unchanged; malformed input fails before the original cell is published.
void applicationLoadConfig(const std::filesystem::path& filename,ReplayIntegerLog& log)
{
    if (!log.clock || (log.replaying && !log.stream))
        throw std::invalid_argument("Application config has no canonical clock/Joylog binding");
    if (log.replaying) {
        applicationBootState.trackSelectedInConfigFile=static_cast<int32_t>(replayIntegerLogRead(&log,32,0));
        return;
    }
    const auto selected=hp2::loadNativeTrackConfiguration(filename);
    applicationBootState.trackSelectedInConfigFile=selected;
    replayIntegerLogWrite(&log,static_cast<uint32_t>(selected),32,0);
}

// Complete PAL12F190..12F374. The total checksum word is consumed without a
// comparison in the release. Every sixteenth call additionally compares the
// five components; bBreak is adapted to a descriptive native failure.
void applicationVerifyJoylogChecksum(ApplicationChecksumState& state,ReplayIntegerLog& log,
    WorldSimulation* simulation,const hp2::frontend::FlailerScreenList* screens,
    const hp2::frontend::ApplicationFrontendFlow& flow)
{
    hp2::Require(log.clock,"Application checksum has no canonical game clock");
    uint16_t world=0,screen=0;
    if (simulation) {
        hp2::Require(simulation->world && simulation->replay && simulation->clock==log.clock,
            "Application checksum world has no canonical clock/replay bindings");
        world=worldCalculateDebugChecksum(simulation->world,static_cast<uint32_t>(log.clock->worldTimer),
            simulation->replay->checksumContribution);
    }
    if (screens && screens->active) {
        hp2::Require(screens->active->screen,"Active source screen node has no first screen");
        screen=static_cast<uint16_t>(screens->active->screen->screenId);
    }
    const uint16_t random=static_cast<uint16_t>(vectorDefaultRandomState);
    const uint16_t loop=static_cast<uint16_t>(log.clock->realLoopCounter);
    const uint16_t flowState=static_cast<uint16_t>(flow.currentState);
    const uint16_t total=static_cast<uint16_t>(world+loop+random+screen+flowState);
    if (log.replaying)(void)replayIntegerLogRead(&log,16,3);
    else replayIntegerLogWrite(&log,total,16,3);
    if ((state.calls++&15u)!=0)return;
    const uint16_t components[]={world,screen,random,flowState,loop};
    // Source re-reads the live replaying flag here: consuming the total may
    // have closed the stream at its end. Do not retain a stale mode snapshot.
    if (log.replaying) {
        unsigned differences=0;
        for (const auto value:components)
            differences+=value!=replayIntegerLogRead(&log,16,3);
        if (differences)throw std::runtime_error("Original application Joylog checksum mismatch");
    } else {
        for (const auto value:components)replayIntegerLogWrite(&log,value,16,3);
    }
}

namespace hp2 {

struct ApplicationResourceRuntime::Storage {
    FileIoRuntime io;
    QueuedFileService queued;
    struct AnimationRoot {
        CarReplayAnimationRegistry registry{};
        AnimationRoot() {carReplayAnimationRegistryInitialize(&registry);}
    } animations;
    EditorMarkerResources models{};
    std::unique_ptr<CarViewCameraResources,decltype(&carViewCameraResourcesDispose)> cameras;
    AcidFXRuntime fx;
    ChopperEffectData effectsRoot;
    frontend::FlailerResources flailer;
    ResourceRegistry registry;
    ResourceGlobalBindings global;
    ResourceLoader loader;
    const DemoDiscManager* demo=nullptr;

    Storage(const GameResources& files,const CarCinematicServices& views,ReplayIntegerLog& log,CarInfoAvailability availability)
        :queued(files,io,log),cameras(carViewCameraResourcesCreate(),carViewCameraResourcesDispose),fx(views),
         effectsRoot(std::vector<std::uint8_t>{}),flailer(models),
         registry(models,animations.registry,effectsRoot,flailer),global{fx,availability},loader(files,registry,queued)
    {
        if (!cameras)throw std::bad_alloc();
        Require(availability==CarInfoAvailability::FullGame || availability==CarInfoAvailability::DemoDisc,
            "Application resources require an actual content-policy owner");
    }
};

ApplicationResourceRuntime::ApplicationResourceRuntime(const GameResources& files,
    const CarCinematicServices& cameras,ReplayIntegerLog& log,CarInfoAvailability availability)
    :storage_(std::make_unique<Storage>(files,cameras,log,availability)) {}

ApplicationResourceRuntime::ApplicationResourceRuntime(const GameResources& files,
    const CarCinematicServices& cameras,ReplayIntegerLog& log,const DemoDiscManager& demo)
    :ApplicationResourceRuntime(files,cameras,log,demo.Availability())
{
    storage_->demo=&demo;
}

ApplicationResourceRuntime::~ApplicationResourceRuntime()=default;
ResourceRegistry& ApplicationResourceRuntime::Registry() noexcept {return storage_->registry;}
ResourceLoader& ApplicationResourceRuntime::Loader() noexcept {return storage_->loader;}
QueuedFileService& ApplicationResourceRuntime::QueuedFiles() noexcept {return storage_->queued;}
FileIoRuntime& ApplicationResourceRuntime::FileIo() noexcept {return storage_->io;}
AcidFXRuntime& ApplicationResourceRuntime::FX() noexcept {return storage_->fx;}
CarViewCameraResources& ApplicationResourceRuntime::CameraResources() noexcept {return *storage_->cameras;}
ResourceFileBindings ApplicationResourceRuntime::GlobalFileBindings() const
{
    if (storage_->demo)storage_->global.availability=storage_->demo->Availability();
    ResourceFileBindings bindings;
    bindings.kind=ResourceFileKind::Global;bindings.global=&storage_->global;
    return bindings;
}

} // namespace hp2

namespace hp2 {
namespace {
// The released process owns one input service. This C-ABI trampoline keeps
// a lifetime borrow only; no second controller packet or game state exists.
NativeApplicationHost* applicationHost{};
int32_t ReadApplicationInput(int32_t port,JoystickControls* destination)
{
    Require(applicationHost,"Physical input outside application lifetime");
    Require(destination,"Physical input has no native destination");
    return applicationHost->ReadInput(port,*destination);
}
void RequireAudio(WorldAudioResult result,const char* operation,const bSound* sound=nullptr)
{
    if(result==WORLD_AUDIO_OK)return;
    auto message=std::string(operation)+" failed, audio result="+std::to_string(static_cast<int>(result));
    if(sound) {
        const auto cause=sound->FailureDescription();
        if(!cause.empty())message+="; "+cause;
    }
    throw std::runtime_error(message);
}
}

struct ApplicationRuntime::Storage {
    platform::NativeDurationStatistics sourceUpdateTiming,presentationTiming;
    double deferredPresentationCpuMilliseconds{},deferredCaptureMilliseconds{},deferredSealMilliseconds{};
    std::uint64_t deferredLogicalUpdates{},deferredSceneCaptures{};
    NativeFrameTiming frameTiming;
    NativeSimulationClock simulationClock;
    bool pendingClockAdvance{},pendingLegacyRealClock{},sourceJoylogTiming{};
    std::uint32_t scheduledStepQuanta{60};
    float replayStepSeconds{1.0f/60.0f};
    std::uint64_t presentationTimingEpoch{};
    bool frameTimingStarted{};
    std::chrono::steady_clock::time_point renderTimingWindowStart{std::chrono::steady_clock::now()};
    std::uint64_t renderTimedFrames{},gpuTimedFrames{},lastCompletedGpuFrame{};
    double renderUpdateMs{},renderCaptureMs{},renderSealMs{},renderGatherMs{},renderFenceMs{},renderSubmitMs{},renderPresentMs{},renderGpuMs{};
    std::uint64_t renderLogicalUpdates{},renderSceneCaptures{},renderPreparedFrames{},renderCachedSceneryStrips{},renderSceneryPipelineHits{};
    std::uint64_t renderCompiledPipelines{},renderPendingPipelineDraws{},renderGpuWorldVertices{};
    double renderCarCaptureMs{};
    std::array<double,2> renderShadowCaptureMs{};
    EcstasyVulkanRenderProfile renderProfile{};
    EcstasySceneCaptureProfile captureProfile{};
    EcstasyVulkanGpuProfile renderGpuProfile{};
    std::uint64_t renderGpuProfileSamples{};
    WorldStepProfile worldProfile{};
    platform::NativeDurationStatistics worldTiming,audioTiming,inputTiming,resourceTiming,sceneryTiming;
    struct SlowFrameCounters {
        platform::NativeDurationStatistics world,audio,input,resource,scenery,poll,wait,diagnostics,tick;
        EcstasySceneCaptureProfile capture;
        double collisionMs{},thinkMs{},effectsMs{};
    } slowFrame;
    NativeFrameTiming::Clock::time_point slowLastPresent{};
    float slowLastDrawDistance{nativeRenderSettings().drawDistanceScale};
    std::uint64_t lastSlowGpuFrame{};
    NativeFrameTiming::Clock::time_point lastAcceptedPresent{};
    std::uint64_t presentsOver33ms{},presentsOver50ms{};
    double longestPresentIntervalMs{};
    void ServiceFrameResources()
    {
        platform::ScopedNativeDuration measure(resourceTiming,&slowFrame.resource);
        texturePackRegistry.ServiceReads();resources->Loader().ServiceResourceLoading();
        replayAvailableBytes=host.AvailableMemory();
    }
    GameResources& files;NativeApplicationHost& host;
    std::filesystem::path profileDirectory,legacyProfileDirectory;
    GameTime clock{};ReplayIntegerLog joylog{};ApplicationChecksumState checksum{};
    DemoDiscManager& demo{TheDemoDiscManager};
    CarCinematicServices cameras{};
    std::array<CarCinematicView,4> views{};
    std::array<EcstasyRenderTarget,15> targets{};
    std::unique_ptr<EnvironmentMap> environmentMap;
    std::unique_ptr<ApplicationResourceRuntime> resources;
    ResourceFile* global{};
    std::unique_ptr<frontend::TrackInfoResources> tracks;
    frontend::LanguageManager language;
    frontend::FrontendPlayerManager profilesOwner;
    // Immutable launch preference wins over an older asynchronously loaded
    // profile's PC display extension. FRED remains the live display owner.
    std::optional<frontend::NativeDisplaySettings> launchDisplay;
    std::array<frontend::FrontendPlayerInfo,2> profiles{};
    std::unique_ptr<frontend::HighScoreTable> highScores;
    RaceParameters parameters{};RaceRuntimeSettings runtime{};
    std::array<RaceDriverInfo,RACE_DRIVER_LIMIT> drivers{};
    frontend::RaceEvent event;
    frontend::DemoModeReplay demoReplay;
    frontend::ApplicationFrontendFlow flow;
    frontend::FrontendRaceSetupBindings setup{};
    std::unique_ptr<TexturePack> dynamicTextures,hudTextures,carTextures;
    CarTextureNamingPolicy carNaming;PositionMarkerRegistry positionMarkers{};
    GameInputRouter input{};std::array<GameInputRegistration,256> registrations{};
    JoystickSystem samples{};RealDriverTuning driverTuning{};
    WorldAudioRequestQueue audioQueue{};std::array<WorldAudioRequest,4096> audioRequests{};
    bSound sound;CarSoundBindings soundBindings{};
    frontend::SoundVolumeSettings volumes;
    frontend::FlailerSoundClock soundClock{0,4000.0f};frontend::FlailerSoundState uiSound;
    WorldAudioMessages speechMessages{};WorldStreamAudio speechStream{};
    Sound3dSampleRegistry positionalSamples;WorldImpactAudio impactAudio{};WorldAudioEffects impactEffects{};
    frontend::FlailerUiState ui;frontend::FlailerRenderManager renderer;
    frontend::FlailerScreenList screens;
    std::unique_ptr<frontend::FlailerHelpMenu> help;
    std::unique_ptr<frontend::FlailerGlobalBackgroundAnimation> background;
    std::array<std::unique_ptr<frontend::FlailerPopup>,2> popups;
    std::unique_ptr<frontend::FlailerLoadingScreenAnimation> loadingAnimation;
    std::vector<std::unique_ptr<frontend::FlailerScreenStorage>> screenStorage;
    FlailerFrontendInput frontendInput{};frontend::FrontendDemoMode demoMode;
    frontend::MainMenuScreenServices menus{};frontend::FrontendStartupServices startup{};
    frontend::SelectCarScreenServices carSelection{};frontend::SelectTrackScreenServices trackSelection{};frontend::FrontendFrame frontendFrame;
    std::unique_ptr<movie::MoviePlayer> movie;
    std::unique_ptr<frontend::GameFlowFrontendResources> frontendPhase;
    std::unique_ptr<frontend::GameFlowTrackResources> trackPhase;
    std::unique_ptr<frontend::MemcardScreens> memcards;
    frontend::GameFlowBindings flowBindings{};
    std::unique_ptr<EcstasyFrame> frame{std::make_unique<EcstasyFrame>()};
    int32_t skipStartSequence{},alternateSkin{};
    // PAL1BE910 requests the same bStartGlare consumed by StuffSunGlare.
    // The application's Sun owner retains its original textures and clock.
    int32_t impactPresentationMode{1};
    size_t replayAvailableBytes{};
    bool initialized{},inputBound{},audioBound{},audioOpened{},speechBound{},menusBound{},mainMenuBound{},languageInitialized{};
    bool speechNoiseBound{},soundScapeBound{},jukeboxBound{};
    bool shutdownStarted{},shutdownComplete{};
    // Extra presentations may borrow the backend's immutable prepared scene
    // only while every source input remains unchanged. Logical state changes,
    // resource publication and independently updated overlays invalidate it.
    bool preparedSceneValid{},preparedShowFrameRate{},preparedDebugCaptionPresent{};
    GameSession* preparedSession{};
    std::uint64_t preparedResourceRevision{};
    std::array<char,48> preparedFrameRateCaption{};
    std::string preparedDebugCaption;
    std::exception_ptr shutdownFailure;
    void RecordCleanupFailure(const char* operation,std::exception_ptr failure) noexcept
    {
        if(!failure)return;
        if(!shutdownFailure)shutdownFailure=failure;
        char detail[768]{};
        try {std::rethrow_exception(failure);}
        catch(const std::exception& error) {
            std::snprintf(detail,sizeof(detail),"%s: %s",operation,error.what());
        } catch(...) {
            std::snprintf(detail,sizeof(detail),"%s: non-standard exception",operation);
        }
        host.RecordSessionEvent("shutdown.failure",detail);
    }
    template<class F> void Retire(const char* operation,F&& action) noexcept
    {
        try {action();}catch(...) {RecordCleanupFailure(operation,std::current_exception());}
    }
    template<class F> void RetireAudio(const char* operation,F&& action) noexcept
    {
        Retire(operation,[&] {
            auto result=action();
            for(unsigned retry=0;result==WORLD_AUDIO_CAPACITY && retry<4;++retry) {
                RequireAudio(sound.CancelDeviceRequestsAfterClose(),"Terminal audio cancellation",&sound);
                result=action();
            }
            RequireAudio(result,operation,&sound);
        });
    }
    template<class F> void RetireSpeech(const char* operation,F&& action) noexcept
    {
        Retire(operation,[&] {
            auto result=action();
            for(unsigned retry=0;(result==WORLD_STREAM_AUDIO_BUSY || result==WORLD_STREAM_AUDIO_CAPACITY) && retry<4;++retry) {
                RequireAudio(sound.CancelDeviceRequestsAfterClose(),"Terminal speech cancellation",&sound);
                result=action();
            }
            Require(result==WORLD_STREAM_AUDIO_OK,operation);
        });
    }
    std::array<int32_t,7> journalState{{INT32_MIN,INT32_MIN,INT32_MIN,INT32_MIN,INT32_MIN,INT32_MIN,INT32_MIN}};
    uint32_t journalSummaryTicks{};
    void RecordLifecycle(WorldSimulation* simulation,GameSession* session)
    {
        const auto* first=parameters.drivers && parameters.driverCount>0?parameters.drivers:nullptr;
        const auto* active=screens.active?screens.active->screen:nullptr;
        const std::array<int32_t,7> state{{active?active->screenId:-1,session?session->state:-1,
            parameters.selectedScenario,parameters.mode,parameters.driverCount,
            first?first->appearance.modelCode:-1,first?first->appearance.skinNumber:-1}};
        char detail[640]{};
        if(state!=journalState) {
            journalState=state;
            std::snprintf(detail,sizeof(detail),"screen=%08x race_state=%d scenario=%d mode=%d drivers=%d player_model=%d skin=%d demo=%d",
                static_cast<unsigned>(state[0]),state[1],state[2],state[3],state[4],state[5],state[6],demoReplay.active);
            host.RecordSessionEvent("application.state",detail);
        }
        const auto ticks=static_cast<uint32_t>(clock.realTimer);
        if(!simulation || !simulation->world || ticks-journalSummaryTicks<20000u)return;
        journalSummaryTicks=ticks;
        const auto& world=*simulation->world;const auto* car=world.totalCars>0?world.carTable[0]:nullptr;
        const auto* movement=car?static_cast<const CarMovement*>(car->movement):nullptr;
        const auto* vehicle=movement?movement->bindings.state:nullptr;
        const auto* replay=simulation->replay?simulation->replay->manager:nullptr;
        std::snprintf(detail,sizeof(detail),"world_loop=%d timer=%d scenario=%d cars=%d player_model=%d mover=%d "
            "position=(%.3f,%.3f,%.3f) speed=%.3f restoring=%d recording=%d exhausted=%d desynchronized=%d "
            "source_sync_policy=%d source_samples=%u source_mismatches=%u first_source_mismatch_loop=%d "
            "application_fps=%.2f frame_ms=%.3f cpu_ms=%.3f gpu_ms=%.3f gpu_available=%d "
            "catch_up=%d disable_catch_up_pref=%d",
            clock.worldLoopCounter,clock.worldTimer,parameters.selectedScenario,world.totalCars,state[5],movement?movement->mode:-1,
            static_cast<double>(car?car->position[0]:0.0f),static_cast<double>(car?car->position[1]:0.0f),
            static_cast<double>(car?car->position[2]:0.0f),static_cast<double>(vehicle?vehicle->speed:0.0f),
            replay?replay->restoring:0,replay?replay->recording:0,replay?replay->exhausted:0,replay?replay->desynchronized:0,
            replay?static_cast<int>(replay->synchronizationPolicy):0,
            static_cast<unsigned>(replay?replay->sourceReferenceSamples:0),
            static_cast<unsigned>(replay?replay->sourceReferenceMismatchCount:0),
            replay?replay->firstSourceReferenceMismatchLoop:-1,
            applicationFrameTiming.framesPerSecond,applicationFrameTiming.frameMilliseconds,
            applicationFrameTiming.cpuMilliseconds,applicationFrameTiming.gpuMilliseconds,
            applicationFrameTiming.gpuTimeAvailable?1:0,parameters.relativePaceEnabled,
            applicationNativeGameplay.disableCatchUp?1:0);
        if(demoReplay.active)demoReplay.RecordSessionEvent("demo.status",nullptr,detail);
        else host.RecordSessionEvent("gameplay.status",detail);
        // A bounded observation of the actual owners makes a live AI failure
        // distinguishable from authored input playback and device starvation.
        // It never refreshes a road, services a voice or changes a controller.
        const auto* speech=worldSpeechGetInstance();
        const auto* voice=speechStream.voice.instance;
        std::snprintf(detail,sizeof(detail),"world_loop=%d enabled=%d speech_enabled=%d queued=%u retained=%u "
            "speech_state=%d events=%u voice_playing=%d voice_paused=%d voice_muted=%d voice_volume=%.5f category6=%.5f",
            clock.worldLoopCounter,carSoundGlobals.policy.audioSystemEnabled,soundDriverState.speechEnabled,
            static_cast<unsigned>(audioQueue.count),static_cast<unsigned>(audioQueue.retainedCount),speechStream.state,
            static_cast<unsigned>(speech?speech->queue.count:0),voice?voice->playing:0,voice?voice->paused:0,
            voice?voice->muted:0,static_cast<double>(voice?voice->volume:0.0f),
            static_cast<double>(sound.GetCategoryVolume(6)));
        host.RecordSessionEvent("audio.status",detail);
        if(session && (session->state==RC_STATE_AI_DEMO || demoReplay.active)) {
            const auto* observed=car;
            const auto* players=trackPhase->Players();
            if(players && players->count && players->players[0] && players->players[0]->car)
                observed=players->players[0]->car->bindings.owner;
            const auto* driver=observed?static_cast<const CarDriver*>(observed->controller):nullptr;
            const auto* motion=observed?static_cast<const CarMovement*>(observed->movement):nullptr;
            const auto* ai=driver && driver->kind==CAR_DRIVER_AI?driver->owner.ai:nullptr;
            const auto* road=ai?ai->base.currentRoad:nullptr;
            const auto* action=ai?ai->base.action:nullptr;
            std::snprintf(detail,sizeof(detail),"world_loop=%d path=%s recording=%d driver=%d kind=%d "
                "mover=%d action=%d action_status=%d road_group=%d lane=%d path_points=%d target_speed=%.3f progress=%.6f",
                clock.worldLoopCounter,demoReplay.active?"authored_input":"live_ai",demoReplay.selectedRecording,
                observed?observed->driverNumber:-1,driver?static_cast<int>(driver->kind):-1,motion?motion->mode:-1,
                action?action->kind:-1,action?static_cast<int>(action->status):-1,
                road && road->group?road->group->id:-1,road?road->lane:-1,ai?ai->input.command.count:0,
                static_cast<double>(ai?ai->input.targetSpeed:0.0f),
                static_cast<double>(motion?motion->routeLocation.progress:0.0f));
            host.RecordSessionEvent("demo.driver",detail);
        }
    }
    void AudioResult(WorldAudioResult result,const char* operation) const
    {
        if(result!=WORLD_AUDIO_OK) {
            char detail[256]{};std::snprintf(detail,sizeof(detail),"operation=%s result=%d",operation,static_cast<int>(result));
            host.RecordSessionEvent("audio.failure",detail);
        }
        RequireAudio(result,operation,&sound);
    }
    void RefreshNativeDisplayGeometry()
    {
        const auto width=host.Width(),height=host.Height();
        Require(width>0 && height>0,"Native render extent is unavailable");
        cameras.projection.nativeSourceVideoMode=gameTimeGetVideoMode(&clock);
        if(targets[0].width==width && targets[0].height==height && cameras.projection.horizontalResolution==width)return;
        for(std::size_t index=0;index<8;++index) {targets[index].width=width;targets[index].height=height;}
        cameras.projection.horizontalResolution=width;
        // The source view switch sets player/split/rear subrectangles. The
        // six cubemap faces and mapped environment image keep authored sizes.
        ecstasyViewsApplyMode(&cameras);
    }
    Storage(GameResources& source,NativeApplicationHost& physical,const std::filesystem::path& directory,
        const std::filesystem::path& legacyDirectory)
        :files(source),host(physical),profileDirectory(directory),legacyProfileDirectory(legacyDirectory)
    {
        gameTimeInitialize(&clock);gameTimeUseNativeTiming(&clock);joylog.clock=&clock;cameras.clock=&clock;
        parameters.drivers=drivers.data();parameters.runtimeSettings=&runtime;
        profilesOwner.players={{&profiles[0],&profiles[1]}};
    }
    void DrainFiles()
    {
        auto& loader=resources->Loader();
        for(;;) {
            bool pending=!loader.IsResourceLoadingComplete() || loader.HasPendingFileReads();
            for(const auto* pack:texturePackRegistry.packs)pending=pending || pack->HasPendingReads();
            if(!pending)break;
            // The real queued reads remain owned and serviced here. Windows
            // messages must also be dispatched during this startup wait so
            // moving/closing the already shown HWND is not deferred until
            // every resource and texture transfer finishes.
            if(!host.PollEvents())throw std::runtime_error("Application was closed during resource loading");
            texturePackRegistry.ServiceReads();loader.ServiceResourceLoading();
            if(audioOpened)AudioResult(host.PumpAudio(sound),"Physical audio during resource load");
            std::this_thread::yield();
        }
        // No pending read remains; preserve the original retained-failure
        // checks without entering another wait that starves the message pump.
        for(auto* pack:texturePackRegistry.packs)pack->WaitForReads();
    }
    void RegisterScreen(frontend::FlailerScreenConfig& configuration)
    {
        for(const auto& stored:screenStorage)
            Require(std::strcmp(stored->Node().configuration->name,configuration.name)!=0,"Duplicate source screen registration");
        auto owner=std::make_unique<frontend::FlailerScreenStorage>(ui,configuration);
        auto& node=owner->Node();node.previous=screens.root.previous;node.next=&screens.root;
        screens.root.previous->next=&node;screens.root.previous=&node;screenStorage.push_back(std::move(owner));
    }
    void BindMenus()
    {
        using namespace frontend;
        screens.ui=&ui;screens.root.previous=screens.root.next=&screens.root;
        help=std::make_unique<FlailerHelpMenu>(ui);screens.help=help.get();
        background=std::make_unique<FlailerGlobalBackgroundAnimation>(ui);screens.background=background.get();
        for(size_t i=0;i<popups.size();++i) {
            popups[i]=std::make_unique<FlailerPopup>(ui);screens.popups[i]=popups[i].get();
        }
        frontendInput={&input,&screens,{}};screens.input=&frontendInput;screens.soundBindings=&soundBindings;
        demoMode.screens=&screens;demoMode.frontend=&profilesOwner;demoMode.raceSetup=&setup;
        demoMode.demoDisc=&demo;demoMode.files=&files;
        menus={&ui,&screens,&profilesOwner,&setup,&soundBindings,&demoMode};
        startup={&menus,&joylog,dynamicTextures.get()};
        menusBound=true;
        BindFrontendStartup(startup);
        BindCreditsScreens(startup);
        const auto nativeDirectory=profileDirectory.u8string();
        const auto legacyDirectory=legacyProfileDirectory.u8string();
        memcards=std::make_unique<MemcardScreens>(profilesOwner,nativeDirectory.empty()?nullptr:nativeDirectory.c_str(),
            legacyDirectory.empty()?nullptr:legacyDirectory.c_str());
        BindMemcardScreens(*memcards,menus);
        trackSelection.ui=&ui;trackSelection.screens=&screens;trackSelection.frontend=&profilesOwner;
        trackSelection.tracks=tracks.get();trackSelection.resources=&files;trackSelection.raceSetup=&setup;
        trackSelection.cameras=&cameras;trackSelection.frame=&frontendFrame;BindTrackSelectScreen(trackSelection);
        carSelection={&menus,&cameras,&resources->Registry(),&carNaming,&positionMarkers,nullptr,&frontendFrame,&resources->FX()};
        resources->FX().BindGameFlow(flow.currentState);
        BindSelectCarScreen(carSelection);
        BindCampaignScreen(carSelection);
        FlailerScreenConfig* configurations[]={&fNewMainMenuScreen,&fCopsOnModeSelectScreen,
            &fCopsOffModeSelecttScreen,&fOptions,&fNameEntry,&fTitleScreen,&fMumboJumboScreen,&TrackSelectScreenConfiguration(),
            &fPreRaceBlurbScreen,&fPreRace2PBeACopScreen,&fPreRaceStatsScreen,&fPreRaceLetterboxScreen,
            &fPauseMenuScreen,&fPauseDisplayScreen,&fPauseOptionsScreen,&fPauseControlsScreen,&fPauseAudioScreen,
            &fPauseRaceObjectiveScreen,&fPauseRaceObjective2PScreen,&fPauseDebugMenu,&CampaignScreenConfiguration()};
        for(auto* configuration:configurations)RegisterScreen(*configuration);
        for(auto& configuration:EndGameScreenConfigurations())RegisterScreen(configuration);
        for(auto& configuration:MemcardScreenConfigurations())RegisterScreen(configuration);
        for(auto& configuration:LanguageScreenConfigurations())RegisterScreen(configuration);
        for(auto& configuration:CreditsScreenConfigurations())RegisterScreen(configuration);
        for(auto& configuration:AudioDisplayScreenConfigurations())RegisterScreen(configuration);
        for(auto& configuration:CustomizeHUDScreenConfigurations())RegisterScreen(configuration);
        for(auto& configuration:DisplayCalibrationScreenConfigurations())RegisterScreen(configuration);
        for(auto& configuration:RaceSetupScreenConfigurations())RegisterScreen(configuration);
        for(auto& configuration:JoystickScreenConfigurations())RegisterScreen(configuration);
        for(auto& configuration:SelectCarScreenConfigurations())RegisterScreen(configuration);
        screens.mainScreen=fNewMainMenuScreen.name;
        loadingAnimation=std::make_unique<FlailerLoadingScreenAnimation>(ui,flow,profilesOwner,demo,0xd0131dd1u);
        screens.loadingAnimation=loadingAnimation.get();menusBound=true;
    }
    WorldSimulation* Simulation() noexcept
    {
        auto* simulation=trackPhase?trackPhase->Simulation():nullptr;
        return simulation && simulation->world?simulation:nullptr;
    }
    GameSession* Session() noexcept {return trackPhase?trackPhase->Session():nullptr;}
    bool PreparedSceneResourcesStable() noexcept
    {
        auto* session=Session();
        if(!flow.inGameFlag || !session || session->state!=3 || !resources)return false;
        const auto& loader=resources->Loader();
        if(!loader.IsResourceLoadingComplete() || loader.HasPendingFileReads())return false;
        for(const auto* pack:texturePackRegistry.packs)if(pack->HasPendingReads())return false;
        return true;
    }
    static std::array<char,48> CurrentFrameRateCaption() noexcept
    {
        std::array<char,48> caption{};
        if(applicationShowFrameRate) {
            if(applicationFrameTiming.frameRateAvailable)
                std::snprintf(caption.data(),caption.size(),"FPS: %.1f",applicationFrameTiming.framesPerSecond);
            else std::snprintf(caption.data(),caption.size(),"FPS: --");
        }
        return caption;
    }
    void InvalidatePreparedScene() noexcept
    {
        preparedSceneValid=false;host.InvalidatePreparedFrame();
    }
    void RememberPreparedScene()
    {
        preparedSceneValid=PreparedSceneResourcesStable();
        if(!preparedSceneValid)return;
        preparedSession=Session();preparedResourceRevision=resources->Registry().Models().bindingRevision;
        preparedShowFrameRate=applicationShowFrameRate;preparedFrameRateCaption=CurrentFrameRateCaption();
        const auto* caption=applicationDebugToolsEnabled?joystickNativeDebugCaption():nullptr;
        preparedDebugCaptionPresent=caption!=nullptr;preparedDebugCaption=caption?caption:"";
    }
    bool PreparedSceneUnchanged() noexcept
    {
        if(!preparedSceneValid || !PreparedSceneResourcesStable() || preparedSession!=Session() ||
           preparedResourceRevision!=resources->Registry().Models().bindingRevision ||
           preparedShowFrameRate!=applicationShowFrameRate || preparedFrameRateCaption!=CurrentFrameRateCaption())return false;
        const auto* caption=applicationDebugToolsEnabled?joystickNativeDebugCaption():nullptr;
        return preparedDebugCaptionPresent==(caption!=nullptr) && preparedDebugCaption==(caption?caption:"");
    }
    void ServiceFlow()
    {
        // FRONTB ResourceFile owns its bank; each field here is a borrow.
        setup.events=profilesOwner.boundEventResources;trackSelection.events=profilesOwner.boundEventResources;
        // Screen registration precedes FRONTB loading. Bind the main-menu
        // callbacks only after that real event owner is published, before
        // ServiceGameFlow can activate the loaded frontend. The binding
        // keeps process-lifetime services; subsequent banks refresh the
        // borrowed pointers above when the frontend is loaded again.
        if(setup.events && !mainMenuBound) {
            frontend::BindMainMenuScreen(menus);mainMenuBound=true;
        }
        const auto result=frontend::ServiceGameFlow(flow,flowBindings);
        if(result.status!=frontend::GameFlowServiceStatus::Complete &&
           result.status!=frontend::GameFlowServiceStatus::PendingRenderCompletion)
            throw std::runtime_error("Original GameFlow failed at "+std::to_string(result.sourceFunction)+
                ", state="+std::to_string(result.state));
        setup.events=profilesOwner.boundEventResources;trackSelection.events=profilesOwner.boundEventResources;
    }
#ifdef __SWITCH__
    void UpdateSwitchControls()
    {
        const auto* session=Session();
        nx::UpdateControllerSession(profilesOwner.playerCount,profilesOwner.IsChallengeGame());
        nx::gameplayPresentation=flow.inGameFlag!=0;

        nx::drivingControls=session && (session->state==RC_STATE_PRE_RACE ||
            session->state==RC_STATE_RACE_IN_PROGRESS) &&
            (!screens.active || !screens.active->screen || screens.active->screen->screenId==-1);
    }
#endif
    void GenerateInput(ReplayManager* replay)
    {
        platform::ScopedNativeDuration measure(inputTiming,&slowFrame.input);
#ifdef __SWITCH__
        UpdateSwitchControls();
#endif
        host.BeginInputStep(simulationClock.Deadline());
        const auto result=joystickGenerateEvents(input,samples,joylog,replay,clock,demo);
        if(result!=GAME_INPUT_OK && frontendInput.dispatchFailure) {
            const auto failure=frontendInput.dispatchFailure;frontendInput.dispatchFailure={};
            std::rethrow_exception(failure);
        }
        Require(result==GAME_INPUT_OK,"Source joystick scanner/typed callback dispatch failed");
    }
};

ApplicationRuntime::ApplicationRuntime(GameResources& files,NativeApplicationHost& host,
    const std::filesystem::path& profileDirectory,const std::filesystem::path& legacyProfileDirectory)
    :storage_(std::make_unique<Storage>(files,host,profileDirectory,legacyProfileDirectory))
{
    Require(!applicationHost,"Only one source application process may own input");applicationHost=&host;
}
ApplicationRuntime::~ApplicationRuntime()
{
    (void)ShutdownNoThrow();
    if(applicationHost==&storage_->host)applicationHost=nullptr;
}
EcstasyFrame& ApplicationRuntime::Frame() noexcept {return *storage_->frame;}
bSound& ApplicationRuntime::Audio() noexcept {return storage_->sound;}
const GameTime& ApplicationRuntime::Clock() const noexcept {return storage_->clock;}
const frontend::FrontendPlayerManager& ApplicationRuntime::Frontend() const {return storage_->profilesOwner;}
const frontend::ApplicationFrontendFlow& ApplicationRuntime::Flow() const noexcept {return storage_->flow;}
void ApplicationRuntime::SetDisplayOptions(int32_t width,int32_t height,bool fullscreen,bool vsync)
{
    auto& s=*storage_;Require(s.initialized,"Display settings precede profile initialization");
    Require(width>0 && height>0,"Display extent must be positive");
    s.host.ApplyDisplaySettings(width,height,fullscreen,vsync);
    s.profilesOwner.nativeDisplay={width,height,fullscreen,vsync};
    s.launchDisplay=s.profilesOwner.nativeDisplay;s.RefreshNativeDisplayGeometry();
}

void ApplicationRuntime::Initialize(const std::filesystem::path& configFile)
{
    using namespace frontend;auto& s=*storage_;
    Require(!s.shutdownStarted && !s.initialized && !s.resources,"Application initialization cannot replace live or retired owners");
    Require(s.host.Width()>0 && s.host.Height()>0,"Application has no actual display extent");
    // InitializeEverything12EE98. Native allocation/thread/window setup is
    // supplied by host. All game services retain their actual dependencies.
    s.demo.Init(s.clock,applicationLaunchOptions,DemoDiscConfiguration{});
    s.demoReplay.BindSessionHost(&s.host);
    applicationSeedRandomNumber(s.joylog);
    applicationLoadConfig(configFile.empty()?s.profileDirectory/"config.ini":configFile,s.joylog);
    if(!s.host.SupportsAudio())carSoundGlobals.policy.audioSystemEnabled=0;
    eMathInit();
    InitStomper();
    InitDisplaySystem(s.files,s.clock);
    for(auto& view:s.cameras.screens)ecstasyViewInitialize(&view);
    for(auto& view:s.views)carCinematicViewInitialize(&s.cameras,&view);
    for(size_t i=0;i<s.targets.size();++i) {
        auto& target=s.targets[i];target.id=static_cast<int32_t>(i);target.enabled=1;
        target.width=s.host.Width();target.height=s.host.Height();
        target.horizontalScale=target.verticalScale=target.fieldOfViewScale=1.0f;
        if(i>=8) {
            // PAL SetScreenBuffers: six 64-square faces and the mapped
            // 256-square environment image retain their authored extents.
            target.width=target.height=i==14?256:64;target.format=0x32;
            target.opaqueOnly=i==14?0:1;
        }
    }
    ecstasyViewsBind(&s.cameras,s.views.data(),s.targets.data());
    s.environmentMap=std::make_unique<EnvironmentMap>(s.cameras,s.targets.data(),s.targets.size());
    ecstasyViewProjectionInitialize(&s.cameras.projection,s.host.Width());
    s.cameras.projection.nativeAspectCorrection=1;
    s.cameras.projection.nativeSourceVideoMode=gameTimeGetVideoMode(&s.clock);
    s.resources=std::make_unique<ApplicationResourceRuntime>(s.files,s.cameras,s.joylog,s.demo);
    auto& loader=s.resources->Loader();auto& registry=s.resources->Registry();
    gameInputRouterInitialize(&s.input,s.registrations.data(),s.registrations.size());
    joystickInitialize(&s.samples,&ReadApplicationInput);s.inputBound=true;
    Require(joystickInitializeTypeMonitoring(&s.input)==GAME_INPUT_OK,"Joystick monitor registration failed");
    easterEggs.input=&s.input;realDriverTuningInitialize(&s.driverTuning);positionMarkersInitialize(&s.positionMarkers);
    // Final LoadGlobalChunks1682D0 paths and flags. Missing files remain
    // physical failures; no generated bundle or alternate file is injected.
    loader.LoadResourceFile("GLOBAL/GLOBALA.BUN",0,6,s.resources->GlobalFileBindings());
    s.global=loader.LoadResourceFile("GLOBAL/GLOBALB.LZC",0,9,s.resources->GlobalFileBindings());
    s.global->ChangeFilenameForHotChunking("GLOBAL/GLOBALB.BUN");s.DrainFiles();
    const auto& global=s.global->Resources();
    Require(global.cars && global.track && global.characters && global.driverBehavior,
        "GLOBALB did not publish its original catalogues");
    s.tracks=std::make_unique<TrackInfoResources>(*global.track);
    s.language.Init(s.files);s.languageInitialized=true;
    s.dynamicTextures=std::make_unique<TexturePack>(texturePackRegistry,s.files,registry.Models(),"DynTextures","GLOBAL/DYNTEX.BIN");
    Require(s.dynamicTextures->LoadTextureTableFromFile("GLOBAL/DYNTEX.BIN"),"Missing final resource GLOBAL/DYNTEX.BIN");
    s.hudTextures=std::make_unique<TexturePack>(texturePackRegistry,s.files,registry.Models(),"HudTextures","GLOBAL/HUDTEX.BIN");
    Require(s.hudTextures->LoadTextureTableFromFile("GLOBAL/HUDTEX.BIN"),"Missing final resource GLOBAL/HUDTEX.BIN");
    s.carTextures=std::make_unique<TexturePack>(texturePackRegistry,s.files,registry.Models(),"CarTextures","CARS/TEXTURES.BIN");
    Require(s.carTextures->LoadTextureTableFromFile("CARS/TEXTURES.BIN"),"Missing final resource CARS/TEXTURES.BIN");
    worldAudioInitialize(&s.audioQueue,nullptr,0,s.audioRequests.data(),s.audioRequests.size());
    s.AudioResult(s.sound.Bind(s.audioQueue,s.files,carSoundGlobals.policy.audioSystemEnabled),"bSound binding");s.audioBound=true;
    s.soundBindings.audio=&s.audioQueue;s.soundBindings.globals=&carSoundGlobals;s.soundBindings.engineGlobals=&carEngineSoundGlobals;
    s.soundBindings.cars=global.cars;s.soundBindings.clock=&s.clock;s.soundBindings.files=&s.files;
    s.soundBindings.manager=&s.sound.Mixer();s.soundBindings.volumes=&s.volumes;
    s.soundBindings.spatial={nullptr,&s.cameras,&s.clock,&carSoundGlobals.speedOfSound,&carSoundGlobals.multiplayerAzimuth};
    s.soundBindings.physicalProgressOwner=&s;
    s.soundBindings.servicePhysicalAudio=[](void* context)->WorldAudioResult {
        auto& owner=*static_cast<Storage*>(context);
        if(!owner.audioOpened)return WORLD_AUDIO_INVALID_BINDING;
        if(!owner.host.PollEvents())return WORLD_AUDIO_BACKEND_FAILURE;
        return owner.host.PumpAudio(owner.sound);
    };
    s.AudioResult(s.sound.BindSoundClient(&s.soundBindings),"Sound source callback binding");
    s.uiSound.audio=&s.audioQueue;s.uiSound.enabled=&carSoundGlobals.policy.audioSystemEnabled;
    s.uiSound.clock=&s.soundClock;s.uiSound.manager=&s.sound.Mixer();s.uiSound.volumes=&s.volumes;
    worldAudioMessagesInitialize(&s.speechMessages);
    Require(worldStreamAudioBind(s.speechStream,s.speechMessages,s.sound,s.files,s.clock,carSoundGlobals,soundDriverState)==WORLD_STREAM_AUDIO_OK,
        "Original SED/speech binding failed");s.speechBound=true;
    auto* speech=worldSpeechGetInstance();Require(speech && speech->noise,"Original speech owner allocation failed");
    speech->messages=&s.speechMessages;speech->streamedAudio=&s.speechStream;
    s.AudioResult(speech->noise->Bind(&s.audioQueue,&worldAudioRuntime),"Speech noise binding");s.speechNoiseBound=true;
    if(carSoundGlobals.policy.audioSystemEnabled) {
        s.AudioResult(s.sound.Initialize(),"Sound driver initialization");
        s.AudioResult(s.host.OpenAudio(s.sound),"Physical audio device");s.audioOpened=true;
        s.AudioResult(s.sound.SetPath("sound\\"),"Source audio bank path");
        s.AudioResult(soundLoadRegion(s.soundBindings,"common"),"Source common sound bank");
        while(!soundDriverState.commonRegionLoaded) {
            s.AudioResult(s.host.PumpAudio(s.sound),"Physical common bank read");
            s.AudioResult(s.sound.Update(),"Common bank source completion");
            if(!s.host.PollEvents())throw std::runtime_error("Application was closed during sound initialization");
            std::this_thread::yield();
        }
        s.AudioResult(soundJukeboxPlayer.Bind(s.sound,s.files),"Original jukebox binding");s.jukeboxBound=true;
        if(applicationLaunchOptions.musicEnabled)
            s.AudioResult(soundJukeboxPlayer.LoadCartridge(s.demo.ActivationWord()?
                "SOUND/JukeboxMDemoDisk.JBC":"SOUND/JukeboxM.JBC",false),"Original music cartridge");
        sound::SetVolumeDefaults(s.uiSound,s.demo);
        s.AudioResult(s.host.PumpAudio(s.sound),"Physical mixer initialization");
    }
    s.language.Bind({s.profilesOwner,s.flow,*s.dynamicTextures,loader,registry.Flailer(),s.speechStream,
        &s,[](void* context) {
            auto& owner=*static_cast<Storage*>(context);
            Require(owner.audioOpened,"Localized speech retirement requires its physical audio device");
            owner.AudioResult(owner.host.PumpAudio(owner.sound),"Physical localized speech retirement");
            // Pump consumes the retained STOP itself. Do not run another
            // source Update/region completion while inside a flow teardown.
            std::this_thread::yield();
        }});
    // Final LoadLanguageInfo100808 tail100818 calls SetLanguage(0).
    s.language.SetLanguage(0);
    // PAL LoadGlobalChunks1683DC..16847C loads the initial dynamic image
    // after language selection, then services its real queued pixel reads.
    // English language0 itself does not request this texture.
    const uint32_t initialTexture=s.demo.ActivationWord() ? hashString("FIRSTLEGAL"):0xab0e817du;
    if(s.demo.ActivationWord() || !s.dynamicTextures->GetLoadedTexture(initialTexture))
        s.dynamicTextures->LoadTexturesFromFile(&initialTexture,1);
    s.DrainFiles();
    s.highScores=std::make_unique<HighScoreTable>(*s.tracks,s.profilesOwner);s.profilesOwner.highScores=s.highScores.get();
    for(auto& profile:s.profiles){profile.cars=global.cars;profile.language=&s.language;}
    s.setup.parameters=&s.parameters;s.setup.cars=global.cars;s.setup.language=&s.language;
    s.setup.tracks=s.tracks.get();s.setup.characters=&global.characters->Catalogue();
    s.setup.behavior=&global.driverBehavior->Settings();s.setup.applicationOptions=&applicationLaunchOptions;
    s.setup.demoDisc=&applicationLaunchOptions.bVersionE3;
    s.setup.currentEvent=&s.event;s.setup.runtimeSettings=&s.runtime;s.setup.demoReplay=&s.demoReplay;s.setup.flow=&s.flow;
    s.profilesOwner.InitWithDefaults(s.setup);
    frontend::InitializeDisplayCalibration(s.profilesOwner,s.clock);
    if(carSoundGlobals.policy.audioSystemEnabled)s.profilesOwner.CaptureAudioOptions(s.soundBindings,true);
    s.ui.resources=&registry.Flailer();s.ui.sound=&s.uiSound;s.ui.render=&s.renderer;
    s.ui.language=&s.language;s.ui.clock=&s.clock;s.ui.flow=&s.flow;
    // The native backend uses the original CPU RGBA branch. The decoder
    // and Flailer consumer must borrow the same source flag, not separate
    // zero/one defaults that publish pixels but suppress their bitmap.
    s.renderer.bitmapComposite.enabled=0;
    s.movie=std::make_unique<movie::MoviePlayer>(s.files,s.resources->FileIo(),registry,s.clock,
        carSoundGlobals.policy.audioSystemEnabled,s.renderer.bitmapComposite.enabled,&s.audioQueue);s.ui.movie=s.movie.get();
    s.renderer.bitmapComposite.movie=s.movie.get();
    // PAL InitFlailerMenuSystem1696C0 calls InitGlobalMenuSound16DED8
    // before the screen-list constructor; Common is now fully published.
    frontend::InitGlobalMenuSound(s.uiSound,s.soundBindings);
    s.BindMenus();
    s.frontendPhase=std::make_unique<GameFlowFrontendResources>(loader,s.startup,applicationLaunchOptions);
    ResourceFileBindings trackBinding;trackBinding.parameters=&s.parameters;trackBinding.sound=&s.soundBindings;
    trackBinding.cameras=&s.resources->CameraResources();trackBinding.particles=&s.resources->FX().Particles();
    s.trackPhase=std::make_unique<GameFlowTrackResources>(loader,*global.cars,*s.hudTextures,trackBinding);
    s.impactEffects={&s.audioQueue,&worldAudioRuntime.randomState,&carSoundGlobals.policy.audioSystemEnabled};
    s.AudioResult(worldImpactAudioBind(&s.impactAudio,&s.impactEffects),"World impact audio binding");
    s.replayAvailableBytes=s.host.AvailableMemory();
    GameFlowTrackProcessBindings process{s.resources.get(),s.global,&s.clock,&applicationLaunchOptions,&s.input,
        &s.cameras,&s.carNaming,&s.positionMarkers,&s.driverTuning,&s.positionalSamples,&s.impactAudio,
        &s.skipStartSequence,&s.alternateSkin,&s.replayAvailableBytes,
        &s.impactPresentationMode,&s.resources->Registry().Sun().Glare().start};
    s.trackPhase->BindProcessResources(process);s.soundBindings.spatial.players=s.trackPhase->Players();
    s.AudioResult(s.positionalSamples.Bind(&s.audioQueue,s.soundBindings.spatial,&carSoundGlobals.policy.audioSystemEnabled),
        "Shared positional audio binding");
    s.AudioResult(soundScapeBindServices(s.soundBindings),"Process soundscape binding");s.soundScapeBound=true;
    s.flowBindings={&s.clock,&s.demo,s.dynamicTextures.get(),&s.ui,s.frontendPhase.get(),s.trackPhase.get(),s.frame.get()};
    carLoader.Bind(loader,*global.cars,*s.carTextures,s.language,s.carNaming,s.positionMarkers,nullptr);
    Require(!applicationLaunchOptions.skipFrontend,"StartSkipFEGame requires its original FRED producer");
    ChangeGameFlow(s.flow,1,0);s.initialized=true;
}

bool ApplicationRuntime::Tick()
{
    using namespace frontend;auto& s=*storage_;Require(s.initialized,"Application frame precedes initialization");
    const auto tickStarted=s.host.FrameTime();
    platform::ScopedNativeDuration slowTickMeasure(s.slowFrame.tick);
    if(!s.frameTimingStarted) {
        s.frameTiming.Reset(tickStarted);s.simulationClock.Reset(tickStarted);
        applicationFrameTiming={};s.frameTimingStarted=true;
        s.presentationTimingEpoch=s.host.PresentationTimingEpoch();s.renderTimingWindowStart=tickStarted;
    }
    bool eventsReady{};
    {platform::ScopedNativeDuration measure(s.slowFrame.poll);eventsReady=s.host.PollEvents();}
    if(!eventsReady || s.demo.ExitRequested() || applicationDebugExitRequested)return false;
#ifdef __SWITCH2__
    // Menu changes apply before capture; retained draws cannot keep the old LOD.
    if(configureNativeFrameLimitQuality(nx::userSettings.frameLimit))s.InvalidatePreparedScene();
#endif
    if(s.presentationTimingEpoch!=s.host.PresentationTimingEpoch()) {
        s.presentationTimingEpoch=s.host.PresentationTimingEpoch();
        const auto resumed=s.host.FrameTime();
        s.lastAcceptedPresent={};s.slowLastPresent={};s.slowFrame={};
        s.InvalidatePreparedScene();
        s.frameTiming.Reset(resumed);s.simulationClock.Reset(resumed);applicationFrameTiming={};
    }
    const float frameDrawDistance=nativeRenderSettings().drawDistanceScale;
    constexpr bool frameEmergency=false;
    auto observed=s.host.FrameTime();
    if(!s.simulationClock.UpdateDue(observed) && !s.host.PresentationDue(observed)) {
        {platform::ScopedNativeDuration measure(s.slowFrame.wait);
            s.host.WaitUntil(std::min(s.simulationClock.Deadline(),s.host.PresentationDeadline()));}
        return true;
    }
    bool reusedPreparation=false;
    if(!s.simulationClock.UpdateDue(observed) && s.PreparedSceneUnchanged()) {
        platform::ScopedNativeDuration measure(s.presentationTiming);
        reusedPreparation=s.host.TrySubmitPreparedFrame();
        // Preserve the display owner's counter for this real render. It is
        // independent of logical time, input sampling and GPU completion.
        if(reusedPreparation)++s.cameras.renderFrameCounter;
    }
    const auto sourceBefore=s.sourceUpdateTiming.totalNanoseconds;
    platform::ScopedNativeDuration sourceMeasure(s.sourceUpdateTiming);
    bool advancedPresentation=false;
    bool sceneGeometryEmitted=true;
    std::uint32_t updates=0,captures=0;
    double captureMilliseconds=0.0,sealMilliseconds=0.0;
    if(!reusedPreparation)for(;;) {
        advancedPresentation=s.simulationClock.UpdateDue(observed);
        if(advancedPresentation) {
            s.InvalidatePreparedScene();
            // The previous logical frame stayed unchanged while additional
            // presentations borrowed it. Retire its post-draw mutations once.
            if(s.pendingClockAdvance) {
                Require(worldParticlesAdvance(&s.resources->FX().FrameParticles(),s.clock.realTimeElapsed)==WORLD_SERVICES_OK,
                    "FrameEffects lost its logical update owner");
                texturePackRegistry.UpdateAnimations(s.resources->Registry().Models(),s.clock.worldTimeFrames,s.clock.realTimeFrames);
                if(s.pendingLegacyRealClock)gameTimeAdvanceRealTime(&s.clock);
                else gameTimeAdvanceNativeRealTime(&s.clock);
                gameTimeAdvanceNativeWorldTime(&s.clock);
                s.pendingClockAdvance=false;
            }
            if(s.memcards)s.memcards->Service();
            if(s.launchDisplay)s.profilesOwner.nativeDisplay=*s.launchDisplay;
            const auto& display=s.profilesOwner.nativeDisplay;
            s.host.ApplyDisplaySettings(display.width,display.height,display.fullscreen,display.vsync);
            s.RefreshNativeDisplayGeometry();
#ifdef __SWITCH__
            const int preferredLanguage=nx::LanguageForSession(s.profilesOwner.currentLanguage);
            if(s.profilesOwner.currentLanguage!=preferredLanguage) {
                char detail[96]{};std::snprintf(detail,sizeof(detail),"profile=%d selected_session=%d",
                    s.profilesOwner.currentLanguage,preferredLanguage);
                s.host.RecordSessionEvent("language.profile_override",detail);
                s.profilesOwner.currentLanguage=preferredLanguage;
            }
#endif
            if(s.language.currentLanguage!=s.profilesOwner.currentLanguage)s.language.SetLanguage(s.profilesOwner.currentLanguage);
            const auto* previousSimulation=s.Simulation();
            const bool previouslyInGame=s.flow.inGameFlag!=0;
            s.ServiceFlow();
            if(auto* session=s.Session()) {
                Require(raceCoordinatorExecuteQueuedMessages(session)!=0,"RaceCoordinator queued-message dispatch failed");
                (void)raceCoordinatorCheckForDemoDiscTimeout(session);
                Require(session->result==RC_RESULT_OK,"RaceCoordinator demo timeout failed");
                Require(raceCoordinatorSendMessage(session,3,0)!=0,"RaceCoordinator frame message failed");
            }
            s.ServiceFlow();
            auto* simulation=s.Simulation();auto* session=s.Session();
            if(simulation!=previousSimulation || previouslyInGame!=(s.flow.inGameFlag!=0)) {
                // Loading/retirement is an explicit lifecycle boundary. Its
                // elapsed service time must not become catch-up movement in
                // a newly created race or skip the first visible countdown.
                observed=s.host.FrameTime();s.simulationClock.Reset(observed);
                s.replayStepSeconds=1.0f/60.0f;
            }
            s.RecordLifecycle(simulation,session);
            // PAL12F40C gates the application checksum on the live InGameFlag.
            // World owns the in-race checksum; do not duplicate it or consume a
            // second Joylog channel3 record during the same gameplay frame.
            if(!s.flow.inGameFlag)
                applicationVerifyJoylogChecksum(s.checksum,s.joylog,simulation,&s.screens,s.flow);
            auto* players=s.trackPhase->Players();carFramePlayerDetermineAllInputMaps(players);
            auto* replay=session && session->bindings && session->bindings->replayOwner?&session->bindings->replayOwner->State():nullptr;
            if(replay)replay->owner->DoFancyJoylogDebugging(s.clock,s.joylog);
            const bool restoringFrame=replay && replay->owner->WillRestoreFrame(s.clock);
            const auto elapsed=restoringFrame ? s.replayStepSeconds:1.0f/60.0f;
            const bool sourceJoylog=s.joylog.stream || s.joylog.replaying || s.joylog.recording;
            if(replay && !restoringFrame) {
                // Joylog has no version marker for a different control law.
                // Restored native/PAL replays use their own captured policy.
                replay->steeringPolicy=sourceJoylog ? REPLAY_STEERING_LEGACY:REPLAY_STEERING_TIME_BASED;
            }
            if(sourceJoylog) {
                // The original Joylog has no native clock-version marker.
                // Keep both of its clock domains on the recorded contract.
                s.clock.nativeTimingEnabled=0;s.clock.worldTimerPolicy=GAME_TIME_TIMER_LEGACY;
            } else if(s.sourceJoylogTiming) {
                const auto worldPolicy=s.clock.worldTimerPolicy;
                gameTimeUseNativeTiming(&s.clock);
                if(restoringFrame)s.clock.worldTimerPolicy=worldPolicy;
            }
            s.sourceJoylogTiming=sourceJoylog;s.pendingLegacyRealClock=sourceJoylog;
            Require((s.pendingLegacyRealClock ? gameTimePrepareRealTimestep(&s.clock,elapsed,&s.joylog):
                gameTimePrepareNativeRealTimestep(&s.clock,elapsed,&s.joylog))!=0,"Native real timestep preparation failed");
            s.scheduledStepQuanta=s.clock.realTimeQuantized>0 ? static_cast<std::uint32_t>(s.clock.realTimeQuantized):60u;
            if(!restoringFrame)s.replayStepSeconds=1.0f/60.0f;
            if(IsGameFlowPaused(session)) {gameTimeSetWorldElapsed(&s.clock,0.0f);s.GenerateInput(replay);}
            else if(!session) {gameTimeSetWorldElapsed(&s.clock,s.clock.realTimeElapsed);s.GenerateInput(nullptr);}
            else {
                Require(simulation && replay && session->bindings && session->bindings->startScreens,
                    "Live coordinator has no canonical World/Replay/screen graph");
                worldUpdatePause(&simulation->world->timing,s.clock.realTimeElapsed);
                if(worldIsPaused(&simulation->world->timing)) {gameTimeSetWorldElapsed(&s.clock,0.0f);s.GenerateInput(replay);}
                else {
                    replay->owner->BeginFrame(s.clock,session->bindings->startScreens->services);
                    if(replay->replayPausedThisFrame) {gameTimeSetWorldElapsed(&s.clock,0.0f);s.GenerateInput(replay);}
                    else {
                        WorldStepOptions options{replay->recording,replay->restoring,0,0,0};
                        WorldStepIO io{replay,
                            [](void* context,int bits,int channel,int32_t* value) {
                                auto& source=*static_cast<ReplayManager*>(context);
                                *value=static_cast<int32_t>(replayManagerRead(&source,static_cast<uint32_t>(bits),static_cast<uint32_t>(channel)));
                                return source.stream && source.stream->result==BIT_STREAM_OK?1:0;},
                            [](void* context,int32_t value,int bits,int channel) {
                                replayManagerWrite(static_cast<ReplayManager*>(context),static_cast<uint32_t>(value),
                                    static_cast<uint32_t>(bits),static_cast<uint32_t>(channel));},
                            [](void*){return hp2NativeTickerSource();},
                            [](void*,uint32_t from,uint32_t to){return platform::tickerMilliseconds(from,to);}};
                        float worldElapsed{},recordedElapsed{};
                        Require(worldGetNativeTimestep(&simulation->world->timing,&s.clock,&options,&io,&worldElapsed,&recordedElapsed)==WORLD_STEP_OK,
                            "Source World timestep/replay stream failed");
                        if(options.replaying && recordedElapsed>0.0f) {
                            s.replayStepSeconds=recordedElapsed;
                            // This is the file's unscaled timestep. Changing the
                            // presentation rate cannot consume its frames faster.
                            s.scheduledStepQuanta=static_cast<std::uint32_t>(gameTimeQuantizeElapsed(recordedElapsed));
                        }
                        gameTimeSetWorldElapsed(&s.clock,worldElapsed);s.GenerateInput(replay);
                        platform::ScopedNativeDuration measure(s.worldTiming,&s.slowFrame.world);
                        const double collisionBefore=s.worldProfile.collisionMs,thinkBefore=s.worldProfile.thinkingMs,
                            effectsBefore=s.worldProfile.effectsMs;
                        const auto stepResult=worldDoTimestep(simulation,worldElapsed,platform::nativeDiagnosticsEnabled ? &s.worldProfile:nullptr);
                        s.slowFrame.collisionMs+=s.worldProfile.collisionMs-collisionBefore;
                        s.slowFrame.thinkMs+=s.worldProfile.thinkingMs-thinkBefore;
                        s.slowFrame.effectsMs+=s.worldProfile.effectsMs-effectsBefore;
                        measure.Finish();
                        if(stepResult!=WORLD_SIMULATION_OK) {
                            // Report actual owner results without replacing a failure
                            // with a successful step or attributing a global collision
                            // failure to the last Car visited by the source loops.
                            auto detail=std::string("Original World timestep failed (world=")+
                                std::to_string(static_cast<int>(stepResult));
                            if(const auto* field=simulation->stateFailure.field) {
                                // Read the same failed owner once, before lengthy service
                                // counters. Do not recompute derived state on an error path.
                                const auto index=simulation->stateFailure.car;
                                detail+=", state_phase="+std::string(stepResult==WORLD_SIMULATION_REPLAY_FAILURE?
                                    "before_replay":"after_step");
                                detail+=", state_field="+std::string(field);
                                detail+=", state_element="+std::to_string(simulation->stateFailure.element);
                                detail+=", state_car="+std::to_string(index);
                                detail+=", loop="+std::to_string(s.clock.worldLoopCounter);
                                if(index>=0 && index<simulation->world->totalCars)
                                    if(const auto* car=simulation->world->carTable[index]) {
                                        detail+=", state_driver_number="+std::to_string(car->driverNumber);
                                        if(static_cast<std::size_t>(index)<simulation->frameCount && simulation->drivers)
                                            if(const auto* driver=simulation->drivers[index])
                                                detail+=", state_driver_kind="+std::to_string(static_cast<int>(driver->kind));
                                    }
                            }
                            if(simulation->collisions) {
                                detail+=", collision="+std::to_string(static_cast<int>(simulation->collisions->result));
                                if(const auto* objects=simulation->collisions->objects) {
                                    detail+=", objects_occupied="+std::to_string(objects->occupiedObjects);
                                    detail+=", objects_capacity="+std::to_string(objects->totalObjectCapacity);
                                    detail+=", objects_simulating="+std::to_string(objects->simulatingCount);
                                }
                                if(const auto* impact=simulation->collisions->contactAudio) {
                                    detail+=", impact_audio_registered="+std::to_string(impact->registered);
                                    detail+=", impact_audio_enabled="+std::to_string(
                                        impact->audio && impact->audio->enabled?*impact->audio->enabled:-1);
                                }
                            }
                            if(stepResult==WORLD_SIMULATION_DRIVER_FAILURE || stepResult==WORLD_SIMULATION_MOVEMENT_FAILURE ||
                                stepResult==WORLD_SIMULATION_FRAME_FAILURE) {
                                const auto index=simulation->failedCar;
                                detail+=", car="+std::to_string(index);
                                if(index>=0 && static_cast<std::size_t>(index)<simulation->frameCount && simulation->drivers)
                                    if(const auto* driver=simulation->drivers[index]) {
                                        detail+=", driver_kind="+std::to_string(static_cast<int>(driver->kind));
                                        detail+=", driver_result="+std::to_string(static_cast<int>(driver->result));
                                        const DriverAIBase* base=nullptr;
                                        const AIControls* input=nullptr;
                                        int ownerResult=-1;
                                        if(driver->kind==CAR_DRIVER_AI && driver->owner.ai) {
                                            base=&driver->owner.ai->base;input=&driver->owner.ai->input;
                                            ownerResult=driver->owner.ai->result;
                                        } else if(driver->kind==CAR_DRIVER_TRAFFIC && driver->owner.traffic) {
                                            base=&driver->owner.traffic->base;input=&driver->owner.traffic->input;
                                            ownerResult=driver->owner.traffic->result;
                                        }
                                        if(base) {
                                            // Observe the failing owners directly. Do not refresh a
                                            // route, invoke a controller, or mutate the recorded fault.
                                            detail+=", driver_owner_result="+std::to_string(ownerResult);
                                            detail+=", driver_base_result="+std::to_string(static_cast<int>(base->result));
                                            detail+=", driver_input_result="+std::to_string(input?static_cast<int>(input->result):-1);
                                            detail+=", driver_action_kind="+std::to_string(base->action?base->action->kind:-1);
                                            if(base->currentRoad) {
                                                detail+=", driver_road_group="+std::to_string(base->currentRoad->group?base->currentRoad->group->id:-1);
                                                detail+=", driver_pose_group="+std::to_string(base->currentRoad->recovery.recoveryPose?
                                                    base->currentRoad->recovery.recoveryPose->groupId:-1);
                                            }
                                        }
                                    }
                                if(index>=0 && static_cast<std::size_t>(index)<simulation->frameCount && simulation->frames) {
                                    const auto* failed=simulation->frames[index];
                                    if(failed) {
                                        detail+=", frame="+std::to_string(static_cast<int>(failed->result));
                                        if(failed->movement)detail+=", movement="+
                                            std::to_string(static_cast<int>(failed->movement->result));
                                        if(const auto* player=failed->environment.player) {
                                            detail+=", player_result="+std::to_string(player->result);
                                            detail+=", player_cinematic_state="+std::to_string(player->movement.steeringFeedbackBusy);
                                            if(const auto* registry=player->registry) {
                                                detail+=", driving_line_gate="+(registry->enableVisibleDrivingLine ?
                                                    std::to_string(*registry->enableVisibleDrivingLine):std::string("unbound"));
                                                if(registry->sessionState)detail+=", session_state="+
                                                    std::to_string(*registry->sessionState);
                                            }
                                        }
                                    }
                                }
                            }
                            detail+=")";
                            if(s.demoReplay.active)s.demoReplay.RecordSessionEvent("demo.world_error",nullptr,detail.c_str());
                            throw std::runtime_error(detail);
                        }
                    }
                    replayManagerEndFrame(replay,&s.clock);
                }
            }
            s.soundClock.ticks=static_cast<uint32_t>(s.clock.realTimer);
            MenuSystemTick(&s.screens);
            s.trackPhase->ServicePresentation(s.flow,s.profilesOwner);
            carFramePlayerUpdateNonGameStateAll(players);
            // PAL12F58C..12F5A8 updates the existing RC+2C ReplayScreen after the
            // Player HUD and before camera movers. Tick is a separate RC message.
            if(session)raceCoordinatorUpdateReplayScreen(*session);
            if(simulation)Require(carViewMoversAdvance(s.trackPhase->ViewMovers(),s.clock.realTimeElapsed)==CAR_VIEW_MOVER_OK,
                "Source camera coordinator update failed");
            // UpdateCameraMovers15EF28 advances the top concrete CameraMover of ten
            // views. C++ dispatch targets the actual owner, never a replacement pose.
            for(auto& screen:s.cameras.screens)if(screen.last)
                Require(carCinematicAdvanceOwned(screen.last)==CAR_CINEMATIC_OK,"Active camera mover update failed");
            {
                platform::ScopedNativeDuration measure(s.audioTiming,&s.slowFrame.audio);
                s.AudioResult(soundUpdate(s.soundBindings,s.trackPhase->Simulation(),s.speechStream,s.profilesOwner,s.parameters,s.flow,&s.joylog),"Original sound frame");
                if(s.audioOpened)s.AudioResult(s.host.PumpAudio(s.sound),"Physical audio frame");
            }
            joystickUpdatePortRumblers(s.input,s.clock);
            for(int32_t port=0;port<2;++port)if(s.input.pendingMotorDirty[port]) {
                s.host.SetRumble(port,s.input.pendingMotorBytes[port][0],s.input.pendingMotorBytes[port][1]);s.input.pendingMotorDirty[port]=0;
            }
            if(simulation)carCameraShakeAdvance(s.trackPhase->ViewMovers()->environment.shake,s.clock.realTimeElapsed);
            // Temporal driver state belongs to the same logical update as
            // input and cameras. Every visible view samples this prepared
            // pose; extra presentations and culling never advance its clock.
            if(simulation && simulation->world)
                for(std::int32_t index=0;index<simulation->world->totalCars;++index)
                    if(auto* car=simulation->world->carTable[index])
                        if(car->visual)carDriverVisualAdvance(car->visual);
        }
        Require(s.frame->State()==EcstasyFrame::Phase::Complete || s.frame->Commands().empty(),"Previous CPU frame capture is still live");
        if(s.frame->State()==EcstasyFrame::Phase::Complete)s.frame->BeginNextFrame();
        else Require(s.frame->State()==EcstasyFrame::Phase::Building && s.frame->Commands().empty(),"Previous CPU frame capture is still live");
        s.flowBindings.renderFrame=s.frame.get();
#ifdef __SWITCH__
        s.UpdateSwitchControls();
#endif
        EcstasySceneBindings scene{s.cameras,s.ui,s.trackPhase.get(),&s.flow,&s.parameters,&s.flow.currentState,&s.clock,&s.frontendFrame,&s.screens};
        scene.environmentMap=s.environmentMap.get();scene.effects=&s.resources->FX();
        scene.carCaptureMilliseconds=&s.renderCarCaptureMs;
        scene.sceneryCaptureTiming=&s.sceneryTiming;scene.sceneryFrameTiming=&s.slowFrame.scenery;
        scene.captureProfile=&s.captureProfile;
        scene.advancePresentation=advancedPresentation;scene.deferPresentationUpdates=true;
        if(advancedPresentation) {
            s.simulationClock.CompleteStep(s.scheduledStepQuanta);
            ++updates;
        }
#ifdef __SWITCH__
        if(s.flow.inGameFlag && ((advancedPresentation && s.simulationClock.UpdateDue(observed)) ||
            !s.host.PresentationDue(s.host.FrameTime())))scene.emitRaceGeometry=false;
        sceneGeometryEmitted=scene.emitRaceGeometry;
#endif
        s.RememberPreparedScene();
        if(scene.emitRaceGeometry)s.host.BeginSceneCapture();
        const auto captureStarted=std::chrono::steady_clock::now();
        const auto previousCapturePhases=s.captureProfile.milliseconds;
        EcstasyDisplayFrame(scene,*s.frame);
        for(std::size_t i=0;i<previousCapturePhases.size();++i)
            s.slowFrame.capture.milliseconds[i]+=s.captureProfile.milliseconds[i]-previousCapturePhases[i];
        const auto& shadowCapture=s.frame->CarShadowCaptureMilliseconds();
        for(std::size_t shadow=0;shadow<shadowCapture.size();++shadow)
            s.renderShadowCaptureMs[shadow]+=shadowCapture[shadow];
        s.renderCachedSceneryStrips+=s.frame->ReusedOpaqueSceneryStrips();
        const auto sealStarted=std::chrono::steady_clock::now();
        s.frame->Seal();
        const auto sealCompleted=std::chrono::steady_clock::now();
        captureMilliseconds+=std::chrono::duration<double,std::milli>(sealStarted-captureStarted).count();
        sealMilliseconds+=std::chrono::duration<double,std::milli>(sealCompleted-sealStarted).count();
        ++captures;
        if(advancedPresentation) {
            s.pendingClockAdvance=true;
            // Catch up complete logical updates before one presentation. Bound
            // work per host call, but retain all outstanding time for later calls.
            if(updates<8u && s.simulationClock.UpdateDue(observed)) {
                s.InvalidatePreparedScene();
                s.frame->CancelBeforeSubmit();
                s.ServiceFrameResources();
                continue;
            }
        }
        break;
    }
    sourceMeasure.Finish();
    const auto capturedUpdateMs=static_cast<double>(s.sourceUpdateTiming.totalNanoseconds-sourceBefore)*1.0e-6;
    const bool simulationBehind=s.simulationClock.UpdateDue(observed);
    if(!reusedPreparation && (simulationBehind || !sceneGeometryEmitted || !s.host.PresentationDue(s.host.FrameTime()))) {
        // A logical frame still executes the source capture/post-draw contract,
        // but a capped display or a bounded catch-up call does not submit it
        // to the GPU. Drain existing debt before another blocking presentation:
        // otherwise a GPU slower than eight logical steps per frame would add
        // time faster than this loop could consume it, despite retaining debt.
        // Return through PollEvents between batches to preserve responsiveness.
        s.InvalidatePreparedScene();s.frame->CancelBeforeSubmit();
        s.deferredPresentationCpuMilliseconds+=capturedUpdateMs;
        s.deferredCaptureMilliseconds+=captureMilliseconds;s.deferredSealMilliseconds+=sealMilliseconds;
        s.deferredLogicalUpdates+=updates;s.deferredSceneCaptures+=captures;
        if(advancedPresentation) {
            s.ServiceFrameResources();
        }
        if(!simulationBehind) {platform::ScopedNativeDuration measure(s.slowFrame.wait);
            s.host.WaitUntil(std::min(s.simulationClock.Deadline(),s.host.PresentationDeadline()));}
        return !s.demo.ExitRequested();
    }
    if(!reusedPreparation) {
        platform::ScopedNativeDuration measure(s.presentationTiming);s.host.SubmitFrame(*s.frame);
    }
    // Native timings have separate clock domains. CPU work includes capture,
    // Gather and recording/submission; waiting and presentation are reported
    // separately. GPU time is from the preceding completed Vulkan submission,
    // never the wall time spent by the CPU inside SubmitFrame.
    EcstasyVulkanFrameStatistics renderTiming{};
    const bool hasRenderTiming=s.host.ReadRenderStatistics(renderTiming);
    // Preserve the legacy aggregate, but expose display capture and Seal
    // separately. The residual includes input, simulation, camera/audio,
    // resource service and frame retirement; it is not physics time alone.
    // Canceled/deferred captures belong to the next submitted window too.
    const auto updateMs=capturedUpdateMs+s.deferredPresentationCpuMilliseconds;
    const auto captureMs=captureMilliseconds+s.deferredCaptureMilliseconds;
    const auto sealMs=sealMilliseconds+s.deferredSealMilliseconds;
    const auto logicalUpdates=updates+s.deferredLogicalUpdates;
    const auto sceneCaptures=captures+s.deferredSceneCaptures;
    s.deferredPresentationCpuMilliseconds=0.0;
    s.deferredCaptureMilliseconds=0.0;s.deferredSealMilliseconds=0.0;
    s.deferredLogicalUpdates=0;s.deferredSceneCaptures=0;
    applicationGpuTimingAvailable=hasRenderTiming && renderTiming.gpuTimeAvailable;
    if(s.clock.realTimeFrames>0 && (s.clock.realTimeFrames&8)!=((s.clock.realTimeFrames-1)&8)) {
        const auto rate=[](double milliseconds) {return static_cast<float>(1000.0/std::max(milliseconds,0.001));};
        applicationPreviousCpuFrameRate=rate(updateMs+renderTiming.gatherMilliseconds+renderTiming.recordSubmitMilliseconds);
        applicationPreviousGpuFrameRate=applicationGpuTimingAvailable?rate(renderTiming.gpuMilliseconds):0.0f;
    }
    const auto diagnosticsStarted=s.host.FrameTime();
    if(hasRenderTiming && platform::nativeDiagnosticsEnabled) {
        ++s.renderTimedFrames;s.renderUpdateMs+=updateMs;
        s.renderCaptureMs+=captureMs;s.renderSealMs+=sealMs;
        s.renderLogicalUpdates+=logicalUpdates;s.renderSceneCaptures+=sceneCaptures;
        s.renderPreparedFrames+=renderTiming.reusedPreparation?1u:0u;
        s.renderCompiledPipelines+=renderTiming.pipelineCreations;
        s.renderPendingPipelineDraws+=renderTiming.pendingPipelineDraws;
        s.renderGpuWorldVertices+=renderTiming.gpuWorldVertices;
        s.renderSceneryPipelineHits+=renderTiming.sceneryPipelineHits;
        s.renderGatherMs+=renderTiming.gatherMilliseconds;s.renderFenceMs+=renderTiming.fenceWaitMilliseconds;
        s.renderSubmitMs+=renderTiming.recordSubmitMilliseconds;s.renderPresentMs+=renderTiming.presentMilliseconds;
        const auto& profile=renderTiming.renderProfile;
        s.renderProfile.gatherValidationMilliseconds+=profile.gatherValidationMilliseconds;
        s.renderProfile.gatherFinishMilliseconds+=profile.gatherFinishMilliseconds;
        s.renderProfile.pipelineLookupMilliseconds+=profile.pipelineLookupMilliseconds;
        s.renderProfile.recordSceneMilliseconds+=profile.recordSceneMilliseconds;
        s.renderProfile.queueSubmitMilliseconds+=profile.queueSubmitMilliseconds;
        s.renderProfile.shadowGatherSamples+=profile.shadowGatherSamples;
        for(std::size_t i=0;i<profile.submitPhaseMilliseconds.size();++i)
            s.renderProfile.submitPhaseMilliseconds[i]+=profile.submitPhaseMilliseconds[i];
        for(std::size_t i=0;i<profile.gatherCategoryMilliseconds.size();++i) {
            s.renderProfile.gatherCategoryMilliseconds[i]+=profile.gatherCategoryMilliseconds[i];
            s.renderProfile.gatherCategoryCommands[i]+=profile.gatherCategoryCommands[i];
        }
        s.renderProfile.gatherCategorySamples+=profile.gatherCategorySamples;
        s.renderProfile.coldVertexJobs+=profile.coldVertexJobs;s.renderProfile.coldVertices+=profile.coldVertices;
        s.renderProfile.gatherVertexJobsMilliseconds+=profile.gatherVertexJobsMilliseconds;
        s.renderProfile.gatherIndicesMilliseconds+=profile.gatherIndicesMilliseconds;
        s.renderProfile.frameRetirementMilliseconds+=profile.frameRetirementMilliseconds;
        s.renderProfile.historyFrames+=profile.historyFrames;s.renderProfile.historyCopies+=profile.historyCopies;
        s.renderProfile.historySeeds+=profile.historySeeds;
        s.renderProfile.historyFramesWithoutTick+=profile.historyFramesWithoutTick;
        for(std::size_t shadow=0;shadow<profile.shadowStrips.size();++shadow) {
            s.renderProfile.shadowStrips[shadow]+=profile.shadowStrips[shadow];
            s.renderProfile.shadowTriangles[shadow]+=profile.shadowTriangles[shadow];
            s.renderProfile.shadowGatherMilliseconds[shadow]+=profile.shadowGatherMilliseconds[shadow];
        }
        if(renderTiming.gpuTimeAvailable && renderTiming.completedGpuFrame!=s.lastCompletedGpuFrame) {
            ++s.gpuTimedFrames;s.renderGpuMs+=renderTiming.gpuMilliseconds;
            s.lastCompletedGpuFrame=renderTiming.completedGpuFrame;
            const auto& gpu=renderTiming.gpuProfile;
            if(gpu.available) {
                ++s.renderGpuProfileSamples;
                s.renderGpuProfile.setupMilliseconds+=gpu.setupMilliseconds;
                s.renderGpuProfile.mainMilliseconds+=gpu.mainMilliseconds;
                s.renderGpuProfile.mirrorMilliseconds+=gpu.mirrorMilliseconds;
                s.renderGpuProfile.auxMilliseconds+=gpu.auxMilliseconds;
                s.renderGpuProfile.tailMilliseconds+=gpu.tailMilliseconds;
            }
        }
        const auto timingObserved=s.host.FrameTime();
        if(renderTiming.presentAccepted) {
            if(s.lastAcceptedPresent!=NativeFrameTiming::Clock::time_point{}) {
                const auto interval=std::chrono::duration<double,std::milli>(
                    timingObserved-s.lastAcceptedPresent).count();
                s.longestPresentIntervalMs=std::max(s.longestPresentIntervalMs,interval);
                s.presentsOver33ms+=interval>1000.0/30.0;
                s.presentsOver50ms+=interval>50.0;
            }
            s.lastAcceptedPresent=timingObserved;
        }
        if(timingObserved-s.renderTimingWindowStart>=std::chrono::seconds(5)) {
            const auto count=static_cast<double>(s.renderTimedFrames);
            const auto windowMilliseconds=std::chrono::duration<double,std::milli>(timingObserved-s.renderTimingWindowStart).count();
            char detail[768]{};
            std::snprintf(detail,sizeof(detail),
                "frames=%llu cpu_update_ms=%.3f cpu_gather_ms=%.3f cpu_submit_ms=%.3f fence_wait_ms=%.3f present_ms=%.3f gpu_ms=%.3f gpu_samples=%llu gpu_completed_frame=%llu hud_fps=%.2f hud_frame_ms=%.3f "
                "cpu_update_only_ms=%.3f cpu_capture_ms=%.3f cpu_seal_ms=%.3f logical_updates=%llu scene_captures=%llu window_ms=%.3f prepared_frames=%llu compiled_pipelines=%llu skipped_pipeline_draws=%llu cpu_cars_ms=%.3f gpu_world_vertices=%llu cached_scenery_strips=%llu scenery_pipeline_hits=%llu",
                static_cast<unsigned long long>(s.renderTimedFrames),s.renderUpdateMs/count,s.renderGatherMs/count,
                s.renderSubmitMs/count,s.renderFenceMs/count,s.renderPresentMs/count,
                s.gpuTimedFrames?s.renderGpuMs/static_cast<double>(s.gpuTimedFrames):0.0,
                static_cast<unsigned long long>(s.gpuTimedFrames),static_cast<unsigned long long>(s.lastCompletedGpuFrame),
                applicationFrameTiming.framesPerSecond,applicationFrameTiming.frameMilliseconds,
                std::max(0.0,s.renderUpdateMs-s.renderCaptureMs-s.renderSealMs)/count,
                s.renderCaptureMs/count,s.renderSealMs/count,
                static_cast<unsigned long long>(s.renderLogicalUpdates),
                static_cast<unsigned long long>(s.renderSceneCaptures),windowMilliseconds,
                static_cast<unsigned long long>(s.renderPreparedFrames),
                static_cast<unsigned long long>(s.renderCompiledPipelines),
                static_cast<unsigned long long>(s.renderPendingPipelineDraws),s.renderCarCaptureMs/count,
                static_cast<unsigned long long>(s.renderGpuWorldVertices),
                static_cast<unsigned long long>(s.renderCachedSceneryStrips),
                static_cast<unsigned long long>(s.renderSceneryPipelineHits));
            s.host.RecordSessionEvent("render.timing_window",detail);
            const auto milliseconds=[](std::uint64_t ns){return static_cast<double>(ns)*1.0e-6;};
            std::snprintf(detail,sizeof(detail),
                "world_loop=%d scenario=%d world_steps=%llu world_ms=%.3f world_step_peak_ms=%.3f "
                "think_ms=%.3f drive_ms=%.3f collision_ms=%.3f world_effects_ms=%.3f world_finish_ms=%.3f "
                "audio_ms=%.3f audio_step_peak_ms=%.3f input_ms=%.3f resource_ms=%.3f resource_peak_ms=%.3f "
                "scenery_ms=%.3f scenery_view_peak_ms=%.3f present_interval_peak_ms=%.3f intervals_over_33ms=%llu intervals_over_50ms=%llu",
                s.clock.worldLoopCounter,s.parameters.selectedScenario,
                static_cast<unsigned long long>(s.worldTiming.calls),
                milliseconds(s.worldTiming.totalNanoseconds)/count,milliseconds(s.worldTiming.maximumNanoseconds),
                s.worldProfile.thinkingMs/count,s.worldProfile.driveMs/count,s.worldProfile.collisionMs/count,
                s.worldProfile.effectsMs/count,s.worldProfile.finishMs/count,
                milliseconds(s.audioTiming.totalNanoseconds)/count,milliseconds(s.audioTiming.maximumNanoseconds),
                milliseconds(s.inputTiming.totalNanoseconds)/count,
                milliseconds(s.resourceTiming.totalNanoseconds)/count,milliseconds(s.resourceTiming.maximumNanoseconds),
                milliseconds(s.sceneryTiming.totalNanoseconds)/count,milliseconds(s.sceneryTiming.maximumNanoseconds),
                s.longestPresentIntervalMs,static_cast<unsigned long long>(s.presentsOver33ms),
                static_cast<unsigned long long>(s.presentsOver50ms));
            s.host.RecordSessionEvent("render.phase_window",detail);
            const auto effectSteps=static_cast<double>(std::max<std::uint64_t>(1,s.worldProfile.sampledSteps));
            std::snprintf(detail,sizeof(detail),
                "scenario=%d fps=%.2f interactions_ms=%.3f feedback_ms=%.3f particles_ms=%.3f surface_effects_ms=%.3f "
                "cops=%.1f cops_within_150m=%.1f particles=%.1f emitters=%.1f particle_pool=shared_free_list",
                s.parameters.selectedScenario,1000.0*count/windowMilliseconds,
                s.worldProfile.interactionsMs/count,s.worldProfile.feedbackMs/count,
                s.worldProfile.particlesMs/count,s.worldProfile.surfaceEffectsMs/count,
                s.worldProfile.copSamples/effectSteps,s.worldProfile.nearbyCopSamples/effectSteps,
                s.worldProfile.particleSamples/effectSteps,s.worldProfile.emitterSamples/effectSteps);
            s.host.RecordSessionEvent("render.chase_window",detail);
            const auto& cpu=s.renderProfile;
            const auto shadowSamples=static_cast<double>(std::max<std::uint64_t>(1,cpu.shadowGatherSamples));
            std::snprintf(detail,sizeof(detail),
                "scenario=%d fps=%.2f cpu_sun_shadow_capture_ms=%.3f cpu_road_shadow_capture_ms=%.3f "
                "sun_shadow_strips=%.1f road_shadow_strips=%.1f sun_shadow_triangles=%.1f road_shadow_triangles=%.1f "
                "shadow_gather_samples=%llu cpu_sun_shadow_gather_ms=%.3f cpu_road_shadow_gather_ms=%.3f "
                "cpu_gather_validate_ms=%.3f cpu_gather_packets_ms=%.3f cpu_gather_finish_ms=%.3f "
                "cpu_pipeline_lookup_ms=%.3f cpu_record_scene_ms=%.3f cpu_queue_submit_ms=%.3f cpu_submit_other_ms=%.3f",
                s.parameters.selectedScenario,1000.0*count/windowMilliseconds,
                s.renderShadowCaptureMs[0]/count,s.renderShadowCaptureMs[1]/count,
                cpu.shadowStrips[0]/count,cpu.shadowStrips[1]/count,cpu.shadowTriangles[0]/count,cpu.shadowTriangles[1]/count,
                static_cast<unsigned long long>(cpu.shadowGatherSamples),
                cpu.shadowGatherMilliseconds[0]/shadowSamples,cpu.shadowGatherMilliseconds[1]/shadowSamples,
                cpu.gatherValidationMilliseconds/count,
                std::max(0.0,s.renderGatherMs-cpu.gatherValidationMilliseconds-cpu.gatherFinishMilliseconds)/count,
                cpu.gatherFinishMilliseconds/count,cpu.pipelineLookupMilliseconds/count,
                cpu.recordSceneMilliseconds/count,cpu.queueSubmitMilliseconds/count,
                std::max(0.0,s.renderSubmitMs-cpu.pipelineLookupMilliseconds-cpu.recordSceneMilliseconds-cpu.queueSubmitMilliseconds)/count);
            s.host.RecordSessionEvent("render.detail_window",detail);
            const auto& capture=s.captureProfile;
            std::snprintf(detail,sizeof(detail),
                "scenario=%d fps=%.2f setup_ms=%.3f background_ms=%.3f world_ms=%.3f particles_ms=%.3f "
                "solids_ms=%.3f alpha_mirrors_ms=%.3f effects_ms=%.3f hud_ms=%.3f emitted=%llu discarded=%llu",
                s.parameters.selectedScenario,1000.0*count/windowMilliseconds,
                capture.milliseconds[0]/count,capture.milliseconds[1]/count,
                capture.milliseconds[2]/count,capture.milliseconds[3]/count,
                capture.milliseconds[4]/count,capture.milliseconds[5]/count,
                capture.milliseconds[6]/count,capture.milliseconds[7]/count,
                static_cast<unsigned long long>(capture.emittedCaptures),
                static_cast<unsigned long long>(capture.discardedCaptures));
            s.host.RecordSessionEvent("render.capture_detail",detail);
            const auto& phases=cpu.submitPhaseMilliseconds;
            double accounted=cpu.pipelineLookupMilliseconds+cpu.recordSceneMilliseconds+cpu.queueSubmitMilliseconds;
            for(const auto phase:phases)accounted+=phase;
            std::snprintf(detail,sizeof(detail),
                "scenario=%d fps=%.2f preflight_ms=%.3f acquire_ms=%.3f reclaim_ms=%.3f textures_ms=%.3f "
                "targets_ms=%.3f upload_ms=%.3f draw_plan_ms=%.3f postprocess_ms=%.3f finalize_ms=%.3f other_ms=%.3f retire_capture_ms=%.3f",
                s.parameters.selectedScenario,1000.0*count/windowMilliseconds,
                phases[0]/count,phases[1]/count,phases[2]/count,phases[3]/count,phases[4]/count,
                phases[5]/count,phases[6]/count,phases[7]/count,phases[8]/count,
                std::max(0.0,s.renderSubmitMs-accounted)/count,cpu.frameRetirementMilliseconds/count);
            s.host.RecordSessionEvent("render.submit_detail",detail);
            const auto categorySamples=static_cast<double>(std::max<std::uint64_t>(1,cpu.gatherCategorySamples));
            const auto& category=cpu.gatherCategoryMilliseconds;
            const auto& commands=cpu.gatherCategoryCommands;
            std::snprintf(detail,sizeof(detail),
                "scenario=%d sampled_frames=%llu sample_stride=32 scenery_ms=%.3f solids_ms=%.3f particles_ms=%.3f "
                "world_ms=%.3f ui_ms=%.3f effects_ms=%.3f commands=(%.0f,%.0f,%.0f,%.0f,%.0f,%.0f) "
                "vertex_jobs_ms=%.3f resident_indices_ms=%.3f cold_jobs=%.1f cold_vertices=%.1f",
                s.parameters.selectedScenario,static_cast<unsigned long long>(cpu.gatherCategorySamples),
                category[0]/categorySamples,category[1]/categorySamples,category[2]/categorySamples,
                category[3]/categorySamples,category[4]/categorySamples,category[5]/categorySamples,
                commands[0]/categorySamples,commands[1]/categorySamples,commands[2]/categorySamples,
                commands[3]/categorySamples,commands[4]/categorySamples,commands[5]/categorySamples,
                cpu.gatherVertexJobsMilliseconds/count,cpu.gatherIndicesMilliseconds/count,
                cpu.coldVertexJobs/count,cpu.coldVertices/count);
            s.host.RecordSessionEvent("render.gather_detail",detail);
            if(cpu.historyFrames) {
                std::snprintf(detail,sizeof(detail),
                    "frames=%llu clean_copies=%llu seeds=%llu frames_without_tick=%llu source=pre_blur_scene",
                    static_cast<unsigned long long>(cpu.historyFrames),static_cast<unsigned long long>(cpu.historyCopies),
                    static_cast<unsigned long long>(cpu.historySeeds),
                    static_cast<unsigned long long>(cpu.historyFramesWithoutTick));
                s.host.RecordSessionEvent("render.history_window",detail);
            }
            if(s.renderGpuProfileSamples) {
                const auto samples=static_cast<double>(s.renderGpuProfileSamples);
                const auto& gpu=s.renderGpuProfile;
                std::snprintf(detail,sizeof(detail),
                    "scenario=%d samples=%llu gpu_setup_span_ms=%.3f gpu_main_span_ms=%.3f gpu_mirror_span_ms=%.3f "
                    "gpu_aux_span_ms=%.3f gpu_tail_span_ms=%.3f",
                    s.parameters.selectedScenario,static_cast<unsigned long long>(s.renderGpuProfileSamples),
                    gpu.setupMilliseconds/samples,gpu.mainMilliseconds/samples,gpu.mirrorMilliseconds/samples,
                    gpu.auxMilliseconds/samples,gpu.tailMilliseconds/samples);
                s.host.RecordSessionEvent("render.gpu_pass_window",detail);
            }
            s.renderShadowCaptureMs={};s.renderProfile={};s.captureProfile={};s.renderGpuProfile={};s.renderGpuProfileSamples=0;
            s.worldProfile={};s.worldTiming={};s.audioTiming={};s.inputTiming={};
            s.resourceTiming={};s.sceneryTiming={};
            s.presentsOver33ms=0;s.presentsOver50ms=0;s.longestPresentIntervalMs=0.0;
            s.renderTimingWindowStart=timingObserved;s.renderTimedFrames=0;s.gpuTimedFrames=0;
            s.renderUpdateMs=0.0;s.renderCaptureMs=0.0;s.renderSealMs=0.0;
            s.renderLogicalUpdates=0;s.renderSceneCaptures=0;s.renderPreparedFrames=0;
            s.renderCompiledPipelines=0;s.renderPendingPipelineDraws=0;s.renderCarCaptureMs=0.0;
            s.renderGpuWorldVertices=0;s.renderCachedSceneryStrips=0;s.renderSceneryPipelineHits=0;
            s.renderGatherMs=0.0;s.renderFenceMs=0.0;
            s.renderSubmitMs=0.0;s.renderPresentMs=0.0;s.renderGpuMs=0.0;
        }
    }

    s.slowFrame.diagnostics.Observe(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        s.host.FrameTime()-diagnosticsStarted).count()));
    const auto tailStarted=s.host.FrameTime();
    Require(s.frame->State()==EcstasyFrame::Phase::Complete,"Physical presenter did not retire the CPU frame capture");
    if(advancedPresentation) {
        s.ServiceFrameResources();
    }
    if(s.preparedSceneValid && !s.PreparedSceneUnchanged())s.InvalidatePreparedScene();
    // No PAL/NTSC wait: presentation follows the physical backend/VSync.
    // Observe its complete wall interval, including resource and device waits.
    const auto completed=s.host.FrameTime();
    if(s.presentationTimingEpoch!=s.host.PresentationTimingEpoch()) {
        // Reset only on a physical window-resume epoch, never an active stall.
        s.presentationTimingEpoch=s.host.PresentationTimingEpoch();
        s.slowLastPresent={};s.slowFrame={};
        s.InvalidatePreparedScene();
        s.frameTiming.Reset(completed);s.simulationClock.Reset(completed);applicationFrameTiming={};
    } else if(s.frameTiming.Observe(completed,hasRenderTiming?&renderTiming:nullptr,updateMs)) {
        applicationFrameTiming=s.frameTiming.Read();
    }
    const auto slowCompleted=s.host.FrameTime();
    const double tailMilliseconds=std::chrono::duration<double,std::milli>(slowCompleted-tailStarted).count();
    slowTickMeasure.Finish();
    if(hasRenderTiming && renderTiming.presentAccepted && platform::nativeDiagnosticsEnabled) {
        const double interval=s.slowLastPresent==NativeFrameTiming::Clock::time_point{} ? 0.0:
            std::chrono::duration<double,std::milli>(slowCompleted-s.slowLastPresent).count();
        char slowDetail[2304]{};
        const auto ms=[](const platform::NativeDurationStatistics& value){return static_cast<double>(value.totalNanoseconds)*1.0e-6;};
        const auto peak=[](const platform::NativeDurationStatistics& value){return static_cast<double>(value.maximumNanoseconds)*1.0e-6;};
        const auto& slow=s.slowFrame;const auto& profile=renderTiming.renderProfile;
        const auto& phases=profile.submitPhaseMilliseconds;
        const bool qualityChanged=frameDrawDistance!=s.slowLastDrawDistance || frameDrawDistance!=nativeRenderSettings().drawDistanceScale;
        if(interval>50.0) {
            std::snprintf(slowDetail,sizeof(slowDetail),
                "frame=%llu loop=%d scenario=%d in_game=%d interval_ms=%.3f tick_ms=%.3f outside_tick_ms=%.3f "
                "update_ms=%.3f capture_ms=%.3f seal_ms=%.3f logical_updates=%llu captures=%llu "
                "world_ms=%.3f collision_ms=%.3f think_ms=%.3f effects_ms=%.3f world_peak_ms=%.3f "
                "audio_ms=%.3f audio_peak_ms=%.3f input_ms=%.3f resource_ms=%.3f resource_peak_ms=%.3f scenery_ms=%.3f "
                "capture_world_ms=%.3f capture_solids_ms=%.3f capture_particles_ms=%.3f capture_hud_ms=%.3f "
                "gather_ms=%.3f validate_ms=%.3f vertex_jobs_ms=%.3f indices_ms=%.3f "
                "submit_ms=%.3f pipeline_ms=%.3f record_ms=%.3f fence_ms=%.3f present_ms=%.3f retire_ms=%.3f "
                "preflight_ms=%.3f acquire_ms=%.3f reclaim_ms=%.3f textures_ms=%.3f targets_ms=%.3f "
                "upload_ms=%.3f draw_plan_ms=%.3f postprocess_ms=%.3f finalize_ms=%.3f "
                "poll_ms=%.3f wait_ms=%.3f diagnostics_ms=%.3f tail_ms=%.3f "
                "gpu_frame=%llu gpu_ms=%.3f gpu_valid=%d compiled=%llu skipped=%llu "
                "distance=%.2f next_distance=%.2f emergency=%d quality_changed=%d",
                static_cast<unsigned long long>(renderTiming.submittedFrame),s.clock.worldLoopCounter,
                s.parameters.selectedScenario,s.flow.inGameFlag,interval,ms(slow.tick),std::max(0.0,interval-ms(slow.tick)),
                updateMs,captureMs,sealMs,static_cast<unsigned long long>(logicalUpdates),static_cast<unsigned long long>(sceneCaptures),
                ms(slow.world),slow.collisionMs,slow.thinkMs,slow.effectsMs,peak(slow.world),
                ms(slow.audio),peak(slow.audio),ms(slow.input),ms(slow.resource),peak(slow.resource),ms(slow.scenery),
                slow.capture.milliseconds[2],slow.capture.milliseconds[4],slow.capture.milliseconds[3],slow.capture.milliseconds[7],
                renderTiming.gatherMilliseconds,profile.gatherValidationMilliseconds,profile.gatherVertexJobsMilliseconds,profile.gatherIndicesMilliseconds,
                renderTiming.recordSubmitMilliseconds,profile.pipelineLookupMilliseconds,profile.recordSceneMilliseconds,
                renderTiming.fenceWaitMilliseconds,renderTiming.presentMilliseconds,profile.frameRetirementMilliseconds,
                phases[0],phases[1],phases[2],phases[3],phases[4],phases[5],phases[6],phases[7],phases[8],
                ms(slow.poll),ms(slow.wait),ms(slow.diagnostics),tailMilliseconds,
                static_cast<unsigned long long>(renderTiming.completedGpuFrame),renderTiming.gpuMilliseconds,renderTiming.gpuTimeAvailable,
                static_cast<unsigned long long>(renderTiming.pipelineCreations),static_cast<unsigned long long>(renderTiming.pendingPipelineDraws),
                frameDrawDistance,nativeRenderSettings().drawDistanceScale,frameEmergency,qualityChanged);
        }
        // A previous frame's GPU duration is available only after its fence.
        // Keep that submission ID explicit; never label it as this CPU frame.
        char gpuDetail[192]{};
        if(renderTiming.gpuTimeAvailable && renderTiming.gpuMilliseconds>50.0 && renderTiming.completedGpuFrame!=s.lastSlowGpuFrame) {
            std::snprintf(gpuDetail,sizeof(gpuDetail),"frame=%llu gpu_ms=%.3f observed_at_frame=%llu",
                static_cast<unsigned long long>(renderTiming.completedGpuFrame),renderTiming.gpuMilliseconds,
                static_cast<unsigned long long>(renderTiming.submittedFrame));
            s.lastSlowGpuFrame=renderTiming.completedGpuFrame;
        }
        s.slowFrame={};s.slowLastPresent=slowCompleted;s.slowLastDrawDistance=frameDrawDistance;
        // Logging cost belongs to the following physical interval. The Switch
        // host buffers these records until an existing summary flush.
        if(slowDetail[0] || gpuDetail[0]) {
            platform::ScopedNativeDuration measure(s.slowFrame.diagnostics);
            if(slowDetail[0])s.host.RecordSessionEvent("render.slow_frame",slowDetail);
            if(gpuDetail[0])s.host.RecordSessionEvent("render.slow_gpu",gpuDetail);
        }
    }
    if constexpr(!platform::nativeDiagnosticsEnabled)s.slowFrame={};
    return !s.demo.ExitRequested();
}

std::exception_ptr ApplicationRuntime::ShutdownNoThrow() noexcept
{
    using namespace frontend;auto& s=*storage_;
    if(s.shutdownComplete || s.shutdownStarted)return s.shutdownFailure;
    s.shutdownStarted=true;
    s.InvalidatePreparedScene();
    s.host.RecordSessionEvent("shutdown.begin","terminal owner retirement");
    if(s.resources) {
        s.resources->Registry().Models().fixupsBlocked=1;
        s.resources->Loader().BeginShutdown();
        carLoader.BeginShutdown(s.resources->Loader());
        for(auto* pack:texturePackRegistry.packs)
            if(&pack->CatalogueRoot()==&s.resources->Registry().Models())pack->BeginShutdown();
    }
    // Close/join the actual consumer while all borrowed buffers still live.
    // Cancellation rejects retained commands; it never manufactures an ACK.
    if(s.audioOpened)s.Retire("Physical audio shutdown",[&] {
        s.AudioResult(s.host.CloseAudio(s.sound),"Physical audio shutdown");s.audioOpened=false;
    });
    if(s.audioBound)s.RetireAudio("Audio request cancellation",[&] {
        return s.sound.CancelDeviceRequestsAfterClose();
    });
    s.Retire("CPU frame cancellation",[&] {
        Require(s.frame->State()!=EcstasyFrame::Phase::Submitted,
            "Shutdown precedes retirement of the submitted CPU frame capture");
        if(s.frame->State()!=EcstasyFrame::Phase::Complete)s.frame->CancelBeforeSubmit();
    });
    s.Retire("Pending frontend frame",[&]{s.frontendFrame.Clear();});
    s.Retire("Pending UI submissions",[&]{s.renderer.CompleteSubmissions();});
    s.Retire("Active screen",[&]{if(s.screens.active)s.screens.ChangeToScreen(nullptr,false);});
    s.screens.active=nullptr;
    s.Retire("Input callbacks",[&] {
        while(s.input.count) {
            const auto registration=s.input.registrations[s.input.count-1];
            Require(gameInputUnregister(&s.input,registration.device,registration.event)!=0,
                "Terminal input callback has no registered owner");
        }
    });
    if(s.menusBound) {
        s.Retire("Campaign screen",[]{UnbindCampaignScreen();});
        s.Retire("Memcard screens",[]{UnbindMemcardScreens();});
        s.Retire("Car selection",[]{UnbindSelectCarScreen();});
        s.Retire("Track selection",[]{UnbindTrackSelectScreen();});
        s.Retire("Credits",[]{UnbindCreditsScreens();});
        s.Retire("Frontend startup",[]{UnbindFrontendStartup();});
        s.Retire("Main menu",[]{UnbindMainMenuScreen();});
        s.menusBound=false;s.mainMenuBound=false;
    } else if(s.mainMenuBound) {
        s.Retire("Main menu",[]{UnbindMainMenuScreen();});s.mainMenuBound=false;
    }
    // The authorized profile write joins in the owner destructor. Do not
    // dispatch its queued UI continuation during terminal cleanup.
    s.memcards.reset();
    if(s.movie)s.RecordCleanupFailure("Movie",s.movie->Shutdown());
    // Gameplay borrows these screens/backgrounds. Retire it before deleting
    // their owners, but retain phase fileBindings until all reads are joined.
    if(s.trackPhase)s.RecordCleanupFailure("Track phase",s.trackPhase->Close());
    if(s.frontendPhase)s.RecordCleanupFailure("Frontend phase",s.frontendPhase->Close());
    s.loadingAnimation.reset();s.screens.loadingAnimation=nullptr;
    s.background.reset();s.help.reset();s.screenStorage.clear();
    for(auto& popup:s.popups)popup.reset();
    s.screens.background=nullptr;s.screens.help=nullptr;s.screens.popups.fill(nullptr);
    s.screens.movieTextures={};s.screens.movieFilename.clear();s.screens.overrideHelpItems=nullptr;
    s.screens.root.next=s.screens.root.previous=&s.screens.root;
    if(s.resources) {
        s.Retire("Flailer strips",[&]{s.resources->Registry().Flailer().DeleteAllStrips();});
        s.RecordCleanupFailure("Resource loader",s.resources->Loader().Shutdown());
    }
    s.trackPhase.reset();s.frontendPhase.reset();s.global=nullptr;
    if(s.languageInitialized) {
        if(s.speechBound && s.language.textEnabled)
            s.RetireSpeech("Localized speech cleanup",[&]{return soundUnloadLocalizedSpeech(s.speechStream);});
        s.Retire("Language",[&]{s.language.Close();s.languageInitialized=false;});
    }
    if(s.speechBound) {
        auto* speech=worldSpeechGetInstance();
        speech->messages=nullptr;speech->streamedAudio=nullptr;speech->session=nullptr;
        s.RetireSpeech("Speech process shutdown",[&]{return worldStreamAudioDestroy(&s.speechStream);});
        s.speechBound=false;
    }
    if(s.audioBound) {
        if(s.soundScapeBound)s.RetireAudio("Soundscape",[]{return soundScapeDispose();});
        if(s.jukeboxBound)s.RetireAudio("Music",[&] {
            Require(soundJukeboxPlayer.audio==&s.audioQueue,"Music shutdown lost its actual audio owner");
            return soundJukeboxPlayer.Close();
        });
        s.RetireAudio("Positional sound",[&]{return s.positionalSamples.Close();});
        s.RetireAudio("Sound process",[&]{return s.sound.ShutdownAfterOwnersRetired();});
        s.Retire("Audio completion callbacks",[&] {
            Require((!soundDriverState.completionFlow || soundDriverState.completionFlow==&s.flow) &&
                (!soundDriverState.completionBindings || soundDriverState.completionBindings==&s.flowBindings),
                "Sound shutdown encountered another application's completion owner");
            soundDriverState.completion=SoundRegionCompletion::None;
            soundDriverState.completionFlow=nullptr;soundDriverState.completionBindings=nullptr;
            soundDriverState.commonRegionLoaded=0;soundDriverState.frontendRegionCache=nullptr;
            for(auto& region:soundDriverState.regions)region={};
        });
        s.audioBound=false;
    }
    if(s.soundScapeBound)s.Retire("Weather clock",[&] {
        Require(weatherManUnbindClock(&s.clock)==WEATHER_MAN_OK,
            "Weather clock retirement has a different owner or live regions");s.soundScapeBound=false;
    });
    if(s.jukeboxBound)s.Retire("Music callback",[&] {
        s.AudioResult(soundJukeboxPlayer.UnbindAfterAudioShutdown(s.sound,s.audioQueue),"Music callback shutdown");
        s.jukeboxBound=false;
    });
    if(s.speechNoiseBound)s.Retire("Speech noise callback",[&] {
        auto* speech=worldSpeechGetInstance();Require(speech && speech->noise,"Speech noise owner disappeared");
        s.AudioResult(speech->noise->UnbindAfterAudioShutdown(s.audioQueue),"Speech noise callback shutdown");
        s.speechNoiseBound=false;
    });
    s.renderer.bitmapComposite.movie=nullptr;s.ui.movie=nullptr;
    s.movie.reset();s.environmentMap.reset();
    for(auto* pack:{s.carTextures.get(),s.hudTextures.get(),s.dynamicTextures.get()})
        if(pack)s.RecordCleanupFailure("Texture pack",pack->Shutdown());
    s.carTextures.reset();s.hudTextures.reset();s.dynamicTextures.reset();
    s.highScores.reset();s.tracks.reset();s.resources.reset();
    if(s.inputBound) {
        s.Retire("Input type monitoring",[&]{joystickCloseTypeMonitoring(&s.input);});
        s.Retire("Input samples",[&]{joystickShutdown(&s.samples);});s.inputBound=false;
    }
    if(easterEggs.input==&s.input)easterEggs.input=nullptr;
    s.Retire("Stomper",[]{CloseStomper();});
    s.initialized=false;s.shutdownComplete=true;
    s.host.RecordSessionEvent("shutdown.end",s.shutdownFailure?"completed with cleanup failure":"complete");
    return s.shutdownFailure;
}

void ApplicationRuntime::Shutdown()
{
    if(auto failure=ShutdownNoThrow())std::rethrow_exception(failure);
}
}
