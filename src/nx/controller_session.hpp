#pragma once

#include "indep/inc/CarDriver.hpp"
#include <array>
#include <algorithm>
#include <cstdlib>

namespace hp2::nx {
inline bool separatePlayers{};
inline void* controllerSupportContext{};
inline bool (*controllerSupport)(void*){};
inline bool RequestTwoPlayerControllers() {
    const bool ready=controllerSupport && controllerSupport(controllerSupportContext);
    separatePlayers=ready;return ready;
}
inline void UpdateControllerSession(int players,bool challenge) noexcept {
    if(players!=2 || !challenge)separatePlayers=false;
}
inline platform::ControllerState MergeControllers(const std::array<platform::ControllerState,9>& sources) noexcept {
    platform::ControllerState result;
    for(const auto& source:sources)if(source.connected) {
        result.connected=true;result.held|=source.held;
        for(std::size_t axis=0;axis<result.sticks.size();++axis)
            if(std::abs(int(source.sticks[axis])-128)>std::abs(int(result.sticks[axis])-128))result.sticks[axis]=source.sticks[axis];
        for(std::size_t channel=0;channel<result.pressures.size();++channel)
            result.pressures[channel]=std::max(result.pressures[channel],source.pressures[channel]);
    }
    return result;
}
inline platform::ControllerState RoutedController(const std::array<platform::ControllerState,9>& sources,int port,bool split) noexcept {
    if(port<0 || port>1)return {};
    if(split)return sources[static_cast<std::size_t>(port)];
    return port==0 ? MergeControllers(sources):platform::ControllerState{};
}
}
