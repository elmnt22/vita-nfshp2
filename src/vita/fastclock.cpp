// libstdc++'s steady_clock::now() on Vita calls newlib gettimeofday (~6 us per
// call, and wall time). The link wraps it (-Wl,--wrap=<mangled now>) with the
// kernel process timer: microseconds since launch, monotonic, ~0.7 us.
#include <psp2/kernel/processmgr.h>

#include <chrono>

extern "C" std::chrono::steady_clock::time_point __wrap__ZNSt6chrono3_V212steady_clock3nowEv() noexcept
{
    return std::chrono::steady_clock::time_point(
        std::chrono::microseconds(static_cast<long long>(sceKernelGetProcessTimeWide())));
}
