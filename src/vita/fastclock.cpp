// libstdc++'s steady_clock::now() on Vita calls newlib gettimeofday (~6 us per
// call, and wall time). The link wraps it (-Wl,--wrap=<mangled now>) with the
// kernel process timer: microseconds since launch, monotonic, ~0.7 us.
#include <psp2/kernel/processmgr.h>

#include <chrono>

#include <atomic>
#include <cstdint>
#include <cstdio>
extern uint32_t diagClockReads;

// Diagnostic (clock_census.txt): timer reads per call site. Fixed open-
// addressed table keyed by return address; races only lose a count.
namespace vita_clock {
bool census=false;
namespace {
constexpr unsigned Slots=1024;
std::atomic<std::uintptr_t> sites[Slots];
std::atomic<std::uint32_t> counts[Slots];
}
void Note(const void* site) noexcept
{
    const auto key=reinterpret_cast<std::uintptr_t>(site);
    for(unsigned at=(key>>1)*2654435761u%Slots,n=0;n<32;++n,at=(at+1)%Slots) {
        auto held=sites[at].load(std::memory_order_relaxed);
        if(held!=key && !held && !sites[at].compare_exchange_strong(held,key,std::memory_order_relaxed) && held!=key)continue;
        if(held && held!=key)continue;
        counts[at].fetch_add(1,std::memory_order_relaxed);return;
    }
}
// Top sites since the last dump, as "0xaddr:count ..." (counts reset).
void Dump(char* out,std::size_t size) noexcept
{
    struct Row {std::uintptr_t site;std::uint32_t count;} top[24]{};
    for(unsigned at=0;at<Slots;++at) {
        const auto count=counts[at].exchange(0,std::memory_order_relaxed);
        if(!count)continue;
        Row row{sites[at].load(std::memory_order_relaxed),count};
        for(auto& slot:top)if(row.count>slot.count)std::swap(row,slot);
    }
    std::size_t used=0;out[0]=0;
    for(const auto& row:top) {
        if(!row.count || used+24>=size)break;
        used+=std::snprintf(out+used,size-used,"0x%08lx:%lu ",static_cast<unsigned long>(row.site),static_cast<unsigned long>(row.count));
    }
}
}

extern "C" std::chrono::steady_clock::time_point __wrap__ZNSt6chrono3_V212steady_clock3nowEv() noexcept
{
    ++diagClockReads;
    if(vita_clock::census)vita_clock::Note(__builtin_return_address(0));
    return std::chrono::steady_clock::time_point(
        std::chrono::microseconds(static_cast<long long>(sceKernelGetProcessTimeWide())));
}
