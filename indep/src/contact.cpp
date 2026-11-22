#include "indep/inc/Collision.hpp"
#include "indep/inc/Collision.hpp"
#include "indep/inc/Smokeable.hpp"
#include "indep/inc/Car.hpp"
#include "indep/inc/World.hpp"
#include "indep/inc/Munitions.hpp"
#include <limits.h>
#include "indep/inc/Contact.hpp"
#include <stddef.h>
#include "indep/inc/VoronoiClip.hpp"
#include "indep/inc/Contact.hpp"
#include "bware/inc/bMath.hpp"
#include "bware/inc/bMath.hpp"
#include "indep/inc/Contact.hpp"

#include <math.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <stdint.h>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#else
#include <condition_variable>
#include <mutex>
#include <thread>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#define HP2_SPEC_MXCSR 1
#endif
#endif

int32_t collisionWitnessQueryDetached(CollisionWitness* witness, const float firstFrame[16],
    const float firstPosition[4], const float secondFrame[16], const float secondPosition[4],
    uint32_t* randomState, uint64_t counters[4]);
int32_t collisionWitnessQueryAbortable(CollisionWitness* witness, const float firstFrame[16],
    const float firstPosition[4], const float secondFrame[16], const float secondPosition[4],
    uint32_t* randomState, uint64_t counters[4], const std::atomic<int>* stop);
int32_t collisionWitnessQueryPredict(CollisionWitness* witness, const float firstFrame[16],
    const float firstPosition[4], const float secondFrame[16], const float secondPosition[4],
    int32_t* pending, uint64_t counters[4]);
// Contact pipeline (exact): a whole queue pass's find-time and separation
// queries are published at once in random-state order. Vita
// contact_pipe_off.txt / PC HP2_CONTACT_PIPE=0.
int collisionContactPipeline=[] {
#if !defined(__vita__) && !defined(__SWITCH__)
    const char* value=getenv("HP2_CONTACT_PIPE");return value ? atoi(value):1;
#else
    return 1;
#endif
}();
uint64_t collisionPipeZeroKind[6]={},collisionPipeRelayouts=0;
uint64_t collisionPipeLong=0,collisionPipeZeroDraw=0,collisionPipeBehind[9]={};
uint64_t collisionPipeMissPair=0,collisionPipeMissStart=0,collisionPipeShort=0,collisionPipeDrawDelta[4]={0,0,0,0};
uint64_t collisionPipeContacts=0,collisionPipeEntryMisses=0,collisionPipeBreaks=0,collisionPipeTasks=0,collisionPipeUsed=0,collisionPipePasses=0;
// Speculative rounds stop queries the caller will discard (exact: a stopped
// query is never committed). Vita spec_abort_off.txt / PC HP2_SPEC_ABORT=0.
int collisionSpeculativeAbort=[] {
#if !defined(__vita__) && !defined(__SWITCH__)
    const char* value=getenv("HP2_SPEC_ABORT");return value ? atoi(value):1;
#else
    return 1;
#endif
}();
uint64_t collisionSpecAborted=0;
extern uint64_t collisionWitnessQueries,collisionWitnessIterations,collisionWitnessRestarts,collisionWitnessExhausted;
extern double collisionWitnessMilliseconds;

// Speculative separation (exact): 0 serial, 1 speculative, 2 verify (serial
// result kept, speculative result compared), 3 verify for the first
// collisionSpeculationVerifyBudget separations, then speculative.
int collisionSpeculationMode=0;
extern int collisionWitnessBench;
void collisionWitnessBenchRecord(const CollisionWitness* witness,const float firstFrame[16],const float firstPosition[4],
    const float secondFrame[16],const float secondPosition[4],uint32_t state);
int collisionSeparatePipelined=1; // pipelined separation (Vita sep_pipe_off.txt: 0)
uint64_t collisionSpeculationVerifyBudget=0;
void collisionWitnessMemoBypass(bool bypass); // voronoiclip.cpp
#if !defined(__vita__)
#include <stdlib.h>
// PC: HP2_COLLISION_SPEC="<mode> [verify budget]".
static const int collisionSpeculationEnvironment=[] {
    if (const char* value=getenv("HP2_COLLISION_SPEC")) {
        char* end=nullptr;
        collisionSpeculationMode=(int)strtol(value,&end,10);
        if (end && *end) collisionSpeculationVerifyBudget=strtoull(end,nullptr,10);
    }
    return 0;
}();
#endif
// Diagnostics (main thread only).
uint64_t collisionSpecSeparations=0,collisionSpecAccepted=0,collisionSpecWasted=0,collisionSpecMispredicts=0,
    collisionSpecRounds=0,collisionSpecVerified=0,collisionSpecMismatches=0,
    collisionFindQueries=0,collisionSeparateQueries=0,collisionSeparateCalls=0;

extern uint32_t (*collisionWitnessClockMicros)();
// Solve split (main thread, diagnostics): 0 queue queries, 1 queue serial
// fallbacks, 2 find-time bisection, 3 separation, 4 contact resolve body,
// 5 resolve-loop pair finding, 6 helper pickup latency, 7 unused.
uint64_t collisionSplitMicros[8]{},collisionSplitCounts[8]{};
std::atomic<uint64_t> collisionPickupMicros{0},collisionPickupCount{0},collisionPublishCount{0};
// Per helper slot: batches joined, tasks run.
std::atomic<uint64_t> collisionSlotBatches[2]{},collisionSlotTasks[2]{};
static thread_local int collisionHelperSlot=-1;
extern int diagFineTimers;
uint32_t diagFineClock();
uint32_t collisionSplitNow()
{
    if(!diagFineTimers)return 0u;
    if(collisionWitnessClockMicros)return collisionWitnessClockMicros();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint32_t collisionPoolHotMicros=60; // needs the host clock (Vita); pool_hot.txt: "<us>" (0 off)
std::atomic<uint64_t> collisionPoolHelperTasks{0},collisionPoolHelperMicros{0};
uint64_t collisionPoolMainMicros=0;
uint64_t collisionPoolMainTasks=0,collisionPoolRuns=0,collisionPoolRunMicros=0,collisionPoolWaitMicros=0;
namespace {
// Two helper threads (Vita cores 1 and 2) that claim indices of the batch the
// main thread publishes. The main thread claims indices too and only waits
// for tasks a helper has actually started, so a helper that is not scheduled
// never stalls it.
class CollisionHelperPool {
public:
    static CollisionHelperPool& Instance()
    {
        static CollisionHelperPool* pool=new CollisionHelperPool(); // never destroyed (detached helpers)
        return *pool;
    }
    unsigned Helpers() const {return helpers_;}
    // While armed (the collision solve), helpers poll for batches instead of
    // sleeping: a semaphore wakeup costs about as much as a whole query.
    // They run below the render/GL/audio threads, so polling only uses idle
    // core time; the main thread never waits for a task nobody claimed.
    void Arm(bool on)
    {
        if(helpers_==0 || armed_.load()==on)return;
        armed_.store(on);
        if(on) {
#if defined(__vita__)
            sceKernelSignalSema(sema_,static_cast<int>(helpers_));
#else
            {std::lock_guard<std::mutex> lock(mutex_);pending_+=helpers_;}
            wakeup_.notify_all();
#endif
        }
    }
    // Pipelined batches: Publish opens a batch without running it, the
    // caller claims indices itself (ClaimOne) while it consumes results in
    // order, and Close stops further claims and waits for in-flight tasks.
    void Publish(unsigned count,void (*function)(void*,unsigned),void* context)
    {
        generation_=(generation_+1)&0xffffu;
        state_.store((generation_<<16)|0xffffu);
        function_=function;context_=context;count_=count;floatingPoint_=ReadFloatingPoint();
        publishMicros_=collisionSplitNow();pickedUp_.store(false,std::memory_order_relaxed);
        collisionPublishCount.fetch_add(1,std::memory_order_relaxed);
        state_.store(generation_<<16);
        const unsigned wake=armed_.load() ? 0u:(count-1<helpers_ ? count-1:helpers_);
        if(wake)
#if defined(__vita__)
        sceKernelSignalSema(sema_,static_cast<int>(wake));
#else
        {
            {std::lock_guard<std::mutex> lock(mutex_);pending_+=wake;}
            if(wake>1)wakeup_.notify_all();else wakeup_.notify_one();
        }
#endif
    }
    bool ClaimOne(unsigned& index) {return Claim(generation_,index);}
    void Close()
    {
        state_.store((generation_<<16)|0xffffu);
        while(inFlight_.load()!=0) {}
    }
    void Run(unsigned count,void (*function)(void*,unsigned),void* context)
    {
        if(helpers_==0 || count<2) {for(unsigned index=0;index<count;++index)function(context,index);return;}
        const uint32_t runStart=diagFineClock();
        generation_=(generation_+1)&0xffffu;
        state_.store((generation_<<16)|0xffffu);
        function_=function;context_=context;count_=count;floatingPoint_=ReadFloatingPoint();
        publishMicros_=collisionSplitNow();pickedUp_.store(false,std::memory_order_relaxed);
        collisionPublishCount.fetch_add(1,std::memory_order_relaxed);
        state_.store(generation_<<16);
        const unsigned wake=armed_.load() ? 0u:(count-1<helpers_ ? count-1:helpers_);
        if(wake)
#if defined(__vita__)
        sceKernelSignalSema(sema_,static_cast<int>(wake));
#else
        {
            {std::lock_guard<std::mutex> lock(mutex_);pending_+=wake;}
            if(wake>1)wakeup_.notify_all();else wakeup_.notify_one();
        }
#endif
        unsigned index;
        while(Claim(generation_,index)) {
            const uint32_t taskStart=diagFineClock();
            function(context,index);++collisionPoolMainTasks;
            if(diagFineTimers)collisionPoolMainMicros+=diagFineClock()-taskStart;
        }
        const uint32_t waitStart=diagFineClock();
        while(inFlight_.load()!=0) {}
        if(diagFineTimers && collisionWitnessClockMicros) {
            const uint32_t now=collisionWitnessClockMicros();
            collisionPoolRunMicros+=now-runStart;collisionPoolWaitMicros+=now-waitStart;
        }
        ++collisionPoolRuns;
    }
private:
    CollisionHelperPool()
    {
#if defined(__vita__)
        sema_=sceKernelCreateSema("hp2_collide",0,0,1<<20,nullptr);
        if(sema_<0)return;
        for(unsigned slot=0;slot<2;++slot) {
            pthread_attr_t attributes;pthread_attr_init(&attributes);
            pthread_attr_setstacksize(&attributes,128*1024);
            pthread_t thread;
            slots_[slot]=slot;
            if(pthread_create(&thread,&attributes,[](void* argument) -> void* {
                const unsigned slot=*static_cast<unsigned*>(argument);
                sceKernelChangeThreadCpuAffinityMask(0,slot==0 ? SCE_KERNEL_CPU_MASK_USER_1:SCE_KERNEL_CPU_MASK_USER_2);
                sceKernelChangeThreadPriority(0,185);
                collisionHelperSlot=static_cast<int>(slot);
                Instance().Loop();
                return nullptr;
            },&slots_[slot])==0) {pthread_detach(thread);++helpers_;}
            pthread_attr_destroy(&attributes);
        }
#else
        const unsigned hardware=std::thread::hardware_concurrency();
        const unsigned wanted=hardware>=3 ? 2u:hardware>=2 ? 1u:0u;
        for(unsigned slot=0;slot<wanted;++slot) {
            try {std::thread([this,slot] {collisionHelperSlot=static_cast<int>(slot);Loop();}).detach();++helpers_;} catch(...) {break;}
        }
#endif
    }
    static uint32_t ReadFloatingPoint()
    {
#if defined(__arm__)
        uint32_t value;asm volatile("vmrs %0, fpscr" : "=r"(value));return value;
#elif defined(HP2_SPEC_MXCSR)
        return _mm_getcsr();
#else
        return 0;
#endif
    }
    static void WriteFloatingPoint(uint32_t value)
    {
#if defined(__arm__)
        asm volatile("vmsr fpscr, %0" : : "r"(value));
#elif defined(HP2_SPEC_MXCSR)
        _mm_setcsr(value);
#else
        (void)value;
#endif
    }
    bool Claim(uint32_t generation,unsigned& index)
    {
        uint32_t state=state_.load();
        for(;;) {
            if((state>>16)!=generation)return false;
            const unsigned next=state&0xffffu;
            if(next>=count_)return false;
            if(state_.compare_exchange_weak(state,state+1)) {index=next;return true;}
        }
    }
    void Loop()
    {
        for(;;) {
#if defined(__vita__)
            sceKernelWaitSema(sema_,1,nullptr);
#else
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wakeup_.wait(lock,[this] {return pending_>0;});
                --pending_;
            }
#endif
            // Hot polling (collisionPoolHotMicros>0): after a batch the
            // helper keeps its task priority while it polls, for a bounded
            // time only, so a thread it preempts (which may hold a lock the
            // main thread needs) always runs again within that bound.
            bool hot=false;uint32_t hotSince=0;unsigned polls=0;
            do {
                // Cheap poll: only enter the claim protocol for an open batch.
                const uint32_t peek=state_.load(std::memory_order_relaxed);
                if((peek&0xffffu)==0xffffu || (peek&0xffffu)>=count_) {
                    if(hot && (++polls&31u)==0 && collisionWitnessClockMicros &&
                        collisionWitnessClockMicros()-hotSince>collisionPoolHotMicros) {
#if defined(__vita__)
                        sceKernelChangeThreadPriority(0,185);
#endif
                        hot=false;
                    }
                    if(armed_.load(std::memory_order_relaxed))continue;
                    break;
                }
                // Claim first (inFlight_ covers the claim), then raise the
                // priority: a syscall before the claim lets the main thread
                // take the task itself. Run above render/GL so the main
                // thread never waits on a preempted helper.
                inFlight_.fetch_add(1);
                const uint32_t generation=state_.load()>>16;
                unsigned index;
                if(Claim(generation,index)) {
                    if(collisionHelperSlot>=0) collisionSlotBatches[collisionHelperSlot].fetch_add(1,std::memory_order_relaxed);
                    if(!pickedUp_.exchange(true,std::memory_order_relaxed)) {
                        collisionPickupMicros.fetch_add(collisionSplitNow()-publishMicros_,std::memory_order_relaxed);
                        collisionPickupCount.fetch_add(1,std::memory_order_relaxed);
                    }
#if defined(__vita__)
                    if(!hot)sceKernelChangeThreadPriority(0,110);
#endif
                    const uint32_t saved=ReadFloatingPoint();
                    WriteFloatingPoint(floatingPoint_);
                    do {
                        const uint32_t taskStart=diagFineClock();
                        function_(context_,index);collisionPoolHelperTasks.fetch_add(1,std::memory_order_relaxed);
                        if(collisionHelperSlot>=0) collisionSlotTasks[collisionHelperSlot].fetch_add(1,std::memory_order_relaxed);
                        if(diagFineTimers)collisionPoolHelperMicros.fetch_add(diagFineClock()-taskStart,std::memory_order_relaxed);
                    } while(Claim(generation,index));
                    WriteFloatingPoint(saved);
                    inFlight_.fetch_sub(1);
                    if(collisionPoolHotMicros && collisionWitnessClockMicros) {hot=true;hotSince=collisionWitnessClockMicros();polls=0;}
                    else {
#if defined(__vita__)
                        sceKernelChangeThreadPriority(0,185);
#endif
                        hot=false;
                    }
                } else inFlight_.fetch_sub(1);
            } while(armed_.load(std::memory_order_relaxed));
#if defined(__vita__)
            if(hot)sceKernelChangeThreadPriority(0,185);
#endif
        }
    }
    std::atomic<uint32_t> state_{0};
    std::atomic<int> inFlight_{0};
    std::atomic<bool> pickedUp_{true};
    volatile uint32_t publishMicros_{0};
    std::atomic<bool> armed_{false};
    void (*function_)(void*,unsigned){};
    void* context_{};
    unsigned count_{};
    uint32_t floatingPoint_{};
    uint32_t generation_{0};
    unsigned helpers_{0};
#if defined(__vita__)
    SceUID sema_{-1};
    unsigned slots_[2]{};
#else
    std::mutex mutex_;
    std::condition_variable wakeup_;
    unsigned pending_{0};
#endif
};
}

// Generic use of the collision helpers outside the collision solve (the
// async rear-view scenery capture): publish, claim the rest, close (waits).
unsigned collisionHelperPoolSize() {return CollisionHelperPool::Instance().Helpers();}
void collisionHelperPublish(unsigned count,void (*function)(void*,unsigned),void* context)
{
    CollisionHelperPool::Instance().Publish(count,function,context);
}
bool collisionHelperClaim(unsigned& index) {return CollisionHelperPool::Instance().ClaimOne(index);}
void collisionHelperClose() {CollisionHelperPool::Instance().Close();}
unsigned collisionHelperCount()
{
    return collisionSpeculationMode==0 ? 0u:CollisionHelperPool::Instance().Helpers();
}
void collisionHelperRun(unsigned count,void (*function)(void*,unsigned),void* context)
{
    CollisionHelperPool::Instance().Run(count,function,context);
}
void collisionSpeculationArm(int on)
{
    if(collisionSpeculationMode==0)return;
    CollisionHelperPool::Instance().Arm(on!=0);
}

void collisionPointVelocity(float output[4],const float linear[4],const float angular[4],const float lever[4])
{
    float rotational[4];collisionResponseCross3(rotational,angular,lever);
    for(int lane=0;lane<3;++lane) output[lane]=linear[lane]+rotational[lane];
    output[3]=0.0f;
}

void collisionSystemDetachContact(CollisionQueuedContact* contact)
{
    contact->previous->next=contact->next;
    contact->next->previous=contact->previous;
    contact->occupied=0;
}

void collisionQueueRemoveShape(CollisionSystem* system, const CollisionShape* shape)
{
    CollisionQueuedContact* contact=system->contactRoot.next;
    while (contact!=&system->contactRoot) {
        CollisionQueuedContact* next=contact->next;
        if (contact->view.first==shape || contact->view.second==shape) collisionSystemDetachContact(contact);
        contact=next;
    }
}

float collisionAngularInverseMass(const float inverseInertia[16],const float lever[4],const float normal[4])
{
    float crossed[4],transformed[4],velocity[4];
    collisionResponseCross3(crossed,lever,normal);
    vectorMatrixTransformPoint(transformed,inverseInertia,crossed);
    collisionResponseCross3(velocity,transformed,lever);
    const float x=normal[0]*velocity[0],y=normal[1]*velocity[1],z=normal[2]*velocity[2];
    return (x+y)+z;
}


static void contactPlaneNormalAtPosition(float normal[4], const CollisionShape* planeShape,
    const float position[4], const CollisionContactTuning* tuning)
{
    const CollisionFeature* plane=&planeShape->geometry->faces[0];
    const float projection=collisionResponseDot3(plane->vector,position);
    const float limit=plane->offset-tuning->planeBias;
    const int positive=limit<projection;
    for(int lane=0;lane<3;++lane) normal[lane]=positive ? plane->vector[lane] : 0.0f-plane->vector[lane];
    normal[3]=0.0f;
}

namespace {
struct SeparationInputs {
    float firstFrame[16];
    float secondFrame[16];
    float displacement[20][4];
    float adjustedFirst[20][4];
    float adjustedSecond[20][4];
};

void separateSerial(const SeparationInputs& inputs,CollisionWitness* witness,uint32_t* randomState,float output[4])
{
    for (int iteration=0;iteration<20;++iteration) {
        ++collisionSeparateQueries;
        if (collisionWitnessQuery(witness,inputs.firstFrame,inputs.adjustedFirst[iteration],
                                   inputs.secondFrame,inputs.adjustedSecond[iteration],randomState)!=2) {
            memcpy(output,inputs.displacement[iteration],4*sizeof(float));
            return;
        }
    }
}

bool sameWitness(const CollisionWitness& first,const CollisionWitness& second)
{
    return first.side==second.side && first.first==second.first && first.second==second.second &&
        first.firstGeometry==second.firstGeometry && first.secondGeometry==second.secondGeometry &&
        memcmp(first.firstPoint,second.firstPoint,sizeof(first.firstPoint))==0 &&
        memcmp(first.secondPoint,second.secondPoint,sizeof(first.secondPoint))==0 &&
        memcmp(&first.distance,&second.distance,sizeof(first.distance))==0 &&
        first.firstDisplay==second.firstDisplay && first.secondDisplay==second.secondDisplay;
}

// A query only writes side/points/distance (sometimes partially) and its
// feature pair. Speculative queries start with a signalling-NaN pattern no
// arithmetic or authored geometry produces, so unwritten words are known and
// take the predecessor's values when the chain is committed in order.
constexpr uint32_t kUnwritten=0x7fa5a5a5u;
constexpr unsigned kSpeculationWidth=3;

struct SpeculativeQuery {
    CollisionWitness witness;
    CollisionFeature* inFirst;
    CollisionFeature* inSecond;
    uint32_t inState;
    uint32_t state;
    int32_t result;
    uint64_t counters[4];
    const float* firstPosition;
    const float* secondPosition;
    const float* firstFrame;
    const float* secondFrame;
};

struct SpeculativeBatch {
    const CollisionWitness* base;
    SpeculativeQuery* queries;
};

void fillUnwritten(void* data,size_t bytes)
{
    for (size_t offset=0;offset<bytes;offset+=4) memcpy(static_cast<char*>(data)+offset,&kUnwritten,4);
}

void runSpeculativeQuery(void* context,unsigned index)
{
    const auto& batch=*static_cast<const SpeculativeBatch*>(context);
    SpeculativeQuery& query=batch.queries[index];
    query.witness=*batch.base;
    query.witness.first=query.inFirst;
    query.witness.second=query.inSecond;
    fillUnwritten(&query.witness.side,sizeof(query.witness.side));
    fillUnwritten(query.witness.firstPoint,sizeof(query.witness.firstPoint));
    fillUnwritten(query.witness.secondPoint,sizeof(query.witness.secondPoint));
    fillUnwritten(&query.witness.distance,sizeof(query.witness.distance));
    query.state=query.inState;
    query.counters[0]=query.counters[1]=query.counters[2]=query.counters[3]=0;
    query.result=collisionWitnessQueryDetached(&query.witness,query.firstFrame,query.firstPosition,
        query.secondFrame,query.secondPosition,&query.state,query.counters);
}

void mergeWords(void* destination,const void* source,size_t bytes)
{
    for (size_t offset=0;offset<bytes;offset+=4) {
        uint32_t word;memcpy(&word,static_cast<const char*>(source)+offset,4);
        if (word!=kUnwritten) memcpy(static_cast<char*>(destination)+offset,&word,4);
    }
}

void mergeWitness(CollisionWitness& current,const CollisionWitness& result)
{
    current.first=result.first;
    current.second=result.second;
    mergeWords(&current.side,&result.side,sizeof(current.side));
    mergeWords(current.firstPoint,result.firstPoint,sizeof(current.firstPoint));
    mergeWords(current.secondPoint,result.secondPoint,sizeof(current.secondPoint));
    mergeWords(&current.distance,&result.distance,sizeof(current.distance));
}

void predictAfter(const CollisionWitness& current,const SpeculativeQuery& previous,SpeculativeQuery& next)
{
    uint32_t draw=previous.inState;
    next.inFirst=&current.firstGeometry->vertices[vectorRandomBounded((uint32_t)current.firstGeometry->vertexCount,&draw)];
    next.inSecond=&current.secondGeometry->vertices[vectorRandomBounded((uint32_t)current.secondGeometry->vertexCount,&draw)];
    next.inState=draw;
}

bool predicted(const CollisionWitness& current,uint32_t state,const SpeculativeQuery& query)
{
    return current.first==query.inFirst && current.second==query.inSecond && state==query.inState;
}

void commitQuery(CollisionWitness& current,uint32_t& state,const SpeculativeQuery& query)
{
    mergeWitness(current,query.witness);
    state=query.state;
    collisionWitnessQueries+=query.counters[0];collisionWitnessIterations+=query.counters[1];
    collisionWitnessRestarts+=query.counters[2];collisionWitnessExhausted+=query.counters[3];
    ++collisionSpecAccepted;
}

// Pipelined form of separateSpeculative: the whole predicted chain (every
// remaining iteration) is published at once; helpers claim queries in
// order as they become free and the caller commits them in order, claiming
// queries itself while the next one is unfinished. The same predictions and
// commit rule as separateSpeculative, without a barrier per three queries.
struct PipelinedQuery {
    SpeculativeQuery query;
    std::atomic<uint32_t> done;
    std::atomic<int> stop;
};
struct PipelinedBatch {
    const CollisionWitness* base;
    PipelinedQuery* queries;
};
void runPipelinedQuery(void* context,unsigned index)
{
    const auto& batch=*static_cast<const PipelinedBatch*>(context);
    PipelinedQuery& entry=batch.queries[index];
    SpeculativeQuery& query=entry.query;
    query.witness=*batch.base;
    query.witness.first=query.inFirst;
    query.witness.second=query.inSecond;
    fillUnwritten(&query.witness.side,sizeof(query.witness.side));
    fillUnwritten(query.witness.firstPoint,sizeof(query.witness.firstPoint));
    fillUnwritten(query.witness.secondPoint,sizeof(query.witness.secondPoint));
    fillUnwritten(&query.witness.distance,sizeof(query.witness.distance));
    query.state=query.inState;
    query.counters[0]=query.counters[1]=query.counters[2]=query.counters[3]=0;
    query.result=collisionWitnessQueryAbortable(&query.witness,query.firstFrame,query.firstPosition,
        query.secondFrame,query.secondPosition,&query.state,query.counters,&entry.stop);
    entry.done.store(1,std::memory_order_release);
}
void separatePipelined(const SeparationInputs& inputs,CollisionWitness* witness,uint32_t* randomState,float output[4])
{
    const auto started=std::chrono::steady_clock::now();
    ++collisionSpecSeparations;
    auto& pool=CollisionHelperPool::Instance();
    CollisionWitness current=*witness;
    uint32_t state=*randomState;
    static PipelinedQuery queries[20];
    const uint32_t firstCount=(uint32_t)current.firstGeometry->vertexCount;
    const uint32_t secondCount=(uint32_t)current.secondGeometry->vertexCount;
    int iteration=0;
    bool finished=false;
    while (iteration<20 && !finished) {
        const unsigned width=(unsigned)(20-iteration);
        queries[0].query.inFirst=current.first;queries[0].query.inSecond=current.second;queries[0].query.inState=state;
        for (unsigned k=1;k<width;++k) {
            uint32_t draw=queries[k-1].query.inState;
            queries[k].query.inFirst=&current.firstGeometry->vertices[vectorRandomBounded(firstCount,&draw)];
            queries[k].query.inSecond=&current.secondGeometry->vertices[vectorRandomBounded(secondCount,&draw)];
            queries[k].query.inState=draw;
        }
        for (unsigned k=0;k<width;++k) {
            auto& query=queries[k].query;
            query.firstPosition=inputs.adjustedFirst[iteration+k];
            query.secondPosition=inputs.adjustedSecond[iteration+k];
            query.firstFrame=inputs.firstFrame;
            query.secondFrame=inputs.secondFrame;
            queries[k].done.store(0,std::memory_order_relaxed);
            queries[k].stop.store(0,std::memory_order_relaxed);
        }
        const CollisionWitness base=current;
        PipelinedBatch batch{&base,queries};
        pool.Publish(width,runPipelinedQuery,&batch);
        ++collisionSpecRounds;
        unsigned committed=0;
        for (unsigned k=0;k<width;++k) {
            while (!queries[k].done.load(std::memory_order_acquire)) {
                unsigned index;
                if (pool.ClaimOne(index)) runPipelinedQuery(&batch,index);
            }
            const auto& query=queries[k].query;
            if (k>0 && (current.first!=query.inFirst || current.second!=query.inSecond || state!=query.inState)) {
                ++collisionSpecMispredicts;break;
            }
            if (collisionWitnessBench) {
                CollisionWitness input=base;input.first=query.inFirst;input.second=query.inSecond;
                collisionWitnessBenchRecord(&input,query.firstFrame,query.firstPosition,query.secondFrame,query.secondPosition,query.inState);
            }
            mergeWitness(current,query.witness);
            state=query.state;
            collisionWitnessQueries+=query.counters[0];collisionWitnessIterations+=query.counters[1];
            collisionWitnessRestarts+=query.counters[2];collisionWitnessExhausted+=query.counters[3];
            ++collisionSpecAccepted;++collisionSeparateQueries;++committed;
            ++iteration;
            if (query.result!=2) {
                memcpy(output,inputs.displacement[iteration-1],4*sizeof(float));
                finished=true;
                break;
            }
        }
        // Everything after the last committed query is discarded: stop the
        // ones still running so Close does not wait for them.
        if (collisionSpeculativeAbort) for (unsigned k=committed;k<width;++k) queries[k].stop.store(1,std::memory_order_relaxed);
        pool.Close();
        for (unsigned k=committed;k<width;++k) if (queries[k].done.load(std::memory_order_acquire)) {
            ++collisionSpecWasted;
            if (queries[k].query.result<0) ++collisionSpecAborted;
        }
    }
    *witness=current;
    *randomState=state;
    collisionWitnessMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
}

// Each speculative query k>0 assumes query k-1 converged after at least one
// step and restarted exactly once (two vertex draws), the common case. The
// chain is committed in order; the first query whose actual input differs
// from its assumption ends the round and is recomputed from exact inputs.
void separateSpeculative(const SeparationInputs& inputs,CollisionWitness* witness,uint32_t* randomState,float output[4])
{
    const auto started=std::chrono::steady_clock::now();
    ++collisionSpecSeparations;
    CollisionWitness current=*witness;
    uint32_t state=*randomState;
    SpeculativeQuery queries[kSpeculationWidth];
    const uint32_t firstCount=(uint32_t)current.firstGeometry->vertexCount;
    const uint32_t secondCount=(uint32_t)current.secondGeometry->vertexCount;
    int iteration=0;
    bool finished=false;
    while (iteration<20 && !finished) {
        const unsigned width=20-iteration<(int)kSpeculationWidth ? (unsigned)(20-iteration):kSpeculationWidth;
        queries[0].inFirst=current.first;queries[0].inSecond=current.second;queries[0].inState=state;
        for (unsigned k=1;k<width;++k) {
            uint32_t draw=queries[k-1].inState;
            queries[k].inFirst=&current.firstGeometry->vertices[vectorRandomBounded(firstCount,&draw)];
            queries[k].inSecond=&current.secondGeometry->vertices[vectorRandomBounded(secondCount,&draw)];
            queries[k].inState=draw;
        }
        for (unsigned k=0;k<width;++k) {
            queries[k].firstPosition=inputs.adjustedFirst[iteration+k];
            queries[k].secondPosition=inputs.adjustedSecond[iteration+k];
            queries[k].firstFrame=inputs.firstFrame;
            queries[k].secondFrame=inputs.secondFrame;
        }
        const CollisionWitness base=current;
        SpeculativeBatch batch{&base,queries};
        CollisionHelperPool::Instance().Run(width,runSpeculativeQuery,&batch);
        ++collisionSpecRounds;
        for (unsigned k=0;k<width;++k) {
            if (k>0 && (current.first!=queries[k].inFirst || current.second!=queries[k].inSecond || state!=queries[k].inState)) {
                ++collisionSpecMispredicts;collisionSpecWasted+=width-k;break;
            }
            mergeWitness(current,queries[k].witness);
            state=queries[k].state;
            collisionWitnessQueries+=queries[k].counters[0];collisionWitnessIterations+=queries[k].counters[1];
            collisionWitnessRestarts+=queries[k].counters[2];collisionWitnessExhausted+=queries[k].counters[3];
            ++collisionSpecAccepted;++collisionSeparateQueries;
            ++iteration;
            if (queries[k].result!=2) {
                memcpy(output,inputs.displacement[iteration-1],4*sizeof(float));
                collisionSpecWasted+=width-k-1;
                finished=true;
                break;
            }
        }
    }
    *witness=current;
    *randomState=state;
    collisionWitnessMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
}
}

namespace {
void buildSeparationInputs(const CollisionSystem* system,const CollisionQueuedContact* contact,float rewind,SeparationInputs& inputs)
{
    const CollisionShape* first=contact->view.first;
    const CollisionShape* second=contact->view.second;
    float firstPosition[4],secondPosition[4],normal[4]={};
    float* firstFrame=inputs.firstFrame;
    float* secondFrame=inputs.secondFrame;
    collisionResponseInterpolatePose(first,firstPosition,firstFrame,rewind);
    collisionResponseInterpolatePose(second,secondPosition,secondFrame,rewind);
    // Native continuous-contact correction: the source PAL separates these
    // interpolated poses but chooses the plane side using the end-of-step
    // center. A thin body crossing that center plane can consequently be
    // pushed through the floor. Select the side at this same rewind pose;
    // the original bounded separation search, impulse solve and residual
    // advance remain responsible for the actual contact response.
    if (first->kind==3) contactPlaneNormalAtPosition(normal,first,secondPosition,&system->tuning->contactOrder);
    else if (second->kind==3) {
        contactPlaneNormalAtPosition(normal,second,firstPosition,&system->tuning->contactOrder);
        for (int lane=0;lane<3;++lane) normal[lane]=0.0f-normal[lane];
    } else {
        for (int lane=0;lane<3;++lane) normal[lane]=secondPosition[lane]-firstPosition[lane];
        const float inverseLength=finiteRatio(1.0f,sqrtf(collisionResponseDot3(normal,normal)));
        for (int lane=0;lane<3;++lane) normal[lane]=normal[lane]*inverseLength;
    }
    // The twenty candidate displacements depend only on the iteration index
    // (same arithmetic, same order as the original loop).
    float distance=0.0f,step=system->tuning->separationInitialStep;
    const int32_t firstMovable=collisionParticipantMovable(contact->pair->first);
    const int32_t secondMovable=collisionParticipantMovable(contact->pair->second);
    for (int iteration=0;iteration<20;++iteration) {
        distance=distance+step;
        if (step<system->tuning->separationMaximumStep) step=step+system->tuning->separationStepIncrease;
        float* displacement=inputs.displacement[iteration];
        float* adjustedFirst=inputs.adjustedFirst[iteration];
        float* adjustedSecond=inputs.adjustedSecond[iteration];
        displacement[0]=displacement[1]=displacement[2]=displacement[3]=0.0f;
        for (int lane=0;lane<3;++lane) displacement[lane]=normal[lane]*distance;
        const float limit=system->tuning->separationVerticalLimit;
        if (limit<displacement[2]) displacement[2]=limit;
        else if (displacement[2]<-limit) displacement[2]=-limit;
        for (int lane=0;lane<3;++lane) {
            adjustedFirst[lane]=firstPosition[lane]-(firstMovable ? displacement[lane] : 0.0f);
            adjustedSecond[lane]=secondPosition[lane]+(secondMovable ? displacement[lane] : 0.0f);
        }
        adjustedFirst[3]=adjustedSecond[3]=1.0f;
    }
}
}

static void collisionContactSeparateBody(CollisionSystem* system, CollisionQueuedContact* contact, float rewind);
void collisionContactSeparate(CollisionSystem* system, CollisionQueuedContact* contact, float rewind)
{
    const uint32_t start=collisionSplitNow();
    collisionContactSeparateBody(system,contact,rewind);
    collisionSplitMicros[3]+=collisionSplitNow()-start;++collisionSplitCounts[3];
}
static void collisionContactSeparateBody(CollisionSystem* system, CollisionQueuedContact* contact, float rewind)
{
    ++collisionSeparateCalls;
    SeparationInputs inputs;
    buildSeparationInputs(system,contact,rewind,inputs);
    CollisionWitness* witness=&contact->pair->witness;
    const bool verify=collisionSpeculationMode==2 ||
        (collisionSpeculationMode==3 && collisionSpecVerified<collisionSpeculationVerifyBudget);
    if (verify) {
        CollisionWitness copy=*witness;
        uint32_t state=*system->randomState;
        float displacement[4];memcpy(displacement,contact->displacement,sizeof(displacement));
        separateSerial(inputs,witness,system->randomState,contact->displacement);
        if (collisionSeparatePipelined) separatePipelined(inputs,&copy,&state,displacement);
        else separateSpeculative(inputs,&copy,&state,displacement);
        ++collisionSpecVerified;
        if (!sameWitness(copy,*witness) || state!=*system->randomState ||
            memcmp(displacement,contact->displacement,sizeof(displacement))!=0) ++collisionSpecMismatches;
        return;
    }
    if (collisionSpeculationMode!=0 && CollisionHelperPool::Instance().Helpers()>0) {
        if (collisionSeparatePipelined) separatePipelined(inputs,witness,system->randomState,contact->displacement);
        else separateSpeculative(inputs,witness,system->randomState,contact->displacement);
    }
    else separateSerial(inputs,witness,system->randomState,contact->displacement);
}

static float findTimeSerial(CollisionSystem* system, CollisionQueuedContact* contact)
{
    CollisionShape* first=contact->view.first;
    CollisionShape* second=contact->view.second;
    const float firstTime=first->active ? first->previousTime : 1000.0f;
    const float secondTime=second->active ? second->previousTime : 1000.0f;
    float upper=minimumFinite(firstTime,secondTime), lower=0.0f, midpoint=0.0f;
    int32_t result=0;
    memset(contact->displacement,0,sizeof(contact->displacement));
    for (int iteration=0;iteration<8;++iteration) {
        const float half=(upper-lower)*0.5f;
        midpoint=lower+half;
        float firstPosition[4],secondPosition[4],firstFrame[16],secondFrame[16];
        collisionResponseInterpolatePose(first,firstPosition,firstFrame,midpoint);
        collisionResponseInterpolatePose(second,secondPosition,secondFrame,midpoint);
        ++collisionFindQueries;
        result=collisionWitnessQuery(&contact->pair->witness,firstFrame,firstPosition,
                                     secondFrame,secondPosition,system->randomState);
        if (result==2) lower=midpoint;
        else {
            if (contact->pair->witness.distance<system->tuning->contactDistance) return midpoint;
            upper=midpoint;
        }
    }
    if (result==2) collisionContactSeparate(system,contact,midpoint);
    return midpoint;
}

namespace {
// One early-resolving bisection round: query 0 plus both children.
struct FindRound {
    const CollisionWitness* base;
    SpeculativeQuery* queries;
    std::atomic<uint32_t> done[3];
    std::atomic<int> stop[3];
    float contactDistance;
};
void runFindRoundQuery(void* context,unsigned index)
{
    auto& round=*static_cast<FindRound*>(context);
    SpeculativeQuery& query=round.queries[index];
    if (round.stop[index].load(std::memory_order_relaxed)) {query.result=-1;round.done[index].store(1,std::memory_order_release);return;}
    query.witness=*round.base;
    query.witness.first=query.inFirst;
    query.witness.second=query.inSecond;
    fillUnwritten(&query.witness.side,sizeof(query.witness.side));
    fillUnwritten(query.witness.firstPoint,sizeof(query.witness.firstPoint));
    fillUnwritten(query.witness.secondPoint,sizeof(query.witness.secondPoint));
    fillUnwritten(&query.witness.distance,sizeof(query.witness.distance));
    query.state=query.inState;
    query.counters[0]=query.counters[1]=query.counters[2]=query.counters[3]=0;
    query.result=collisionWitnessQueryAbortable(&query.witness,query.firstFrame,query.firstPosition,
        query.secondFrame,query.secondPosition,&query.state,query.counters,&round.stop[index]);
    // Query 0 decides which child is discarded: a penetrating result (2)
    // continues with child 1, any other with child 2 (or neither when it
    // converges, which the caller handles after committing it).
    if (index==0 && collisionSpeculativeAbort) round.stop[query.result==2 ? 2:1].store(1,std::memory_order_relaxed);
    round.done[index].store(1,std::memory_order_release);
}
struct FindTimePose {
    float firstPosition[4],secondPosition[4],firstFrame[16],secondFrame[16];
};
// Bisection without the final separation: returns the midpoint and leaves
// the last query result in *result (the caller separates when it is 2).
float findTimeSpeculative(const CollisionSystem* system,const CollisionQueuedContact* contact,
    CollisionWitness* witness,uint32_t* randomState,int32_t* finalResult)
{
    const auto started=std::chrono::steady_clock::now();
    const CollisionShape* first=contact->view.first;
    const CollisionShape* second=contact->view.second;
    const float firstTime=first->active ? first->previousTime : 1000.0f;
    const float secondTime=second->active ? second->previousTime : 1000.0f;
    float upper=minimumFinite(firstTime,secondTime), lower=0.0f, midpoint=0.0f;
    int32_t result=0;
    CollisionWitness current=*witness;
    uint32_t state=*randomState;
    const float contactDistance=system->tuning->contactDistance;
    SpeculativeQuery queries[3];
    FindTimePose poses[3];
    float midpoints[3];
    int iteration=0;
    bool done=false;
    const auto setPose=[&](unsigned slot,float time) {
        midpoints[slot]=time;
        collisionResponseInterpolatePose(first,poses[slot].firstPosition,poses[slot].firstFrame,time);
        collisionResponseInterpolatePose(second,poses[slot].secondPosition,poses[slot].secondFrame,time);
        queries[slot].firstPosition=poses[slot].firstPosition;queries[slot].secondPosition=poses[slot].secondPosition;
        queries[slot].firstFrame=poses[slot].firstFrame;queries[slot].secondFrame=poses[slot].secondFrame;
    };
    // Applies one committed result exactly as the serial loop does.
    const auto apply=[&](unsigned slot) {
        midpoint=midpoints[slot];
        result=queries[slot].result;
        ++iteration;++collisionFindQueries;
        if (result==2) lower=midpoint;
        else {
            if (current.distance<contactDistance) {done=true;return;}
            upper=midpoint;
        }
    };
    while (iteration<8 && !done) {
        {
            const float half=(upper-lower)*0.5f;
            setPose(0,lower+half);
        }
        queries[0].inFirst=current.first;queries[0].inSecond=current.second;queries[0].inState=state;
        unsigned width=1;
        if (iteration+1<8) {
            // Slot 1: this query penetrates (lower=midpoint). Slot 2: it does not.
            const float a=midpoints[0];
            {const float half=(upper-a)*0.5f;setPose(1,a+half);}
            {const float half=(a-lower)*0.5f;setPose(2,lower+half);}
            predictAfter(current,queries[0],queries[1]);
            queries[2].inFirst=queries[1].inFirst;queries[2].inSecond=queries[1].inSecond;queries[2].inState=queries[1].inState;
            width=3;
        }
        const CollisionWitness base=current;
        SpeculativeBatch batch{&base,queries};
        if (width==3 && collisionSpeculativeAbort) {
            // Early-resolving round: once query 0 is known, the child that
            // will not be used is stopped and the round ends as soon as the
            // chosen child is done (no barrier on the discarded one).
            auto& pool=CollisionHelperPool::Instance();
            FindRound round{&base,queries,{},{},0.0f};
            for (unsigned k=0;k<3;++k) {round.done[k].store(0,std::memory_order_relaxed);round.stop[k].store(0,std::memory_order_relaxed);}
            round.contactDistance=contactDistance;
            pool.Publish(3,runFindRoundQuery,&round);
            ++collisionSpecRounds;
            unsigned index;
            while (!round.done[0].load(std::memory_order_acquire))
                if (pool.ClaimOne(index)) runFindRoundQuery(&round,index);
            commitQuery(current,state,queries[0]);
            apply(0);
            if (done || iteration>=8) {
                round.stop[1].store(1,std::memory_order_relaxed);round.stop[2].store(1,std::memory_order_relaxed);
                pool.Close();collisionSpecWasted+=2;break;
            }
            const unsigned chosen=result==2 ? 1u:2u;
            round.stop[3-chosen].store(1,std::memory_order_relaxed);
            while (!round.done[chosen].load(std::memory_order_acquire))
                if (pool.ClaimOne(index)) runFindRoundQuery(&round,index);
            pool.Close();
            ++collisionSpecWasted;
            if (queries[3-chosen].result<0 && round.done[3-chosen].load(std::memory_order_acquire)) ++collisionSpecAborted;
            if (!predicted(current,state,queries[chosen])) {++collisionSpecMispredicts;++collisionSpecWasted;continue;}
            commitQuery(current,state,queries[chosen]);
            apply(chosen);
            continue;
        }
        CollisionHelperPool::Instance().Run(width,runSpeculativeQuery,&batch);
        ++collisionSpecRounds;
        commitQuery(current,state,queries[0]);
        apply(0);
        if (width==1) continue;
        if (done || iteration>=8) {collisionSpecWasted+=2;break;}
        const unsigned chosen=result==2 ? 1u:2u;
        ++collisionSpecWasted;
        if (!predicted(current,state,queries[chosen])) {++collisionSpecMispredicts;++collisionSpecWasted;continue;}
        commitQuery(current,state,queries[chosen]);
        apply(chosen);
    }
    *witness=current;
    *randomState=state;
    *finalResult=result;
    collisionWitnessMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    return midpoint;
}
}

// ---------------------------------------------------------------------------
// Contact pipeline. Within one queue pass the contacts are linked only by the
// shared random state: each contact's find-time/separation queries read the
// pair's witness, the shapes' poses (unchanged during the pass) and the
// random state. Every witness query that converges ends with one terminal
// restart (two vertex draws), so the start of every query in the pass can be
// predicted once the number of queries per contact is known. The RNG-free
// planning pass (warm starts, run with the queue prediction) predicts each
// contact's bisection path and separation length; the exact queries of the
// whole pass are then published at once and committed in order. A query is
// used only if its start vertices, random state and pose (midpoint bits /
// separation iteration) equal the actual ones, so the result is the serial
// one by construction; any other query is computed serially on the spot and
// the pipeline is closed.
namespace {
struct ContactPlan {
    int valid,complete,findCount,separate,separateCount;
    float midpoints[8];
    float rewind;
    FindTimePose poses[8];
    SeparationInputs inputs;
    CollisionWitness base;
};
std::vector<ContactPlan> contactPlans;
struct PipeTask {
    SpeculativeQuery query;
    const CollisionWitness* base;
    std::atomic<uint32_t> done;
    std::atomic<int> stop;
};
constexpr size_t kPipeTaskCapacity=1024;
constexpr size_t kPipeContactCapacity=96;
PipeTask* pipeTasks=nullptr;
struct PipeContact {
    CollisionPair* pair;
    size_t plan,firstTask,taskCount,seed;
    CollisionFeature* startFirst;
    CollisionFeature* startSecond;
    uint32_t startState;
};
PipeContact pipeContacts[kPipeContactCapacity];
size_t pipeContactCount=0,pipeCursor=0,pipeTaskCount=0;
bool pipeActive=false;
const CollisionPipelineSeed* pipeSeeds=nullptr;
size_t pipeSeedCount=0;

void runPipeTask(void*,unsigned index)
{
    PipeTask& task=pipeTasks[index];
    SpeculativeQuery& query=task.query;
    if (task.stop.load(std::memory_order_relaxed)) {query.result=-1;task.done.store(1,std::memory_order_release);return;}
    query.witness=*task.base;
    query.witness.first=query.inFirst;
    query.witness.second=query.inSecond;
    fillUnwritten(&query.witness.side,sizeof(query.witness.side));
    fillUnwritten(query.witness.firstPoint,sizeof(query.witness.firstPoint));
    fillUnwritten(query.witness.secondPoint,sizeof(query.witness.secondPoint));
    fillUnwritten(&query.witness.distance,sizeof(query.witness.distance));
    query.state=query.inState;
    query.counters[0]=query.counters[1]=query.counters[2]=query.counters[3]=0;
    query.result=collisionWitnessQueryAbortable(&query.witness,query.firstFrame,query.firstPosition,
        query.secondFrame,query.secondPosition,&query.state,query.counters,&task.stop);
    task.done.store(1,std::memory_order_release);
}

void pipelineClose()
{
    if (!pipeActive) return;
    for (size_t k=0;k<pipeTaskCount;++k) pipeTasks[k].stop.store(1,std::memory_order_relaxed);
    CollisionHelperPool::Instance().Close();
    pipeActive=false;
}

// Serial reference without helpers (verify only): findTimeSerial with the
// serial separation.
float findTimeReference(CollisionSystem* system,CollisionQueuedContact* contact)
{
    struct MemoBypass {MemoBypass(){collisionWitnessMemoBypass(true);}~MemoBypass(){collisionWitnessMemoBypass(false);}} memoBypass;
    CollisionShape* first=contact->view.first;
    CollisionShape* second=contact->view.second;
    const float firstTime=first->active ? first->previousTime : 1000.0f;
    const float secondTime=second->active ? second->previousTime : 1000.0f;
    float upper=minimumFinite(firstTime,secondTime), lower=0.0f, midpoint=0.0f;
    int32_t result=0;
    memset(contact->displacement,0,sizeof(contact->displacement));
    for (int iteration=0;iteration<8;++iteration) {
        const float half=(upper-lower)*0.5f;
        midpoint=lower+half;
        float firstPosition[4],secondPosition[4],firstFrame[16],secondFrame[16];
        collisionResponseInterpolatePose(first,firstPosition,firstFrame,midpoint);
        collisionResponseInterpolatePose(second,secondPosition,secondFrame,midpoint);
        result=collisionWitnessQuery(&contact->pair->witness,firstFrame,firstPosition,
                                     secondFrame,secondPosition,system->randomState);
        if (result==2) lower=midpoint;
        else {
            if (contact->pair->witness.distance<system->tuning->contactDistance) return midpoint;
            upper=midpoint;
        }
    }
    if (result==2) {
        SeparationInputs inputs;
        buildSeparationInputs(system,contact,midpoint,inputs);
        separateSerial(inputs,&contact->pair->witness,system->randomState,contact->displacement);
    }
    return midpoint;
}

// findTimeSerial + separation for one planned contact, taking each query
// from the pipeline when its inputs are exactly the actual ones.
float pipelineContactCommit(const CollisionSystem* system,const CollisionQueuedContact* contact,const PipeContact& pc,
    CollisionWitness& current,uint32_t& state,float displacement[4])
{
    const ContactPlan& plan=contactPlans[pc.plan];
    auto& pool=CollisionHelperPool::Instance();
    const CollisionShape* first=contact->view.first;
    const CollisionShape* second=contact->view.second;
    const float firstTime=first->active ? first->previousTime : 1000.0f;
    const float secondTime=second->active ? second->previousTime : 1000.0f;
    float upper=minimumFinite(firstTime,secondTime), lower=0.0f, midpoint=0.0f;
    const float contactDistance=system->tuning->contactDistance;
    memset(displacement,0,4*sizeof(float));
    PipeTask* tasks=pipeTasks+pc.firstTask;
    struct UsedCheck {
        const PipeContact& pc;size_t used{};bool broke{};
        ~UsedCheck() {if (!broke && used<pc.taskCount) ++collisionPipeLong;}
    } usedCheck{pc};
    const auto take=[&](size_t position)->bool {
        if (!pipeActive || position>=pc.taskCount) return false;
        PipeTask& task=tasks[position];
        if (!predicted(current,state,task.query)) return false;
        unsigned index;
        while (!task.done.load(std::memory_order_acquire))
            if (pool.ClaimOne(index)) runPipeTask(nullptr,index);
        if (task.query.result<0) return false;
        commitQuery(current,state,task.query);
        if (task.query.counters[2]==0) {
            ++collisionPipeZeroDraw;
            // 0..2 result; +3 when it is a separation query
            ++collisionPipeZeroKind[(task.query.result<0 ? 0:task.query.result>2 ? 2:task.query.result)+(position>=8 ? 3:0)];
        }
        ++collisionPipeUsed;++usedCheck.used;
        return true;
    };
    const auto breakPipe=[&] {usedCheck.broke=true;if (pipeActive) {++collisionPipeBreaks;pipelineClose();}};
    int32_t result=0;
    for (int iteration=0;iteration<8;++iteration) {
        const float half=(upper-lower)*0.5f;
        midpoint=lower+half;
        ++collisionFindQueries;
        if (iteration<plan.findCount && memcmp(&plan.midpoints[iteration],&midpoint,sizeof(midpoint))==0 &&
            take((size_t)iteration)) result=tasks[iteration].query.result;
        else {
            breakPipe();
            float firstPosition[4],secondPosition[4],firstFrame[16],secondFrame[16];
            collisionResponseInterpolatePose(first,firstPosition,firstFrame,midpoint);
            collisionResponseInterpolatePose(second,secondPosition,secondFrame,midpoint);
            result=collisionWitnessQuery(&current,firstFrame,firstPosition,secondFrame,secondPosition,&state);
        }
        if (result==2) lower=midpoint;
        else {
            if (current.distance<contactDistance) return midpoint;
            upper=midpoint;
        }
    }
    if (result!=2) return midpoint;
    ++collisionSeparateCalls;
    const bool planned=plan.separate && plan.findCount==8 && memcmp(&plan.rewind,&midpoint,sizeof(midpoint))==0;
    SeparationInputs local;
    const SeparationInputs* inputs=&plan.inputs;
    if (!planned) {buildSeparationInputs(system,contact,midpoint,local);inputs=&local;}
    for (int iteration=0;iteration<20;++iteration) {
        ++collisionSeparateQueries;
        const size_t position=8u+(size_t)iteration;
        if (planned && take(position)) result=tasks[position].query.result;
        else if (planned && position>=pc.taskCount) ++collisionPipeShort;
        else {
            breakPipe();
            result=collisionWitnessQuery(&current,inputs->firstFrame,inputs->adjustedFirst[iteration],
                inputs->secondFrame,inputs->adjustedSecond[iteration],&state);
        }
        if (result!=2) {memcpy(displacement,inputs->displacement[iteration],4*sizeof(float));return midpoint;}
    }
    return midpoint;
}
}

void collisionContactPlanReserve(size_t count)
{
    if (contactPlans.size()<count) contactPlans.resize(count);
    for (size_t index=0;index<count;++index) contactPlans[index].valid=0;
}

bool collisionContactPipelineEnabled()
{
    return collisionContactPipeline && collisionSpeculationMode!=0 && CollisionHelperPool::Instance().Helpers()>0;
}

// Helper thread (inside the queue prediction run): RNG-free plan of one
// contact from the queue query's converged witness. Reads shapes only.
void collisionContactPlanPrepare(size_t index,const CollisionSystem* system,CollisionPair* pair,const CollisionWitness& converged)
{
    ContactPlan& plan=contactPlans[index];
    plan.valid=0;plan.complete=0;plan.findCount=0;plan.separate=0;plan.separateCount=0;
    const CollisionShape* first=pair->first->shape;
    const CollisionShape* second=pair->second->shape;
    const float firstTime=first->active ? first->previousTime : 1000.0f;
    const float secondTime=second->active ? second->previousTime : 1000.0f;
    float upper=minimumFinite(firstTime,secondTime), lower=0.0f, midpoint=0.0f;
    const float contactDistance=system->tuning->contactDistance;
    plan.base=converged;
    CollisionWitness witness=converged;
    uint64_t counters[4]={0,0,0,0};
    int32_t result=0;
    plan.valid=1;
    for (int iteration=0;iteration<8;++iteration) {
        const float half=(upper-lower)*0.5f;
        midpoint=lower+half;
        FindTimePose& pose=plan.poses[iteration];
        plan.midpoints[iteration]=midpoint;
        collisionResponseInterpolatePose(first,pose.firstPosition,pose.firstFrame,midpoint);
        collisionResponseInterpolatePose(second,pose.secondPosition,pose.secondFrame,midpoint);
        int32_t pending=0;
        result=collisionWitnessQueryPredict(&witness,pose.firstFrame,pose.firstPosition,pose.secondFrame,
            pose.secondPosition,&pending,counters);
        plan.findCount=iteration+1;
        if (pending<0) return;
        if (result==2) lower=midpoint;
        else {
            if (witness.distance<contactDistance) {plan.complete=1;return;}
            upper=midpoint;
        }
    }
    if (result!=2) {plan.complete=1;return;}
    plan.separate=1;plan.rewind=midpoint;
    CollisionQueuedContact probe{};
    probe.view.first=pair->first->shape;probe.view.second=pair->second->shape;probe.pair=pair;
    buildSeparationInputs(system,&probe,midpoint,plan.inputs);
    for (int iteration=0;iteration<20;++iteration) {
        int32_t pending=0;
        result=collisionWitnessQueryPredict(&witness,plan.inputs.firstFrame,plan.inputs.adjustedFirst[iteration],
            plan.inputs.secondFrame,plan.inputs.adjustedSecond[iteration],&pending,counters);
        plan.separateCount=iteration+1;
        if (pending<0) return;
        if (result!=2) break;
    }
    plan.complete=1;
}

// Lays out seeds [begin,count) from the given random state. With given
// features, the first seed's queue query is already committed and its
// contact starts from them.
static void pipelineLayout(size_t begin,uint32_t state,CollisionFeature* givenFirst,CollisionFeature* givenSecond)
{
    pipelineClose();
    size_t tasks=0,contacts=0;
    for (size_t at=begin;at<pipeSeedCount;++at) {
        const auto& seed=pipeSeeds[at];
        const CollisionWitness& queued=*seed.queueWitness;
        CollisionFeature* startFirst=queued.first;
        CollisionFeature* startSecond=queued.second;
        if (at==begin && givenFirst) {startFirst=givenFirst;startSecond=givenSecond;}
        else if (seed.queuePending==1) {
            startFirst=&queued.firstGeometry->vertices[vectorRandomBounded((uint32_t)queued.firstGeometry->vertexCount,&state)];
            startSecond=&queued.secondGeometry->vertices[vectorRandomBounded((uint32_t)queued.secondGeometry->vertexCount,&state)];
        }
        if (!seed.contact) continue;
        const ContactPlan& plan=contactPlans[seed.plan];
        if (!plan.valid) break;
        const size_t need=(size_t)plan.findCount+(size_t)plan.separateCount;
        if (contacts==kPipeContactCapacity || tasks+need>kPipeTaskCapacity) break;
        PipeContact& pc=pipeContacts[contacts++];
        pc.pair=seed.pair;pc.plan=seed.plan;pc.firstTask=tasks;pc.taskCount=need;pc.seed=at;
        pc.startFirst=startFirst;pc.startSecond=startSecond;pc.startState=state;
        for (size_t k=0;k<need;++k) {
            PipeTask& task=pipeTasks[tasks++];
            SpeculativeQuery& query=task.query;
            query.inFirst=startFirst;query.inSecond=startSecond;query.inState=state;
            if (k<(size_t)plan.findCount) {
                const FindTimePose& pose=plan.poses[k];
                query.firstPosition=pose.firstPosition;query.secondPosition=pose.secondPosition;
                query.firstFrame=pose.firstFrame;query.secondFrame=pose.secondFrame;
            } else {
                const size_t j=k-(size_t)plan.findCount;
                query.firstPosition=plan.inputs.adjustedFirst[j];query.secondPosition=plan.inputs.adjustedSecond[j];
                query.firstFrame=plan.inputs.firstFrame;query.secondFrame=plan.inputs.secondFrame;
            }
            task.base=&plan.base;
            task.done.store(0,std::memory_order_relaxed);
            task.stop.store(0,std::memory_order_relaxed);
            // The common case: converged after at least one step, one
            // terminal restart (two draws) that seeds the next query. The
            // final separating query sometimes ends on a new pair without a
            // restart; the next contact then lays out again from the actual
            // state (see collisionContactFindTime).
            startFirst=&plan.base.firstGeometry->vertices[vectorRandomBounded((uint32_t)plan.base.firstGeometry->vertexCount,&state)];
            startSecond=&plan.base.secondGeometry->vertices[vectorRandomBounded((uint32_t)plan.base.secondGeometry->vertexCount,&state)];
        }
        if (!plan.complete) break;
    }
    if (tasks==0) return;
    pipeContactCount=contacts;pipeCursor=0;pipeTaskCount=tasks;
    collisionPipeTasks+=tasks;
    pipeActive=true;
    CollisionHelperPool::Instance().Publish((unsigned)tasks,runPipeTask,nullptr);
}

// Main thread, after the queue prediction: lay out the pass's exact queries
// in random-state order and publish them.
void collisionContactPipelineStart(CollisionSystem* system,const CollisionPipelineSeed* seeds,size_t count)
{
    pipelineClose();
    pipeSeeds=nullptr;pipeSeedCount=0;
    if (!collisionContactPipelineEnabled() || count==0) return;
    if (!pipeTasks) pipeTasks=new PipeTask[kPipeTaskCapacity];
    pipeSeeds=seeds;pipeSeedCount=count;
    ++collisionPipePasses;
    pipelineLayout(0,*system->randomState,nullptr,nullptr);
}

void collisionContactPipelineEnd()
{
    pipelineClose();
    pipeSeeds=nullptr;pipeSeedCount=0;
}

float collisionContactFindTime(CollisionSystem* system, CollisionQueuedContact* contact)
{
    if (pipeActive) {
        const PipeContact* pc=nullptr;
        if (pipeCursor<pipeContactCount && pipeContacts[pipeCursor].pair==contact->pair) pc=&pipeContacts[pipeCursor++];
        if (!pc) ++collisionPipeMissPair;
        if (pc && (contact->pair->witness.first!=pc->startFirst || contact->pair->witness.second!=pc->startSecond ||
                   *system->randomState!=pc->startState)) {
            ++collisionPipeMissStart;
            // Diagnostic: how many draws the actual state is ahead (+) of the plan.
            uint32_t ahead=pc->startState,behind=*system->randomState;int delta=-1;
            for (int k=1;k<=8 && delta<0;++k) {(void)vectorRandomBounded(2u,&ahead);if (ahead==*system->randomState) delta=k;}
            for (int k=1;k<=8 && delta<0;++k) {(void)vectorRandomBounded(2u,&behind);if (behind==pc->startState) delta=100+k;}
            if (delta>100) ++collisionPipeBehind[delta-100];
            ++collisionPipeDrawDelta[delta<0 ? 3:delta<100 ? (delta==2 ? 0:2):1];
            // The plans are RNG-free: lay the rest of the pass out again from
            // the actual state and this contact's actual start.
            ++collisionPipeRelayouts;
            pipelineLayout(pc->seed,*system->randomState,contact->pair->witness.first,contact->pair->witness.second);
            pc=pipeActive && pipeContactCount>0 && pipeContacts[0].pair==contact->pair ? &pipeContacts[pipeCursor++]:nullptr;
        }
        if (!pc) {++collisionPipeEntryMisses;pipelineClose();}
        else {
            ++collisionPipeContacts;
            const bool verify=collisionSpeculationMode==2 ||
                (collisionSpeculationMode==3 && collisionSpecVerified<collisionSpeculationVerifyBudget);
            if (verify) {
                CollisionWitness copy=contact->pair->witness;
                uint32_t state=*system->randomState;
                float displacement[4];
                const float midpoint=pipelineContactCommit(system,contact,*pc,copy,state,displacement);
                const float expected=findTimeReference(system,contact);
                ++collisionSpecVerified;
                if (!sameWitness(copy,contact->pair->witness) || state!=*system->randomState ||
                    memcmp(displacement,contact->displacement,sizeof(displacement))!=0 ||
                    memcmp(&midpoint,&expected,sizeof(midpoint))!=0) ++collisionSpecMismatches;
                return expected;
            }
            const uint32_t findStart=collisionSplitNow();
            const float midpoint=pipelineContactCommit(system,contact,*pc,contact->pair->witness,*system->randomState,contact->displacement);
            collisionSplitMicros[2]+=collisionSplitNow()-findStart;++collisionSplitCounts[2];
            return midpoint;
        }
    }
    const bool verify=collisionSpeculationMode==2 ||
        (collisionSpeculationMode==3 && collisionSpecVerified<collisionSpeculationVerifyBudget);
    if (verify) {
        CollisionWitness copy=contact->pair->witness;
        uint32_t state=*system->randomState;
        int32_t result=0;
        float displacement[4]={0.0f,0.0f,0.0f,0.0f};
        const float midpoint=findTimeSpeculative(system,contact,&copy,&state,&result);
        if (result==2) {
            SeparationInputs inputs;
            buildSeparationInputs(system,contact,midpoint,inputs);
            if (collisionSeparatePipelined) separatePipelined(inputs,&copy,&state,displacement);
            else separateSpeculative(inputs,&copy,&state,displacement);
        }
        const float expected=findTimeSerial(system,contact);
        ++collisionSpecVerified;
        if (!sameWitness(copy,contact->pair->witness) || state!=*system->randomState ||
            memcmp(displacement,contact->displacement,sizeof(displacement))!=0 ||
            memcmp(&midpoint,&expected,sizeof(midpoint))!=0) ++collisionSpecMismatches;
        return expected;
    }
    if (collisionSpeculationMode==0 || CollisionHelperPool::Instance().Helpers()==0) return findTimeSerial(system,contact);
    memset(contact->displacement,0,sizeof(contact->displacement));
    int32_t result=0;
    const uint32_t findStart=collisionSplitNow();
    const float midpoint=findTimeSpeculative(system,contact,&contact->pair->witness,system->randomState,&result);
    collisionSplitMicros[2]+=collisionSplitNow()-findStart;++collisionSplitCounts[2];
    if (result==2) collisionContactSeparate(system,contact,midpoint);
    return midpoint;
}

void collisionContactFrame(const CollisionSystem* system, const CollisionQueuedContact* contact,
                            float point[4], float normal[4])
{
    const CollisionShape* first=contact->view.first;
    const CollisionShape* second=contact->view.second;
    float firstPoint[4],secondPoint[4];
    vectorMatrixTransformPoint(firstPoint,collisionResponseShapeFrame(first),contact->pair->witness.firstPoint);
    vectorMatrixTransformPoint(secondPoint,collisionResponseShapeFrame(second),contact->pair->witness.secondPoint);
    for (int lane=0;lane<3;++lane) {
        firstPoint[lane]=firstPoint[lane]+collisionResponseShapePosition(first)[lane];
        secondPoint[lane]=secondPoint[lane]+collisionResponseShapePosition(second)[lane];
        point[lane]=(firstPoint[lane]+secondPoint[lane])*0.5f;
    }
    point[3]=1.0f;
    normal[3]=0.0f;
    if (!first->active) collisionContactPlaneNormal(normal,first,second,&system->tuning->contactOrder);
    else if (!second->active) {
        collisionContactPlaneNormal(normal,second,first,&system->tuning->contactOrder);
        for (int lane=0;lane<3;++lane) normal[lane]=0.0f-normal[lane];
    } else {
        for (int lane=0;lane<3;++lane) normal[lane]=secondPoint[lane]-firstPoint[lane];
        float length=sqrtf(collisionResponseDot3(normal,normal));
        if (length<system->tuning->coincidentPointDistance) {
            for (int lane=0;lane<3;++lane) normal[lane]=collisionResponseShapePosition(second)[lane]-collisionResponseShapePosition(first)[lane];
            length=sqrtf(collisionResponseDot3(normal,normal));
        }
        const float inverseLength=finiteRatio(1.0f,length);
        for (int lane=0;lane<3;++lane) normal[lane]=normal[lane]*inverseLength;
    }
}

void collisionContactProjectPoint(const CollisionQueuedContact* contact, float point[4])
{
    float between[4]={},upTangent[4]={};
    const float* firstPosition=collisionResponseShapePosition(contact->view.first);
    const float* secondPosition=collisionResponseShapePosition(contact->view.second);
    for (int lane=0;lane<3;++lane) between[lane]=secondPosition[lane]-firstPosition[lane];
    const float inverseLength=finiteRatio(1.0f,sqrtf(collisionResponseDot3(between,between)));
    for (int lane=0;lane<3;++lane) between[lane]=between[lane]*inverseLength;
    const float verticalProjection=-between[2];
    for (int lane=0;lane<3;++lane) upTangent[lane]=between[lane]*verticalProjection;
    upTangent[2]=upTangent[2]+1.0f;
    const float inverseTangentLength=finiteRatio(1.0f,sqrtf(collisionResponseDot3(upTangent,upTangent)));
    for (int lane=0;lane<3;++lane) upTangent[lane]=upTangent[lane]*inverseTangentLength;
    float lever[4]={};
    for (int lane=0;lane<3;++lane) lever[lane]=point[lane]-firstPosition[lane];
    const float projection=-collisionResponseDot3(lever,upTangent);
    for (int lane=0;lane<3;++lane) point[lane]=point[lane]+upTangent[lane]*projection;
    point[3]=1.0f;
}

void collisionContactSolve(CollisionSystem* system, CollisionQueuedContact* contact)
{
    CollisionParticipant* first=contact->pair->first;
    CollisionParticipant* second=contact->pair->second;
    float point[4],normal[4],firstLever[4]={},secondLever[4]={};
    collisionContactFrame(system,contact,point,normal);
    /* The impact is retained even when this contact is already separating. */
    collisionPairImpact(system,contact->pair,point);
    if (system->result!=COLLISION_SYSTEM_OK) return;
    for (int lane=0;lane<3;++lane) {
        firstLever[lane]=point[lane]-collisionResponseShapePosition(first->shape)[lane];
        secondLever[lane]=point[lane]-collisionResponseShapePosition(second->shape)[lane];
    }
    for (;;) {
        float firstLinear[4],firstAngular[4],secondLinear[4],secondAngular[4];
        float firstVelocity[4],secondVelocity[4],relative[4]={},tangent[4]={};
        collisionParticipantVelocity(first,firstLinear,firstAngular);
        collisionParticipantVelocity(second,secondLinear,secondAngular);
        collisionPointVelocity(firstVelocity,firstLinear,firstAngular,firstLever);
        collisionPointVelocity(secondVelocity,secondLinear,secondAngular,secondLever);
        for (int lane=0;lane<3;++lane) relative[lane]=secondVelocity[lane]-firstVelocity[lane];
        float normalSpeed=collisionResponseDot3(relative,normal);
        for (int lane=0;lane<3;++lane) tangent[lane]=normal[lane]*(-normalSpeed)+relative[lane];
        if (0.0f<=normalSpeed) return;
        const float restitution=maximumFinite(collisionParticipantRestitution(system,first),
                                               collisionParticipantRestitution(system,second));
        const float firstInverseMass=collisionParticipantInverseMass(system,first);
        const float secondInverseMass=collisionParticipantInverseMass(system,second);
        float inertia[16];
        float inverseMass=firstInverseMass+secondInverseMass;
        collisionParticipantInverseInertia(system,first,inertia);
        inverseMass=inverseMass+collisionAngularInverseMass(inertia,firstLever,normal);
        collisionParticipantInverseInertia(system,second,inertia);
        inverseMass=inverseMass+collisionAngularInverseMass(inertia,secondLever,normal);
        const float maximumInverseMass=maximumFinite(firstInverseMass,secondInverseMass);
        const float minimumImpulse=finiteRatio(system->tuning->normalImpulseMinimum,maximumInverseMass);
        const float numerator=-(restitution+1.0f)*normalSpeed;
        float magnitude=finiteRatio(numerator,inverseMass);
        if (magnitude<minimumImpulse) magnitude=minimumImpulse;
        float normalImpulse[4]={},negativeImpulse[4]={};
        for (int lane=0;lane<3;++lane) {
            normalImpulse[lane]=normal[lane]*magnitude;
            negativeImpulse[lane]=0.0f-normalImpulse[lane];
        }
        // Native resource/effect callbacks can fail. Preserve that first
        // error before a failed activation repeats this original solver loop.
        float secondFraction=collisionParticipantImpulseFraction(second,normalImpulse,secondLever);
        if (system->result!=COLLISION_SYSTEM_OK) return;
        float firstFraction=collisionParticipantImpulseFraction(first,negativeImpulse,firstLever);
        if (system->result!=COLLISION_SYSTEM_OK) return;
        if (firstFraction==-57575.0f || secondFraction==-57575.0f) return;
        int fractionEndsContact=1;
        if (firstFraction<0.0f) firstFraction=-firstFraction;
        else if (secondFraction<0.0f) secondFraction=-secondFraction;
        else fractionEndsContact=0;
        const float fraction=minimumFinite(firstFraction,secondFraction);
        magnitude=fraction*magnitude;
        const float firstFriction=collisionParticipantFriction(system,first);
        const float secondFriction=collisionParticipantFriction(system,second);
        float friction=maximumFinite(firstFriction,secondFriction);
        CollisionParticipant* wallCar=NULL;
        if (firstInverseMass==0.0f && secondFriction==0.3f && second->shape->kind==0
                && (double)collisionResponseShapeFrame(second->shape)[10]>0.7) wallCar=second;
        else if (secondInverseMass==0.0f && firstFriction==0.3f && first->shape->kind==0
                && (double)collisionResponseShapeFrame(first->shape)[10]>0.7) wallCar=first;
        if (wallCar) friction=0.1f;
        const float tangentSpeed=sqrtf(collisionResponseDot3(tangent,tangent));
        float frictionMagnitude=magnitude*friction;
        if (tangentSpeed<system->tuning->tangentSpeedScale)
            frictionMagnitude=frictionMagnitude*finiteRatio(tangentSpeed,system->tuning->tangentSpeedScale);
        // Zero tangential speed has zero friction impulse. Dividing 0/0
        // would manufacture a console saturation value, not a direction.
        const float tangentScale=tangentSpeed==0.0f ? 0.0f : finiteRatio(-frictionMagnitude,tangentSpeed);
        float secondImpulse[4]={},firstImpulse[4]={};
        for (int lane=0;lane<3;++lane) {
            const float tangentialImpulse=tangent[lane]*tangentScale;
            const float normalPart=normal[lane]*magnitude;
            secondImpulse[lane]=normalPart+tangentialImpulse;
            firstImpulse[lane]=0.0f-secondImpulse[lane];
        }
        if (wallCar) {
            const float* velocity=wallCar->shape->bodyMode ? wallCar->shape->body->linearVelocity : wallCar->shape->vehicle.velocity;
            const float speed=sqrtf(collisionResponseDot3(velocity,velocity));
            if (30.0f<speed) {
                float scale=-0.025f;
                if (speed*scale<-0.4f) scale=finiteRatio(-0.4f,speed);
                float correction[4]={};
                for (int lane=0;lane<3;++lane) correction[lane]=velocity[lane]*scale;
                correction[2]=0.0f;
                float* impulse=firstInverseMass==0.0f ? secondImpulse : firstImpulse;
                float* lever=firstInverseMass==0.0f ? secondLever : firstLever;
                const float impulseSpeed=sqrtf(collisionResponseDot3(impulse,impulse));
                for (int lane=0;lane<3;++lane) impulse[lane]=impulse[lane]+correction[lane];
                if (1.0f<speed+impulseSpeed) {
                    const float scaledSpeed=speed*0.5714285969734192f;
                    const float leverScale=finiteRatio(impulseSpeed,scaledSpeed+impulseSpeed);
                    for (int lane=0;lane<3;++lane) lever[lane]=lever[lane]*leverScale;
                }
            }
        }
        collisionParticipantApplyImpulse(system,second,secondImpulse,secondLever);
        if (system->result!=COLLISION_SYSTEM_OK) return;
        collisionParticipantApplyImpulse(system,first,firstImpulse,firstLever);
        if (system->result!=COLLISION_SYSTEM_OK) return;
        collisionImpactAccumulate(system,contact->pair,point,secondImpulse,tangentSpeed);
        if (system->result!=COLLISION_SYSTEM_OK) return;
        if (fraction==1.0f || fractionEndsContact) return;
    }
}

void collisionContactResolve(CollisionSystem* system, CollisionQueuedContact* contact)
{
    CollisionParticipant* first=contact->pair->first;
    CollisionParticipant* second=contact->pair->second;
    collisionParticipantNotify(first,second);
    if (system->result!=COLLISION_SYSTEM_OK) return;
    collisionParticipantNotify(second,first);
    if (system->result!=COLLISION_SYSTEM_OK) return;
    if (first->shape->active) collisionParticipantApplyStep(first,contact->view.time);
    if (second->shape->active) collisionParticipantApplyStep(second,contact->view.time);
    float firstPosition[4],secondPosition[4];
    for (int lane=0;lane<3;++lane) {
        firstPosition[lane]=collisionResponseShapePosition(first->shape)[lane]-(collisionParticipantMovable(first) ? contact->displacement[lane] : 0.0f);
        secondPosition[lane]=collisionResponseShapePosition(second->shape)[lane]+(collisionParticipantMovable(second) ? contact->displacement[lane] : 0.0f);
    }
    firstPosition[3]=secondPosition[3]=1.0f;
    if (first->shape->active) collisionShapeStorePosition(first->shape,firstPosition);
    if (second->shape->active) collisionShapeStorePosition(second->shape,secondPosition);
    collisionParticipantPrepare(first);
    collisionParticipantPrepare(second);
    collisionContactSolve(system,contact);
    if (system->result!=COLLISION_SYSTEM_OK) return;
    collisionParticipantPrepare(first);
    collisionParticipantPrepare(second);
    collisionParticipantCapture(system,first,contact->view.time);
    collisionParticipantCapture(system,second,contact->view.time);
}

void collisionContactPlaneNormal(float normal[4], const CollisionShape* planeShape,
                                  const CollisionShape* movingShape, const CollisionContactTuning* tuning)
{
    contactPlaneNormalAtPosition(normal,planeShape,collisionResponseShapePosition(movingShape),tuning);
}

float collisionContactProjection(const CollisionContact* contact, int32_t* valid,
                                 const CollisionContactTuning* tuning)
{
    const CollisionShape* plane;
    const CollisionShape* moving;
    if(contact->first->kind==3) {
        if(!contact->second->active) { *valid=0;return 0.0f; }
        plane=contact->first;moving=contact->second;
    } else if(contact->second->kind==3 && contact->first->active) {
        plane=contact->second;moving=contact->first;
    } else { *valid=0;return 0.0f; }
    float normal[4];
    collisionContactPlaneNormal(normal,plane,moving,tuning);
    *valid=1;
    return collisionResponseDot3(collisionResponseShapePosition(moving),normal);
}

int32_t collisionContactOrder(const CollisionContact* first, const CollisionContact* second,
                              const CollisionContactTuning* tuning)
{
    const float difference=first->time-second->time;
    if(fabsf(difference)<tuning->orderTolerance) {
        int32_t firstValid,secondValid;
        const float firstProjection=collisionContactProjection(first,&firstValid,tuning);
        const float secondProjection=collisionContactProjection(second,&secondValid,tuning);
        if(!firstValid || !secondValid) return 1;
        return !(secondProjection<firstProjection);
    }
    return !(second->time<first->time);
}

const CollisionContactTuning collisionContactDefaultTuning = {-5.000100135803223f, 0.0f};
