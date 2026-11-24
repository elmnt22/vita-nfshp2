#include "audio.hpp"

#include "bsound/include/NativeEnvelope.hpp"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <optional>
extern int vitaWatchedThreads[5];
#include <utility>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <deque>
#include <unordered_map>
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

// SPU2 4-point Gaussian interpolation (the PS2 voice resampler). Weights are
// the hardware table scaled to float; four weights sum to ~0x7F80/0x8000.
constexpr int16_t gaussTable[512]={
#include "spu2_gauss.inc"
};
struct GaussWeights {
    alignas(16) float w[256][4];
    GaussWeights()
    {
        for(int i=0;i<256;++i) {
            w[i][0]=gaussTable[0xFF-i]/32768.0f;w[i][1]=gaussTable[0x1FF-i]/32768.0f;
            w[i][2]=gaussTable[0x100+i]/32768.0f;w[i][3]=gaussTable[i]/32768.0f;
        }
    }
};
const GaussWeights gaussWeights;
bool linearInterpolation=false; // audio_linear.txt restores the 2-point blend
bool reverbDisabled=false;      // audio_dry.txt mixes dry only (previous behaviour)
bool reverbForced=false;        // audio_reverb_force.txt: diagnostic, 0.85 depth everywhere

// SPU2 reverb, one per core. The PS2 runs it at half the 48 kHz output rate
// with the 39-tap half-band FIR on both sides. Formula and presets are the
// documented SPU ones: register offsets in 8-byte units, work area in
// halfwords, products divided by 0x8000, stores saturated to int16.
constexpr int16_t reverbFir[39]={-1,0,2,0,-10,0,35,0,-103,0,266,0,-616,0,1332,0,-2960,0,10246,16384,
    10246,0,-2960,0,1332,0,-616,0,266,0,-103,0,35,0,-10,0,2,0,-1};
// The FIR taps the filters below actually use, as floats (no per-tap
// conversion): the 20 even taps and the centre tap.
struct ReverbFirTaps {
    float even[20];float centre;
    constexpr ReverbFirTaps():even{},centre(reverbFir[19]) {for(int t=0;t<20;++t)even[t]=reverbFir[2*t];}
};
constexpr ReverbFirTaps reverbTaps{};
struct ReverbPreset {uint32_t bytes;uint16_t r[32];};
// Order: dAPF1 dAPF2 vIIR vCOMB1-4 vWALL vAPF1 vAPF2 mLSAME mRSAME mLCOMB1 mRCOMB1
// mLCOMB2 mRCOMB2 dLSAME dRSAME mLDIFF mRDIFF mLCOMB3 mRCOMB3 mLCOMB4 mRCOMB4
// dLDIFF dRDIFF mLAPF1 mRAPF1 mLAPF2 mRAPF2 vLIN vRIN.
constexpr ReverbPreset reverbHall{0xADE0,{0x01A5,0x0139,0x6000,0x5000,0x4C00,0xB800,0xBC00,0xC000,0x6000,0x5C00,
    0x15BA,0x11BB,0x14C2,0x10BD,0x11BC,0x0DC1,0x11C0,0x0DC3,0x0DC0,0x09C1,0x0BC4,0x07C1,0x0A00,0x06CD,
    0x09C2,0x05C1,0x05C0,0x041A,0x0274,0x013A,0x8000,0x8000}};
constexpr ReverbPreset reverbSpace{0xF6C0,{0x033D,0x0231,0x7E00,0x5000,0xB400,0xB000,0x4C00,0xB000,0x6000,0x5400,
    0x1ED6,0x1A31,0x1D14,0x183B,0x1BC2,0x16B2,0x1A32,0x15EF,0x15EE,0x1055,0x1334,0x0F2D,0x11F6,0x0C5D,
    0x1056,0x0AE1,0x0AE0,0x07A2,0x0464,0x0232,0x8000,0x8000}};
const ReverbPreset* FindReverbPreset(int mode) noexcept
{
    // libsd SD_REV_MODE: 5 HALL, 6 SPACE (the mode this game emits).
    return mode==6 ? &reverbSpace:mode==5 ? &reverbHall:nullptr;
}

struct Spu2Reverb {
    const ReverbPreset* preset{};
    std::vector<int16_t> work;
    uint32_t pos{};
    int32_t v[10]{};      // dAPF1 dAPF2 (halfwords), vIIR vCOMB1-4 vWALL vAPF1 vAPF2
    uint32_t m[20]{};     // address registers in halfwords
    uint32_t b[8]{};      // same/diff previous samples and APF delayed taps
    bool idle{true};      // skipped while the return depth is zero
    int32_t vLIN{},vRIN{};
    static constexpr std::size_t H=38,UH=19;
    std::array<float,H+framesPerBuffer> inL{},inR{};      // 48 kHz send, FIR history first
    std::array<float,UH+framesPerBuffer/2> outL{},outR{}; // 24 kHz wet, FIR history first

    void Configure(const ReverbPreset* next)
    {
        if(next==preset)return;
        preset=next;
        if(!preset) {work.clear();return;}
        const auto& r=preset->r;
        v[0]=r[0]*4;v[1]=r[1]*4;
        for(int i=2;i<10;++i)v[i]=static_cast<int16_t>(r[i]);
        for(int i=0;i<20;++i)m[i]=r[10+i]*4u;
        vLIN=static_cast<int16_t>(r[30]);vRIN=static_cast<int16_t>(r[31]);
        work.assign(preset->bytes/2,0);pos=0;
        // Offsets wrap once here: the per-sample path then needs no division
        // (the Cortex-A9 has no hardware divide).
        const auto size=static_cast<uint32_t>(work.size());
        for(auto& offset:m)offset%=size;
        const auto back=[size](uint32_t offset,uint32_t by){return (offset+size-by%size)%size;};
        enum {mLSAME,mRSAME,mLDIFF=8,mRDIFF,mLAPF1=16,mRAPF1,mLAPF2,mRAPF2};
        b[0]=back(m[mLSAME],1);b[1]=back(m[mRSAME],1);b[2]=back(m[mLDIFF],1);b[3]=back(m[mRDIFF],1);
        b[4]=back(m[mLAPF1],v[0]);b[5]=back(m[mRAPF1],v[0]);b[6]=back(m[mLAPF2],v[1]);b[7]=back(m[mRAPF2],v[1]);
        Clear();
    }
    void Clear()
    {
        std::fill(work.begin(),work.end(),int16_t{0});
        inL.fill(0);inR.fill(0);outL.fill(0);outR.fill(0);
    }
    static int32_t Mul(int32_t a,int32_t b) noexcept {return (a*b)>>15;}
    static int32_t Sat(int32_t a) noexcept {return std::clamp<int32_t>(a,-32768,32767);}
    uint32_t Index(uint32_t offset) const noexcept
    {
        const auto size=static_cast<uint32_t>(work.size());
        const auto index=pos+offset; // offset < size
        return index>=size ? index-size:index;
    }
    int32_t Read(uint32_t offset) const noexcept {return work[Index(offset)];}
    void Write(uint32_t offset,int32_t value) noexcept {work[Index(offset)]=static_cast<int16_t>(Sat(value));}
    void Step(int32_t inputL,int32_t inputR,int32_t& outputL,int32_t& outputR) noexcept
    {
        enum {mLSAME,mRSAME,mLCOMB1,mRCOMB1,mLCOMB2,mRCOMB2,dLSAME,dRSAME,mLDIFF,mRDIFF,
            mLCOMB3,mRCOMB3,mLCOMB4,mRCOMB4,dLDIFF,dRDIFF,mLAPF1,mRAPF1,mLAPF2,mRAPF2};
        const int32_t dAPF1=v[0],dAPF2=v[1],vIIR=v[2],vC1=v[3],vC2=v[4],vC3=v[5],vC4=v[6],
            vWALL=v[7],vAPF1=v[8],vAPF2=v[9];
        const int32_t Lin=Mul(vLIN,inputL),Rin=Mul(vRIN,inputR);
        {
            // Same-side and different-side reflections (IIR through the walls).
            const int32_t prevL=Read(b[0]),prevR=Read(b[1]);
            const int32_t sameL=Mul(Lin+Mul(Read(m[dLSAME]),vWALL)-prevL,vIIR)+prevL;
            const int32_t sameR=Mul(Rin+Mul(Read(m[dRSAME]),vWALL)-prevR,vIIR)+prevR;
            const int32_t dprevL=Read(b[2]),dprevR=Read(b[3]);
            const int32_t diffL=Mul(Lin+Mul(Read(m[dRDIFF]),vWALL)-dprevL,vIIR)+dprevL;
            const int32_t diffR=Mul(Rin+Mul(Read(m[dLDIFF]),vWALL)-dprevR,vIIR)+dprevR;
            Write(m[mLSAME],sameL);Write(m[mRSAME],sameR);
            Write(m[mLDIFF],diffL);Write(m[mRDIFF],diffR);
        }
        // Early echo: four combs.
        int32_t l=Mul(vC1,Read(m[mLCOMB1]))+Mul(vC2,Read(m[mLCOMB2]))+Mul(vC3,Read(m[mLCOMB3]))+Mul(vC4,Read(m[mLCOMB4]));
        int32_t r=Mul(vC1,Read(m[mRCOMB1]))+Mul(vC2,Read(m[mRCOMB2]))+Mul(vC3,Read(m[mRCOMB3]))+Mul(vC4,Read(m[mRCOMB4]));
        // Late reverb: two all-pass stages.
        {
            const int32_t al=Read(b[4]),ar=Read(b[5]);
            l=Sat(l-Mul(vAPF1,al));r=Sat(r-Mul(vAPF1,ar));
            Write(m[mLAPF1],l);Write(m[mRAPF1],r);
            l=Mul(l,vAPF1)+al;r=Mul(r,vAPF1)+ar;
        }
        {
            const int32_t al=Read(b[6]),ar=Read(b[7]);
            l=Sat(l-Mul(vAPF2,al));r=Sat(r-Mul(vAPF2,ar));
            Write(m[mLAPF2],l);Write(m[mRAPF2],r);
            l=Mul(l,vAPF2)+al;r=Mul(r,vAPF2)+ar;
        }
        outputL=Sat(l);outputR=Sat(r);
        pos=pos+1>=work.size() ? 0:pos+1;
    }
    // wet: interleaved 48 kHz send mix (int16 scale). Adds gain * reverb return.
    void Process(const float* wet,float gainL,float gainR,std::array<float,framesPerBuffer*2>& output) noexcept
    {
        if(!preset || work.empty())return;
        constexpr std::size_t N=framesPerBuffer;
        for(std::size_t i=0;i<N;++i) {inL[H+i]=wet[2*i];inR[H+i]=wet[2*i+1];}
        // Decimate by 2: y[k] = sum c[t]*x[2k+1-t]; x[i] lives at in[H+i].
        for(std::size_t k=0;k<N/2;++k) {
            float sl=0,sr=0;
            const std::size_t newest=H+2*k+1;
            for(std::size_t t=0;t<20;++t) {sl+=reverbTaps.even[t]*inL[newest-2*t];sr+=reverbTaps.even[t]*inR[newest-2*t];}
            sl+=reverbTaps.centre*inL[newest-19];sr+=reverbTaps.centre*inR[newest-19];
            int32_t ol,orr;
            Step(Sat(static_cast<int32_t>(sl*(1.0f/32768.0f))),Sat(static_cast<int32_t>(sr*(1.0f/32768.0f))),ol,orr);
            outL[UH+k]=static_cast<float>(ol);outR[UH+k]=static_cast<float>(orr);
        }
        // Interpolate by 2: zero-stuffed z[2u]=out[u], y[n] = 2*sum c[t]*z[n-t].
        const float scaleL=gainL*(2.0f/32768.0f),scaleR=gainR*(2.0f/32768.0f);
        for(std::size_t n=0;n<N;++n) {
            float sl=0,sr=0;
            if(n&1) {
                // Only the centre tap meets an even z index.
                const auto u=static_cast<std::size_t>(static_cast<long>(UH)+(static_cast<long>(n)-19)/2); // n-19 even, >= -18
                sl=reverbTaps.centre*outL[u];sr=reverbTaps.centre*outR[u];
            } else {
                for(std::size_t t=0;t<20;++t) {
                    const std::size_t u=UH+n/2-t; // (n-2t)/2 + UH, >= 0
                    sl+=reverbTaps.even[t]*outL[u];sr+=reverbTaps.even[t]*outR[u];
                }
            }
            output[2*n]+=sl*scaleL;output[2*n+1]+=sr*scaleR;
        }
        std::copy(inL.end()-H,inL.end(),inL.begin());std::copy(inR.end()-H,inR.end(),inR.begin());
        std::copy(outL.end()-UH,outL.end(),outL.begin());std::copy(outR.end()-UH,outR.end(),outR.begin());
    }
};


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
    bool Running() const noexcept {return started_;}
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
        ::vitaWatchedThreads[4]=sceKernelGetThreadId();
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
    int effectCore{-1}; // reverb bus this voice sends to (authored MSH flag), -1 dry only
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

    void Mix(std::array<float,framesPerBuffer*2>& output,float* wet)
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
            const auto render=[&](auto channels,auto gauss,auto send) {
                constexpr std::size_t channelCount=decltype(channels)::value;
                while(frame<framesPerBuffer && cursor<end) {
                    const auto index=static_cast<std::size_t>(cursor);
                    const auto next=std::min(index+1,end-1);
                    const auto fraction=static_cast<float>(cursor-index);
                    if(!constantEnvelope)envelope.AdvanceFrames(1);
                    const auto gain=constantEnvelope ? constantGain:envelope.Gain();
                    float left=0,right=0;
                    for(std::size_t channel=0;channel<channelCount;++channel) {
                        float value;
                        if constexpr(decltype(gauss)::value) {
                            // SPU2 order: oldest, older, old, new around the cursor.
                            const auto& w=gaussWeights.w[static_cast<int>(fraction*256.0f)&255];
                            const auto previous=index ? index-1:0;
                            const auto after=std::min(index+2,end-1);
                            value=w[0]*pcm.samples[previous*channelCount+channel]+
                                w[1]*pcm.samples[index*channelCount+channel]+
                                w[2]*pcm.samples[next*channelCount+channel]+
                                w[3]*pcm.samples[after*channelCount+channel];
                        } else {
                            const float a=pcm.samples[index*channelCount+channel];
                            const float b=pcm.samples[next*channelCount+channel];
                            value=a+(b-a)*fraction;
                        }
                        value*=gain;
                        left+=value*mix.output[channel][0];
                        right+=value*mix.output[channel][1];
                    }
                    output[2*frame]+=left;output[2*frame+1]+=right;
                    if constexpr(decltype(send)::value) {wet[2*frame]+=left;wet[2*frame+1]+=right;}
                    cursor+=step;++frame;
                }
            };
            using One=std::integral_constant<std::size_t,1>;using Two=std::integral_constant<std::size_t,2>;
            using Yes=std::true_type;using No=std::false_type;
            const bool gauss=!linearInterpolation;
            if(pcm.channels==1) {
                if(wet) {if(gauss)render(One{},Yes{},Yes{});else render(One{},No{},Yes{});}
                else {if(gauss)render(One{},Yes{},No{});else render(One{},No{},No{});}
            } else {
                if(wet) {if(gauss)render(Two{},Yes{},Yes{});else render(Two{},No{},Yes{});}
                else {if(gauss)render(Two{},Yes{},No{});else render(Two{},No{},No{});}
            }
        }
        if(constantEnvelope)envelope.AdvanceFrames(frame);
    }
};

// Reverb control is written by commands under the voice lock; the output
// thread copies it per buffer and owns the reverb work areas.
// The game never submits a preset (SoundScape starts on its current one):
// like the PC device, each core starts in the only mode it selects, SPACE.
struct EffectControl {int preset{6};uint16_t volume{};int8_t pan{};uint8_t mode{2};bool enabled{true};uint32_t clears{};};

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
        const auto exists=[](const char* path) {if(auto* f=std::fopen(path,"rb")) {std::fclose(f);return true;} return false;};
        linearInterpolation=exists("ux0:data/NFSHP2/audio_linear.txt");
        reverbDisabled=exists("ux0:data/NFSHP2/audio_dry.txt");
        reverbForced=exists("ux0:data/NFSHP2/audio_reverb_force.txt");
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
        ::vitaWatchedThreads[3]=sceKernelGetThreadId();
        std::size_t current=0;
        while(running.load(std::memory_order_relaxed)) {
            auto& buffer=buffers[current];current=(current+1)%buffers.size();
            using Clock=std::chrono::steady_clock;
            const auto mixStarted=Clock::now();
            {
                // Lock per voice, not per buffer: a whole-buffer hold (~2.7 ms)
                // stalled every game-thread command apply behind the mixer.
                // Voices are a fixed array; commands replace entries only
                // under this same lock, so each voice mixes consistently.
                mixBuffer.fill(0.0f);
                std::array<EffectControl,2> control;
                {std::lock_guard<std::mutex> lock(mutex);control=effects;}
                if(reverbForced)for(auto& effect:control)effect.volume=std::max<uint16_t>(effect.volume,13926);
                // A core returns sound only at nonzero depth (the game sets it
                // only in tunnels / enclosed spots); at zero depth both the
                // sends and the reverb are skipped. Its work area restarts
                // empty when depth returns, under the game's 0.333/update ramp.
                bool live[2]{};
                for(std::size_t core=0;core<2;++core) {
                    auto& bus=reverbs[core];
                    const auto& effect=control[core];
                    bus.Configure(reverbDisabled ? nullptr:FindReverbPreset(effect.preset));
                    if(effect.clears!=reverbClears[core]) {reverbClears[core]=effect.clears;bus.Clear();}
                    live[core]=bus.preset && effect.enabled && effect.volume;
                    if(live[core] && bus.idle)bus.Clear();
                    bus.idle=!live[core];
                    if(live[core])wetBuffers[core].fill(0.0f);
                }
                std::uint32_t active=0,sending=0;
                for(auto& voice:voices) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(voice.playing && !voice.paused && !voice.chunks.empty()) {
                        ++active;
                        float* wet=nullptr;
                        if(voice.effectCore>=0 && voice.effectCore<2 && live[voice.effectCore]) {
                            wet=wetBuffers[static_cast<std::size_t>(voice.effectCore)].data();++sending;
                        }
                        voice.Mix(mixBuffer,wet);
                    }
                }
                std::uint32_t processed=0;
                for(std::size_t core=0;core<2;++core) {
                    auto& bus=reverbs[core];
                    const auto& effect=control[core];
                    if(!live[core])continue;
                    // PAL267270 effect return: linear +/-129 pan on the 14-bit depth.
                    const int pan=effect.mode<2 ? 0:std::max(-127,static_cast<int>(effect.pan));
                    const int left=pan>0 ? std::max(0,static_cast<int>(double(pan)*-129.0+effect.volume)):effect.volume;
                    const int right=pan<0 ? std::max(0,static_cast<int>(double(pan)*129.0+effect.volume)):effect.volume;
                    bus.Process(wetBuffers[core].data(),left/16383.0f,right/16383.0f,mixBuffer);
                    ++processed;
                }
                std::lock_guard<std::mutex> lock(mutex);
                statistics.reverbBuffers+=processed;statistics.wetVoiceBuffers+=sending;
                for(std::size_t core=0;core<2;++core) {
                    statistics.reverbPreset[core]=control[core].preset;statistics.reverbVolume[core]=control[core].volume;
                }
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

    EffectControl& EffectCore(const WorldAudioRequest& operation)
    {
        if(operation.effectCore<0 || operation.effectCore>=static_cast<int32_t>(effects.size()))
            throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Effect command has no Vita core"};
        return effects[static_cast<std::size_t>(operation.effectCore)];
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
        // VoicePlay 2625B8..2625F4: the authored MSH+26 halfword enables the
        // core's reverb send; PlayChain (speech, sampleType 1) never does.
        const auto* cue=operation.cue;
        if(operation.instance->sampleType!=1 && cue && cue->metadata && cue->metadataSize==52 &&
            (cue->metadata[0x26] || cue->metadata[0x27]))
            voice.effectCore=static_cast<int>(id/24);
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
    std::shared_ptr<AsyncResult<JukeboxStart>> jukeboxOpen;
    uint64_t jukeboxOpenSequence{};
    AudioWorker worker;
    void PreDecode(const WorldAudioRequest&) {}
    // Stream/chain ranges (speech, scanner) read the memory card and decode
    // ADPCM on the worker; Apply returns Blocked until this request's PCM is
    // ready, so the game thread never waits on file I/O. Order is unchanged.
    // Keyed by request sequence: the queue may apply later independent
    // requests while an earlier one is still Blocked on its decode.
    std::unordered_map<uint64_t,std::shared_ptr<AsyncResult<PcmWave>>> streamDecodes;
    bool DecodedRange(const WorldAudioRequest& operation,PcmWave& pcm)
    {
        auto found=streamDecodes.find(operation.sequence);
        if(found==streamDecodes.end()) {
            auto range=CaptureStreamDecodeRange(operation);
            auto job=std::make_shared<AsyncResult<PcmWave>>();
            found=streamDecodes.emplace(operation.sequence,job).first;
            worker.Post([job,range=std::move(range)]() {
                try {job->value.emplace(DecodeStreamRangeSnapshot(range));}
                catch(...) {job->error=std::current_exception();}
                job->done.store(true,std::memory_order_release);
            });
        }
        if(!found->second->done.load(std::memory_order_acquire))return false;
        auto job=std::move(found->second);
        streamDecodes.erase(found);
        pcm=job->Take();
        return true;
    }
    NativeAudioApply Apply(const WorldAudioRequest& operation)
    {
        if(operation.instance && operation.instance->retiring && operation.kind!=WORLD_AUDIO_STOP) {
            // A cancelled request never returns for its decode; finished
            // jobs are dropped here, unfinished ones when they complete.
            streamDecodes.erase(operation.sequence);
            // An in-flight cue decode stays tracked until done (bank unload waits on it).
            if(const auto cue=cueDecodes.find(operation.sequence);
                cue!=cueDecodes.end() && cue->second->done.load(std::memory_order_acquire))cueDecodes.erase(cue);
            return NativeAudioApply::Cancelled;
        }
        switch(operation.kind) {
        case WORLD_AUDIO_INITIALIZE_DEVICE:
            StartOutput();
            break;
        case WORLD_AUDIO_LOAD_REGION:
            if(!operation.bank || !banks.try_emplace(operation.bank,*operation.bank).second)
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Invalid Vita audio bank load"};
            {
                auto warm=std::make_shared<BankWarm>();
                const auto& cache=banks.find(operation.bank)->second;
                warm->total=cache.CueCount();
                bankWarms[operation.bank]=warm;
                PostWarmSlice(&cache,warm);
            }
            check(library.CompleteRegion(operation,true),"Complete Vita audio bank load");
            break;
        case WORLD_AUDIO_UNLOAD_BANK:
            for(auto& voice:voices) {
                if(voice.sample.instance && voice.bank==operation.bank) {
                    library.Voices().ReleaseSample(voice.sample);
                    voice=Voice{};
                }
            }
            CancelWarm(operation.bank);
            // Worker cue decodes read this bank's bytes; let them finish.
            for(auto it=cueDecodes.begin();it!=cueDecodes.end();) {
                if(it->second->bank!=operation.bank) {++it;continue;}
                // A stopped worker has joined: queued jobs were dropped unrun.
                while(!it->second->done.load(std::memory_order_acquire) && worker.Running())sceKernelDelayThread(200);
                it=cueDecodes.erase(it);
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
                // A cue that is not cached decodes on the worker; the
                // voice starts on a later pump instead of stalling this one.
                std::size_t cueIndex=0;
                auto pcm=bank->second.TryAcquire(address-base,cueIndex);
                if(!pcm) {
                    auto found=cueDecodes.find(operation.sequence);
                    if(found==cueDecodes.end()) {
                        auto job=std::make_shared<CueDecode>();
                        job->bank=operation.bank;
                        found=cueDecodes.emplace(operation.sequence,job).first;
                        const auto* cache=&bank->second;
                        worker.Post([job,cache,cueIndex]() {
                            auto decoded=cache->DecodeResidentRange(cueIndex,1);
                            if(!decoded.empty())job->pcm=std::move(decoded.front());
                            job->done.store(true,std::memory_order_release);
                        });
                    }
                    if(!found->second->done.load(std::memory_order_acquire))return NativeAudioApply::Blocked;
                    auto job=std::move(found->second);
                    cueDecodes.erase(found);
                    // A failed worker decode reports through the original path.
                    pcm=job->pcm ? bank->second.Adopt(cueIndex,std::move(job->pcm)):bank->second.Acquire(address-base);
                }
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
                PcmWave pcm;
                if(!DecodedRange(operation,pcm))return NativeAudioApply::Blocked;
                auto& voice=Allocate(operation,pcm.channels);
                voice.Append(std::move(pcm));
                voice.opened=true;
            }
            break;
        }
        case WORLD_AUDIO_APPEND_CHAIN:
        case WORLD_AUDIO_OPEN_CHAIN: {
            PcmWave pcm;
            if(!DecodedRange(operation,pcm))return NativeAudioApply::Blocked;
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
            for(auto& effect:effects)effect.mode=mode;
            break;
        case WORLD_AUDIO_SET_REGION_PATH:
        case WORLD_AUDIO_SET_CHANNEL_VOLUME:
        case WORLD_AUDIO_FADE_VOLUME:
            // The engine owns these publications and emits actual per-voice changes.
            break;
        case WORLD_AUDIO_SET_CORE_EFFECT_VOLUME:
            EffectCore(operation).volume=operation.effectCoreVolume;break;
        case WORLD_AUDIO_SET_CORE_EFFECT_PAN:
            EffectCore(operation).pan=operation.effectCorePan;break;
        case WORLD_AUDIO_SET_CORE_EFFECT_ATTRIBUTE: {
            auto& effect=EffectCore(operation);
            switch(operation.effectParameter) {
            case 1:effect.preset=operation.effectPreset;effect.enabled=false;break; // mode; STREAM re-enables
            case 2:effect.volume=static_cast<uint16_t>(operation.effectPreset);break; // PAL265518 depth
            case 3: {
                // PAL26553C subtracts 127 from the low byte, then stores a byte.
                const auto bits=static_cast<uint8_t>(static_cast<uint8_t>(operation.effectPreset)-127u);
                std::memcpy(&effect.pan,&bits,sizeof(bits));break;
            }
            case 4:case 5:break; // delay/feedback only shape ECHO/DELAY modes
            default:throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Unknown Vita effect attribute"};
            }
            break;
        }
        case WORLD_AUDIO_SET_CORE_EFFECT_ENABLED: {
            if(operation.effectPreset!=0 && operation.effectPreset!=1)
                throw AudioFailure{WORLD_AUDIO_INVALID_BINDING,"Invalid Vita effect enable value"};
            auto& effect=EffectCore(operation);
            // STREAM EnableEffects clears the core's work area before enabling.
            if(operation.effectPreset)++effect.clears;
            effect.enabled=operation.effectPreset!=0;break;
        }
        default:
            throw AudioFailure{WORLD_AUDIO_BACKEND_FAILURE,"Unsupported Vita audio request"};
        }
        return NativeAudioApply::Applied;
    }

    bSound& library;
    WorldAudioRequestQueue& queue;
    struct CueDecode {
        const WorldAudioBank* bank{};
        std::shared_ptr<const PcmWave> pcm;
        std::atomic<bool> done{false};
    };
    std::unordered_map<uint64_t,std::shared_ptr<CueDecode>> cueDecodes;
    NativeAudioCommandPump commands;
    AudioStatistics statistics;
    std::map<const WorldAudioBank*,NativeSoundBankCache> banks;
    // Cue warm-up: after a bank loads, the worker decodes its resident cues
    // in small slices (music reads interleave between slices) and the game
    // thread installs them in Pump, so first plays no longer decode inline.
    struct BankWarm {
        std::atomic<bool> cancel{false},busy{true};
        std::mutex mutex;
        std::vector<std::pair<std::size_t,std::vector<std::shared_ptr<const PcmWave>>>> ready;
        std::size_t next{},total{};
    };
    std::map<const WorldAudioBank*,std::shared_ptr<BankWarm>> bankWarms;
    void PostWarmSlice(const NativeSoundBankCache* cache,std::shared_ptr<BankWarm> warm)
    {
        worker.Post([this,cache,warm]() {
            constexpr std::size_t slice=8;
            if(warm->cancel.load(std::memory_order_acquire) || warm->next>=warm->total) {
                warm->busy.store(false,std::memory_order_release);return;
            }
            const auto first=warm->next;
            auto decoded=cache->DecodeResidentRange(first,slice);
            {std::lock_guard<std::mutex> lock(warm->mutex);warm->ready.emplace_back(first,std::move(decoded));}
            warm->next=first+slice;
            if(warm->next<warm->total && !warm->cancel.load(std::memory_order_acquire))PostWarmSlice(cache,warm);
            else warm->busy.store(false,std::memory_order_release);
        });
    }
    void InstallWarmedCues()
    {
        for(auto it=bankWarms.begin();it!=bankWarms.end();) {
            auto& warm=*it->second;
            std::vector<std::pair<std::size_t,std::vector<std::shared_ptr<const PcmWave>>>> ready;
            {std::lock_guard<std::mutex> lock(warm.mutex);ready.swap(warm.ready);}
            const auto bank=banks.find(it->first);
            if(bank!=banks.end())for(auto& item:ready)bank->second.InstallDecoded(item.first,std::move(item.second));
            if(!warm.busy.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(warm.mutex);
                if(warm.ready.empty()) {it=bankWarms.erase(it);continue;}
            }
            ++it;
        }
    }
    void CancelWarm(const WorldAudioBank* bank)
    {
        const auto found=bankWarms.find(bank);
        if(found==bankWarms.end())return;
        found->second->cancel.store(true,std::memory_order_release);
        // At most one slice (8 cues) is in flight; the bank must outlive it.
        while(found->second->busy.load(std::memory_order_acquire))sceKernelDelayThread(200);
        bankWarms.erase(found);
    }
    std::array<Voice,48> voices{};
    std::array<OutputBuffer,2> buffers{};
    std::mutex mutex;
    std::atomic<bool> running{false};
    pthread_t thread{};
    bool threadStarted{};
    int port{-1};
    std::array<float,framesPerBuffer*2> mixBuffer{};
    // Reverb control is written by commands under `mutex`; the output thread
    // copies it per buffer and owns the reverb work areas.
    std::array<EffectControl,2> effects{};
    std::array<Spu2Reverb,2> reverbs{};
    std::array<uint32_t,2> reverbClears{};
    std::array<std::array<float,framesPerBuffer*2>,2> wetBuffers{};
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
        const auto installStarted=std::chrono::steady_clock::now();
        state_->InstallWarmedCues();
        const auto commandsStarted=std::chrono::steady_clock::now();
        std::uint32_t requests=0;
        double lockWaitMs=0,applySumMs=0;
        // Only this thread runs commands. One voice-lock hold covers the whole
        // batch: per-request locking paid a cross-core wakeup behind the
        // mixer each time (3-4 ms per pump). The mixer has a 21 ms buffer of
        // slack, and no apply does file I/O or decode (those are on the worker).
        const auto lockStarted=std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> batchLock(state_->mutex);
        lockWaitMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-lockStarted).count();
        state_->statistics.lockWaitMilliseconds+=lockWaitMs;
        check(state_->commands.Pump(state_->queue,256,[this,&requests,&applySumMs](const auto& request) {
            const auto preStarted=std::chrono::steady_clock::now();
            state_->PreDecode(request);
            ++requests;
            const auto applyStarted=std::chrono::steady_clock::now();
            const auto applied=state_->Apply(request);
            const auto applyFinished=std::chrono::steady_clock::now();
            const auto applyMs=std::chrono::duration<double,std::milli>(applyFinished-applyStarted).count();
            applySumMs+=applyMs;
            {
                auto& stats=state_->statistics;
                stats.preDecodeMilliseconds+=std::chrono::duration<double,std::milli>(applyStarted-preStarted).count();
                stats.applyMilliseconds+=applyMs;++stats.requests;
                const auto kind=static_cast<std::size_t>(request.kind);
                if(kind<40) {stats.kindMilliseconds[kind]+=applyMs;++stats.kindCounts[kind];}
            }
            if(applyMs>state_->statistics.applyPeakMilliseconds) {
                state_->statistics.applyPeakMilliseconds=applyMs;
                state_->statistics.applyPeakKind=static_cast<int>(request.kind);
            }
            return applied;
        }),"Pump retained Vita audio commands");
        state_->statistics.decodeMilliseconds+=std::chrono::duration<double,std::milli>(commandsStarted-decodeStarted).count();
        const auto finished=std::chrono::steady_clock::now();
        state_->statistics.commandMilliseconds+=std::chrono::duration<double,std::milli>(finished-commandsStarted).count();
        {
            using Ms=std::chrono::duration<double,std::milli>;
            const auto total=Ms(finished-decodeStarted).count();
            auto& stats=state_->statistics;
            if(total>stats.pumpPeakMilliseconds) {
                stats.pumpPeakMilliseconds=total;
                stats.pumpPeakMusicMs=Ms(installStarted-decodeStarted).count();
                stats.pumpPeakInstallMs=Ms(commandsStarted-installStarted).count();
                stats.pumpPeakCommandsMs=Ms(finished-commandsStarted).count();
                stats.pumpPeakRequests=requests;
                stats.pumpPeakLockWaitMs=lockWaitMs;stats.pumpPeakApplyMs=applySumMs;
            }
        }
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
