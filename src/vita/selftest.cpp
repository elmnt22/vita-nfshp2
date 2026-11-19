// Startup self-tests for Vita-only fast paths (logged, never fatal).
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" std::int32_t ecstasyClassifyBounds(const float minimum[3], const float maximum[3], const float matrix[16]);
extern "C" void vectorMatrixTransform(float output[4], const float matrix[16], const float input[4]);
void vectorMatrixTransformScalar(float output[4], const float matrix[16], const float input[4]);
extern int bMatrixNeonEnabled;

namespace vita_selftest {
namespace {
// Verbatim scalar reference (ecstasye.cpp portable branch).
std::int32_t ReferenceClassify(const float minimum[3], const float maximum[3], const float matrix[16])
{
    float base[4], xStep[4], yStep[4], zStep[4];
    const float x = maximum[0] - minimum[0], y = maximum[1] - minimum[1], z = maximum[2] - minimum[2];
    for (int axis = 0; axis < 4; ++axis) {
        const float first = matrix[axis] * minimum[0] + matrix[4 + axis] * minimum[1];
        base[axis] = (first + matrix[8 + axis] * minimum[2]) + matrix[12 + axis];
        xStep[axis] = matrix[axis] * x; yStep[axis] = matrix[4 + axis] * y; zStep[axis] = matrix[8 + axis] * z;
    }
    float corners[4][4];
    for (int axis = 0; axis < 4; ++axis) {
        corners[0][axis] = base[axis];
        corners[1][axis] = base[axis] + xStep[axis];
        corners[2][axis] = corners[1][axis] + yStep[axis];
        corners[3][axis] = base[axis] + yStep[axis];
    }
    std::uint32_t any = 0, common = 63;
    for (int layer = 0; layer < 2; ++layer) for (int corner = 0; corner < 4; ++corner) {
        float point[4];
        for (int axis = 0; axis < 4; ++axis) point[axis] = layer ? corners[corner][axis] + zStep[axis] : corners[corner][axis];
        const float limit = std::fabs(point[3]); std::uint32_t flags = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (point[axis] > limit) flags |= 1u << (axis * 2);
            if (point[axis] < -limit) flags |= 2u << (axis * 2);
        }
        any |= flags; common &= flags;
    }
    return any == 0 ? 2 : common == 0 ? 1 : 0;
}

std::uint32_t state = 0x12345678u;
float Random(float scale)
{
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return (static_cast<float>(state & 0xffffffu) / 8388608.0f - 1.0f) * scale;
}

volatile std::uint64_t sink;
template<class F> double NsPerCall(F f)
{
    const auto start = sceKernelGetProcessTimeWide();
    for (int i = 0; i < 20000; ++i) sink += f();
    return static_cast<double>(sceKernelGetProcessTimeWide() - start) * 1000.0 / 20000.0;
}
}

void Run(void (*log)(const char* event, const char* detail))
{
    char detail[256];
    std::snprintf(detail, sizeof(detail),
        "steady_now_ns=%.0f process_wide_ns=%.0f process_low_ns=%.0f system_wide_ns=%.0f",
        NsPerCall([] { return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()); }),
        NsPerCall([] { return static_cast<std::uint64_t>(sceKernelGetProcessTimeWide()); }),
        NsPerCall([] { return static_cast<std::uint64_t>(sceKernelGetProcessTimeLow()); }),
        NsPerCall([] { return static_cast<std::uint64_t>(sceKernelGetSystemTimeWide()); }));
    log("vita.selftest.timers", detail);

    unsigned mismatches = 0, inside = 0, outside = 0, crossing = 0;
    const auto started = sceKernelGetProcessTimeWide();
    for (int test = 0; test < 200000; ++test) {
        float matrix[16], minimum[3], maximum[3];
        for (auto& value : matrix) value = Random(4.0f);
        if (test & 1) { matrix[3] = Random(0.1f); matrix[7] = Random(0.1f); matrix[11] = Random(1.0f); matrix[15] = Random(20.0f); }
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = Random(50.0f);
            maximum[axis] = minimum[axis] + std::fabs(Random(test % 3 ? 10.0f : 0.5f));
        }
        if ((test % 97) == 0) maximum[1] = minimum[1];
        const auto expected = ReferenceClassify(minimum, maximum, matrix);
        const auto actual = ecstasyClassifyBounds(minimum, maximum, matrix);
        if (expected != actual) ++mismatches;
        (expected == 2 ? inside : expected == 1 ? crossing : outside)++;
    }
    std::snprintf(detail, sizeof(detail), "cases=200000 mismatches=%u inside=%u crossing=%u outside=%u ms=%.1f",
        mismatches, inside, crossing, outside, (sceKernelGetProcessTimeWide() - started) / 1000.0);
    log("vita.selftest.classify_bounds", detail);

    // NEON matrix transform: enable only when VFP already flushes denormals
    // (then both paths round identically) and random cases match bit for bit.
    std::uint32_t fpscr = 0;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    const bool flushToZero = (fpscr >> 24) & 1u;
    unsigned transformMismatches = 0;
    bMatrixNeonEnabled = 1;
    const auto transformStarted = sceKernelGetProcessTimeWide();
    for (int test = 0; test < 200000; ++test) {
        float matrix[16], input[4], neon[4], scalar[4];
        const float scale = test % 5 == 0 ? 1.0e-19f : test % 7 == 0 ? 1.0e6f : 8.0f;
        for (auto& value : matrix) value = Random(scale);
        for (auto& value : input) value = Random(test % 3 ? 100.0f : 1.0e-20f);
        if (test & 1) input[3] = 1.0f;
        vectorMatrixTransform(neon, matrix, input);
        vectorMatrixTransformScalar(scalar, matrix, input);
        if (std::memcmp(neon, scalar, sizeof(neon)) != 0) ++transformMismatches;
        // Aliased input/output must also match.
        float aliased[4]; std::memcpy(aliased, input, sizeof(aliased));
        vectorMatrixTransform(aliased, matrix, aliased);
        if (std::memcmp(aliased, scalar, sizeof(aliased)) != 0) ++transformMismatches;
    }
    const double transformMs = (sceKernelGetProcessTimeWide() - transformStarted) / 1000.0;
    bMatrixNeonEnabled = flushToZero && transformMismatches == 0;
    std::snprintf(detail, sizeof(detail), "fpscr=0x%08x flush_to_zero=%d cases=400000 mismatches=%u ms=%.1f neon_enabled=%d",
        static_cast<unsigned>(fpscr), flushToZero ? 1 : 0, transformMismatches, transformMs, bMatrixNeonEnabled);
    log("vita.selftest.matrix_transform", detail);
}
}
