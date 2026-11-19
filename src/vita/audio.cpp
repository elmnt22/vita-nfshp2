#include "audio.hpp"

#include "bsound/include/NativeEnvelope.hpp"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <map>
#include <stdexcept>
#include <utility>
#include <type_traits>

namespace hp2::vita {
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


// Result of one background music job (file reads + ADPCM decode).
template<class T> struct AsyncResult {
    std::atomic<bool> done{false};
    std::optional<T> value;
    std::exception_ptr error;
    T Take() {
        if(error)std::rethrow_exception(error);
        return std::move(*value);
    }
};

// One low-priority worker for music file I/O and decode, so neither the game
// thread nor the mixer waits on the memory card. Jobs own their inputs
// (copied decode ranges, shared stream objects); nothing reads live voices.
class AudioWorker {
public:
    ~AudioWorker() {Stop();}
    bool Start()
    {
        if(started_)return true;
        sema_=sceKernelCreateSema("hp2_audio_jobs",0,0,1<<20,nullptr);
        if(sema_<0)return false;
        stopping_=false;
        pthread_attr_t attributes;pthread_attr_init(&attributes);
        // The jukebox opener keeps a 64 KB tail buffer on its stack.
        pthread_attr_setstacksize(&attributes,512*1024);
        started_=pthread_create(&thread_,&attributes,[](void* self) -> void* {
            static_cast<AudioWorker*>(self)->Run();return nullptr;
        },this)==0;
        pthread_attr_destroy(&attributes);
        if(!started_) {sceKernelDeleteSema(sema_);sema_=-1;}
        return started_;
    }
    void Stop() noexcept
    {
        if(!started_)return;
        {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}
        sceKernelSignalSema(sema_,1);
        pthread_join(thread_,nullptr);started_=false;
        sceKernelDeleteSema(sema_);sema_=-1;
        jobs_.clear();
    }
    // Runs job inline when the worker is unavailable.
    void Post(std::function<void()> job)
    {
        if(!started_ && !Start()) {job();return;}
        {std::lock_guard<std::mutex> lock(mutex_);jobs_.push_back(std::move(job));}
        sceKernelSignalSema(sema_,1);
    }
private:
    void Run()
    {
        // Core 2 with the mixer, below it and below the GL thread.
        sceKernelChangeThreadCpuAffinityMask(0,SCE_KERNEL_CPU_MASK_USER_2);
        sceKernelChangeThreadPriority(0,180);
        for(;;) {
            sceKernelWaitSema(sema_,1,nullptr);
            std::function<void()> job;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(stopping_)return;
                if(jobs_.empty())continue;
                job=std::move(jobs_.front());jobs_.pop_front();
            }
            job();
        }
    }
    pthread_t thread_{};
    SceUID sema_{-1};
    bool started_{},stopping_{};
    std::mutex mutex_;
    std::deque<std::function<void()>> jobs_;
};

struct MusicRead {
    std::shared_ptr<NativeJukeboxStream> stream;
    AsyncResult<NativeJukeboxStream::Chunk> result;
};

struct Voice {
    WorldAudioVoiceRef sample{};
    const WorldAudioBank* bank{};
    uint64_t sequence{};
    NativeVoiceMix mix;
    NativeEnvelope envelope{};
    std::deque<std::shared_ptr<const PcmWave>> chunks;
    // The worker reads `music` only through a MusicRead it shares; Finished()
    // is mirrored in musicFinished so no other thread reads the cursor.
    std::shared_ptr<NativeJukeboxStream> music;
    std::shared_ptr<MusicRead> pendingRead;
    bool musicFinished{};
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

    bool Streaming() const noexcept {return movie || (music && !musicFinished);}

    bool NeedsMusic() const noexcept {return chunks.size()<2 && music && !musicFinished && !pendingRead;}

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
    alignas(64) std::array<int16_t,framesPerBuffer*2> samples{};
};
}

struct AudioDevice::State {
    explicit State(bSound& sound):library(sound),queue(*sound.Driver()) {}
    ~State() {StopOutput();}

    // Vita: one BGM port (48 kHz stereo PCM16). A dedicated output thread
    // mixes each buffer under the voice lock and blocks in sceAudioOutOutput,
    // so a slow game frame does not starve the hardware queue.
    void StartOutput()
    {
        port=sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,framesPerBuffer,sampleRate,SCE_AUDIO_OUT_MODE_STEREO);
        if(port<0)throw AudioFailure{WORLD_AUDIO_BACKEND_FAILURE,"sceAudioOutOpenPort ("+std::to_string(port)+")"};
        int volume[2]={SCE_AUDIO_VOLUME_0DB,SCE_AUDIO_VOLUME_0DB};
        sceAudioOutSetVolume(port,static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH|SCE_AUDIO_VOLUME_FLAG_R_CH),volume);
        initialized=true;running=true;
        pthread_attr_t attributes;pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes,256*1024);
        const int created=pthread_create(&thread,&attributes,[](void* self) -> void* {
            static_cast<State*>(self)->OutputThread();return nullptr;
        },this);
        pthread_attr_destroy(&attributes);
        if(created!=0) {running=false;throw AudioFailure{WORLD_AUDIO_BACKEND_FAILURE,"Audio output thread creation failed"};}
        threadStarted=true;
    }

    void StopOutput() noexcept
    {
        worker.Stop();
        if(threadStarted) {running=false;pthread_join(thread,nullptr);threadStarted=false;}
        if(initialized) {sceAudioOutReleasePort(port);port=-1;initialized=false;}
    }

    void OutputThread()
    {
        // Core 2 (scene workers burst there only briefly); above the game threads.
        sceKernelChangeThreadCpuAffinityMask(0,SCE_KERNEL_CPU_MASK_USER_2);
        sceKernelChangeThreadPriority(0,96);
        std::size_t current=0;
        while(running.load(std::memory_order_relaxed)) {
            auto& buffer=buffers[current];current=(current+1)%buffers.size();
            using Clock=std::chrono::steady_clock;
            const auto mixStarted=Clock::now();
            {
                std::lock_guard<std::mutex> lock(mutex);
                mixBuffer.fill(0.0f);
                std::uint32_t active=0;
                for(auto& voice:voices)if(voice.playing && !voice.paused && !voice.chunks.empty()) {++active;voice.Mix(mixBuffer);}
                statistics.voicesPeak=std::max(statistics.voicesPeak,active);
                statistics.voiceBuffers+=active;++statistics.buffers;
                statistics.mixMilliseconds+=std::chrono::duration<double,std::milli>(Clock::now()-mixStarted).count();
            }
            for(std::size_t i=0;i<mixBuffer.size();++i)
                buffer.samples[i]=static_cast<int16_t>(std::clamp(mixBuffer[i],-32768.0f,32767.0f));
            sceAudioOutOutput(port,buffer.samples.data());
        }
        sceAudioOutOutput(port,nullptr); // drain the queued buffer before release
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
            throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Invalid Vita audio channel reservation"};
        auto& voice=voices[id];
        for(std::size_t other=0;other<voices.size();++other) {
            const auto& held=voices[other];
            if(!held.sample.instance)continue;
            if(other==id && held.sample.instance==operation.instance &&
                held.sample.generation==operation.instanceGeneration)continue;
            if(other<id+channels && id<other+held.mix.channels)
                throw AudioFailure{WORLD_AUDIO_STALE_REFERENCE,"Vita audio channels still have a previous owner"};
        }
        check(library.Voices().Assign(operation,id),"Assign Vita audio voice");
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
        check(library.Voices().CompleteUpdate(operation,snapshot),"Observe Vita audio voices");
        for(std::size_t i=0;i<voices.size();++i) {
            const auto& voice=voices[i];
            auto* sample=voice.sample.instance;
            if(snapshot[i].prebuffered && sample && sample->occupied && !sample->retiring &&
                sample->generation==voice.sample.generation && !sample->prebuffered &&
                sample->playSequence==voice.sequence)
                check(sample->sampleType==1?worldAudioSpeechEvent(&queue,voice.sample,voice.sequence,4):
                    worldAudioStreamEvent(&queue,voice.sample,voice.sequence,4),"Notify Vita stream readiness");
        }
    }

    // Stream decodes for the next request, done before taking the voice
    // lock so neither the mixer nor the game thread waits on a decode.
    using JukeboxStart=decltype(PrepareJukeboxStream(CaptureStreamDecodeRange(std::declval<const WorldAudioRequest&>())));
    std::optional<PcmWave> predecoded;
    std::shared_ptr<AsyncResult<JukeboxStart>> jukeboxOpen;
    uint64_t jukeboxOpenSequence{};
    AudioWorker worker;
    void PreDecode(const WorldAudioRequest& operation)
    {
        predecoded.reset();
        if(operation.instance && operation.instance->retiring && operation.kind!=WORLD_AUDIO_STOP)return;
        switch(operation.kind) {
        case WORLD_AUDIO_OPEN_STREAM:
            if(!operation.streamSource || !operation.instance || operation.streamSource->movie)return;
            // Jukebox opens decode on the worker (Apply returns Blocked meanwhile).
            if(operation.instance->completionKind!=WORLD_AUDIO_COMPLETION_JUKEBOX)
                predecoded.emplace(DecodeSoundStreamRange(operation));
            return;
        case WORLD_AUDIO_APPEND_CHAIN:
        case WORLD_AUDIO_OPEN_CHAIN:
            predecoded.emplace(DecodeSoundStreamRange(operation));
            return;
        default:
            return;
        }
    }
    PcmWave DecodedRange(const WorldAudioRequest& operation)
    {
        if(predecoded) {auto pcm=std::move(*predecoded);predecoded.reset();return pcm;}
        return DecodeSoundStreamRange(operation);
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
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Invalid Vita audio bank load"};
            check(library.CompleteRegion(operation,true),"Complete Vita audio bank load");
            break;
        case WORLD_AUDIO_UNLOAD_BANK:
            for(auto& voice:voices) {
                if(voice.sample.instance && voice.bank==operation.bank) {
                    library.Voices().ReleaseSample(voice.sample);
                    voice=Voice{};
                }
            }
            if(banks.erase(operation.bank)!=1)
                throw AudioFailure{WORLD_AUDIO_STALE_REFERENCE,"Vita audio bank was already unloaded"};
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
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"PLAY has no buffered Vita stream"};
            }
            check(library.Voices().Assign(operation,operation.reservedVoiceId),"Start Vita audio voice");
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
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"OPEN has no retained Vita stream"};
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
                if(jukeboxOpen && jukeboxOpenSequence!=operation.sequence) {
                    // A different open owns the slot; a finished orphan is dropped.
                    if(!jukeboxOpen->done.load(std::memory_order_acquire))return NativeAudioApply::Blocked;
                    jukeboxOpen.reset();
                }
                if(!jukeboxOpen) {
                    auto range=CaptureStreamDecodeRange(operation);
                    jukeboxOpen=std::make_shared<AsyncResult<JukeboxStart>>();
                    jukeboxOpenSequence=operation.sequence;
                    worker.Post([job=jukeboxOpen,range=std::move(range)]() {
                        try {job->value.emplace(PrepareJukeboxStream(range));}
                        catch(...) {job->error=std::current_exception();}
                        job->done.store(true,std::memory_order_release);
                    });
                    return NativeAudioApply::Blocked;
                }
                if(!jukeboxOpen->done.load(std::memory_order_acquire))return NativeAudioApply::Blocked;
                auto opened=std::move(jukeboxOpen);
                auto stream=opened->Take();
                auto& voice=Allocate(operation,stream.first.pcm.channels);
                voice.music=std::move(stream.stream);
                voice.pendingRead.reset();
                voice.musicFinished=stream.first.last;
                voice.Append(std::move(stream.first.pcm));
                voice.opened=true;
            } else {
                auto pcm=DecodedRange(operation);
                auto& voice=Allocate(operation,pcm.channels);
                voice.Append(std::move(pcm));
                voice.opened=true;
            }
            break;
        }
        case WORLD_AUDIO_APPEND_CHAIN:
        case WORLD_AUDIO_OPEN_CHAIN: {
            auto pcm=DecodedRange(operation);
            auto* voice=Find(operation);
            if(!voice) {
                if(operation.kind==WORLD_AUDIO_APPEND_CHAIN)
                    throw AudioFailure{WORLD_AUDIO_STALE_REFERENCE,"Vita speech append has no primed voice"};
                voice=&Allocate(operation,pcm.channels);
            }
            if(voice->chunks.empty())voice->cursor=0;
            if(voice->chunks.size()>=64)
                throw AudioFailure{WORLD_AUDIO_CAPACITY,"Vita speech chain exceeds PCM buffer capacity"};
            voice->Append(std::move(pcm));
            if(operation.kind==WORLD_AUDIO_OPEN_CHAIN) {
                check(library.Voices().Assign(operation,operation.reservedVoiceId),"Open Vita speech chain");
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
                voice->pendingRead.reset();
                voice->musicFinished=false;
                voice->cursor=0;
                if(operation.releaseDeviceVoice)*voice=Voice{};
            }
            check(library.Voices().CompleteStop(operation),"Stop Vita audio voice");
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
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Invalid Vita audio mode"};
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
            throw AudioFailure{WORLD_AUDIO_BACKEND_FAILURE,"Unsupported Vita audio request"};
        }
        return NativeAudioApply::Applied;
    }

    bSound& library;
    WorldAudioRequestQueue& queue;
    NativeAudioCommandPump commands;
    AudioStatistics statistics;
    std::map<const WorldAudioBank*,NativeSoundBankCache> banks;
    std::array<Voice,48> voices{};
    std::array<OutputBuffer,2> buffers{};
    std::mutex mutex;
    std::atomic<bool> running{false};
    pthread_t thread{};
    bool threadStarted{};
    int port{-1};
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
        // Music decode runs outside the voice lock (the output thread keeps
        // mixing); only the chunk append is locked. Voices are reassigned
        // only by commands on this thread, so the pointer stays valid.
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            for(auto& voice:state_->voices) {
                if(voice.pendingRead && voice.pendingRead->result.done.load(std::memory_order_acquire)) {
                    auto read=std::move(voice.pendingRead);
                    if(read->stream==voice.music) {
                        auto chunk=read->result.Take();
                        voice.musicFinished=chunk.last;
                        voice.Append(std::move(chunk.pcm));
                    }
                }
                if(!voice.NeedsMusic())continue;
                auto read=std::make_shared<MusicRead>();
                read->stream=voice.music;
                voice.pendingRead=read;
                state_->worker.Post([read]() {
                    try {read->result.value.emplace(read->stream->Read());}
                    catch(...) {read->result.error=std::current_exception();}
                    read->result.done.store(true,std::memory_order_release);
                });
            }
        }
        const auto commandsStarted=std::chrono::steady_clock::now();
        // Only this thread runs commands; the voice lock covers each apply
        // (what the mixer reads), not the stream decode that precedes it.
        check(state_->commands.Pump(state_->queue,256,[this](const auto& request) {
            state_->PreDecode(request);
            std::lock_guard<std::mutex> lock(state_->mutex);
            return state_->Apply(request);
        }),"Pump retained Vita audio commands");
        std::lock_guard<std::mutex> statisticsLock(state_->mutex);
        state_->statistics.decodeMilliseconds+=std::chrono::duration<double,std::milli>(commandsStarted-decodeStarted).count();
        state_->statistics.commandMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-commandsStarted).count();
        if(!state_->initialized)
            throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Missing Vita audio initialize request"};
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
    std::lock_guard<std::mutex> lock(state_->mutex);
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
