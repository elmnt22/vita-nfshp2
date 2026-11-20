#pragma once

#include "bsound/include/bSound.h"

#include <memory>
#include <cstdint>
#include <string>

namespace hp2::nx {
struct AudioStatistics {
    double decodeMilliseconds{},commandMilliseconds{},mixMilliseconds{},outputMilliseconds{};
    std::uint64_t buffers{},voiceBuffers{};
    std::uint32_t voicesPeak{};
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
