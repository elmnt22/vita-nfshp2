// Diagnostic (Windows only): HP2_ALLOC_TRACK=1 replaces the plain global
// operator new/delete, records a call stack for every allocation made after
// a race begins loading, and when the frontend is entered again logs the
// surviving allocations grouped by stack ("alloc.survivor" session events).
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include "bware/inc/bWare.hpp"
#pragma comment(lib,"dbghelp.lib")

namespace {
template<class T> struct RawAllocator {
    using value_type=T;
    RawAllocator()=default;
    template<class U> RawAllocator(const RawAllocator<U>&) noexcept {}
    static HANDLE Heap() noexcept {static const HANDLE heap=HeapCreate(0,0,0);return heap;}
    T* allocate(std::size_t n) {void* p=HeapAlloc(Heap(),0,n*sizeof(T));if(!p)throw std::bad_alloc();return static_cast<T*>(p);}
    void deallocate(T* p,std::size_t) noexcept {HeapFree(Heap(),0,p);}
    template<class U> bool operator==(const RawAllocator<U>&) const noexcept {return true;}
    template<class U> bool operator!=(const RawAllocator<U>&) const noexcept {return false;}
};
constexpr int Depth=14;
struct Record {std::size_t size;std::uint32_t epoch;void* stack[Depth];};
using Map=std::unordered_map<void*,Record,std::hash<void*>,std::equal_to<void*>,RawAllocator<std::pair<void* const,Record>>>;
Map* live=nullptr;
SRWLOCK lock=SRWLOCK_INIT;
std::atomic<std::uint32_t> epoch{0};
thread_local bool inside=false;
const bool enabled=[] {const char* v=std::getenv("HP2_ALLOC_TRACK");return v && *v=='1';}();

void Track(void* p,std::size_t size) noexcept
{
    const auto e=epoch.load(std::memory_order_relaxed);
    if(!enabled || !p || !e || inside)return;
    inside=true;
    Record r{};r.size=size;r.epoch=e;
    RtlCaptureStackBackTrace(2,Depth,r.stack,nullptr);
    AcquireSRWLockExclusive(&lock);
    try {if(!live)live=new(HeapAlloc(RawAllocator<char>::Heap(),0,sizeof(Map))) Map();(*live)[p]=r;}catch(...) {}
    ReleaseSRWLockExclusive(&lock);
    inside=false;
}
void Untrack(void* p) noexcept
{
    if(!enabled || !p || inside)return;
    AcquireSRWLockExclusive(&lock);
    if(live)live->erase(p);
    ReleaseSRWLockExclusive(&lock);
}
void* Allocate(std::size_t size)
{
    if(!size)size=1;
    void* p=std::malloc(size);
    if(!p)throw std::bad_alloc();
    Track(p,size);return p;
}
void Release(void* p) noexcept {if(!p)return;Untrack(p);std::free(p);}
}
extern "C" void* hp2TrackMalloc(std::size_t size) {void* p=std::malloc(size);Track(p,size);return p;}
extern "C" void* hp2TrackCalloc(std::size_t count,std::size_t size) {void* p=std::calloc(count,size);Track(p,count*size);return p;}
extern "C" void* hp2TrackRealloc(void* old,std::size_t size) {
    void* p=std::realloc(old,size);
    if(p || !size) {Untrack(old);Track(p,size);}
    return p;
}
extern "C" void hp2TrackFree(void* p) {Untrack(p);std::free(p);}

namespace {
void* AllocateAligned(std::size_t size,std::align_val_t align)
{
    if(!size)size=1;
    void* p=_aligned_malloc(size,static_cast<std::size_t>(align));
    if(!p)throw std::bad_alloc();
    Track(p,size);return p;
}
void ReleaseAligned(void* p) noexcept {if(!p)return;Untrack(p);_aligned_free(p);}
}
void* operator new(std::size_t size,std::align_val_t a) {return AllocateAligned(size,a);}
void* operator new[](std::size_t size,std::align_val_t a) {return AllocateAligned(size,a);}
void* operator new(std::size_t size,std::align_val_t a,const std::nothrow_t&) noexcept {try {return AllocateAligned(size,a);}catch(...) {return nullptr;}}
void* operator new[](std::size_t size,std::align_val_t a,const std::nothrow_t&) noexcept {try {return AllocateAligned(size,a);}catch(...) {return nullptr;}}
void operator delete(void* p,std::align_val_t) noexcept {ReleaseAligned(p);}
void operator delete[](void* p,std::align_val_t) noexcept {ReleaseAligned(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept {ReleaseAligned(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept {ReleaseAligned(p);}
void operator delete(void* p,std::align_val_t,const std::nothrow_t&) noexcept {ReleaseAligned(p);}
void operator delete[](void* p,std::align_val_t,const std::nothrow_t&) noexcept {ReleaseAligned(p);}
void* operator new(std::size_t size) {return Allocate(size);}
void* operator new[](std::size_t size) {return Allocate(size);}
void* operator new(std::size_t size,const std::nothrow_t&) noexcept {try {return Allocate(size);}catch(...) {return nullptr;}}
void* operator new[](std::size_t size,const std::nothrow_t&) noexcept {try {return Allocate(size);}catch(...) {return nullptr;}}
void operator delete(void* p) noexcept {Release(p);}
void operator delete[](void* p) noexcept {Release(p);}
void operator delete(void* p,std::size_t) noexcept {Release(p);}
void operator delete[](void* p,std::size_t) noexcept {Release(p);}
void operator delete(void* p,const std::nothrow_t&) noexcept {Release(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept {Release(p);}

namespace {
void AllocTrackCheckpoint(const char* tag) noexcept
{
    if(!enabled || !tag)return;
    using BlockSet=std::unordered_map<void*,std::size_t,std::hash<void*>,std::equal_to<void*>,RawAllocator<std::pair<void* const,std::size_t>>>;
    static BlockSet* baseline=nullptr;
    // The walk must not allocate: collect into reserved storage first.
    struct Block {void* p;std::size_t n;unsigned char head[32];};
    const auto walk=[](auto&& visit) {
        std::vector<Block,RawAllocator<Block>> blocks;
        blocks.reserve(2u<<20);
        const HANDLE heap=reinterpret_cast<HANDLE>(_get_heap_handle());
        if(!HeapLock(heap))return;
        PROCESS_HEAP_ENTRY entry{};
        while(HeapWalk(heap,&entry))
            if((entry.wFlags&PROCESS_HEAP_ENTRY_BUSY) && blocks.size()<blocks.capacity()) {
                Block b{entry.lpData,static_cast<std::size_t>(entry.cbData),{}};
                std::memcpy(b.head,entry.lpData,std::min<std::size_t>(32,b.n));
                blocks.push_back(b);
            }
        HeapUnlock(heap);
        for(auto& b:blocks)visit(b);
    };
    if(!std::strcmp(tag,"enter:GAMEFLOW_STATE_LOADING_TRACK_COMMON")) {
        if(!epoch.load()) {
            inside=true;
            baseline=new(HeapAlloc(RawAllocator<char>::Heap(),0,sizeof(BlockSet))) BlockSet();
            walk([&](const Block& b){(*baseline)[b.p]=b.n;});
            inside=false;
        }
        epoch.fetch_add(1,std::memory_order_relaxed);return;
    }
    if(!std::strcmp(tag,"enter:GAMEFLOW_STATE_UNLOADING_FRONTEND") && baseline && epoch.load()) {
        // Untracked survivors: busy heap blocks born after the first race
        // began that no tracked operator new owns (malloc, drivers, ...).
        inside=true;
        std::vector<Block,RawAllocator<Block>> fresh;
        AcquireSRWLockShared(&lock);
        walk([&](const Block& b){
            if(baseline->count(b.p))return;
            if(live && live->count(b.p))return;
            fresh.push_back(b);
        });
        ReleaseSRWLockShared(&lock);
        std::unordered_map<std::size_t,std::pair<std::size_t,std::size_t>,std::hash<std::size_t>,std::equal_to<std::size_t>,
            RawAllocator<std::pair<const std::size_t,std::pair<std::size_t,std::size_t>>>> bySize;
        std::size_t total=0;
        for(auto& f:fresh) {total+=f.n;auto& b=bySize[f.n];++b.first;b.second+=f.n;}
        std::vector<std::pair<std::size_t,std::pair<std::size_t,std::size_t>>,RawAllocator<std::pair<std::size_t,std::pair<std::size_t,std::size_t>>>> sizes(bySize.begin(),bySize.end());
        std::sort(sizes.begin(),sizes.end(),[](auto& a,auto& b){return a.second.second>b.second.second;});
        char line[512];
        std::snprintf(line,sizeof(line),"epoch=%u blocks=%zu total_kb=%zu",epoch.load(),fresh.size(),total/1024);
        hp2::platform::RecordSessionEvent("alloc.untracked",line);
        for(std::size_t at=0;at<sizes.size() && at<25;++at) {
            const unsigned char* sample=nullptr;for(auto& f:fresh)if(f.n==sizes[at].first) {sample=f.head;break;}
            int k=std::snprintf(line,sizeof(line),"size=%zu count=%zu kb=%zu head=",sizes[at].first,sizes[at].second.first,sizes[at].second.second/1024);
            const auto* bytes=sample;
            for(std::size_t i=0;i<32 && i<sizes[at].first && k<480;++i)k+=std::snprintf(line+k,sizeof(line)-k,"%02x",bytes[i]);
            hp2::platform::RecordSessionEvent("alloc.untracked_size",line);
        }
        inside=false;return;
    }
    if(std::strcmp(tag,"enter:GAMEFLOW_STATE_IN_FRONTEND") || !epoch.load())return;
    inside=true;
    struct Group {std::size_t bytes=0,count=0;std::uint32_t firstEpoch=~0u,lastEpoch=0;void* stack[Depth]{};};
    std::vector<Group,RawAllocator<Group>> groups;
    {
        std::unordered_map<std::uint64_t,std::size_t,std::hash<std::uint64_t>,std::equal_to<std::uint64_t>,
            RawAllocator<std::pair<const std::uint64_t,std::size_t>>> index;
        AcquireSRWLockShared(&lock);
        if(live)for(const auto& [p,r]:*live) {
            std::uint64_t h=1469598103934665603ull;
            for(int i=0;i<Depth;++i)h=(h^reinterpret_cast<std::uintptr_t>(r.stack[i]))*1099511628211ull;
            auto [it,added]=index.try_emplace(h,groups.size());
            if(added) {groups.emplace_back();std::memcpy(groups.back().stack,r.stack,sizeof(r.stack));}
            auto& g=groups[it->second];g.bytes+=r.size;++g.count;
            g.firstEpoch=std::min(g.firstEpoch,r.epoch);g.lastEpoch=std::max(g.lastEpoch,r.epoch);
        }
        ReleaseSRWLockShared(&lock);
    }
    std::sort(groups.begin(),groups.end(),[](const Group& a,const Group& b){return a.bytes>b.bytes;});
    static bool symbols=false;
    const HANDLE process=GetCurrentProcess();
    if(!symbols) {SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES);symbols=SymInitialize(process,nullptr,TRUE)!=FALSE;}
    std::size_t total=0;for(const auto& g:groups)total+=g.bytes;
    char line[4096];
    std::snprintf(line,sizeof(line),"epoch=%u groups=%zu total_kb=%zu",epoch.load(),groups.size(),total/1024);
    hp2::platform::RecordSessionEvent("alloc.survivors",line);
    for(std::size_t at=0;at<groups.size() && at<60;++at) {
        const auto& g=groups[at];
        int n=std::snprintf(line,sizeof(line),"rank=%zu kb=%zu count=%zu epochs=%u-%u stack=",at,g.bytes/1024,g.count,g.firstEpoch,g.lastEpoch);
        for(int i=0;i<Depth && g.stack[i] && n<static_cast<int>(sizeof(line))-200;++i) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO)+256]{};
            auto* symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=255;
            DWORD64 displacement=0;
            IMAGEHLP_LINE64 source{};source.SizeOfStruct=sizeof(source);DWORD lineDisplacement=0;
            const auto address=reinterpret_cast<DWORD64>(g.stack[i]);
            const char* name=symbols && SymFromAddr(process,address,&displacement,symbol) ? symbol->Name : "?";
            if(symbols && SymGetLineFromAddr64(process,address,&lineDisplacement,&source)) {
                const char* file=std::strrchr(source.FileName,'\\');file=file?file+1:source.FileName;
                n+=std::snprintf(line+n,sizeof(line)-n,"%s(%s:%lu) | ",name,file,static_cast<unsigned long>(source.LineNumber));
            } else n+=std::snprintf(line+n,sizeof(line)-n,"%s | ",name);
        }
        hp2::platform::RecordSessionEvent("alloc.survivor",line);
    }
    inside=false;
}
const bool registered=[] {if(enabled)hp2::platform::BindHeapCheckpointHook(AllocTrackCheckpoint);return true;}();
}
#endif
