#pragma once

#include "bsound/include/NativeEnvelope.hpp"
#include <cstdint>
#include <algorithm>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hp2::audio {

// Wine/Proton provides an XAudio2 COM bridge to FAudio. The supplied Wine
// logs demonstrate silent playback despite successful device creation, plus
// a crash inside XAudio2_9.dll on its audio thread during retirement. Custom
// application XAPOs are a compatibility risk, not a proven Wine-wide defect.
// Detect the actual host, not a launcher option, and keep Windows unchanged.
inline bool NativeAudioIsWine() noexcept
{
#ifdef _WIN32
    const HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
    return ntdll && (GetProcAddress(ntdll,"wine_get_version") ||
        GetProcAddress(ntdll,"wine_get_build_id"));
#else
    return false;
#endif
}

// A short attack cannot be applied by game-thread SetVolume polling: some
// UI clips finish before the next tick. Shape the source's first PCM samples
// BEFORE submitting the actual buffer to XAudio2 instead. This only edits a
// private voice buffer, never immutable shared bank data or emulated SPU RAM.
class NativeWinePcmAttack {
public:
    void Configure(NativeEnvelopeParameters parameters,std::uint32_t sourceRate) noexcept
    {
        envelope_=NativeEnvelope(parameters);
        sampleRate_=sourceRate>=1000 && sourceRate<=200000 ? sourceRate:NativeEnvelope::OutputRate;
        remainder_=0;
        envelope_.KeyOn(0);
    }
    void Apply(std::vector<std::int16_t>& samples,std::uint16_t channels) noexcept
    {
        if (channels!=1 && channels!=2)return;
        const auto frames=samples.size()/channels;
        for (std::size_t frame=0;frame<frames;++frame) {
            const auto phase=remainder_+NativeEnvelope::OutputRate;
            remainder_=phase%sampleRate_;
            envelope_.AdvanceFrames(phase/sampleRate_);
            if (envelope_.State()==NativeEnvelope::Phase::Sustain)break;
            const float gain=envelope_.Gain();
            for (std::uint16_t channel=0;channel<channels;++channel) {
                const auto at=frame*channels+channel;
                samples[at]=static_cast<std::int16_t>(static_cast<float>(samples[at])*gain);
            }
        }
    }
private:
    NativeEnvelope envelope_{};
    std::uint32_t sampleRate_{NativeEnvelope::OutputRate};
    std::uint64_t remainder_{};
};

// Wine's XAudio2 source volume is used only for live key-off/release. The
// original attack has already been applied sample-accurately in PCM above.
// GetState.SamplesPlayed is the physical cursor: game FPS cannot by itself
// advance the release. This is a native gain adapter, not PS2 emulation.
class NativeWineHostEnvelope {
public:
    void Configure(NativeEnvelopeParameters parameters,std::uint32_t sourceRate) noexcept
    {
        envelope_=NativeEnvelope(parameters);
        sampleRate_=sourceRate>=1000 && sourceRate<=200000 ? sourceRate:NativeEnvelope::OutputRate;
        cursor_=0;
        remainder_=0;
    }
    void KeyOn(std::uint64_t samplesPlayed) noexcept
    {
        cursor_=samplesPlayed;
        remainder_=0;
        envelope_.KeyOn(0);
        // PCM attack is handled before submitting buffers. The native gain
        // starts at unity so one-shot sounds cannot be muted between pumps.
        envelope_.AdvanceFrames(NativeEnvelope::OutputRate);
    }
    void KeyOff(bool streamStop) noexcept {envelope_.KeyOff(streamStop);}
    float Observe(std::uint64_t samplesPlayed) noexcept
    {
        if (samplesPlayed<cursor_) { // A native voice was reset; never interpret wrap as elapsed audio.
            cursor_=samplesPlayed;
            remainder_=0;
            return envelope_.Gain();
        }
        const auto played=samplesPlayed-cursor_;
        cursor_=samplesPlayed;
        const auto scaled=remainder_+played*NativeEnvelope::OutputRate;
        remainder_=scaled%sampleRate_;
        envelope_.AdvanceFrames(scaled/sampleRate_);
        return envelope_.Gain();
    }
    bool Finished() const noexcept {return envelope_.Silent();}
    std::uint16_t Level() const noexcept {return envelope_.Level();}
private:
    NativeEnvelope envelope_{};
    std::uint32_t sampleRate_{NativeEnvelope::OutputRate};
    std::uint64_t cursor_{},remainder_{};
};

} // namespace hp2::audio
