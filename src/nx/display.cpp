#include "display.hpp"
#include "platform.hpp"
#include "config.hpp"
#include <switch.h>
#include <stdexcept>

namespace hp2::nx {
void UpdateDisplayRefresh(bool force) {
#ifdef __SWITCH2__
    // Do not set a swap interval after this: it withdraws VI2's 120 Hz request.
    // VI2 falls back to 60 Hz on a display/system that cannot use 120 Hz.
    static int applied=-1;
    const int high=userSettings.frameLimit==0 || userSettings.frameLimit>60;
    if(!force && applied==high)return;
    auto* window=nwindowGetDefault();
    const Result result=high ? nwindowSetFrameRates(window,0,0,true) :
        nwindowSetSwapInterval(window,1);
    if(R_SUCCEEDED(result))applied=high;
#else
    (void)force;
#endif
}
void ConfigureDisplayWindow() {
    auto* window=nwindowGetDefault();
    if(!window)throw std::runtime_error("No default display window");
#ifdef __SWITCH2__
    const Result result=nwindowSetModeDimensions(window,renderWidth,renderHeight,renderWidth,renderHeight);
#else
    const Result result=nwindowSetDimensions(window,renderWidth,renderHeight);
#endif
    if(R_FAILED(result))throw std::runtime_error("Cannot set the render window dimensions");
    UpdateDisplayRefresh(true);
}
}
