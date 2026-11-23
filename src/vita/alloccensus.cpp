// Vita heap front end (all malloc/calloc/realloc/memalign/free are wrapped at
// link time).
//
// Large blocks (>= BigBytes) come from their own kernel memory blocks, not the
// newlib heap. The kernel maps any free pages into one contiguous block, so a
// big request (a 14 MB frontend bundle after a long race) can't fail because
// the newlib arena is fragmented by small survivors. Each side falls back to
// the other when it is exhausted, so the newlib/kernel split is soft.
//
// Diagnostic: census of large heap blocks (alloc_census.txt in the data
// directory turns it on). malloc/calloc/realloc/memalign/free are wrapped at
// link time; blocks of at least MinBytes are recorded with a short call stack
// and the race epoch they were born in. AllocCensusDump logs the survivors
// born since the first race, grouped by stack (raw addresses for addr2line).
#include <unwind.h>
#include <malloc.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <psp2/kernel/sysmem.h>
#include "alloccensus.hpp"

extern "C" {
size_t __real_malloc_usable_size(void*);
void* __real_malloc(size_t);
void* __real_calloc(size_t,size_t);
void* __real_realloc(void*,size_t);
void* __real_memalign(size_t,size_t);
void __real_free(void*);
}

namespace {
constexpr std::size_t MinBytes=64u*1024u;
constexpr int Depth=8;
constexpr std::size_t Slots=1u<<15;
struct Entry {void* p;std::uint32_t size,epoch;std::uintptr_t stack[Depth];};
Entry table[Slots];
std::atomic_flag spin=ATOMIC_FLAG_INIT;
std::atomic<bool> enabled{false};
std::atomic<std::uint32_t> epoch{0};
std::uint32_t dropped{};
thread_local bool inside=false;

// Kernel-backed big blocks.
constexpr std::size_t BigBytes=1u<<20;
constexpr std::size_t KernelReserve=8u<<20; // stacks, threads, system
constexpr std::size_t BigSlots=1u<<12;
struct Big {void* p;SceUID uid;std::uint32_t size;};
Big bigTable[BigSlots];
std::atomic_flag bigSpin=ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> bigLive{0},bigPeak{0},bigAllocs{0},bigFallbacks{0};
std::atomic<std::uint32_t> bigCount{0};
struct BigLock {
    BigLock() {while(bigSpin.test_and_set(std::memory_order_acquire)) {}}
    ~BigLock() {bigSpin.clear(std::memory_order_release);}
};
std::size_t BigSlotOf(const void* p) {return (reinterpret_cast<std::uintptr_t>(p)>>12)*2654435761u&(BigSlots-1);}
bool IsPage(const void* p) {return p && !(reinterpret_cast<std::uintptr_t>(p)&4095u);}
// Returns the stored size (0 when p is not a big block).
std::size_t BigSize(const void* p)
{
    if(!IsPage(p))return 0;
    BigLock lock;
    for(std::size_t at=BigSlotOf(p),n=0;n<BigSlots && bigTable[at].p;at=(at+1)&(BigSlots-1),++n)
        if(bigTable[at].p==p)return bigTable[at].size;
    return 0;
}
void* BigAlloc(std::size_t n)
{
    if(!n || n>0x7ff00000u)return nullptr;
    SceKernelFreeMemorySizeInfo info{};info.size=sizeof(info);
    const std::size_t total=(n+4095u)&~std::size_t(4095u);
    if(sceKernelGetFreeMemorySize(&info)<0 || static_cast<std::size_t>(info.size_user)<total+KernelReserve)return nullptr;
    if(bigCount.load(std::memory_order_relaxed)>=BigSlots/2)return nullptr;
    const SceUID uid=sceKernelAllocMemBlock("hp2_big",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,total,nullptr);
    if(uid<0)return nullptr;
    void* base=nullptr;
    if(sceKernelGetMemBlockBase(uid,&base)<0 || !base) {sceKernelFreeMemBlock(uid);return nullptr;}
    {
        BigLock lock;
        std::size_t at=BigSlotOf(base);
        while(bigTable[at].p)at=(at+1)&(BigSlots-1);
        bigTable[at]={base,uid,static_cast<std::uint32_t>(n)};
    }
    bigCount.fetch_add(1,std::memory_order_relaxed);bigAllocs.fetch_add(1,std::memory_order_relaxed);
    const auto live=bigLive.fetch_add(total,std::memory_order_relaxed)+total;
    for(auto peak=bigPeak.load(std::memory_order_relaxed);live>peak && !bigPeak.compare_exchange_weak(peak,live);) {}
    return base;
}
// Frees p when it is a big block; false otherwise.
bool BigFree(void* p)
{
    if(!IsPage(p))return false;
    SceUID uid=-1;std::uint32_t size=0;
    {
        BigLock lock;
        std::size_t at=BigSlotOf(p),n=0;
        for(;n<BigSlots && bigTable[at].p && bigTable[at].p!=p;at=(at+1)&(BigSlots-1),++n) {}
        if(n>=BigSlots || bigTable[at].p!=p)return false;
        uid=bigTable[at].uid;size=bigTable[at].size;
        std::size_t hole=at;
        for(std::size_t next=(hole+1)&(BigSlots-1);bigTable[next].p;next=(next+1)&(BigSlots-1)) {
            const auto home=BigSlotOf(bigTable[next].p);
            const bool between=hole<=next ? (home>hole && home<=next):(home>hole || home<=next);
            if(between)continue;
            bigTable[hole]=bigTable[next];hole=next;
        }
        bigTable[hole]=Big{};
    }
    sceKernelFreeMemBlock(uid);
    bigCount.fetch_sub(1,std::memory_order_relaxed);
    bigLive.fetch_sub((static_cast<std::size_t>(size)+4095u)&~std::size_t(4095u),std::memory_order_relaxed);
    return true;
}
std::size_t UsableSize(void* p) {const auto big=BigSize(p);return big ? big:__real_malloc_usable_size(p);}

struct Lock {
    Lock() {while(spin.test_and_set(std::memory_order_acquire)) {}}
    ~Lock() {spin.clear(std::memory_order_release);}
};
std::size_t SlotOf(void* p) {return (reinterpret_cast<std::uintptr_t>(p)>>4)*2654435761u&(Slots-1);}

struct Walk {std::uintptr_t* out;int n;int skip;};
_Unwind_Reason_Code Visit(_Unwind_Context* context,void* arg)
{
    auto& walk=*static_cast<Walk*>(arg);
    const auto ip=_Unwind_GetIP(context);
    if(walk.skip>0) {--walk.skip;return _URC_NO_REASON;}
    walk.out[walk.n++]=ip;
    return walk.n>=Depth ? _URC_END_OF_STACK:_URC_NO_REASON;
}

void Track(void* p,std::size_t size)
{
    if(!p || size<MinBytes || !enabled.load(std::memory_order_relaxed) || inside)return;
    inside=true;
    Entry e{};e.p=p;e.size=static_cast<std::uint32_t>(size);e.epoch=epoch.load(std::memory_order_relaxed);
    Walk walk{e.stack,0,2};
    _Unwind_Backtrace(Visit,&walk);
    {
        Lock lock;
        std::size_t at=SlotOf(p),probes=0;
        while(table[at].p && table[at].p!=p && probes<Slots) {at=(at+1)&(Slots-1);++probes;}
        if(probes<Slots)table[at]=e;else ++dropped;
    }
    inside=false;
}
void Untrack(void* p)
{
    if(!p || !enabled.load(std::memory_order_relaxed))return;
    if(UsableSize(p)<MinBytes)return;
    Lock lock;
    std::size_t at=SlotOf(p),probes=0;
    while(table[at].p && table[at].p!=p && probes<Slots) {at=(at+1)&(Slots-1);++probes;}
    if(table[at].p!=p)return;
    // Backward-shift delete keeps the probe chains intact.
    std::size_t hole=at;
    for(std::size_t next=(hole+1)&(Slots-1);table[next].p;next=(next+1)&(Slots-1)) {
        const auto home=SlotOf(table[next].p);
        const bool between=hole<=next ? (home>hole && home<=next):(home>hole || home<=next);
        if(between)continue;
        table[hole]=table[next];hole=next;
    }
    table[hole]=Entry{};
}
}

extern "C" {
void* __wrap_malloc(size_t n)
{
    void* p=n>=BigBytes ? BigAlloc(n):nullptr;
    if(!p)p=__real_malloc(n);
    if(!p && n>=64u*1024u && (p=BigAlloc(n)))bigFallbacks.fetch_add(1,std::memory_order_relaxed);
    Track(p,n);return p;
}
void* __wrap_calloc(size_t c,size_t n)
{
    if(n && c>SIZE_MAX/n)return nullptr;
    const size_t bytes=c*n;
    if(bytes>=BigBytes) {
        if(void* p=BigAlloc(bytes)) {std::memset(p,0,bytes);Track(p,bytes);return p;}
    }
    void* p=__real_calloc(c,n);
    if(!p && bytes>=64u*1024u && (p=BigAlloc(bytes))) {std::memset(p,0,bytes);bigFallbacks.fetch_add(1,std::memory_order_relaxed);}
    Track(p,bytes);return p;
}
void* __wrap_memalign(size_t a,size_t n)
{
    void* p=(n>=BigBytes && a<=4096u) ? BigAlloc(n):nullptr;
    if(!p)p=__real_memalign(a,n);
    if(!p && a<=4096u && n>=64u*1024u && (p=BigAlloc(n)))bigFallbacks.fetch_add(1,std::memory_order_relaxed);
    Track(p,n);return p;
}
void __wrap_free(void* p)
{
    if(!p)return;
    Untrack(p);
    if(!BigFree(p))__real_free(p);
}
size_t __wrap_malloc_usable_size(void* p) {return p ? UsableSize(p):0;}
void* __wrap_realloc(void* old,size_t n)
{
    if(!old)return __wrap_malloc(n);
    if(!n) {__wrap_free(old);return nullptr;}
    const auto oldBig=BigSize(old);
    if(!oldBig && n<BigBytes) {
        Untrack(old);
        void* p=__real_realloc(old,n);
        if(p) {Track(p,n);return p;}
        Track(old,__real_malloc_usable_size(old));
        if(n<64u*1024u)return nullptr;
        // newlib exhausted: move to a big block below.
    }
    const std::size_t oldSize=oldBig ? oldBig:__real_malloc_usable_size(old);
    if(oldBig && n<=oldBig && n>=BigBytes)return old; // shrinking in place
    void* p=__wrap_malloc(n);
    if(!p)return nullptr;
    std::memcpy(p,old,oldSize<n ? oldSize:n);
    __wrap_free(old);
    return p;
}
}

namespace vita_census {
void Enable() {enabled.store(true);}
bool Enabled() {return enabled.load();}
void NextEpoch() {epoch.fetch_add(1);}

void Dump(const char* tag,void(*line)(const char*,const char*))
{
    if(!enabled.load() || !line)return;
    struct Group {std::uintptr_t stack[Depth];std::uint64_t bytes;std::uint32_t count,minEpoch,maxEpoch;};
    static Group groups[2048];
    std::size_t used=0;std::uint64_t total=0,totalAll=0;std::uint32_t blocks=0;
    {
        Lock lock;
        for(const auto& e:table) {
            if(!e.p)continue;
            totalAll+=e.size;
            if(!e.epoch)continue;
            total+=e.size;++blocks;
            std::size_t g=0;
            for(;g<used;++g)if(!std::memcmp(groups[g].stack,e.stack,sizeof(e.stack)))break;
            if(g==used) {
                if(used==sizeof(groups)/sizeof(groups[0]))continue;
                std::memcpy(groups[used].stack,e.stack,sizeof(e.stack));
                groups[used].bytes=0;groups[used].count=0;groups[used].minEpoch=~0u;groups[used].maxEpoch=0;++used;
            }
            groups[g].bytes+=e.size;++groups[g].count;
            if(e.epoch<groups[g].minEpoch)groups[g].minEpoch=e.epoch;
            if(e.epoch>groups[g].maxEpoch)groups[g].maxEpoch=e.epoch;
        }
    }
    char detail[512];
    std::snprintf(detail,sizeof(detail),"tag=%s epoch=%u survivors_kb=%llu blocks=%u groups=%u tracked_all_kb=%llu dropped=%u",
        tag,epoch.load(),static_cast<unsigned long long>(total/1024),blocks,static_cast<unsigned>(used),
        static_cast<unsigned long long>(totalAll/1024),dropped);
    line("alloc.census",detail);
    for(int round=0;round<40;++round) {
        std::size_t best=used;
        for(std::size_t g=0;g<used;++g)if(groups[g].count && (best==used || groups[g].bytes>groups[best].bytes))best=g;
        if(best==used)break;
        auto& g=groups[best];
        int k=std::snprintf(detail,sizeof(detail),"kb=%llu count=%u epochs=%u-%u stack=",
            static_cast<unsigned long long>(g.bytes/1024),g.count,g.minEpoch,g.maxEpoch);
        for(int i=0;i<Depth && g.stack[i] && k<480;++i)k+=std::snprintf(detail+k,sizeof(detail)-k,"%s%08x",i?",":"",static_cast<unsigned>(g.stack[i]));
        line("alloc.census_group",detail);
        g.count=0;
    }
}
}

namespace vita_heap {
void Stats(char* out,std::size_t size)
{
    SceKernelFreeMemorySizeInfo info{};info.size=sizeof(info);
    sceKernelGetFreeMemorySize(&info);
    std::snprintf(out,size,"big_live_kb=%llu big_peak_kb=%llu big_blocks=%u big_allocs=%llu big_fallbacks=%llu kernel_free_kb=%d",
        static_cast<unsigned long long>(bigLive.load()/1024),static_cast<unsigned long long>(bigPeak.load()/1024),
        bigCount.load(),static_cast<unsigned long long>(bigAllocs.load()),static_cast<unsigned long long>(bigFallbacks.load()),
        info.size_user/1024);
}
}
