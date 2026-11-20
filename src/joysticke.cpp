#include "indep/inc/CarDriver.hpp"
#include "indep/inc/EasterEggs.hpp"
#include "indep/inc/Flags.hpp"
#include "bware/inc/bWare.hpp"
#include <cstdio>
#include <cstdlib>

#include <string.h>

// Preserve the game sample cadence and ring ordering while storing typed
// host controls directly. No SDK packet/header is synthesized or parsed.
static void capture(JoystickSystem* joystick)
{
    if (!joystick->initialized)
        return;
    const int32_t next = (joystick->top + 1) & (JOYSTICK_SAMPLES - 1);
    if (next == joystick->bottom)
        return;

    for (int32_t port = 0; port < JOYSTICK_PORT_COUNT; ++port) {
        JoystickSample* sample = &joystick->samples[port][joystick->top];
        memset(sample, 0, sizeof(*sample));
        if (joystick->read == NULL ||
            !joystick->read(port, &sample->controls)) {
            // Preserve the former disconnected sample's logical values for
            // source edge/hysteresis semantics; its profile rejects input.
            sample->controls.held=0;
            memset(sample->controls.sticks,255,sizeof sample->controls.sticks);
            memset(sample->controls.pressures,255,sizeof sample->controls.pressures);
            sample->controls.mappingProfile=JOYSTICK_PROFILE_DISCONNECTED;
            sample->controllerType=JOYSTICK_PROFILE_DISCONNECTED;
        }
    }
    joystick->top = next;
}

void joystickInitialize(JoystickSystem* joystick, JoystickReader reader)
{
    memset(joystick, 0, sizeof(*joystick));
    joystick->read = reader;
    for(auto& port:joystick->samples)for(auto& sample:port) {
        // Equivalent initial logical values of the original zeroed ring.
        sample.controls.held=0xffff;
        sample.controls.mappingProfile=JOYSTICK_PROFILE_DISCONNECTED;
    }
    joystick->initialized = 1;
}

void joystickShutdown(JoystickSystem* joystick)
{
    joystick->initialized = 0;
    joystick->read = NULL;
}

void joystickSetReader(JoystickSystem* joystick, JoystickReader reader)
{
    joystick->read = reader;
}

void joystickSetVblankReading(JoystickSystem* joystick, int enabled)
{
    joystick->readInVblank = enabled != 0;
}

void joystickReadVblank(JoystickSystem* joystick)
{
    if (joystick->readInVblank)
        capture(joystick);
}

int32_t joystickPadType(const JoystickSample* sample)
{
    const auto profile=sample->controls.mappingProfile;
    return profile>0 && profile<=255?profile:JOYSTICK_PROFILE_DISCONNECTED;
}

void joystickPrepareFrame(JoystickSystem* joystick)
{
    if (!joystick->readInVblank)
        capture(joystick);

    joystick->preparedTop = joystick->top;
    if (joystick->bottom == joystick->preparedTop)
        return;

    for (int32_t port = 0; port < JOYSTICK_PORT_COUNT; ++port) {
        const int32_t padState = joystick->portState[port];
        int32_t cursor = joystick->bottom;
        do {
            JoystickSample* sample = &joystick->samples[port][cursor];
            sample->controllerType = padState == -1 ? -1 : joystickPadType(sample);
            cursor = (cursor + 1) & (JOYSTICK_SAMPLES - 1);
        } while (cursor != joystick->preparedTop);
    }
}

int32_t joystickHasFrame(const JoystickSystem* joystick)
{
    return joystick->bottom != joystick->preparedTop;
}

void joystickAdvanceFrame(JoystickSystem* joystick)
{
    joystick->bottom = (joystick->bottom + 1) & (JOYSTICK_SAMPLES - 1);
}

const JoystickControls* joystickCurrentData(const JoystickSystem* joystick, int32_t port,
                                    int32_t* controllerType, const JoystickControls** previousData)
{
    if (port < 0 || port >= JOYSTICK_PORT_COUNT ||
        controllerType == NULL || previousData == NULL)
        return NULL;
    const int32_t current = joystick->bottom;
    *controllerType = joystick->samples[port][current].controllerType;
    *previousData = &joystick->samples[port][(current - 1) & (JOYSTICK_SAMPLES - 1)].controls;
    return &joystick->samples[port][current].controls;
}


int32_t joystickSupportsPressureMode(uint8_t type)
{
    return type == 0x73 || type == 0x79;
}


#include "indep/inc/CarDriver.hpp"
int32_t joystickCanBeCalibrated(uint8_t type)
{
    return type==0x15 || type==0x16 || type==0x23 || type==0x53;
}
int32_t joystickHasStiffThrottle(const GameInputRouter* router,int32_t port)
{
    const auto type=joystickGetType(router,port);
    return type==0x53 || type==0x73 || type==0x79;
}
int32_t joystickIsTypeWheel(const GameInputRouter* router,int32_t port)
{
    const auto type=joystickGetType(router,port);
    return type==0x15 || type==0x16 || type==0x23;
}
void joystickSetRumble(GameInputRouter* router,int32_t port,uint8_t smallMotor,uint8_t largeMotor)
{
    if (!router || port<0 || port>=2)return;
    auto& bytes=router->pendingMotorBytes[port];
    bytes[0]=smallMotor;bytes[1]=largeMotor;
    bytes[2]=0;bytes[3]=0;bytes[4]=0;bytes[5]=0;
    router->pendingMotorDirty[port]=1;
}

// Native device configuration and logical control mapping.
#include "indep/inc/CarDriver.hpp"
#include <cstring>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace hp2::platform {
NativeInputTimestamp::NativeInputTimestamp(Clock::time_point origin,std::uint64_t milliseconds) noexcept
    :origin_(origin),milliseconds_(milliseconds){}

NativeInputTimestamp::Clock::time_point NativeInputTimestamp::From32(
    std::uint32_t milliseconds,std::uint64_t observedMilliseconds) const
{
    // MSG.time and SDL event timestamps wrap at 2^32 ms. Choose the epoch
    // nearest this observation without an implementation-defined signed cast.
    const auto distance=static_cast<std::uint32_t>(milliseconds-static_cast<std::uint32_t>(observedMilliseconds));
    const auto behind=std::uint64_t{0x100000000ull}-distance;
    std::uint64_t eventMilliseconds{};
    if(distance<=std::uint32_t{0x7fffffffu}) {
        if(observedMilliseconds>std::numeric_limits<std::uint64_t>::max()-distance)
            throw std::overflow_error("Native input timestamp exceeds its clock extent");
        eventMilliseconds=observedMilliseconds+distance;
    } else {
        if(observedMilliseconds<behind)throw std::invalid_argument("Native input timestamp precedes its clock epoch");
        eventMilliseconds=observedMilliseconds-behind;
    }
    const bool forward=eventMilliseconds>=milliseconds_;
    const auto delta=forward?eventMilliseconds-milliseconds_:milliseconds_-eventMilliseconds;
    const auto limit=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()).count();
    if(delta>static_cast<std::uint64_t>(limit))throw std::overflow_error("Native input timestamp duration overflow");
    const auto duration=std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(static_cast<std::int64_t>(delta)));
    const auto origin=origin_.time_since_epoch();
    if(forward) {
        if(origin>Clock::duration::max()-duration)throw std::overflow_error("Native input timestamp addition overflow");
        return Clock::time_point(origin+duration);
    }
    if(origin<Clock::duration::min()+duration)throw std::overflow_error("Native input timestamp subtraction overflow");
    return Clock::time_point(origin-duration);
}

void NativeInputTimeline::Insert(Observation observation)
{
    // Equal-time records retain producer order, including focus loss and a
    // release delivered together. Different sources can arrive out of order.
    if(sampled_ && observation.time<lastBoundary_)++lateObservations_;
    const auto position=std::upper_bound(observations_.begin(),observations_.end(),observation.time,
        [](const auto& time,const Observation& value){return time<value.time;});
    observations_.insert(position,std::move(observation));
}
void NativeInputTimeline::Keyboard(Clock::time_point time,std::uint16_t key,bool down)
{
    if(key>=state_.keys.size())throw std::out_of_range("Native input key");
    Observation observation{};observation.time=time;observation.kind=Kind::Keyboard;
    observation.key=key;observation.value=down;Insert(std::move(observation));
}
void NativeInputTimeline::Controller(Clock::time_point time,std::int32_t port,const NativePadSample& sample)
{
    if(port<0 || port>=2)throw std::out_of_range("Native input controller port");
    Observation observation{};observation.time=time;observation.kind=Kind::Controller;
    observation.port=static_cast<std::uint8_t>(port);observation.pad=sample;Insert(std::move(observation));
}
void NativeInputTimeline::Focus(Clock::time_point time,bool focused)
{
    Observation observation{};observation.time=time;observation.kind=Kind::Focus;
    observation.value=focused;Insert(std::move(observation));
}
void NativeInputTimeline::Character(Clock::time_point time,std::uint32_t character,bool alt,bool control)
{
    Observation observation{};observation.time=time;observation.kind=Kind::Character;
    observation.character=character;observation.alt=alt;observation.control=control;Insert(std::move(observation));
}
const NativeInputSnapshot& NativeInputTimeline::Sample(Clock::time_point boundary,bool textEnabled)
{
    if(sampled_ && boundary<lastBoundary_)throw std::invalid_argument("Native input boundary moved backwards");
    state_.textCommands.clear();
    if(!textEnabled)text_.Cancel();
    while(!observations_.empty() && observations_.front().time<=boundary) {
        const auto& observation=observations_.front();
        switch(observation.kind) {
        case Kind::Keyboard:
            if(state_.focused) {
                state_.keys[observation.key]=observation.value;
                // Windows exposes left/right modifier identities separately.
                // Rebuild generic aliases from the historical state, never
                // from the acquisition cache after a later focus event.
                if(observation.key==0xa0 || observation.key==0xa1)state_.keys[0x10]=state_.keys[0xa0] || state_.keys[0xa1];
                if(observation.key==0xa2 || observation.key==0xa3)state_.keys[0x11]=state_.keys[0xa2] || state_.keys[0xa3];
                if(observation.key==0xa4 || observation.key==0xa5)state_.keys[0x12]=state_.keys[0xa4] || state_.keys[0xa5];
            }
            if((!observation.value && (observation.key==0x12 || observation.key==0xa4 || observation.key==0xa5)) ||
                (observation.value && (observation.key==0x1b || observation.key==0x11 || observation.key==0xa2 ||
                    observation.key==0xa3 || observation.key==0x73)))text_.Cancel();
            break;
        case Kind::Controller: {
            const auto port=observation.port;
            if(state_.pads[port].instanceId!=observation.pad.instanceId ||
                state_.pads[port].connected!=observation.pad.connected)++state_.resetEpoch[port];
            state_.pads[port]=observation.pad;break;
        }
        case Kind::Focus:
            if(state_.focused!=observation.value)for(auto& epoch:state_.resetEpoch)++epoch;
            state_.focused=observation.value;
            if(!state_.focused) {state_.keys.fill(false);text_.Cancel();}
            break;
        case Kind::Character: {
            std::array<char,6> command{};
            if(text_.Character(observation.character,observation.alt,observation.control,state_.focused,textEnabled,command)==
                NativeCheatTextResult::Complete)state_.textCommands.push_back(command);
            break;
        }
        }
        observations_.pop_front();
    }
    std::array<bool,256> ignored{};
    text_.FilterKeyboard(ignored,state_.keys[0x12],state_.keys[0x11],state_.focused,textEnabled);
    state_.keyboardCaptured=text_.Capturing();
    sampled_=true;lastBoundary_=boundary;return state_;
}

bool NativeKeyboardEnabled(const NativeInputSettings& settings,std::int32_t port) noexcept
{
    if(port<0 || port>=2)return false;
    const auto mode=settings.deviceMode[static_cast<std::size_t>(port)];
    return mode==NativeDeviceMode::Keyboard || mode==NativeDeviceMode::KeyboardAndGamepad ||
        (mode==NativeDeviceMode::Automatic && port==0);
}
bool NativeGamepadEnabled(const NativeInputSettings& settings,std::int32_t port) noexcept
{
    if(port<0 || port>=2)return false;
    const auto index=static_cast<std::size_t>(port);
    const auto mode=settings.deviceMode[index];
    return mode==NativeDeviceMode::Gamepad || mode==NativeDeviceMode::KeyboardAndGamepad ||
        (mode==NativeDeviceMode::Automatic && (settings.controller[index]>=0 || !settings.deviceKey[index].empty()));
}
const std::array<std::uint16_t,20>& NativeKeyboardBindings(const NativeInputSettings& settings,std::int32_t port)
{
    if(port<0 || port>=2)throw std::out_of_range("Native input player port");
    return port==0?settings.keys:settings.player2Keys;
}
void ValidateNativeInputSettings(const NativeInputSettings& settings)
{
    for(const auto* keys:{&settings.keys,&settings.player2Keys})for(const auto key:*keys)
        if(key>254)throw std::invalid_argument("Keyboard binding is outside the Windows virtual-key range");
    for(const auto controller:settings.controller)if(controller<-1 || controller>255)
        throw std::invalid_argument("Invalid legacy controller index");
    if(settings.leftDeadzone<0 || settings.leftDeadzone>29490 ||
        settings.rightDeadzone<0 || settings.rightDeadzone>29490)
        throw std::invalid_argument("Stick deadzone must be between 0 and 90 percent");
    if(settings.triggerThreshold<0 || settings.triggerThreshold>229)
        throw std::invalid_argument("Trigger deadzone must be between 0 and 90 percent");
    for(std::size_t player=0;player<2;++player) {
        const auto mode=settings.deviceMode[player];
        if(mode<NativeDeviceMode::Automatic || mode>NativeDeviceMode::Disabled)
            throw std::invalid_argument("Unknown player input device mode");
        const auto& key=settings.deviceKey[player];
        if(key.size()>4096 || std::any_of(key.begin(),key.end(),[](unsigned char c){return c<32 || c==127;}))
            throw std::invalid_argument("Invalid persistent controller identity");
        const auto& profile=settings.profiles[player];
        for(const auto& binding:profile.buttons) {
            if(binding.kind<NativeBindingKind::None || binding.kind>NativeBindingKind::AxisFullNegative)
                throw std::invalid_argument("Unknown controller binding kind");
            if(binding.kind==NativeBindingKind::None)continue;
            const int limit=binding.kind==NativeBindingKind::Button?64:
                binding.kind==NativeBindingKind::Hat?4:16;
            if(binding.index<0 || binding.index>=limit)
                throw std::invalid_argument("Controller binding exceeds supported physical inputs");
            if(binding.kind==NativeBindingKind::Hat && binding.direction!=1 && binding.direction!=2 &&
                binding.direction!=4 && binding.direction!=8)
                throw std::invalid_argument("Hat binding must select one direction");
        }
        for(std::size_t axis=0;axis<4;++axis) {
            const int zone=profile.deadzone[axis]<0?(axis<2?settings.rightDeadzone:settings.leftDeadzone):profile.deadzone[axis];
            if(profile.axes[axis]<-1 || profile.axes[axis]>=16 || profile.deadzone[axis]<-1 ||
                profile.deadzone[axis]>29490 || profile.saturation[axis]<1 ||
                profile.saturation[axis]>32767 || zone>=profile.saturation[axis])
                throw std::invalid_argument("Invalid controller axis, deadzone or saturation range");
        }
        if(profile.triggerThreshold<-1 || profile.triggerThreshold>229 ||
            profile.vibrationScale<0 || profile.vibrationScale>100)
            throw std::invalid_argument("Invalid controller trigger deadzone or vibration strength");
    }
    if(NativeGamepadEnabled(settings,0) && NativeGamepadEnabled(settings,1)) {
        const bool sameKey=!settings.deviceKey[0].empty() && settings.deviceKey[0]==settings.deviceKey[1];
        const bool sameIndex=settings.deviceKey[0].empty() && settings.deviceKey[1].empty() &&
            settings.controller[0]>=0 && settings.controller[0]==settings.controller[1];
        if(sameKey || sameIndex)throw std::invalid_argument("The same controller cannot drive both players");
    }
}

void MigrateNativeInputPreferences(NativeInputSettings& settings)
{
    ValidateNativeInputSettings(settings);
    auto migrated=settings;
    for(auto& profile:migrated.profiles) {
        const auto axes=profile.axes;
        const auto inverted=profile.invert;
        for(std::size_t i=0;i<4;++i) {
            const auto source=settings.swapSticks?(i+2)%4:i;
            profile.axes[i]=axes[source];
            profile.invert[i]=inverted[source] ^ (i==2 && settings.invertSteering) ^
                (i==1 && settings.invertLookY);
        }
    }
    migrated.swapSticks=false;migrated.invertSteering=false;migrated.invertLookY=false;
    for(std::size_t player=0;player<2;++player)
        if(migrated.deviceMode[player]==NativeDeviceMode::Automatic && migrated.controller[player]<0 && migrated.deviceKey[player].empty())
            migrated.deviceMode[player]=player==0?NativeDeviceMode::Keyboard:NativeDeviceMode::Disabled;
    ValidateNativeInputSettings(migrated);settings=std::move(migrated);
}

std::int32_t ResolveNativeDevice(const std::vector<NativeInputDeviceInfo>& devices,
    const std::string& key,std::int32_t fallbackIndex) noexcept
{
    int resolved=-1;
    for(std::size_t i=0;i<devices.size();++i) {
        const bool match=key.empty()?devices[i].index==fallbackIndex:
            devices[i].id==key || (!devices[i].pathKey.empty() && devices[i].pathKey==key);
        if(match) {
            if(resolved>=0)return -1; // Ambiguous identities never mean "the first one".
            resolved=static_cast<std::int32_t>(i);
        }
    }
    return resolved; // An unplugged saved device never becomes a different controller.
}

std::int32_t ResolveNativePlayerDevice(const NativeInputSettings& settings,
    const std::vector<NativeInputDeviceInfo>& devices,std::int32_t port) noexcept
{
    if(!NativeGamepadEnabled(settings,port))return -1;
    const auto player=static_cast<std::size_t>(port);
    if(!settings.deviceKey[player].empty())
        return ResolveNativeDevice(devices,settings.deviceKey[player],-1);
    int reserved=-1;
    if(port==1)reserved=ResolveNativePlayerDevice(settings,devices,0);
    else if(NativeGamepadEnabled(settings,1) && !settings.deviceKey[1].empty())
        reserved=ResolveNativeDevice(devices,settings.deviceKey[1],-1);
    const auto usable=[&](int candidate) {
        if(candidate<0 || candidate==reserved)return false;
        const auto& id=devices[static_cast<std::size_t>(candidate)].id;
        return !id.empty() && ResolveNativeDevice(devices,id,-1)==candidate;
    };
    const int preferred=ResolveNativeDevice(devices,"",settings.controller[player]);
    if(usable(preferred))return preferred;
    for(std::size_t i=0;i<devices.size();++i)
        if(usable(static_cast<int>(i)))return static_cast<int>(i);
    return -1;
}

namespace {
NativePadSample CanonicalRawSample(const NativePadSample& pad) noexcept
{
    if(pad.rawValid)return pad;
    auto result=pad;result.rawValid=true;result.standardized=true;
    result.axisCount=6;result.buttonCount=15;result.hatCount=0;
    constexpr std::uint16_t bits[15]={0x1000,0x2000,0x4000,0x8000,0x0020,0,0x0010,0x0040,0x0080,
        0x0100,0x0200,0x0001,0x0002,0x0004,0x0008};
    for(std::size_t i=0;i<15;++i)result.rawButtons[i]=(pad.buttons&bits[i])!=0;
    const auto downward=[](std::int16_t value) noexcept {
        return static_cast<std::int16_t>(std::clamp(-static_cast<int>(value),-32768,32767));
    };
    result.rawAxes[0]=pad.leftX;result.rawAxes[1]=downward(pad.leftY);
    result.rawAxes[2]=pad.rightX;result.rawAxes[3]=downward(pad.rightY);
    result.rawAxes[4]=static_cast<std::int16_t>((static_cast<int>(pad.leftTrigger)*32767+254)/255);
    result.rawAxes[5]=static_cast<std::int16_t>((static_cast<int>(pad.rightTrigger)*32767+254)/255);
    return result;
}
std::uint8_t NativeBindingPressure(const NativeInputBinding& binding,const NativePadSample& raw) noexcept
{
    if(binding.index<0)return 0;
    const auto index=static_cast<std::size_t>(binding.index);
    if(binding.kind==NativeBindingKind::Button)
        return index<raw.rawButtons.size() && binding.index<raw.buttonCount && raw.rawButtons[index]?255:0;
    if(binding.kind==NativeBindingKind::Hat)
        return index<raw.hats.size() && binding.index<raw.hatCount && (raw.hats[index]&binding.direction)!=0?255:0;
    if(index>=raw.rawAxes.size() || binding.index>=raw.axisCount)return 0;
    const int value=raw.rawAxes[index];
    switch(binding.kind) {
        case NativeBindingKind::AxisPositive:return static_cast<std::uint8_t>(std::max(0,value)*255/32767);
        case NativeBindingKind::AxisNegative:return static_cast<std::uint8_t>(std::max(0,-value)*255/32768);
        case NativeBindingKind::AxisFullPositive:return static_cast<std::uint8_t>((value+32768)*255/65535);
        case NativeBindingKind::AxisFullNegative:return static_cast<std::uint8_t>((32767-value)*255/65535);
        default:return 0;
    }
}
}

bool DetectNativeBinding(const NativePadSample& current,const NativePadSample& baseline,
    NativeInputBinding& binding,bool axisOnly) noexcept
{
    if(!current.connected || !baseline.connected)return false;
    if(current.instanceId>=0 && baseline.instanceId>=0 && current.instanceId!=baseline.instanceId)return false;
    const auto raw=CanonicalRawSample(current),before=CanonicalRawSample(baseline);
    if(!axisOnly) {
        for(int i=0;i<std::min(raw.buttonCount,64);++i)if(raw.rawButtons[static_cast<std::size_t>(i)] &&
            !before.rawButtons[static_cast<std::size_t>(i)]) {
            binding={NativeBindingKind::Button,i,0};return true;
        }
        for(int i=0;i<std::min(raw.hatCount,4);++i)for(const int direction:{1,2,4,8})
            if((raw.hats[static_cast<std::size_t>(i)]&direction)!=0 &&
                (before.hats[static_cast<std::size_t>(i)]&direction)==0) {
                binding={NativeBindingKind::Hat,i,direction};return true;
            }
    }
    int selected=-1,largest=16000;
    for(int i=0;i<std::min(raw.axisCount,16);++i) {
        const auto index=static_cast<std::size_t>(i);
        const int movement=std::abs(static_cast<int>(raw.rawAxes[index])-before.rawAxes[index]);
        if(movement>largest) {largest=movement;selected=i;}
    }
    if(selected<0)return false;
    const auto index=static_cast<std::size_t>(selected);
    auto kind=raw.rawAxes[index]>before.rawAxes[index]?NativeBindingKind::AxisPositive:NativeBindingKind::AxisNegative;
    if(!raw.standardized && before.rawAxes[index]<-24576)kind=NativeBindingKind::AxisFullPositive;
    else if(!raw.standardized && before.rawAxes[index]>24576)kind=NativeBindingKind::AxisFullNegative;
    binding={kind,selected,0};return true;
}

std::string NativeBindingName(const NativeInputBinding& binding,bool standardized)
{
    constexpr const char* buttons[]={"A / Cross","B / Circle","X / Square","Y / Triangle","Back / Share",
        "Guide","Start / Options","Left stick","Right stick","Left shoulder","Right shoulder",
        "D-pad up","D-pad down","D-pad left","D-pad right","Miscellaneous","Paddle 1","Paddle 2",
        "Paddle 3","Paddle 4","Touchpad"};
    constexpr const char* axes[]={"Left stick X","Left stick Y","Right stick X","Right stick Y","Left trigger","Right trigger"};
    if(binding.kind==NativeBindingKind::None)return "Unbound";
    if(binding.kind==NativeBindingKind::Button) {
        if(standardized && binding.index>=0 && binding.index<21)return buttons[binding.index];
        return "Button "+std::to_string(binding.index+1);
    }
    if(binding.kind==NativeBindingKind::Hat) {
        const char* direction=binding.direction==1?"Up":binding.direction==2?"Right":binding.direction==4?"Down":"Left";
        return "Hat "+std::to_string(binding.index+1)+" "+direction;
    }
    std::string name=standardized && binding.index>=0 && binding.index<6?axes[binding.index]:
        "Axis "+std::to_string(binding.index+1);
    switch(binding.kind) {
        case NativeBindingKind::AxisNegative:name+=" -";break;
        case NativeBindingKind::AxisFullPositive:name+=" (full +)";break;
        case NativeBindingKind::AxisFullNegative:name+=" (full -)";break;
        default:name+=" +";break;
    }
    return name;
}
const char* NativeInputActionName(std::size_t action) noexcept
{
    constexpr const char* names[]={"Reset car / select","Left stick","Nitro / right stick","Pause",
        "Menu up / HUD","Menu right","Menu down","Menu left","Shift down / special","Shift up / special",
        "Look back","Handbrake","Camera / back","Horn","Accelerate / confirm","Brake",
        "Steer left","Steer right","Confirm","Back"};
    return action<20?names[action]:"Unknown action";
}

void NativeCheatTextInput::Cancel() noexcept
{
    text_.fill(0);length_=0;capturing_=false;
}
NativeCheatTextResult NativeCheatTextInput::Character(std::uint32_t character,
    bool alt,bool control,bool focused,bool enabled,std::array<char,6>& command) noexcept
{
    if(!alt || control || !focused || !enabled) {
        Cancel();return NativeCheatTextResult::Unhandled;
    }
    const bool wasCapturing=capturing_;
    if(character<32 || character>126) {
        Cancel();return wasCapturing ? NativeCheatTextResult::Cancelled:NativeCheatTextResult::Unhandled;
    }
    const char value=character>='a' && character<='z' ?
        static_cast<char>(character-'a'+'A'):static_cast<char>(character);
    constexpr char prefix[]="HP2C";
    const bool matches=length_<4 ? value==prefix[length_]:value>='0' && value<='9';
    if(!matches) {
        Cancel();
        if(value=='H') {
            text_[0]='H';length_=1;capturing_=true;return NativeCheatTextResult::Pending;
        }
        return wasCapturing ? NativeCheatTextResult::Cancelled:NativeCheatTextResult::Unhandled;
    }
    text_[length_++]=value;capturing_=true;
    if(length_!=text_.size())return NativeCheatTextResult::Pending;
    command=text_;text_.fill(0);length_=0;
    // Keep keyboard capture until Alt is released. A still-held H/C key
    // must not become an unrelated menu press after the final digit.
    return NativeCheatTextResult::Complete;
}
bool NativeCheatTextInput::FilterKeyboard(std::array<bool,256>& keys,
    bool alt,bool control,bool focused,bool enabled) noexcept
{
    if(!alt || control || !focused || !enabled) {Cancel();return false;}
    if(!capturing_)return false;
    keys.fill(false);return true;
}

NativePadSample ConvertSdl2Gamepad(const Sdl2GamepadSnapshot& sample) noexcept
{
    NativePadSample output{};
    if(!sample.connected)return output;
    output.connected=true;
    // SDL2 GameController button indices (A,B,X,Y,Back,Guide,Start,L3,R3,
    // LB,RB,Up,Down,Left,Right) -> public XInput bit layout used by the
    // native logical button adapter. Game logic never depends on SDL ABI.
    constexpr std::uint16_t buttonBits[15]={
        0x1000,0x2000,0x4000,0x8000,0x0020,0,0x0010,0x0040,0x0080,
        0x0100,0x0200,0x0001,0x0002,0x0004,0x0008
    };
    for(std::size_t button=0;button<sample.buttons.size();++button)
        if(sample.buttons[button])output.buttons|=buttonBits[button];
    output.leftX=sample.axes[0];
    output.rightX=sample.axes[2];
    // SDL sticks are positive down, XInput and original scanner positive up.
    const auto upward=[](std::int16_t axis) noexcept {
        return static_cast<std::int16_t>(std::clamp(-static_cast<int>(axis),-32768,32767));
    };
    output.leftY=upward(sample.axes[1]);
    output.rightY=upward(sample.axes[3]);
    const auto trigger=[](std::int16_t axis) noexcept {
        return static_cast<std::uint8_t>(std::clamp(static_cast<int>(axis),0,32767)*255/32767);
    };
    output.leftTrigger=trigger(sample.axes[4]);
    output.rightTrigger=trigger(sample.axes[5]);
    output.rawValid=true;output.standardized=true;output.axisCount=6;output.buttonCount=15;
    std::copy(sample.axes.begin(),sample.axes.end(),output.rawAxes.begin());
    std::copy(sample.buttons.begin(),sample.buttons.end(),output.rawButtons.begin());
    return output;
}

void AssignNativeKeyboardAndGamepad(NativeInputSettings& settings,std::int32_t gamepad)
{
    if(gamepad<0 || gamepad>255)throw std::invalid_argument("Invalid controller selection for player two");
    auto assigned=settings;
    const auto key=settings.controller[0]==gamepad?settings.deviceKey[0]:
        settings.controller[1]==gamepad?settings.deviceKey[1]:std::string{};
    assigned.controller={{-1,gamepad}};
    assigned.deviceMode={{NativeDeviceMode::Keyboard,NativeDeviceMode::Gamepad}};
    assigned.deviceKey[0].clear();assigned.deviceKey[1]=key;
    ValidateNativeInputSettings(assigned);settings=assigned;
}

void AssignNativePlayerOneGamepad(NativeInputSettings& settings,std::int32_t gamepad)
{
    if(gamepad<0 || gamepad>255)throw std::invalid_argument("Invalid controller selection for player one");
    auto assigned=settings;
    if(assigned.controller[0]!=gamepad)assigned.deviceKey[0].clear();
    if(assigned.controller[1]==gamepad) {
        assigned.deviceKey[0]=assigned.deviceKey[1];assigned.deviceKey[1].clear();
        assigned.controller[1]=-1;assigned.deviceMode[1]=NativeDeviceMode::Disabled;
    }
    assigned.controller[0]=gamepad;assigned.deviceMode[0]=NativeDeviceMode::Gamepad;
    ValidateNativeInputSettings(assigned);settings=assigned;
}

ControllerState NativeInputState(const NativeInputSettings& settings,std::int32_t port,
    const NativePadSample& pad,const std::array<bool,256>& keys,bool focused)
{
    if(port<0 || port>=2)throw std::out_of_range("Native input player port");
    const auto player=static_cast<std::size_t>(port);
    const bool connected=NativeGamepadEnabled(settings,port) && pad.connected;
    const bool keyboard=NativeKeyboardEnabled(settings,port);
    ControllerState state;state.connected=connected || keyboard;
    if(!focused)return state;
    const auto& profile=settings.profiles[player];
    if(connected) {
        const auto raw=CanonicalRawSample(pad);
        const int threshold=profile.triggerThreshold<0?settings.triggerThreshold:profile.triggerThreshold;
        for(std::size_t i=0;i<profile.buttons.size();++i) {
            const auto& binding=profile.buttons[i];
            int pressure=NativeBindingPressure(binding,raw);
            const bool analog=binding.kind==NativeBindingKind::AxisPositive || binding.kind==NativeBindingKind::AxisNegative ||
                binding.kind==NativeBindingKind::AxisFullPositive || binding.kind==NativeBindingKind::AxisFullNegative;
            if(analog && pressure<=threshold)pressure=0;
            SetButton(state,static_cast<Ps2Button>(i),pressure>0,static_cast<std::uint8_t>(pressure));
        }
        for(std::size_t i=0;i<4;++i) {
            const auto source=settings.swapSticks?(i+2)%4:i;
            const auto axis=profile.axes[source];
            if(axis<0 || axis>=raw.axisCount || axis>=static_cast<int>(raw.rawAxes.size()))continue;
            int value=raw.rawAxes[static_cast<std::size_t>(axis)];
            const bool inverted=profile.invert[source] ^ (i==2 && settings.invertSteering) ^
                (i==1 && settings.invertLookY);
            if(inverted)value=std::clamp(-value,-32768,32767);
            const int zone=profile.deadzone[i]<0?(i<2?settings.rightDeadzone:settings.leftDeadzone):profile.deadzone[i];
            const int maximum=profile.saturation[i];
            // Preserve the configured deadzone response even when the
            // launcher makes an inherited value explicit on Save.
            if(value>-zone && value<zone)value=0;
            if(maximum!=32767) {
                const int magnitude=std::min(std::abs(value),maximum);
                const int scaled=magnitude*(value<0?32768:32767)/maximum;
                value=value<0?-scaled:scaled;
            }
            state.sticks[i]=NormalizeAxis(static_cast<std::int16_t>(value));
        }
    }
    if(keyboard) {
        const auto& bindings=NativeKeyboardBindings(settings,port);
        const auto held=[&](std::size_t index){const auto key=bindings[index];return key<keys.size() && key && keys[key];};
        for(unsigned i=0;i<16;++i)if(held(i))SetButton(state,static_cast<Ps2Button>(i),true);
        if(held(18))SetButton(state,Ps2Button::Cross,true);
        if(held(19))SetButton(state,Ps2Button::Triangle,true);
        const bool left=held(16),right=held(17);
        const bool inverted=settings.invertSteering ^ profile.invert[settings.swapSticks?0:2];
        if(left!=right)state.sticks[2]=(left!=inverted)?0:255;
    }
    return state;
}

namespace {
// Authored pressure-channel order: Right, Left, Up, Down, Triangle, Circle, Cross,
// Square, L1, R1, L2, R2. -1 means the button has no pressure data.
constexpr std::int8_t kPressure[16] = {
    -1,-1,-1,-1,2,0,3,1,10,11,8,9,4,5,6,7
};
}
void SetButton(ControllerState& state, Ps2Button button, bool pressed,
               std::uint8_t pressure) noexcept {
    const auto bit = static_cast<unsigned>(button);
    if (pressed) state.held |= static_cast<std::uint16_t>(1u << bit);
    else state.held &= static_cast<std::uint16_t>(~(1u << bit));
    if (kPressure[bit] >= 0)
        state.pressures[static_cast<size_t>(kPressure[bit])] = pressed ? pressure : 0;
}
std::uint8_t NormalizeAxis(std::int16_t s) noexcept {
    // Truncate with endpoints preserved, centre 128. SDL axes have -32768..32767.
    return s >= 0 ? static_cast<std::uint8_t>(128 + (static_cast<std::int32_t>(s) * 127) / 32767)
                  : static_cast<std::uint8_t>(128 + (static_cast<std::int32_t>(s) * 128) / 32768);
}
JoystickControls NativeScannerControls(const ControllerState& state) noexcept {
    JoystickControls controls{};
    if(!state.connected) {
        controls.mappingProfile=JOYSTICK_PROFILE_DISCONNECTED;
        std::fill_n(controls.sticks,4,std::uint8_t{255});
        std::fill_n(controls.pressures,12,std::uint8_t{255});
        return controls;
    }
    controls.held=state.held;
    std::copy(state.sticks.begin(),state.sticks.end(),controls.sticks);
    std::copy(state.pressures.begin(),state.pressures.end(),controls.pressures);
    controls.mappingProfile=JOYSTICK_PROFILE_GAMEPAD;
    return controls;
}
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define SDL_MAIN_HANDLED
#include "third_party/SDL2/include/SDL.h"
#include <filesystem>
#include <exception>
#include <set>
#include <string>

namespace {
// Function pointer types come from the shipped, unmodified SDL2 headers.
// The native EXE owns WinMain, the window and focus policy.
struct Sdl2Api {
    HMODULE module{};
    bool initialized{};
    std::string error;
    std::string connectionPrefix;
    std::set<std::string> ambiguousIdentities;
    decltype(&SDL_InitSubSystem) init{};
    decltype(&SDL_QuitSubSystem) quit{};
    decltype(&SDL_SetMainReady) mainReady{};
    decltype(&SDL_GetVersion) version{};
    decltype(&SDL_GetError) getError{};
    decltype(&SDL_SetHint) hint{};
    decltype(&SDL_NumJoysticks) count{};
    decltype(&SDL_JoystickGetDeviceInstanceID) deviceId{};
    decltype(&SDL_IsGameController) isController{};
    decltype(&SDL_GameControllerOpen) openController{};
    decltype(&SDL_GameControllerClose) closeController{};
    decltype(&SDL_GameControllerGetJoystick) controllerJoystick{};
    decltype(&SDL_GameControllerName) controllerName{};
    decltype(&SDL_GameControllerGetAttached) controllerAttached{};
    decltype(&SDL_GameControllerGetButton) controllerButton{};
    decltype(&SDL_GameControllerGetAxis) controllerAxis{};
    decltype(&SDL_GameControllerRumble) controllerRumble{};
    decltype(&SDL_JoystickOpen) openJoystick{};
    decltype(&SDL_JoystickClose) closeJoystick{};
    decltype(&SDL_JoystickName) joystickName{};
    decltype(&SDL_JoystickGetAttached) joystickAttached{};
    decltype(&SDL_JoystickGetGUID) joystickGuid{};
    decltype(&SDL_JoystickGetGUIDString) guidString{};
    decltype(&SDL_JoystickGetSerial) joystickSerial{};
    decltype(&SDL_JoystickPath) joystickPath{};
    decltype(&SDL_JoystickGetButton) joystickButton{};
    decltype(&SDL_JoystickGetAxis) joystickAxis{};
    decltype(&SDL_JoystickGetHat) joystickHat{};
    decltype(&SDL_JoystickNumAxes) joystickAxes{};
    decltype(&SDL_JoystickNumButtons) joystickButtons{};
    decltype(&SDL_JoystickNumHats) joystickHats{};
    decltype(&SDL_JoystickRumble) joystickRumble{};
    decltype(&SDL_JoystickUpdate) update{};
    decltype(&SDL_PumpEvents) pump{};
    decltype(&SDL_PeepEvents) peep{};
    decltype(&SDL_GetTicks64) ticks{};
    decltype(&SDL_RWFromFile) rwFromFile{};
    decltype(&SDL_GameControllerAddMappingsFromRW) mappings{};

    template<class T> bool Resolve(const char* name,T& function) noexcept {
        const auto pointer=GetProcAddress(module,name);
        static_assert(sizeof(pointer)==sizeof(function),"Windows function pointer representation");
        std::memcpy(&function,&pointer,sizeof(function));
        return function!=nullptr;
    }
    Sdl2Api() {
        FILETIME epoch{};GetSystemTimeAsFileTime(&epoch);
        const auto stamp=(static_cast<std::uint64_t>(epoch.dwHighDateTime)<<32)|epoch.dwLowDateTime;
        connectionPrefix=std::to_string(stamp)+"/"+std::to_string(GetCurrentProcessId());
        std::wstring executable(1024,L'\0');
        for(;;) {
            const DWORD length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
            if(!length) {error="Cannot locate the executable directory for SDL2";return;}
            if(length<executable.size()) {executable.resize(length);break;}
            if(executable.size()>=32768) {error="Executable path exceeds the Windows path limit";return;}
            executable.resize(executable.size()*2);
        }
        const auto folder=std::filesystem::path(executable).parent_path();
        const auto library=folder/L"SDL2.dll";
        module=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module) {
            error="SDL2.dll is missing or could not load (Windows error "+std::to_string(GetLastError())+
                "). Build the complete solution so its SDL2 runtime is copied beside NFSHP2.exe.";
            return;
        }
#define HP2_SDL2_RESOLVE(name,field) if(!Resolve(name,field)){error=std::string("SDL2.dll is missing ")+name;return;}
        HP2_SDL2_RESOLVE("SDL_InitSubSystem",init)
        HP2_SDL2_RESOLVE("SDL_QuitSubSystem",quit)
        HP2_SDL2_RESOLVE("SDL_SetMainReady",mainReady)
        HP2_SDL2_RESOLVE("SDL_GetVersion",version)
        HP2_SDL2_RESOLVE("SDL_GetError",getError)
        HP2_SDL2_RESOLVE("SDL_SetHint",hint)
        HP2_SDL2_RESOLVE("SDL_NumJoysticks",count)
        HP2_SDL2_RESOLVE("SDL_JoystickGetDeviceInstanceID",deviceId)
        HP2_SDL2_RESOLVE("SDL_IsGameController",isController)
        HP2_SDL2_RESOLVE("SDL_GameControllerOpen",openController)
        HP2_SDL2_RESOLVE("SDL_GameControllerClose",closeController)
        HP2_SDL2_RESOLVE("SDL_GameControllerGetJoystick",controllerJoystick)
        HP2_SDL2_RESOLVE("SDL_GameControllerName",controllerName)
        HP2_SDL2_RESOLVE("SDL_GameControllerGetAttached",controllerAttached)
        HP2_SDL2_RESOLVE("SDL_GameControllerGetButton",controllerButton)
        HP2_SDL2_RESOLVE("SDL_GameControllerGetAxis",controllerAxis)
        HP2_SDL2_RESOLVE("SDL_GameControllerRumble",controllerRumble)
        HP2_SDL2_RESOLVE("SDL_JoystickOpen",openJoystick)
        HP2_SDL2_RESOLVE("SDL_JoystickClose",closeJoystick)
        HP2_SDL2_RESOLVE("SDL_JoystickName",joystickName)
        HP2_SDL2_RESOLVE("SDL_JoystickGetAttached",joystickAttached)
        HP2_SDL2_RESOLVE("SDL_JoystickGetGUID",joystickGuid)
        HP2_SDL2_RESOLVE("SDL_JoystickGetGUIDString",guidString)
        HP2_SDL2_RESOLVE("SDL_JoystickGetSerial",joystickSerial)
        HP2_SDL2_RESOLVE("SDL_JoystickPath",joystickPath)
        HP2_SDL2_RESOLVE("SDL_JoystickGetButton",joystickButton)
        HP2_SDL2_RESOLVE("SDL_JoystickGetAxis",joystickAxis)
        HP2_SDL2_RESOLVE("SDL_JoystickGetHat",joystickHat)
        HP2_SDL2_RESOLVE("SDL_JoystickNumAxes",joystickAxes)
        HP2_SDL2_RESOLVE("SDL_JoystickNumButtons",joystickButtons)
        HP2_SDL2_RESOLVE("SDL_JoystickNumHats",joystickHats)
        HP2_SDL2_RESOLVE("SDL_JoystickRumble",joystickRumble)
        HP2_SDL2_RESOLVE("SDL_JoystickUpdate",update)
        HP2_SDL2_RESOLVE("SDL_PumpEvents",pump)
        HP2_SDL2_RESOLVE("SDL_PeepEvents",peep)
        HP2_SDL2_RESOLVE("SDL_GetTicks64",ticks)
        HP2_SDL2_RESOLVE("SDL_RWFromFile",rwFromFile)
        HP2_SDL2_RESOLVE("SDL_GameControllerAddMappingsFromRW",mappings)
#undef HP2_SDL2_RESOLVE
        SDL_version reported{};version(&reported);
        if(reported.major!=2 || SDL_VERSIONNUM(reported.major,reported.minor,reported.patch)<SDL_VERSIONNUM(2,24,0)) {
            error="SDL2 2.24 or newer is required for persistent device identification";return;
        }
        mainReady();
        // SDL does not own our Win32 window. NativeInputState explicitly gates
        // gameplay on that window's focus, including both keyboard profiles.
        (void)hint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
        if(init(SDL_INIT_GAMECONTROLLER|SDL_INIT_JOYSTICK|SDL_INIT_EVENTS)!=0) {
            error=std::string("SDL2 controller initialization failed: ")+getError();return;
        }
        initialized=true;
        const auto database=folder/L"gamecontrollerdb.txt";
        std::error_code databaseError;
        if(std::filesystem::is_regular_file(database,databaseError)) {
            const auto utf8=database.u8string();
            if(auto* stream=rwFromFile(utf8.c_str(),"rb"))(void)mappings(stream,1);
        }
    }
    ~Sdl2Api() {
        if(initialized)quit(SDL_INIT_GAMECONTROLLER|SDL_INIT_JOYSTICK|SDL_INIT_EVENTS);
        if(module)FreeLibrary(module);
    }
    Sdl2Api(const Sdl2Api&)=delete;
    Sdl2Api& operator=(const Sdl2Api&)=delete;
};
Sdl2Api& ProcessInputApi()
{
    // Keep SDL's instance identities alive from launcher discovery through
    // game startup; several catalogs share one native subsystem lifetime.
    static Sdl2Api api;
    return api;
}
std::string EncodeInputIdentity(const char* value)
{
    constexpr char hex[]="0123456789abcdef";
    std::string output;
    if(value)for(const auto* at=reinterpret_cast<const unsigned char*>(value);*at;++at) {
        output.push_back(hex[*at>>4]);output.push_back(hex[*at&15]);
    }
    return output;
}
struct Sdl2Device {
    Sdl2Api& api;
    SDL_GameController* controller{};
    SDL_Joystick* joystick{};
    hp2::platform::NativeInputDeviceInfo info;
    hp2::platform::NativePadSample observed;
    std::string identityBase,pathIdentity,connectionIdentity;
    std::uint16_t rumbleLow{},rumbleHigh{};
    ULONGLONG lastRumble{};
    explicit Sdl2Device(Sdl2Api& value):api(value){}
    ~Sdl2Device() {
        if(controller) {(void)api.controllerRumble(controller,0,0,0);api.closeController(controller);}
        else if(joystick) {(void)api.joystickRumble(joystick,0,0,0);api.closeJoystick(joystick);}
    }
    bool Attached() const noexcept {
        return controller?api.controllerAttached(controller)==SDL_TRUE:
            joystick && api.joystickAttached(joystick)==SDL_TRUE;
    }
    static void Canonicalize(hp2::platform::NativePadSample& sample) noexcept {
        if(!sample.standardized)return;
        hp2::platform::Sdl2GamepadSnapshot standard;standard.connected=sample.connected;
        std::copy_n(sample.rawAxes.begin(),standard.axes.size(),standard.axes.begin());
        std::copy_n(sample.rawButtons.begin(),standard.buttons.size(),standard.buttons.begin());
        const auto canonical=hp2::platform::ConvertSdl2Gamepad(standard);
        sample.buttons=canonical.buttons;sample.leftX=canonical.leftX;sample.leftY=canonical.leftY;
        sample.rightX=canonical.rightX;sample.rightY=canonical.rightY;
        sample.leftTrigger=canonical.leftTrigger;sample.rightTrigger=canonical.rightTrigger;
    }
    hp2::platform::NativePadSample Snapshot() const {
        hp2::platform::NativePadSample result;
        if(!Attached())return result;
        result.connected=true;result.rawValid=true;result.standardized=info.standardized;
        result.instanceId=info.instanceId;
        result.axisCount=info.axes;result.buttonCount=info.buttons;result.hatCount=info.hats;
        if(controller) {
            for(int i=0;i<result.axisCount;++i)result.rawAxes[static_cast<std::size_t>(i)]=
                api.controllerAxis(controller,static_cast<SDL_GameControllerAxis>(i));
            for(int i=0;i<result.buttonCount;++i)result.rawButtons[static_cast<std::size_t>(i)]=
                api.controllerButton(controller,static_cast<SDL_GameControllerButton>(i));
            Canonicalize(result);
        } else {
            for(int i=0;i<result.axisCount;++i)result.rawAxes[static_cast<std::size_t>(i)]=api.joystickAxis(joystick,i);
            for(int i=0;i<result.buttonCount;++i)result.rawButtons[static_cast<std::size_t>(i)]=api.joystickButton(joystick,i);
            for(int i=0;i<result.hatCount;++i)result.hats[static_cast<std::size_t>(i)]=api.joystickHat(joystick,i);
        }
        return result;
    }
    void SetRumble(std::uint16_t low,std::uint16_t high,ULONGLONG now) noexcept {
        rumbleLow=low;rumbleHigh=high;lastRumble=now;
        const int result=controller?api.controllerRumble(controller,low,high,250):
            api.joystickRumble(joystick,low,high,250);
        if(result<0) {rumbleLow=0;rumbleHigh=0;}
    }
    void RefreshRumble(ULONGLONG now) noexcept {
        // The original owner publishes only changes in motor strength. Keep
        // a held request alive until its next change, while a stalled or
        // unfocused application cannot leave a motor running indefinitely.
        if((rumbleLow || rumbleHigh) && now-lastRumble>=100)SetRumble(rumbleLow,rumbleHigh,now);
    }
};
}

namespace hp2 {
struct WindowsInputCatalog::Storage {
    Sdl2Api& api{ProcessInputApi()};
    std::vector<std::unique_ptr<Sdl2Device>> devices;
    std::vector<SDL_JoystickID> ids;
    platform::NativeInputTimestamp timestamp{platform::NativeInputTimeline::Clock::now(),api.initialized?api.ticks():0};
    std::vector<platform::NativePadObservation> pending;
    bool captureObservations{};
    ULONGLONG lastUpdate{~ULONGLONG{0}};
    void Observe(platform::NativeInputTimeline::Clock::time_point time,Sdl2Device& device,
        const platform::NativePadSample& sample) {
        device.observed=sample;
        if(captureObservations)pending.push_back({time,device.info.instanceId,sample});
    }
    void Event(const SDL_Event& event,std::uint64_t observedTicks) {
        SDL_JoystickID instance=-1;
        switch(event.type) {
        case SDL_CONTROLLERAXISMOTION:instance=event.caxis.which;break;
        case SDL_CONTROLLERBUTTONDOWN:case SDL_CONTROLLERBUTTONUP:instance=event.cbutton.which;break;
        case SDL_JOYAXISMOTION:instance=event.jaxis.which;break;
        case SDL_JOYBUTTONDOWN:case SDL_JOYBUTTONUP:instance=event.jbutton.which;break;
        case SDL_JOYHATMOTION:instance=event.jhat.which;break;
        case SDL_CONTROLLERDEVICEREMOVED:instance=event.cdevice.which;break;
        case SDL_JOYDEVICEREMOVED:instance=event.jdevice.which;break;
        default:return;
        }
        const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& device){return device->info.instanceId==instance;});
        if(found==devices.end() || !(*found)->observed.connected)return;
        auto& device=**found;auto sample=device.observed;
        if(event.type==SDL_JOYDEVICEREMOVED || event.type==SDL_CONTROLLERDEVICEREMOVED)sample={};
        else if(sample.standardized) {
            if(event.type==SDL_CONTROLLERAXISMOTION && event.caxis.axis<sample.axisCount)
                sample.rawAxes[event.caxis.axis]=event.caxis.value;
            else if((event.type==SDL_CONTROLLERBUTTONDOWN || event.type==SDL_CONTROLLERBUTTONUP) && event.cbutton.button<sample.buttonCount)
                sample.rawButtons[event.cbutton.button]=event.cbutton.state;
            else return; // The raw joystick event duplicates this mapped device.
            Sdl2Device::Canonicalize(sample);
        } else {
            if(event.type==SDL_JOYAXISMOTION && event.jaxis.axis<sample.axisCount)sample.rawAxes[event.jaxis.axis]=event.jaxis.value;
            else if((event.type==SDL_JOYBUTTONDOWN || event.type==SDL_JOYBUTTONUP) && event.jbutton.button<sample.buttonCount)
                sample.rawButtons[event.jbutton.button]=event.jbutton.state;
            else if(event.type==SDL_JOYHATMOTION && event.jhat.hat<sample.hatCount)sample.hats[event.jhat.hat]=event.jhat.value;
            else return;
        }
        Observe(timestamp.From32(event.common.timestamp,observedTicks),device,sample);
    }
    void DrainEvents() {
        const auto observedTicks=api.ticks();std::array<SDL_Event,32> events{};
        for(;;) {
            const int received=api.peep(events.data(),static_cast<int>(events.size()),
                SDL_GETEVENT,SDL_JOYAXISMOTION,SDL_CONTROLLERSTEAMHANDLEUPDATED);
            if(received<0)throw std::runtime_error(std::string("SDL2 input event acquisition failed: ")+api.getError());
            if(!received)break;
            for(int event=0;event<received;++event)Event(events[static_cast<std::size_t>(event)],observedTicks);
        }
    }
    void Refresh() {
        if(!api.initialized)return;
        const auto now=GetTickCount64();
        if(now==lastUpdate)return;
        lastUpdate=now;
        api.pump();api.update();
        const auto observationTime=platform::NativeInputTimeline::Clock::now();
        // Keep every delivered input transition with SDL's own timestamp.
        // PumpEvents stays on the native window thread. A driver which only
        // observes current hardware state at Pump cannot provide older edges.
        DrainEvents();
        const int count=std::max(0,api.count());
        ids.clear();ids.reserve(static_cast<std::size_t>(count));
        for(int index=0;index<count;++index)ids.push_back(api.deviceId(index));
        devices.erase(std::remove_if(devices.begin(),devices.end(),[&](const auto& device) {
            const bool removed=!device->Attached() || std::find(ids.begin(),ids.end(),device->info.instanceId)==ids.end();
            if(removed && device->observed.connected)Observe(observationTime,*device,{});
            return removed;
        }),devices.end());
        for(int index=0;index<count;++index) {
            const auto instance=ids[static_cast<std::size_t>(index)];
            if(instance<0)continue;
            const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& device){return device->info.instanceId==instance;});
            if(found!=devices.end()) {(*found)->info.index=index;continue;}
            auto device=std::make_unique<Sdl2Device>(api);
            if(api.isController(index)==SDL_TRUE)device->controller=api.openController(index);
            if(device->controller)device->joystick=api.controllerJoystick(device->controller);
            else device->joystick=api.openJoystick(index);
            if(!device->joystick)continue;
            auto& info=device->info;info.index=index;info.instanceId=instance;
            info.standardized=device->controller!=nullptr;
            const char* name=device->controller?api.controllerName(device->controller):api.joystickName(device->joystick);
            info.name=name && *name?name:"Unnamed controller";
            char guid[33]{};api.guidString(api.joystickGuid(device->joystick),guid,static_cast<int>(sizeof(guid)));
            info.guid=guid;
            const char* serial=api.joystickSerial(device->joystick);
            const char* path=api.joystickPath(device->joystick);
            const auto serialIdentity=serial && *serial?info.guid+"/serial/"+EncodeInputIdentity(serial):std::string{};
            // XInput#N is an assignable slot, not a physical device path.
            if(path && *path && std::strncmp(path,"XInput#",7)!=0)
                device->pathIdentity=(serialIdentity.empty()?info.guid:serialIdentity)+"/path/"+EncodeInputIdentity(path);
            device->connectionIdentity=info.guid+"/connection/"+api.connectionPrefix+"/"+std::to_string(instance);
            device->identityBase=!serialIdentity.empty()?serialIdentity:
                !device->pathIdentity.empty()?device->pathIdentity:device->connectionIdentity;
            info.id=device->identityBase;
            info.axes=info.standardized?SDL_CONTROLLER_AXIS_MAX:std::clamp(api.joystickAxes(device->joystick),0,16);
            info.buttons=info.standardized?SDL_CONTROLLER_BUTTON_MAX:std::clamp(api.joystickButtons(device->joystick),0,64);
            info.hats=info.standardized?0:std::clamp(api.joystickHats(device->joystick),0,4);
            const auto initial=device->Snapshot();Observe(platform::NativeInputTimeline::Clock::now(),*device,initial);
            devices.push_back(std::move(device));
        }
        // Opening a controller can itself enqueue its initial axis/button
        // state. Drain it in this acquisition, then reconcile any mapping or
        // driver update which had no delta event, at its actual observation.
        DrainEvents();
        for(const auto& device:devices) {
            const auto sample=device->Snapshot();const auto& before=device->observed;
            if(sample.connected!=before.connected || sample.instanceId!=before.instanceId ||
                sample.rawAxes!=before.rawAxes || sample.rawButtons!=before.rawButtons || sample.hats!=before.hats ||
                sample.axisCount!=before.axisCount || sample.buttonCount!=before.buttonCount || sample.hatCount!=before.hatCount)
                Observe(platform::NativeInputTimeline::Clock::now(),*device,sample);
        }
        // Some HID firmware repeats the same factory serial. Remember each
        // observed ambiguity for the process lifetime, including the launcher
        // handoff, and disambiguate by physical path or live connection.
        for(std::size_t i=0;i<devices.size();++i)for(std::size_t j=i+1;j<devices.size();++j) {
            const auto& left=*devices[i];const auto& right=*devices[j];
            if(left.identityBase==right.identityBase)api.ambiguousIdentities.insert(left.identityBase);
            if(!left.pathIdentity.empty() && left.pathIdentity==right.pathIdentity)
                api.ambiguousIdentities.insert(left.pathIdentity);
        }
        for(const auto& device:devices) {
            const bool physicalPath=!device->pathIdentity.empty() && api.ambiguousIdentities.count(device->pathIdentity)==0;
            device->info.pathKey=physicalPath?device->pathIdentity:std::string{};
            device->info.id=api.ambiguousIdentities.count(device->identityBase)==0?device->identityBase:
                physicalPath?device->pathIdentity:device->connectionIdentity;
        }
        std::sort(devices.begin(),devices.end(),[](const auto& left,const auto& right){return left->info.index<right->info.index;});
        for(const auto& device:devices)device->RefreshRumble(now);
    }
    Sdl2Device* Find(const std::string& key,int fallback,bool refresh) {
        if(refresh)Refresh();Sdl2Device* found=nullptr;
        for(const auto& device:devices) {
            const bool match=key.empty()?device->info.index==fallback:device->info.id==key ||
                (!device->info.pathKey.empty() && device->info.pathKey==key);
            if(match) {
                if(found)return nullptr;
                found=device.get();
            }
        }
        return found;
    }
};
WindowsInputCatalog::WindowsInputCatalog():storage_(std::make_unique<Storage>()){}
WindowsInputCatalog::~WindowsInputCatalog()=default;
bool WindowsInputCatalog::Available() const noexcept{return storage_->api.initialized;}
const std::string& WindowsInputCatalog::Error() const noexcept{return storage_->api.error;}
std::vector<platform::NativeInputDeviceInfo> WindowsInputCatalog::Enumerate(bool refresh)
{
    if(refresh)storage_->Refresh();std::vector<platform::NativeInputDeviceInfo> result;result.reserve(storage_->devices.size());
    for(const auto& device:storage_->devices)result.push_back(device->info);
    return result;
}
platform::NativePadSample WindowsInputCatalog::Read(const std::string& key,std::int32_t fallbackIndex,bool refresh)
{
    auto* device=storage_->Find(key,fallbackIndex,refresh);
    return device?device->observed:platform::NativePadSample{};
}
void WindowsInputCatalog::PollObservations(std::vector<platform::NativePadObservation>& output)
{
    storage_->captureObservations=true;storage_->Refresh();
    output.clear();output.swap(storage_->pending);
}
void WindowsInputCatalog::Rumble(const std::string& key,std::int32_t fallbackIndex,std::uint16_t low,std::uint16_t high,bool refresh)
{
    auto* device=storage_->Find(key,fallbackIndex,refresh);if(!device || !device->Attached())return;
    device->SetRumble(low,high,GetTickCount64());
}

void WindowsInputCatalog::StopRumble() noexcept
{
    // No discovery or allocation on focus loss and destruction.
    const auto now=GetTickCount64();
    for(const auto& device:storage_->devices)if(device->Attached())device->SetRumble(0,0,now);
}

struct WindowsInputDevice::Storage {
    HWND window{};
    WindowsInputCatalog catalog;
    platform::NativeInputSettings settings;
    platform::NativeCheatTextInput cheatText;
    platform::NativeInputTimestamp timestamp{platform::NativeInputTimeline::Clock::now(),GetTickCount64()};
    platform::NativeInputTimeline timeline;
    platform::NativeInputSnapshot prepared;
    std::array<bool,256> physicalKeys{};
    std::array<std::int32_t,2> selectedIds{{-1,-1}};
    std::array<std::string,2> selectedKeys;
    std::array<std::uint64_t,2> deliveredResetEpoch{};
    std::array<std::int32_t,2> journalIds{{-2,-2}};
    std::vector<platform::NativePadObservation> observations;
    std::exception_ptr failure;
    bool focused{},seedKeys{},preparedStep{};
    Storage(void* value,const platform::NativeInputSettings& preferences)
        :window(static_cast<HWND>(value)),settings(preferences)
    {
        platform::ValidateNativeInputSettings(settings);
        if(!window)throw std::invalid_argument("Windows input requires its actual game window");
        if(!catalog.Available() && (platform::NativeGamepadEnabled(settings,0) || platform::NativeGamepadEnabled(settings,1)))
            throw std::runtime_error(catalog.Error());
        platform::RecordSessionEvent("input.backends",catalog.Available()?"SDL2 2.32 native controllers and Win32 keyboards":"Win32 keyboards");
    }
    ~Storage() {catalog.StopRumble();}
    void Focus(platform::NativeInputTimeline::Clock::time_point time,bool value) {
        if(value==focused)return;
        focused=value;timeline.Focus(time,value);physicalKeys.fill(false);
        seedKeys=value;
        if(!value) {cheatText.Cancel();catalog.StopRumble();}
    }
    void Key(platform::NativeInputTimeline::Clock::time_point time,std::uint16_t key,bool down,bool delivered=false) {
        // A sent focus loss can be dispatched before older posted key messages.
        // The chronological consumer, not today's foreground state, gates them.
        if(key>=physicalKeys.size() || (!delivered && physicalKeys[key]==down))return;
        physicalKeys[key]=down;timeline.Keyboard(time,key,down);
    }
};
WindowsInputDevice::WindowsInputDevice(void* window,const platform::NativeInputSettings& settings)
    :storage_(std::make_unique<Storage>(window,settings)){}
WindowsInputDevice::~WindowsInputDevice()=default;
platform::NativeInputTimeline::Clock::time_point WindowsInputDevice::MessageTime(std::uint32_t milliseconds) const
{
    return storage_->timestamp.From32(milliseconds,GetTickCount64());
}
bool WindowsInputDevice::HandleWindowInput(std::uint32_t message,std::uintptr_t first,std::intptr_t second,
    platform::NativeInputTimeline::Clock::time_point time) noexcept
{
    auto& device=*storage_;
    try {
        if(device.failure)return false;
        if(message==WM_KILLFOCUS || (message==WM_ACTIVATEAPP && !first)) {
            device.Focus(time,false);return false;
        }
        if(message==WM_SETFOCUS)device.Focus(time,true);
        if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN || message==WM_KEYUP || message==WM_SYSKEYUP) {
            const bool down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;
            auto key=first;
            if(key==VK_SHIFT) {
                const auto side=MapVirtualKeyW(static_cast<UINT>((static_cast<std::uintptr_t>(second)>>16)&0xffu),MAPVK_VSC_TO_VK_EX);
                if(side)key=side;
            }
            if(key==VK_CONTROL)key=(static_cast<std::uintptr_t>(second)&(std::uintptr_t{1}<<24))?VK_RCONTROL:VK_LCONTROL;
            if(key==VK_MENU)key=(static_cast<std::uintptr_t>(second)&(std::uintptr_t{1}<<24))?VK_RMENU:VK_LMENU;
            if(key>0 && key<device.physicalKeys.size())device.Key(time,static_cast<std::uint16_t>(key),down,true);
            if(key==VK_LSHIFT || key==VK_RSHIFT)device.physicalKeys[VK_SHIFT]=device.physicalKeys[VK_LSHIFT] || device.physicalKeys[VK_RSHIFT];
            if(key==VK_LCONTROL || key==VK_RCONTROL)device.physicalKeys[VK_CONTROL]=device.physicalKeys[VK_LCONTROL] || device.physicalKeys[VK_RCONTROL];
            if(key==VK_LMENU || key==VK_RMENU)device.physicalKeys[VK_MENU]=device.physicalKeys[VK_LMENU] || device.physicalKeys[VK_RMENU];
        }
        if((message==WM_SYSKEYUP || message==WM_KEYUP) &&
            (first==VK_MENU || first==VK_LMENU || first==VK_RMENU)) {
            const bool captured=device.cheatText.Capturing();device.cheatText.Cancel();return captured;
        }
        if((message==WM_SYSKEYDOWN || message==WM_KEYDOWN) &&
            (first==VK_ESCAPE || first==VK_CONTROL || first==VK_LCONTROL || first==VK_RCONTROL || first==VK_F4)) {
            device.cheatText.Cancel();return false;
        }
        if(message!=WM_CHAR && message!=WM_SYSCHAR)return false;
        const bool alt=(static_cast<std::uintptr_t>(second)&(std::uintptr_t{1}<<29))!=0 || (GetKeyState(VK_MENU)&0x8000)!=0;
        const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        device.timeline.Character(time,static_cast<std::uint32_t>(first),alt,control);
        std::array<char,6> command{};
        // This acquisition decoder only decides whether Win32 consumes the
        // character. Gameplay capture/unlocks belong to the logical decoder.
        const auto result=device.cheatText.Character(static_cast<std::uint32_t>(first),alt,control,
            device.focused,frontend::easterEggs.active!=0,command);
        return result!=platform::NativeCheatTextResult::Unhandled;
    } catch(...) {
        // Never unwind an allocation/queue failure across the Win32 callback.
        // Poll/BeginStep rethrows at the ordinary application error boundary.
        device.failure=std::current_exception();return false;
    }
}
void WindowsInputDevice::Poll()
{
    using namespace platform;auto& device=*storage_;
    if(device.failure)std::rethrow_exception(device.failure);
    device.catalog.PollObservations(device.observations);
    if(device.failure)std::rethrow_exception(device.failure);
    // SDL's window-thread pump may dispatch sent Win32 focus messages too.
    const auto now=NativeInputTimeline::Clock::now();
    device.Focus(now,GetForegroundWindow()==device.window);
    for(const auto& observation:device.observations)for(std::size_t port=0;port<2;++port)
        if(observation.instanceId==device.selectedIds[port])
            device.timeline.Controller(observation.time,static_cast<std::int32_t>(port),observation.sample);
    const auto devices=device.catalog.Enumerate(false);
    for(std::int32_t port=0;port<2;++port) {
        const auto index=static_cast<std::size_t>(port);
        const auto selected=ResolveNativePlayerDevice(device.settings,devices,port);
        const auto key=selected>=0?devices[static_cast<std::size_t>(selected)].id:std::string{};
        const auto pad=selected>=0?device.catalog.Read(key,-1,false):NativePadSample{};
        if(device.selectedIds[index]!=pad.instanceId)device.timeline.Controller(now,port,pad);
        device.selectedIds[index]=pad.instanceId;device.selectedKeys[index]=key;
    }
    if(device.seedKeys) {
        // Keys held before focus was acquired have no delivered press in this
        // window. Seed only at the present observation, never back-date them.
        for(std::uint16_t key=1;key<255;++key)device.Key(now,key,(GetAsyncKeyState(key)&0x8000)!=0,true);
        device.seedKeys=false;
    }
    if(!device.focused)device.physicalKeys.fill(false);
    std::array<bool,256> ignored{};
    device.cheatText.FilterKeyboard(ignored,device.physicalKeys[VK_MENU],device.physicalKeys[VK_CONTROL],
        device.focused,frontend::easterEggs.active!=0);
}
void WindowsInputDevice::BeginStep(platform::NativeInputTimeline::Clock::time_point boundary)
{
    auto& device=*storage_;
    if(device.failure)std::rethrow_exception(device.failure);
    device.prepared=device.timeline.Sample(boundary,frontend::easterEggs.active!=0);device.preparedStep=true;
    for(const auto& command:device.prepared.textCommands)
        (void)frontend::easterEggs.TryUnlockTextCode(command.data(),command.size());
    for(std::int32_t port=0;port<2;++port) {
        const auto index=static_cast<std::size_t>(port);
        if(device.deliveredResetEpoch[index]!=device.prepared.resetEpoch[index]) {
            joystickNotifyNativeInputReset(port);device.deliveredResetEpoch[index]=device.prepared.resetEpoch[index];
        }
    }
}
int32_t WindowsInputDevice::ReadInput(int32_t port,JoystickControls& destination)
{
    if(port<0 || port>=2)return 0;
    using namespace platform;auto& device=*storage_;const auto index=static_cast<std::size_t>(port);
    if(!device.preparedStep)throw std::logic_error("Windows input read has no logical step boundary");
    const auto& pad=device.prepared.pads[index];
    if(device.journalIds[index]!=pad.instanceId) {
        device.journalIds[index]=pad.instanceId;char detail[128]{};
        std::snprintf(detail,sizeof(detail),"player=%d instance=%d connected=%d keyboard=%d transport=SDL2",
            port+1,pad.instanceId,pad.connected?1:0,NativeKeyboardEnabled(device.settings,port)?1:0);
        RecordSessionEvent("input.device",detail);
    }
    const std::array<bool,256> blocked{};
    const auto& keys=device.prepared.keyboardCaptured?blocked:device.prepared.keys;
    const auto state=NativeInputState(device.settings,port,pad,keys,device.prepared.focused);
    if(!state.connected)return 0;
    destination=NativeScannerControls(state);return 1;
}
void WindowsInputDevice::SetRumble(int32_t port,uint8_t smallMotor,uint8_t large)
{
    if(port<0 || port>=2)throw std::out_of_range("Windows rumble port");
    auto& device=*storage_;const auto index=static_cast<std::size_t>(port);
    if(!platform::NativeGamepadEnabled(device.settings,port))return;
    std::uint16_t low{},high{};
    if(device.settings.vibration && GetForegroundWindow()==device.window) {
        const auto gain=static_cast<unsigned>(device.settings.profiles[index].vibrationScale);
        low=static_cast<std::uint16_t>(static_cast<unsigned>(large)*257u*gain/100u);
        high=static_cast<std::uint16_t>((smallMotor?65535u:0u)*gain/100u);
    }
    if(device.selectedIds[index]>=0)device.catalog.Rumble(device.selectedKeys[index],-1,low,high,false);
}
}
#endif
