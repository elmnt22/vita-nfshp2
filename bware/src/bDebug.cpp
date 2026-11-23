#include "bware/inc/bWare.hpp"
#include <chrono>
#include <atomic>
#include <cstdio>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <malloc.h>
#elif defined(__vita__)
#include <malloc.h>
#endif


namespace hp2::platform {
namespace {std::atomic<NativeSessionSink> sessionSink{nullptr};}
void BindSessionSink(NativeSessionSink sink) noexcept
{sessionSink.store(sink,std::memory_order_release);}
void RecordSessionEvent(const char* event,const char* detail) noexcept
{if(const auto sink=sessionSink.load(std::memory_order_acquire))sink(event,detail);}

HeapUsage QueryHeapUsage() noexcept
{
    HeapUsage usage;
#if defined(__vita__)
    const auto info=mallinfo();
    usage.used=info.uordblks;usage.arena=info.arena;usage.free=info.fordblks;
    usage.largestFree=0;usage.valid=true;
#elif defined(_WIN32)
    const HANDLE heap=reinterpret_cast<HANDLE>(_get_heap_handle());
    if(!HeapLock(heap))return usage;
    PROCESS_HEAP_ENTRY entry{};
    while(HeapWalk(heap,&entry)) {
        if(entry.wFlags&PROCESS_HEAP_ENTRY_BUSY)usage.used+=entry.cbData;
        else if(!(entry.wFlags&(PROCESS_HEAP_REGION|PROCESS_HEAP_UNCOMMITTED_RANGE))) {
            usage.free+=entry.cbData;
            if(entry.cbData>usage.largestFree)usage.largestFree=entry.cbData;
        }
        if(entry.wFlags&PROCESS_HEAP_REGION)usage.arena+=entry.Region.dwCommittedSize;
    }
    HeapUnlock(heap);usage.valid=true;
#endif
    return usage;
}

namespace {std::atomic<HeapCheckpointHook> heapHook{nullptr};}
void BindHeapCheckpointHook(HeapCheckpointHook hook) noexcept {heapHook.store(hook,std::memory_order_release);}
namespace {std::atomic<OutOfMemoryHook> outOfMemoryHook{nullptr};}
void BindOutOfMemoryHook(OutOfMemoryHook hook) noexcept {outOfMemoryHook.store(hook,std::memory_order_release);}
void ReportOutOfMemory(const char* what) noexcept
{
    if(const auto hook=outOfMemoryHook.load(std::memory_order_acquire))hook(what);
}
void RecordHeapCheckpoint(const char* tag) noexcept
{
    if(const auto hook=heapHook.load(std::memory_order_acquire))hook(tag);
    const auto usage=QueryHeapUsage();
    if(!usage.valid)return;
    char detail[192];
    std::snprintf(detail,sizeof(detail),"tag=%s used_kb=%llu arena_kb=%llu free_kb=%llu largest_free_kb=%llu",
        tag?tag:"?",(unsigned long long)(usage.used/1024),(unsigned long long)(usage.arena/1024),
        (unsigned long long)(usage.free/1024),(unsigned long long)(usage.largestFree/1024));
    RecordSessionEvent("memory.checkpoint",detail);
}

std::uint32_t TicksFromNanoseconds(std::uint64_t ns) noexcept {
    constexpr std::uint64_t kBillion = 1000000000ull;
    const auto seconds = ns / kBillion;
    const auto remainder = ns % kBillion;
    const std::uint64_t integerTicks = (seconds * kTickerFrequency) & 0xffffffffull;
    const std::uint64_t fractionalTicks = (remainder * kTickerFrequency) / kBillion;
    return static_cast<std::uint32_t>(integerTicks + fractionalTicks);
}

std::uint32_t TicksNow() noexcept {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return TicksFromNanoseconds(static_cast<std::uint64_t>(nanoseconds));
}

float tickerMilliseconds(std::uint32_t from, std::uint32_t to) noexcept {
    const std::uint32_t difference = to - from;
    float value;
    if (static_cast<std::int32_t>(difference) >= 0) {
        value = static_cast<float>(static_cast<std::int32_t>(difference));
    } else {
        value = static_cast<float>(static_cast<std::int32_t>((difference & 1u) | (difference >> 1)));
        value += value;
    }
    // Retain the game's original clock conversion constant.
    return value / kTimerConversionFrequency * 1000.0f;
}

std::uint32_t fixedTickerDifference(std::uint32_t from, std::uint32_t to) noexcept {
    return ((to - from) << 6) / 293u;
}
} // namespace hp2::platform




#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(__SWITCH__) && !defined(__vita__)
#include <unistd.h>
#endif

extern "C" std::uint32_t hp2NativeTickerSource(void) {
    return hp2::platform::TicksNow();
}

/* PAL250970 asks its actual development host for the computer name through
 * bFunk command58. On Windows the operating system owns that value. Linux
 * supplies the same host-name operation for the explicitly separate CPU
 * verification build. A failed transport produces an empty string/false,
 * matching the source failure branch; it never reports synthetic success. */
extern "C" int32_t bGetComputerName(char* output,int32_t capacity)
{
    if(!output || capacity<=0) return 0;
#if defined(_WIN32)
    DWORD available=static_cast<DWORD>(capacity);
    if(GetComputerNameA(output,&available)) return 1;
#elif !defined(__SWITCH__) && !defined(__vita__)
    if(gethostname(output,static_cast<size_t>(capacity))==0) return 1;
#endif
    output[0]='\0';
    return 0;
}
