#ifndef NFSHP2_BMATH_NEON_HPP
#define NFSHP2_BMATH_NEON_HPP
// Vita NEON helpers for bit-exact matrix work (used only while
// bMatrixNeonEnabled, i.e. after the boot self-test). Operands stay in NEON
// registers: vmulq_n_f32 with a scalar operand moves each one VFP->ARM->NEON
// and building a vector from scalar stores hits a store-forwarding stall,
// both pipeline stalls on the Cortex-A9. VMLA is not fused, so
// ((c0*x+c1*y)+c2*z)+c3*w rounds exactly as the scalar path.
#if defined(__ARM_NEON) && !(defined(_M_X64) || defined(__SSE2__))
#include <arm_neon.h>
extern int bMatrixNeonEnabled;
static inline float32x4_t bNeonTransform(float32x4_t c0, float32x4_t c1, float32x4_t c2, float32x4_t c3, float32x4_t v)
{
    const float32x2_t low = vget_low_f32(v), high = vget_high_f32(v);
    float32x4_t result = vmulq_lane_f32(c0, low, 0);
    result = vmlaq_lane_f32(result, c1, low, 1);
    result = vmlaq_lane_f32(result, c2, high, 0);
    return vmlaq_lane_f32(result, c3, high, 1);
}
static inline float32x4_t bNeonTransformMatrix(const float matrix[16], float32x4_t v)
{
    return bNeonTransform(vld1q_f32(matrix), vld1q_f32(matrix + 4), vld1q_f32(matrix + 8), vld1q_f32(matrix + 12), v);
}
// {unit[0]+p[0], unit[1]+p[1], unit[2]+p[2], unit[3]} (eMulVector3's point).
static inline float32x4_t bNeonPoint3(const float unit[4], const float p[3])
{
    const float32x4_t sum = vaddq_f32(vld1q_f32(unit), vcombine_f32(vld1_f32(p), vld1_dup_f32(p + 2)));
    return vld1q_lane_f32(unit + 3, sum, 3);
}
// Exact inline floorf/truncf (newlib's are software bit routines):
// |x|<2^23 converts through int32 (round toward zero, exact), larger
// magnitudes, NaN and inf are already integral; the sign of a zero result
// follows x as in IEEE floor/trunc. Enabled by the boot self-test.
extern int bFastRoundEnabled;
static inline float bSignedLike(float value, float sign)
{
    unsigned v, s;__builtin_memcpy(&v, &value, 4);__builtin_memcpy(&s, &sign, 4);
    v = (v & 0x7fffffffu) | (s & 0x80000000u);__builtin_memcpy(&value, &v, 4);return value;
}
static inline float bFastTrunc(float x)
{
    if (!(__builtin_fabsf(x) < 8388608.0f)) return x != x ? x + x : x; // NaN as newlib (x+x)
    return bSignedLike(static_cast<float>(static_cast<int>(x)), x);
}
static inline float bFastFloor(float x)
{
    if (!(__builtin_fabsf(x) < 8388608.0f)) return x != x ? x + x : x;
    float t = static_cast<float>(static_cast<int>(x));
    if (t > x) t -= 1.0f;
    return t == 0.0f ? bSignedLike(t, x) : t;
}
#define NFSHP2_BMATH_NEON 1
#else
#define NFSHP2_BMATH_NEON 0
#endif
#endif
