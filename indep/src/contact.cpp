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
        const uint32_t runStart=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
        generation_=(generation_+1)&0xffffu;
        state_.store((generation_<<16)|0xffffu);
        function_=function;context_=context;count_=count;floatingPoint_=ReadFloatingPoint();
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
            const uint32_t taskStart=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
            function(context,index);++collisionPoolMainTasks;
            if(collisionWitnessClockMicros)collisionPoolMainMicros+=collisionWitnessClockMicros()-taskStart;
        }
        const uint32_t waitStart=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
        while(inFlight_.load()!=0) {}
        if(collisionWitnessClockMicros) {
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
                Instance().Loop();
                return nullptr;
            },&slots_[slot])==0) {pthread_detach(thread);++helpers_;}
            pthread_attr_destroy(&attributes);
        }
#else
        const unsigned hardware=std::thread::hardware_concurrency();
        const unsigned wanted=hardware>=3 ? 2u:hardware>=2 ? 1u:0u;
        for(unsigned slot=0;slot<wanted;++slot) {
            try {std::thread([this] {Loop();}).detach();++helpers_;} catch(...) {break;}
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
#if defined(__vita__)
                    if(!hot)sceKernelChangeThreadPriority(0,110);
#endif
                    const uint32_t saved=ReadFloatingPoint();
                    WriteFloatingPoint(floatingPoint_);
                    do {
                        const uint32_t taskStart=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
                        function_(context_,index);collisionPoolHelperTasks.fetch_add(1,std::memory_order_relaxed);
                        if(collisionWitnessClockMicros)collisionPoolHelperMicros.fetch_add(collisionWitnessClockMicros()-taskStart,std::memory_order_relaxed);
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
    query.result=collisionWitnessQueryDetached(&query.witness,query.firstFrame,query.firstPosition,
        query.secondFrame,query.secondPosition,&query.state,query.counters);
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
        pool.Close();
        for (unsigned k=committed;k<width;++k) if (queries[k].done.load(std::memory_order_acquire)) ++collisionSpecWasted;
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

void collisionContactSeparate(CollisionSystem* system, CollisionQueuedContact* contact, float rewind)
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

float collisionContactFindTime(CollisionSystem* system, CollisionQueuedContact* contact)
{
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
    const float midpoint=findTimeSpeculative(system,contact,&contact->pair->witness,system->randomState,&result);
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
