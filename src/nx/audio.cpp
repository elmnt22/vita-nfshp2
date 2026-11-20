#include "audio.hpp"

#include "bsound/include/NativeEnvelope.hpp"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <deque>
#include <map>
#include <stdexcept>
#include <utility>
#include <type_traits>

namespace hp2::nx {
namespace {
using namespace hp2::audio;
constexpr unsigned sampleRate=48000;
constexpr std::size_t framesPerBuffer=1024;
constexpr std::size_t bufferBytes=framesPerBuffer*2*sizeof(int16_t);
static_assert(bufferBytes==0x1000);

struct AudioFailure {
    WorldAudioResult result;
    std::string detail;
};

void check(WorldAudioResult result,const char* detail)
{
    if(result!=WORLD_AUDIO_OK)throw AudioFailure{result,detail};
}

void checkSwitch(Result result,const char* detail)
{
    if(R_FAILED(result))
        throw AudioFailure{WORLD_AUDIO_BACKEND_FAILURE,std::string(detail)+" ("+std::to_string(result)+")"};
}

struct Voice {
    WorldAudioVoiceRef sample{};
    const WorldAudioBank* bank{};
    uint64_t sequence{};
    NativeVoiceMix mix;
    NativeEnvelope envelope{};
    std::deque<std::shared_ptr<const PcmWave>> chunks;
    std::unique_ptr<NativeJukeboxStream> music;
    const WorldAudioStreamSource* movie{};
    Ps2AdpcmHistory movieHistory{};
    double cursor{};
    uint32_t pitch{};
    bool opened{},playing{},paused{},loop{};

    void Append(PcmWave pcm)
    {
        if(pcm.channels!=mix.channels || !pcm.sampleRate || pcm.samples.empty() ||
            pcm.samples.size()%pcm.channels)
            throw AudioFailure{WORLD_AUDIO_RESOURCE_FAILURE,"Invalid decoded stream PCM"};
        chunks.push_back(std::make_shared<PcmWave>(std::move(pcm)));
    }

    bool Streaming() const noexcept {return movie || (music && !music->Finished());}

    void RefillMusic()
    {
        if(chunks.size()<2 && music && !music->Finished())Append(music->Read().pcm);
    }

    void RefillMovie()
    {
        if(movie && chunks.size()<2) {
            PcmWave pcm;
            if(ReadMoviePcmChunk(*movie,movieHistory,pcm))Append(std::move(pcm));
        }
    }

    void Mix(std::array<float,framesPerBuffer*2>& output)
    {
        if(!playing || paused)return;
        const bool constantEnvelope=envelope.State()==NativeEnvelope::Phase::Sustain || envelope.Silent();
        const float constantGain=envelope.Gain();
        std::size_t frame=0;
        while(frame<framesPerBuffer && !chunks.empty()) {
            const auto& pcm=*chunks.front();
            const auto count=pcm.samples.size()/pcm.channels;
            const auto end=loop && pcm.loopEndFrame>pcm.loopStartFrame?
                std::min<std::size_t>(pcm.loopEndFrame,count):count;
            if(cursor>=end) {
                if(loop) {
                    const auto start=pcm.loopStartFrame<end?pcm.loopStartFrame:0;
                    cursor=start+std::fmod(cursor-start,static_cast<double>(end-start));
                } else {cursor-=end;chunks.pop_front();continue;}
            }
            // Rate, channel layout and loop bounds are fixed for this chunk.
            // Keep interpolation and gain arithmetic identical to the scalar
            // mixer while removing invariant divisions/branches per sample.
            const double step=static_cast<double>(pitch?pitch:pcm.sampleRate)/sampleRate;
            const auto render=[&](auto channels) {
                constexpr std::size_t channelCount=decltype(channels)::value;
                while(frame<framesPerBuffer && cursor<end) {
                    const auto index=static_cast<std::size_t>(cursor);
                    const auto next=std::min(index+1,end-1);
                    const auto fraction=static_cast<float>(cursor-index);
                    if(!constantEnvelope)envelope.AdvanceFrames(1);
                    const auto gain=constantEnvelope ? constantGain:envelope.Gain();
                    for(std::size_t channel=0;channel<channelCount;++channel) {
                        const float a=pcm.samples[index*channelCount+channel];
                        const float b=pcm.samples[next*channelCount+channel];
                        const auto value=(a+(b-a)*fraction)*gain;
                        output[2*frame]+=value*mix.output[channel][0];
                        output[2*frame+1]+=value*mix.output[channel][1];
                    }
                    cursor+=step;++frame;
                }
            };
            if(pcm.channels==1)render(std::integral_constant<std::size_t,1>{});
            else render(std::integral_constant<std::size_t,2>{});
        }
        if(constantEnvelope)envelope.AdvanceFrames(frame);
    }
};

struct OutputBuffer {
    alignas(0x1000) std::array<int16_t,framesPerBuffer*2> samples{};
    AudioOutBuffer descriptor{};
    bool queued{};
};
}

struct AudioDevice::State {
    explicit State(bSound& sound):library(sound),queue(*sound.Driver()) {}
    ~State() {StopOutput();}

    void StartOutput()
    {
        checkSwitch(audoutInitialize(),"audoutInitialize");
        initialized=true;
        if(audoutGetSampleRate()!=sampleRate || audoutGetChannelCount()!=2 ||
            audoutGetPcmFormat()!=PcmFormat_Int16)
            throw AudioFailure{WORLD_AUDIO_BACKEND_FAILURE,"Switch output is not 48 kHz stereo PCM16"};
        for(auto& buffer:buffers) {
            buffer.descriptor.buffer=buffer.samples.data();
            buffer.descriptor.buffer_size=bufferBytes;
            buffer.descriptor.data_size=bufferBytes;
        }
        checkSwitch(audoutStartAudioOut(),"audoutStartAudioOut");
    }

    void StopOutput() noexcept
    {
        if(initialized) {
            (void)audoutStopAudioOut();
            audoutExit();
            initialized=false;
        }
    }

    void Submit()
    {
        using Clock=std::chrono::steady_clock;
        auto outputStarted=Clock::now();
        // libnx returns one buffer address per call. Repeat until the service
        // has no more released buffers, then refill only those it returned.
        for(std::size_t i=0;i<buffers.size();++i) {
            AudioOutBuffer* released{};
            u32 count{};
            checkSwitch(audoutGetReleasedAudioOutBuffer(&released,&count),"audoutGetReleasedAudioOutBuffer");
            if(!count)break;
            const auto found=std::find_if(buffers.begin(),buffers.end(),[released](const auto& buffer) {
                return &buffer.descriptor==released;
            });
            if(found==buffers.end())
                throw AudioFailure{WORLD_AUDIO_BACKEND_FAILURE,"Switch audio returned an unknown PCM buffer"};
            found->queued=false;
        }
        for(auto& buffer:buffers) {
            if(buffer.queued)continue;
            const auto mixStarted=Clock::now();
            statistics.outputMilliseconds+=std::chrono::duration<double,std::milli>(mixStarted-outputStarted).count();
            mixBuffer.fill(0.0f);
            std::uint32_t active=0;
            for(auto& voice:voices)if(voice.playing && !voice.paused && !voice.chunks.empty()) {++active;voice.Mix(mixBuffer);}
            statistics.voicesPeak=std::max(statistics.voicesPeak,active);
            statistics.voiceBuffers+=active;++statistics.buffers;
            for(std::size_t i=0;i<mixBuffer.size();++i)
                buffer.samples[i]=static_cast<int16_t>(std::clamp(mixBuffer[i],-32768.0f,32767.0f));
            outputStarted=Clock::now();
            statistics.mixMilliseconds+=std::chrono::duration<double,std::milli>(outputStarted-mixStarted).count();
            armDCacheFlush(buffer.samples.data(),bufferBytes);
            checkSwitch(audoutAppendAudioOutBuffer(&buffer.descriptor),"audoutAppendAudioOutBuffer");
            buffer.queued=true;
        }
        statistics.outputMilliseconds+=std::chrono::duration<double,std::milli>(Clock::now()-outputStarted).count();
    }

    Voice* Find(const WorldAudioRequest& operation)
    {
        for(auto& voice:voices)
            if(voice.sample.instance==operation.instance && voice.sample.generation==operation.instanceGeneration)
                return &voice;
        return nullptr;
    }

    Voice& RequireVoice(const WorldAudioRequest& operation)
    {
        auto* voice=Find(operation);
        if(!voice)throw AudioFailure{WORLD_AUDIO_STALE_REFERENCE,"Audio command lost its voice"};
        return *voice;
    }

    Voice& Allocate(const WorldAudioRequest& operation,uint16_t channels)
    {
        const auto id=operation.reservedVoiceId;
        if(!operation.instance || (channels!=1 && channels!=2) ||
            operation.reservedVoiceChannels!=channels || id+channels>voices.size())
            throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Invalid Switch audio channel reservation"};
        auto& voice=voices[id];
        for(std::size_t other=0;other<voices.size();++other) {
            const auto& held=voices[other];
            if(!held.sample.instance)continue;
            if(other==id && held.sample.instance==operation.instance &&
                held.sample.generation==operation.instanceGeneration)continue;
            if(other<id+channels && id<other+held.mix.channels)
                throw AudioFailure{WORLD_AUDIO_STALE_REFERENCE,"Switch audio channels still have a previous owner"};
        }
        check(library.Voices().Assign(operation,id),"Assign Switch audio voice");
        voice=Voice{};
        voice.sample={operation.instance,operation.instanceGeneration};
        voice.sequence=operation.sequence;
        voice.bank=operation.bank;
        voice.envelope=NativeEnvelope(CaptureVoiceEnvelope(operation));
        voice.mix.Initialize(channels,!operation.instance->sampleType,0x3FFF,0,mode);
        return voice;
    }

    void Observe(const WorldAudioRequest& operation)
    {
        bVoiceStatusSnapshot snapshot{};
        for(std::size_t i=0;i<voices.size();++i) {
            auto& voice=voices[i];
            voice.RefillMovie();
            snapshot[i]={voice.sample,voice.sequence,voice.playing && (!voice.chunks.empty() || voice.Streaming()),
                voice.opened && !voice.chunks.empty(),voice.mix.channels};
        }
        check(library.Voices().CompleteUpdate(operation,snapshot),"Observe Switch audio voices");
        for(std::size_t i=0;i<voices.size();++i) {
            const auto& voice=voices[i];
            auto* sample=voice.sample.instance;
            if(snapshot[i].prebuffered && sample && sample->occupied && !sample->retiring &&
                sample->generation==voice.sample.generation && !sample->prebuffered &&
                sample->playSequence==voice.sequence)
                check(sample->sampleType==1?worldAudioSpeechEvent(&queue,voice.sample,voice.sequence,4):
                    worldAudioStreamEvent(&queue,voice.sample,voice.sequence,4),"Notify Switch stream readiness");
        }
    }

    NativeAudioApply Apply(const WorldAudioRequest& operation)
    {
        if(operation.instance && operation.instance->retiring && operation.kind!=WORLD_AUDIO_STOP)
            return NativeAudioApply::Cancelled;
        switch(operation.kind) {
        case WORLD_AUDIO_INITIALIZE_DEVICE:
            StartOutput();
            break;
        case WORLD_AUDIO_LOAD_REGION:
            if(!operation.bank || !banks.try_emplace(operation.bank,*operation.bank).second)
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Invalid Switch audio bank load"};
            check(library.CompleteRegion(operation,true),"Complete Switch audio bank load");
            break;
        case WORLD_AUDIO_UNLOAD_BANK:
            for(auto& voice:voices) {
                if(voice.sample.instance && voice.bank==operation.bank) {
                    library.Voices().ReleaseSample(voice.sample);
                    voice=Voice{};
                }
            }
            if(banks.erase(operation.bank)!=1)
                throw AudioFailure{WORLD_AUDIO_STALE_REFERENCE,"Switch audio bank was already unloaded"};
            break;
        case WORLD_AUDIO_DRIVER_UPDATE:
            Observe(operation);
            break;
        case WORLD_AUDIO_PRIME:
            Allocate(operation,operation.reservedVoiceChannels);
            break;
        case WORLD_AUDIO_PLAY: {
            Voice* voice=Find(operation);
            if(!operation.instance)
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"PLAY has no retained sample"};
            if(!operation.instance->sampleType) {
                const auto bank=banks.find(operation.bank);
                if(bank==banks.end() || !operation.cue || operation.cue->metadataSize!=52)
                    throw AudioFailure{WORLD_AUDIO_RESOURCE_FAILURE,"PLAY has no resident audio bank cue"};
                const auto base=reinterpret_cast<uintptr_t>(operation.bank->metadata);
                const auto address=reinterpret_cast<uintptr_t>(operation.cue->metadata);
                if(!base || address<base || address-base>operation.bank->metadataSize ||
                    operation.bank->metadataSize-(address-base)<52)
                    throw AudioFailure{WORLD_AUDIO_RESOURCE_FAILURE,"PLAY has invalid resident cue metadata"};
                auto pcm=bank->second.Acquire(address-base);
                voice=&Allocate(operation,pcm->channels);
                voice->chunks.push_back(std::move(pcm));
                voice->loop=GetLoop(operation.cue->metadata[0x21])!=0;
            } else if(!voice || !voice->opened) {
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"PLAY has no buffered Switch stream"};
            }
            check(library.Voices().Assign(operation,operation.reservedVoiceId),"Start Switch audio voice");
            voice->mix.Initialize(voice->mix.channels,!operation.instance->sampleType,
                operation.sampleVolume,operation.samplePan,mode);
            voice->pitch=operation.samplePitch;
            voice->sequence=operation.sequence;
            voice->envelope.KeyOn(0);
            voice->playing=true;
            voice->paused=false;
            break;
        }
        case WORLD_AUDIO_OPEN_STREAM: {
            const auto* source=operation.streamSource;
            if(!source || !operation.instance)
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"OPEN has no retained Switch stream"};
            if(source->movie) {
                auto* voice=Find(operation);
                if(!voice || voice->movie!=source) {
                    voice=&Allocate(operation,2);
                    voice->movie=source;
                }
                voice->RefillMovie();
                if(voice->chunks.empty())return NativeAudioApply::Blocked;
                voice->opened=true;
                voice->sequence=operation.sequence;
            } else if(operation.instance->completionKind==WORLD_AUDIO_COMPLETION_JUKEBOX) {
                auto stream=PrepareJukeboxStream(CaptureStreamDecodeRange(operation));
                auto& voice=Allocate(operation,stream.first.pcm.channels);
                voice.music=std::move(stream.stream);
                voice.Append(std::move(stream.first.pcm));
                voice.opened=true;
            } else {
                auto pcm=DecodeSoundStreamRange(operation);
                auto& voice=Allocate(operation,pcm.channels);
                voice.Append(std::move(pcm));
                voice.opened=true;
            }
            break;
        }
        case WORLD_AUDIO_APPEND_CHAIN:
        case WORLD_AUDIO_OPEN_CHAIN: {
            auto pcm=DecodeSoundStreamRange(operation);
            auto* voice=Find(operation);
            if(!voice) {
                if(operation.kind==WORLD_AUDIO_APPEND_CHAIN)
                    throw AudioFailure{WORLD_AUDIO_STALE_REFERENCE,"Switch speech append has no primed voice"};
                voice=&Allocate(operation,pcm.channels);
            }
            if(voice->chunks.empty())voice->cursor=0;
            if(voice->chunks.size()>=64)
                throw AudioFailure{WORLD_AUDIO_CAPACITY,"Switch speech chain exceeds PCM buffer capacity"};
            voice->Append(std::move(pcm));
            if(operation.kind==WORLD_AUDIO_OPEN_CHAIN) {
                check(library.Voices().Assign(operation,operation.reservedVoiceId),"Open Switch speech chain");
                voice->opened=true;
                voice->sequence=operation.sequence;
            }
            break;
        }
        case WORLD_AUDIO_STOP: {
            auto* voice=Find(operation);
            if(voice) {
                voice->playing=voice->paused=voice->opened=voice->loop=false;
                voice->chunks.clear();
                voice->movie=nullptr;
                voice->movieHistory={};
                voice->music.reset();
                voice->cursor=0;
                if(operation.releaseDeviceVoice)*voice=Voice{};
            }
            check(library.Voices().CompleteStop(operation),"Stop Switch audio voice");
            break;
        }
        case WORLD_AUDIO_SET_SAMPLE_VOLUME:
            RequireVoice(operation).mix.SetVolume(operation.sampleVolume,mode);
            break;
        case WORLD_AUDIO_SET_PITCH_OFFSET:
            RequireVoice(operation).pitch=operation.samplePitch;
            break;
        case WORLD_AUDIO_SET_CONTROL_CODE:
            RequireVoice(operation).mix.SetAzimuth(operation.controlCode,mode);
            break;
        case WORLD_AUDIO_SET_PAUSED:
            RequireVoice(operation).paused=operation.parameterValue!=0.0f;
            break;
        case WORLD_AUDIO_SET_MUTED:
            RequireVoice(operation).mix.Mute(operation.muted!=0,operation.instance->savedMuteVolume,mode);
            break;
        case WORLD_AUDIO_SET_AUDIO_MODE:
            if(operation.audioMode<0 || operation.audioMode>2)
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Invalid Switch audio mode"};
            mode=static_cast<uint8_t>(operation.audioMode+1);
            for(auto& voice:voices)if(voice.sample.instance)voice.mix.SetMode(mode);
            break;
        case WORLD_AUDIO_SET_REGION_PATH:
        case WORLD_AUDIO_SET_CHANNEL_VOLUME:
        case WORLD_AUDIO_FADE_VOLUME:
            // The engine owns these publications and emits actual per-voice changes.
            break;
        case WORLD_AUDIO_SET_CORE_EFFECT_VOLUME:
        case WORLD_AUDIO_SET_CORE_EFFECT_PAN:
        case WORLD_AUDIO_SET_CORE_EFFECT_ATTRIBUTE:
        case WORLD_AUDIO_SET_CORE_EFFECT_ENABLED:
            // This first backend mixes the dry outputs. The reverb buses
            // do not affect the retained dry voice gain or sample timing.
            break;
        default:
            throw AudioFailure{WORLD_AUDIO_BACKEND_FAILURE,"Unsupported Switch audio request"};
        }
        return NativeAudioApply::Applied;
    }

    bSound& library;
    WorldAudioRequestQueue& queue;
    NativeAudioCommandPump commands;
    AudioStatistics statistics;
    std::map<const WorldAudioBank*,NativeSoundBankCache> banks;
    std::array<Voice,48> voices{};
    std::array<OutputBuffer,3> buffers{};
    std::array<float,framesPerBuffer*2> mixBuffer{};
    uint8_t mode{2};
    bool initialized{};
};

AudioDevice::AudioDevice()=default;
AudioDevice::~AudioDevice()=default;

WorldAudioResult AudioDevice::Open(bSound& sound)
{
    if(state_ || !sound.Driver())return WORLD_AUDIO_INVALID_BINDING;
    error_.clear();
    state_=std::make_unique<State>(sound);
    const auto result=Pump();
    if(result!=WORLD_AUDIO_OK)Close();
    return result;
}

WorldAudioResult AudioDevice::Pump()
{
    if(!state_)return WORLD_AUDIO_INVALID_BINDING;
    try {
        const auto decodeStarted=std::chrono::steady_clock::now();
        for(auto& voice:state_->voices)voice.RefillMusic();
        const auto commandsStarted=std::chrono::steady_clock::now();
        state_->statistics.decodeMilliseconds+=std::chrono::duration<double,std::milli>(commandsStarted-decodeStarted).count();
        check(state_->commands.Pump(state_->queue,256,[this](const auto& request) {
            return state_->Apply(request);
        }),"Pump retained Switch audio commands");
        state_->statistics.commandMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-commandsStarted).count();
        if(!state_->initialized)
            throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Missing Switch audio initialize request"};
        state_->Submit();
        return WORLD_AUDIO_OK;
    } catch(const AudioFailure& failure) {
        error_=failure.detail;
        if(const auto& request=state_->commands.Current())
            error_+=" [request "+std::to_string(request->kind)+", sequence "+std::to_string(request->sequence)+"]";
        return failure.result;
    } catch(const std::exception& failure) {
        error_=failure.what();
        return WORLD_AUDIO_RESOURCE_FAILURE;
    }
}

AudioStatistics AudioDevice::TakeStatistics() noexcept
{
    if(!state_)return {};
    auto result=state_->statistics;state_->statistics={};return result;
}

WorldAudioResult AudioDevice::Close()
{
    if(!state_)return WORLD_AUDIO_INVALID_BINDING;
    auto& sound=state_->library;
    state_->StopOutput();
    for(const auto& voice:state_->voices)if(voice.sample.instance)sound.Voices().ReleaseSample(voice.sample);
    state_.reset();
    return sound.CancelDeviceRequestsAfterClose();
}
}
