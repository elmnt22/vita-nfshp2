#pragma once

#ifdef __SWITCH2__
#define HP2_NX_INSTALL_DIRECTORY "sdmc:/switch/NFSHP2-Switch2"
#else
#define HP2_NX_INSTALL_DIRECTORY "sdmc:/switch/NFSHP2-Switch"
#endif

namespace hp2::nx {
#ifdef __SWITCH2__
inline constexpr int renderWidth=1920,renderHeight=1080;
inline constexpr int defaultFrameLimit=60,maximumFrameLimit=120;
#else
inline constexpr int renderWidth=1280,renderHeight=720;
inline constexpr int defaultFrameLimit=30,maximumFrameLimit=60;
#endif
inline constexpr char installDirectory[]=HP2_NX_INSTALL_DIRECTORY;
}
