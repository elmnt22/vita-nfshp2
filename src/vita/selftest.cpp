// Startup self-tests for Vita-only fast paths (logged, never fatal).
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" std::int32_t ecstasyClassifyBounds(const float minimum[3], const float maximum[3], const float matrix[16]);
#include "bware/inc/bMath.hpp" // vectorMatrixTransform/Point (header-inline on Vita)
void vectorMatrixTransformScalar(float output[4], const float matrix[16], const float input[4]);
extern "C" void vectorMatrixMultiply(float output[16], const float left[16], const float right[16]);
extern "C" void eMulVector3(float output[4], const float matrix[16], const float input[3]);
extern "C" float eUnitVector4[4];
#include "bware/inc/bMathNeon.hpp"

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
    {
        // Pure call cost on warm data: 64 varied boxes, one matrix.
        static float boxes[64][6];static float matrix[16];
        for (auto& value : matrix) value = Random(4.0f);
        matrix[15] = 20.0f;
        for (auto& box : boxes) for (int axis = 0; axis < 3; ++axis) {
            box[axis] = Random(50.0f);box[3 + axis] = box[axis] + std::fabs(Random(10.0f));
        }
        unsigned at = 0;
        const double ns = NsPerCall([&] {
            const auto* box = boxes[(at++) & 63u];
            return static_cast<std::uint64_t>(ecstasyClassifyBounds(box, box + 3, matrix));
        });
        std::snprintf(detail, sizeof(detail), "classify_ns=%.0f", ns);
        log("vita.selftest.classify_cost", detail);
    }

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
        // eMulVector3: NEON builds {unit+p, unit[3]} in registers.
        {
            const float point[4] = {eUnitVector4[0] + input[0], eUnitVector4[1] + input[1],
                eUnitVector4[2] + input[2], eUnitVector4[3]};
            float viaNeon[4], reference[4];
            eMulVector3(viaNeon, matrix, input);
            vectorMatrixTransformScalar(reference, matrix, point);
            if (std::memcmp(viaNeon, reference, sizeof(viaNeon)) != 0) ++transformMismatches;
        }
        // vectorMatrixTransformPoint: {x,y,z,1}, also aliased.
        {
            const float point[4] = {input[0], input[1], input[2], 1.0f};
            float viaNeon[4], reference[4], aliasedPoint[4];
            vectorMatrixTransformPoint(viaNeon, matrix, input);
            vectorMatrixTransformScalar(reference, matrix, point);
            if (std::memcmp(viaNeon, reference, sizeof(viaNeon)) != 0) ++transformMismatches;
            std::memcpy(aliasedPoint, input, sizeof(aliasedPoint));
            vectorMatrixTransformPoint(aliasedPoint, matrix, aliasedPoint);
            if (std::memcmp(aliasedPoint, reference, sizeof(aliasedPoint)) != 0) ++transformMismatches;
        }
        // vectorMatrixMultiply: four column transforms, also aliased.
        if ((test & 7) == 0) {
            float right[16], product[16], reference[16];
            for (auto& value : right) value = Random(scale);
            vectorMatrixMultiply(product, matrix, right);
            for (int column = 0; column < 4; ++column) vectorMatrixTransformScalar(reference + column * 4, matrix, right + column * 4);
            if (std::memcmp(product, reference, sizeof(product)) != 0) ++transformMismatches;
            vectorMatrixMultiply(right, matrix, right);
            if (std::memcmp(right, reference, sizeof(right)) != 0) ++transformMismatches;
        }
    }
    const double transformMs = (sceKernelGetProcessTimeWide() - transformStarted) / 1000.0;
    bMatrixNeonEnabled = flushToZero && transformMismatches == 0;
    std::snprintf(detail, sizeof(detail), "fpscr=0x%08x flush_to_zero=%d cases=400000 mismatches=%u ms=%.1f neon_enabled=%d",
        static_cast<unsigned>(fpscr), flushToZero ? 1 : 0, transformMismatches, transformMs, bMatrixNeonEnabled);
    log("vita.selftest.matrix_transform", detail);
    {
        // Inline floor/trunc vs newlib. Denormal inputs are excluded: in
        // flush-to-zero mode every arithmetic result feeding them is already
        // flushed, so the callers never pass one.
        unsigned roundMismatches = 0, roundCases = 0;std::uint32_t firstBad[3]{};
        const auto check = [&](float x) {
            std::uint32_t xb;std::memcpy(&xb, &x, 4);
            if ((xb & 0x7f800000u) == 0 && (xb & 0x007fffffu) != 0) return; // denormal (bit test: FZ compares see 0)
            ++roundCases;
            const float a = bFastFloor(x), b = std::floor(x), c = bFastTrunc(x), d = std::trunc(x);
            if (std::memcmp(&a, &b, 4) != 0 || std::memcmp(&c, &d, 4) != 0) {
                if (roundMismatches < 3) std::memcpy(&firstBad[roundMismatches], &x, 4);
                ++roundMismatches;
            }
        };
        const float edges[] = {0.0f, -0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f, 8388607.5f, -8388607.5f,
            8388608.0f, -8388608.0f, 16777216.0f, -16777217.0f, 1e30f, -1e30f, 2147483648.0f, -2147483904.0f,
            0.99999994f, -0.99999994f, 1e-30f, -1e-30f, __builtin_inff(), -__builtin_inff(), __builtin_nanf("")};
        for (float x : edges) check(x);
        for (int test = 0; test < 400000; ++test) {
            const float scale = test % 4 == 0 ? 1.0f : test % 4 == 1 ? 1000.0f : test % 4 == 2 ? 1.0e7f : 1.0e-3f;
            check(Random(scale));
            std::uint32_t bits = static_cast<std::uint32_t>(test) * 2654435761u;float raw;std::memcpy(&raw, &bits, 4);
            check(raw);
        }
        bFastRoundEnabled = roundMismatches == 0;
        std::snprintf(detail, sizeof(detail), "cases=%u mismatches=%u enabled=%d first=%08x,%08x,%08x", roundCases, roundMismatches, bFastRoundEnabled,
            static_cast<unsigned>(firstBad[0]), static_cast<unsigned>(firstBad[1]), static_cast<unsigned>(firstBad[2]));
        log("vita.selftest.fast_round", detail);
    }
}
}
