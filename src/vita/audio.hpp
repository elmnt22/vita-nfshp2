#pragma once

#include "bsound/include/bSound.h"

#include <memory>
#include <cstdint>
#include <string>

namespace hp2::vita {
struct AudioStatistics {
    double decodeMilliseconds{},commandMilliseconds{},mixMilliseconds{},outputMilliseconds{};
    std::uint64_t buffers{},voiceBuffers{};
    std::uint32_t voicesPeak{};
    // Reverb: wet buffers processed, voices sending, last preset/depth per core.
    std::uint64_t reverbBuffers{},wetVoiceBuffers{};
    int reverbPreset[2]{-1,-1};int reverbVolume[2]{};
    double applyPeakMilliseconds{};
    int applyPeakKind{-1};
    // Slowest pump in the window: its request count and phase split.
    double pumpPeakMilliseconds{},pumpPeakMusicMs{},pumpPeakInstallMs{},pumpPeakCommandsMs{};
    std::uint32_t pumpPeakRequests{};
    double pumpPeakLockWaitMs{},pumpPeakApplyMs{};
    // Per request kind: apply ms and count; PreDecode and request totals.
    double kindMilliseconds[40]{};std::uint32_t kindCounts[40]{};
    double preDecodeMilliseconds{},applyMilliseconds{},lockWaitMilliseconds{};std::uint64_t requests{};
};
class AudioDevice final {
public:
    AudioDevice();
    ~AudioDevice();
    WorldAudioResult Open(bSound& sound);
    WorldAudioResult Pump();
    WorldAudioResult Close();
    AudioStatistics TakeStatistics() noexcept;
    const std::string& Error() const noexcept {return error_;}

private:
    struct State;
    std::unique_ptr<State> state_;
    std::string error_;
};
}
