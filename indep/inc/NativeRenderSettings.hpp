#ifndef NFSHP2_NATIVE_RENDER_SETTINGS_HPP
#define NFSHP2_NATIVE_RENDER_SETTINGS_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace hp2 {
struct NativeRenderSettings {
#if defined(__SWITCH__) && !defined(__SWITCH2__)
    float drawDistanceScale{0.3f};
    float mirrorDistanceScale{0.5f};
#else
    float drawDistanceScale{1.0f};
    float mirrorDistanceScale{1.0f};
#endif
};
inline NativeRenderSettings validateNativeRenderSettings(NativeRenderSettings value) noexcept
{
#if defined(__SWITCH__) && !defined(__SWITCH2__)
    // The Switch port uses one fixed scene profile, including legacy configs.
    value.drawDistanceScale=0.3f;value.mirrorDistanceScale=0.5f;
#else
    value.drawDistanceScale=std::isfinite(value.drawDistanceScale) ?
        std::clamp(value.drawDistanceScale,1.0f,4.0f):1.0f;
    value.mirrorDistanceScale=std::isfinite(value.mirrorDistanceScale) ?
        std::clamp(value.mirrorDistanceScale,0.5f,4.0f):1.0f;
#endif
    return value;
}

// Configured on the application thread before a frame is built. Gather workers
// borrow these immutable settings for that frame; launcher edits apply at startup.
inline NativeRenderSettings nativeRenderSettingsStorage{};
inline void configureNativeRenderSettings(const NativeRenderSettings& value) noexcept
{
    nativeRenderSettingsStorage=validateNativeRenderSettings(value);
}
inline const NativeRenderSettings& nativeRenderSettings() noexcept
{
    return nativeRenderSettingsStorage;
}
inline float nativeViewDistanceScale(std::int32_t viewId) noexcept
{
    return viewId==3 ? nativeRenderSettings().mirrorDistanceScale :
        viewId==1 || viewId==2 ? nativeRenderSettings().drawDistanceScale:1.0f;
}

// Updated only at application-frame boundaries, before scene workers run.
struct NativeSceneQuality {
    float sceneryLod,vehicleLod,highDetailSurface,nearDetailEnter,nearDetailExit;
    bool reducedOpponents;
};
inline constexpr NativeSceneQuality switchSceneQuality{1.0f,1.75f,35.0f,35.0f,50.0f,true};
inline constexpr NativeSceneQuality upstreamSceneQuality{3.0f,2.5f,125.0f,125.0f,165.0f,false};
// Vita: retail PAL LOD selection (multipliers 1.0, all opponents full
// detail) plus the Switch near-detail guard against large-structure pops.
inline constexpr NativeSceneQuality vitaSceneQuality{1.0f,1.0f,35.0f,35.0f,50.0f,false};
#if defined(__SWITCH__) && !defined(__SWITCH2__)
inline NativeSceneQuality nativeSceneQualityStorage=switchSceneQuality;
#elif defined(__vita__)
inline NativeSceneQuality nativeSceneQualityStorage=vitaSceneQuality;
#else
inline NativeSceneQuality nativeSceneQualityStorage=upstreamSceneQuality;
#endif
inline const NativeSceneQuality& nativeSceneQuality() noexcept {return nativeSceneQualityStorage;}

// Approved visual cut (2026-11-25): scenery projected pixel-size scale below
// retail ([0] main views, [1] mirror; 1 = retail): earlier LOD steps and
// pixel-size culling. Driven per frame by SceneryDynamicCut (heavy frames
// only), written on the application thread before the scene is built.
inline std::array<float,2> ecstasySceneryPixelCut{{1.0f,1.0f}};
// Ramps the scale toward `minimum` while the smoothed CPU frame cost (max of
// game thread and render gather+record) exceeds `high` ms, back to 1 below
// `low`. enabled=0 keeps retail.
struct SceneryDynamicCut {
    int enabled=0;
    float minimum=0.6f,high=31.0f,low=27.0f,down=0.04f,up=0.01f;
    float smoothed=0.0f;
    std::uint64_t cutFrames=0,frames=0;double cutSum=0.0;
};
inline SceneryDynamicCut sceneryDynamicCut{};
inline bool configureNativeFrameLimitQuality(int limit) noexcept {
#ifdef __SWITCH2__
    const bool reduced=limit==120 || limit==0;
    if(nativeSceneQualityStorage.reducedOpponents==reduced)return false;
    nativeSceneQualityStorage=reduced ? switchSceneQuality:upstreamSceneQuality;
    // Keep the larger main distance even in the high-FPS scene profile.
    nativeRenderSettingsStorage.drawDistanceScale=1.0f;
    nativeRenderSettingsStorage.mirrorDistanceScale=reduced ? 0.5f:1.0f;
    return true;
#else
    (void)limit;return false;
#endif
}

#if defined(__vita__)
// Vita is game-thread bound: frustum-crossing scenery goes through the cached
// GPU-clipped batch (render/GL threads have headroom) instead of per-strip
// CPU culling in RenderModel (~90 us per model on the game thread).
inline constexpr std::uint32_t nativeGpuSceneryClipMaximumStrips=512;
#else
inline constexpr std::uint32_t nativeGpuSceneryClipMaximumStrips=32;
#endif

} // namespace hp2
#endif
