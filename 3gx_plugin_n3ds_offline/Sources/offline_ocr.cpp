#include "offline_ocr.hpp"
#include "offline_fs.hpp"

#ifdef __3DS__
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace OfflineOcr {

namespace {

enum OpType : uint8_t {
    OP_CONV = 1,
    OP_GAP = 2,
    OP_MUL_SLOT = 3,
    OP_AVGPOOL = 4,
    OP_CONCAT_POOL = 5,
    OP_SVTR_BLOCK = 6,
    OP_LAYERNORM = 7,
    OP_CTC_HEAD = 8,
    OP_CONV_S16 = 9,
};

enum ActType : uint8_t {
    ACT_NONE = 0,
    ACT_RELU = 1,
    ACT_HARDSWISH = 2,
    ACT_SWISH = 3,
    ACT_HARDSIGMOID = 4,
};

static inline uint16_t ReadU16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t ReadU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline float ReadF32(const uint8_t *p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

static inline void ReadF32Vec(const uint8_t *buf, size_t &pos, size_t count, std::vector<float> &out) {
    out.resize(count);
    if (count > 0) {
        std::memcpy(out.data(), buf + pos, count * 4);
        pos += count * 4;
    }
}

static inline float FastExpf(float x) {
    if (x < -87.0f) return 0.0f;
    if (x > 87.0f) x = 87.0f;
    float z = x * 1.4426950408889634f;
    int32_t n = (int32_t)(z >= 0.0f ? (z + 0.5f) : (z - 0.5f));
    float f = x - (float)n * 0.6931471805599453f;
    float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f + f * 0.04166667f)));
    union { uint32_t u; float fl; } sc;
    sc.u = (uint32_t)(n + 127) << 23;
    return p * sc.fl;
}

static inline float ApplyAct(float x, uint8_t act) {
    switch (act) {
        case ACT_NONE: return x;
        case ACT_RELU: return x > 0.0f ? x : 0.0f;
        case ACT_HARDSWISH: {
            float t = x * (1.0f / 6.0f) + 0.5f;
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return x;
            return x * t;
        }
        case ACT_SWISH: {
            float cx = x;
            if (cx < -40.0f) cx = -40.0f;
            if (cx > 40.0f) cx = 40.0f;
            return x / (1.0f + FastExpf(-cx));
        }
        case ACT_HARDSIGMOID: {
            float t = x * (1.0f / 6.0f) + 0.5f;
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            return t;
        }
        case 5: { // Sigmoid
            float cx = x;
            if (cx < -40.0f) cx = -40.0f;
            if (cx > 40.0f) cx = 40.0f;
            return 1.0f / (1.0f + FastExpf(-cx));
        }
        case 6: { // HardSigmoid (1/6)
            float t = x * (1.0f / 6.0f) + 0.5f;
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            return t;
        }
        default: return x;
    }
}

static inline uint32_t Pack2S16(int32_t lo, int32_t hi) {
#ifdef __ARM_FEATURE_SIMD32
    uint32_t res;
    __asm__("pkhbt %0, %1, %2, lsl #16" : "=r"(res) : "r"(lo), "r"(hi));
    return res;
#else
    return ((uint32_t)(uint16_t)lo) | (((uint32_t)(uint16_t)hi) << 16);
#endif
}

static inline int32_t Smlad(uint32_t a, uint32_t b, int32_t acc) {
#ifdef __ARM_FEATURE_SIMD32
    __asm__("smlad %0, %1, %2, %0" : "+r"(acc) : "r"(a), "r"(b));
    return acc;
#else
    int16_t a0 = (int16_t)(a & 0xFFFFu);
    int16_t a1 = (int16_t)(a >> 16);
    int16_t b0 = (int16_t)(b & 0xFFFFu);
    int16_t b1 = (int16_t)(b >> 16);
    return acc + (int32_t)a0 * (int32_t)b0 + (int32_t)a1 * (int32_t)b1;
#endif
}

static inline int16_t RoundToS16(float q) {
#if defined(__3DS__) && defined(__VFP_FP__)
    float tmp;
    int32_t r;
    __asm__("vcvtr.s32.f32 %0, %1" : "=t"(tmp) : "t"(q));
    __asm__("vmov %0, %1" : "=r"(r) : "t"(tmp));
    return (int16_t)r;
#else
    return (int16_t)(int32_t)(q >= 0.0f ? (q + 0.5f) : (q - 0.5f));
#endif
}

static inline int16_t ClampRoundToS16(float q) {
    if (q > 3072.0f) q = 3072.0f;
    else if (q < -3072.0f) q = -3072.0f;
    return RoundToS16(q);
}

static inline int32_t Smmulr(int32_t a, int32_t b) {
#ifdef __ARM_FEATURE_SIMD32
    int32_t r;
    __asm__("smmulr %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
#else
    return (int32_t)(((int64_t)a * (int64_t)b + 0x80000000LL) >> 32);
#endif
}

static inline int16_t Ssat13(int32_t x) {
#ifdef __ARM_FEATURE_SIMD32
    int32_t r;
    __asm__("ssat %0, #13, %1" : "=r"(r) : "r"(x));
    return (int16_t)r;
#else
    if (x > 4095) return 4095;
    if (x < -4096) return -4096;
    return (int16_t)x;
#endif
}

struct FixHSwishLayer {
    int32_t m2, sh2, rnd2, b2;
    bool fastPost;
};

struct FixHSwishChan {
    int32_t m1, sh1, rnd1, bvQ13;
};

struct FixLinearParams {
    int32_t m, sh, rnd, b;
};

static inline void MakeScaleShift(double val, int32_t &outM, int32_t &outSh, int32_t &outRnd) {
    if (val == 0.0) { outM = 0; outSh = 0; outRnd = 0; return; }
    int exp = 0;
    double mant = std::frexp(val, &exp);
    long long ml = std::llround(mant * 2147483648.0);
    if (ml > 2147483647LL) ml = 2147483647LL;
    else if (ml < -2147483647LL) ml = -2147483647LL;
    outM = (int32_t)ml;
    outSh = -(exp + 1);
    outRnd = (outSh > 0 && outSh < 31) ? (1 << (outSh - 1)) : 0;
}

static inline FixHSwishLayer MakeFixHSwishLayer(float postScale, float postBias, float invOutScale) {
    FixHSwishLayer l;
    MakeScaleShift((double)postScale * (double)invOutScale / 8192.0, l.m2, l.sh2, l.rnd2);
    double b2f = (double)postBias * (double)invOutScale;
    l.fastPost = false;
    if (l.sh2 > 0 && l.sh2 < 20) {
        double scaled = (b2f + 0.5) * (double)(1 << l.sh2);
        if (scaled > -1e9 && scaled < 1e9) {
            l.rnd2 = (int32_t)std::llround(scaled);
            l.b2 = 0;
            l.fastPost = true;
        } else {
            l.b2 = (int32_t)std::llround(b2f);
        }
    } else {
        l.b2 = (int32_t)std::llround(b2f);
    }
    return l;
}

static inline FixHSwishChan MakeFixHSwishChan(float sc, float bv) {
    FixHSwishChan c;
    MakeScaleShift((double)sc * 8192.0, c.m1, c.sh1, c.rnd1);
    c.bvQ13 = (int32_t)std::llround((double)bv * 8192.0);
    return c;
}

template <bool NegSh1>
static inline int16_t ApplyFixHSwishFast(int32_t acc, int32_t m1, int32_t sh1Pos, int32_t rnd1, int32_t bvQ13,
                                         int32_t m2, int32_t sh2, int32_t rnd2)
{
    int32_t t1 = Smmulr(acc, m1);
    int32_t x = (NegSh1 ? (t1 << sh1Pos) : ((t1 + rnd1) >> sh1Pos)) + bvQ13;
    int32_t h;
    if (x <= -24576) h = 0;
    else if (x >= 24576) h = x;
    else h = Smmulr(x * (x + 24576), 87381);
    int32_t t2 = Smmulr(h, m2);
    return Ssat13((t2 + rnd2) >> sh2);
}

static inline int16_t ApplyFixHSwishGeneric(int32_t acc, const FixHSwishChan &c, const FixHSwishLayer &l) {
    int32_t t1 = Smmulr(acc, c.m1);
    int32_t x = (c.sh1 >= 0 ? ((t1 + c.rnd1) >> c.sh1) : (t1 << -c.sh1)) + c.bvQ13;
    int32_t h;
    if (x <= -24576) h = 0;
    else if (x >= 24576) h = x;
    else h = Smmulr(x * (x + 24576), 87381);
    int32_t t2 = Smmulr(h, l.m2);
    int32_t y = (l.sh2 >= 0 ? ((t2 + l.rnd2) >> l.sh2) : (t2 << -l.sh2)) + l.b2;
    return Ssat13(y);
}

static inline int16_t ApplyFixHSwishFastCh(int32_t acc, const FixHSwishChan &c, int32_t m2, int32_t sh2, int32_t rnd2) {
    int32_t t1 = Smmulr(acc, c.m1);
    int32_t x = (c.sh1 >= 0 ? ((t1 + c.rnd1) >> c.sh1) : (t1 << -c.sh1)) + c.bvQ13;
    int32_t h;
    if (x <= -24576) h = 0;
    else if (x >= 24576) h = x;
    else h = Smmulr(x * (x + 24576), 87381);
    int32_t t2 = Smmulr(h, m2);
    return Ssat13((t2 + rnd2) >> sh2);
}

__attribute__((noinline))
static void ApplyFixHSwishRowStrided(const int32_t *rowAcc, int16_t *dstHwc, int wout, int coutPad,
                                     const FixHSwishChan &ch, const FixHSwishLayer &lay)
{
    if (!lay.fastPost) {
        for (int ox = 0; ox < wout; ++ox) {
            dstHwc[(size_t)ox * coutPad] = ApplyFixHSwishGeneric(rowAcc[ox], ch, lay);
        }
        return;
    }
    const int32_t m1 = ch.m1, rnd1 = ch.rnd1, bvQ13 = ch.bvQ13;
    const int32_t m2 = lay.m2, sh2 = lay.sh2, rnd2 = lay.rnd2;
    if (ch.sh1 < 0) {
        const int32_t sh1Neg = -ch.sh1;
        for (int ox = 0; ox < wout; ++ox) {
            dstHwc[(size_t)ox * coutPad] = ApplyFixHSwishFast<true>(rowAcc[ox], m1, sh1Neg, rnd1, bvQ13, m2, sh2, rnd2);
        }
    } else {
        const int32_t sh1 = ch.sh1;
        for (int ox = 0; ox < wout; ++ox) {
            dstHwc[(size_t)ox * coutPad] = ApplyFixHSwishFast<false>(rowAcc[ox], m1, sh1, rnd1, bvQ13, m2, sh2, rnd2);
        }
    }
}

static inline FixLinearParams MakeFixLinear(float sc, float bv, float postScale, float postBias, float invOutScale) {
    FixLinearParams p;
    MakeScaleShift((double)sc * (double)postScale * (double)invOutScale, p.m, p.sh, p.rnd);
    double bf = ((double)bv * (double)postScale + (double)postBias) * (double)invOutScale;
    if (p.sh > 0 && p.sh < 20) {
        double scaled = (bf + 0.5) * (double)(1 << p.sh);
        if (scaled > -1e9 && scaled < 1e9) {
            p.rnd = (int32_t)std::llround(scaled);
            p.b = 0;
            return p;
        }
    }
    p.b = (int32_t)std::llround(bf);
    return p;
}

static inline int16_t ApplyFixLinearToS16(int32_t acc, const FixLinearParams &p) {
    int32_t t = Smmulr(acc, p.m);
    int32_t y = (p.sh >= 0 ? ((t + p.rnd) >> p.sh) : (t << -p.sh)) + p.b;
    return Ssat13(y);
}

enum TensorLayout {
    LAY_F32_CHW = 0,
    LAY_S16_CHW = 1,
    LAY_S16_HWC = 2
};

static inline int32_t DotS16(const int16_t *a, const int16_t *b, int nPairs) {
    int32_t acc = 0;
#ifdef __ARM_FEATURE_SIMD32
    const uint32_t *pa = (const uint32_t *)a;
    const uint32_t *pb = (const uint32_t *)b;
    int i = 0;
    for (; i + 3 < nPairs; i += 4) {
        uint32_t va0 = pa[i + 0], vb0 = pb[i + 0];
        uint32_t va1 = pa[i + 1], vb1 = pb[i + 1];
        uint32_t va2 = pa[i + 2], vb2 = pb[i + 2];
        uint32_t va3 = pa[i + 3], vb3 = pb[i + 3];
        __asm__ volatile("smlad %0, %1, %2, %0" : "+r"(acc) : "r"(va0), "r"(vb0));
        __asm__ volatile("smlad %0, %1, %2, %0" : "+r"(acc) : "r"(va1), "r"(vb1));
        __asm__ volatile("smlad %0, %1, %2, %0" : "+r"(acc) : "r"(va2), "r"(vb2));
        __asm__ volatile("smlad %0, %1, %2, %0" : "+r"(acc) : "r"(va3), "r"(vb3));
    }
    for (; i < nPairs; ++i) {
        uint32_t va = pa[i], vb = pb[i];
        __asm__ volatile("smlad %0, %1, %2, %0" : "+r"(acc) : "r"(va), "r"(vb));
    }
#else
    const int n = nPairs * 2;
    for (int i = 0; i < n; ++i) {
        acc += (int32_t)a[i] * (int32_t)b[i];
    }
#endif
    return acc;
}

static inline void DotS16_2x2(
    const int16_t *a0, const int16_t *a1,
    const int16_t *b0, const int16_t *b1, int nPairs,
    int32_t &c00, int32_t &c01, int32_t &c10, int32_t &c11)
{
    c00 = 0; c01 = 0; c10 = 0; c11 = 0;
#ifdef __ARM_FEATURE_SIMD32
    const uint32_t *pa0 = (const uint32_t *)a0;
    const uint32_t *pa1 = (const uint32_t *)a1;
    const uint32_t *pb0 = (const uint32_t *)b0;
    const uint32_t *pb1 = (const uint32_t *)b1;
    int pairs8 = nPairs >> 3;
    uint32_t va0, va1, vb0, vb1;
    if (pairs8 > 0) {
        __asm__ volatile(
            "1:\n\t"
            "pld [%[pb0], #64]\n\t"
            "pld [%[pb1], #64]\n\t"
            "ldr %[va0], [%[pa0]], #4\n\t"
            "ldr %[vb0], [%[pb0]], #4\n\t"
            "ldr %[vb1], [%[pb1]], #4\n\t"
            "ldr %[va1], [%[pa1]], #4\n\t"
            "smlad %[c00], %[va0], %[vb0], %[c00]\n\t"
            "smlad %[c10], %[va0], %[vb1], %[c10]\n\t"
            "smlad %[c01], %[va1], %[vb0], %[c01]\n\t"
            "smlad %[c11], %[va1], %[vb1], %[c11]\n\t"
            "ldr %[va0], [%[pa0]], #4\n\t"
            "ldr %[vb0], [%[pb0]], #4\n\t"
            "ldr %[vb1], [%[pb1]], #4\n\t"
            "ldr %[va1], [%[pa1]], #4\n\t"
            "smlad %[c00], %[va0], %[vb0], %[c00]\n\t"
            "smlad %[c10], %[va0], %[vb1], %[c10]\n\t"
            "smlad %[c01], %[va1], %[vb0], %[c01]\n\t"
            "smlad %[c11], %[va1], %[vb1], %[c11]\n\t"
            "ldr %[va0], [%[pa0]], #4\n\t"
            "ldr %[vb0], [%[pb0]], #4\n\t"
            "ldr %[vb1], [%[pb1]], #4\n\t"
            "ldr %[va1], [%[pa1]], #4\n\t"
            "smlad %[c00], %[va0], %[vb0], %[c00]\n\t"
            "smlad %[c10], %[va0], %[vb1], %[c10]\n\t"
            "smlad %[c01], %[va1], %[vb0], %[c01]\n\t"
            "smlad %[c11], %[va1], %[vb1], %[c11]\n\t"
            "ldr %[va0], [%[pa0]], #4\n\t"
            "ldr %[vb0], [%[pb0]], #4\n\t"
            "ldr %[vb1], [%[pb1]], #4\n\t"
            "ldr %[va1], [%[pa1]], #4\n\t"
            "smlad %[c00], %[va0], %[vb0], %[c00]\n\t"
            "smlad %[c10], %[va0], %[vb1], %[c10]\n\t"
            "smlad %[c01], %[va1], %[vb0], %[c01]\n\t"
            "smlad %[c11], %[va1], %[vb1], %[c11]\n\t"
            "ldr %[va0], [%[pa0]], #4\n\t"
            "ldr %[vb0], [%[pb0]], #4\n\t"
            "ldr %[vb1], [%[pb1]], #4\n\t"
            "ldr %[va1], [%[pa1]], #4\n\t"
            "smlad %[c00], %[va0], %[vb0], %[c00]\n\t"
            "smlad %[c10], %[va0], %[vb1], %[c10]\n\t"
            "smlad %[c01], %[va1], %[vb0], %[c01]\n\t"
            "smlad %[c11], %[va1], %[vb1], %[c11]\n\t"
            "ldr %[va0], [%[pa0]], #4\n\t"
            "ldr %[vb0], [%[pb0]], #4\n\t"
            "ldr %[vb1], [%[pb1]], #4\n\t"
            "ldr %[va1], [%[pa1]], #4\n\t"
            "smlad %[c00], %[va0], %[vb0], %[c00]\n\t"
            "smlad %[c10], %[va0], %[vb1], %[c10]\n\t"
            "smlad %[c01], %[va1], %[vb0], %[c01]\n\t"
            "smlad %[c11], %[va1], %[vb1], %[c11]\n\t"
            "ldr %[va0], [%[pa0]], #4\n\t"
            "ldr %[vb0], [%[pb0]], #4\n\t"
            "ldr %[vb1], [%[pb1]], #4\n\t"
            "ldr %[va1], [%[pa1]], #4\n\t"
            "smlad %[c00], %[va0], %[vb0], %[c00]\n\t"
            "smlad %[c10], %[va0], %[vb1], %[c10]\n\t"
            "smlad %[c01], %[va1], %[vb0], %[c01]\n\t"
            "smlad %[c11], %[va1], %[vb1], %[c11]\n\t"
            "ldr %[va0], [%[pa0]], #4\n\t"
            "ldr %[vb0], [%[pb0]], #4\n\t"
            "ldr %[vb1], [%[pb1]], #4\n\t"
            "ldr %[va1], [%[pa1]], #4\n\t"
            "smlad %[c00], %[va0], %[vb0], %[c00]\n\t"
            "smlad %[c10], %[va0], %[vb1], %[c10]\n\t"
            "smlad %[c01], %[va1], %[vb0], %[c01]\n\t"
            "smlad %[c11], %[va1], %[vb1], %[c11]\n\t"
            "subs %[cnt], %[cnt], #1\n\t"
            "bne 1b\n\t"
            : [c00] "+r"(c00), [c01] "+r"(c01), [c10] "+r"(c10), [c11] "+r"(c11),
              [pa0] "+r"(pa0), [pa1] "+r"(pa1), [pb0] "+r"(pb0), [pb1] "+r"(pb1),
              [cnt] "+r"(pairs8),
              [va0] "=&r"(va0), [va1] "=&r"(va1), [vb0] "=&r"(vb0), [vb1] "=&r"(vb1)
            :
            : "cc"
        );
    }
    int rem = nPairs & 7;
    if (rem > 0) {
        __asm__ volatile(
            "2:\n\t"
            "ldr %[va0], [%[pa0]], #4\n\t"
            "ldr %[vb0], [%[pb0]], #4\n\t"
            "ldr %[vb1], [%[pb1]], #4\n\t"
            "ldr %[va1], [%[pa1]], #4\n\t"
            "smlad %[c00], %[va0], %[vb0], %[c00]\n\t"
            "smlad %[c10], %[va0], %[vb1], %[c10]\n\t"
            "smlad %[c01], %[va1], %[vb0], %[c01]\n\t"
            "smlad %[c11], %[va1], %[vb1], %[c11]\n\t"
            "subs %[cnt], %[cnt], #1\n\t"
            "bne 2b\n\t"
            : [c00] "+r"(c00), [c01] "+r"(c01), [c10] "+r"(c10), [c11] "+r"(c11),
              [pa0] "+r"(pa0), [pa1] "+r"(pa1), [pb0] "+r"(pb0), [pb1] "+r"(pb1),
              [cnt] "+r"(rem),
              [va0] "=&r"(va0), [va1] "=&r"(va1), [vb0] "=&r"(vb0), [vb1] "=&r"(vb1)
            :
            : "cc"
        );
    }
#else
    const int n = nPairs * 2;
    for (int i = 0; i < n; ++i) {
        int32_t x0 = a0[i], x1 = a1[i];
        int32_t w0 = b0[i], w1 = b1[i];
        c00 += x0 * w0; c01 += x1 * w0;
        c10 += x0 * w1; c11 += x1 * w1;
    }
#endif
}

static inline uint64_t NowTicks() {
#ifdef __3DS__
    return svcGetSystemTick();
#else
    return 0;
#endif
}

#ifdef __3DS__
struct PersistentHelper {
    LightEvent wakeEvent;
    LightEvent doneEvent;
    std::atomic<bool> inited{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> inUse{false};
    std::atomic<uint64_t> busyTicks{0};
    std::atomic<uint32_t> dispatches{0};
    void (*fn)(void *){nullptr};
    void *arg{nullptr};
    CTRPluginFramework::ThreadEx *thread{nullptr};

    static void LoopEntry(void *selfPtr) {
        PersistentHelper *self = (PersistentHelper *)selfPtr;
        for (;;) {
            LightEvent_Wait(&self->wakeEvent);
            uint64_t t0 = NowTicks();
            if (self->fn) self->fn(self->arg);
            self->busyTicks.fetch_add(NowTicks() - t0, std::memory_order_relaxed);
            LightEvent_Signal(&self->doneEvent);
        }
    }

    bool Dispatch(int core, int prio, void (*f)(void *), void *a) {
        if (failed.load(std::memory_order_acquire)) return false;
        bool expected = false;
        if (!inUse.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        if (!inited.load(std::memory_order_acquire)) {
            LightEvent_Init(&wakeEvent, RESET_ONESHOT);
            LightEvent_Init(&doneEvent, RESET_ONESHOT);
            thread = new CTRPluginFramework::ThreadEx(LoopEntry, 16384, prio, core);
            if (R_FAILED(thread->Start(this))) {
                delete thread;
                thread = nullptr;
                failed.store(true, std::memory_order_release);
                inUse.store(false, std::memory_order_release);
                return false;
            }
            inited.store(true, std::memory_order_release);
        }
        fn = f;
        arg = a;
        dispatches.fetch_add(1, std::memory_order_relaxed);
        LightEvent_Signal(&wakeEvent);
        return true;
    }

    void Wait() {
        LightEvent_Wait(&doneEvent);
        fn = nullptr;
        arg = nullptr;
        inUse.store(false, std::memory_order_release);
    }
};

static PersistentHelper &GetPersistentHelper(int core) {
    static PersistentHelper s_helpers[4];
    int idx = (core >= 0 && core < 4) ? core : 2;
    return s_helpers[idx];
}

static void ProbePingEntry(void *arg) {
    std::atomic<bool> *pong = (std::atomic<bool> *)arg;
    pong->store(true, std::memory_order_release);
}

static bool ProbeCoreSchedulable(int core) {
    if (core == 0 || core == 2) return true;
    if (core < 0 || core >= 4) return false;
    static std::atomic<int> s_probeState[4];
    int st = s_probeState[core].load(std::memory_order_acquire);
    if (st != 0) return st > 0;

    std::atomic<bool> *pong = new std::atomic<bool>(false);
    CTRPluginFramework::ThreadEx *probe = new CTRPluginFramework::ThreadEx(ProbePingEntry, 4096, 0x32, core);
    if (R_FAILED(probe->Start(pong))) {
        delete probe;
        delete pong;
        s_probeState[core].store(-1, std::memory_order_release);
        return false;
    }
    for (int i = 0; i < 10; ++i) {
        if (pong->load(std::memory_order_acquire)) {
            probe->Join(true);
            delete probe;
            delete pong;
            s_probeState[core].store(1, std::memory_order_release);
            return true;
        }
        svcSleepThread(5000000ULL);
    }
    // Timed out: leave probe/pong allocated and never Join() so we never deadlock.
    s_probeState[core].store(-1, std::memory_order_release);
    return false;
}

static void DispatchHelpers(int effCore, void (*fn)(void *), void *arg,
                            int outCores[3], int &outCount)
{
    outCount = 0;
    const int candidates[3] = {
        (effCore == 0) ? 0 : 2,
        3,
        1
    };
    const int prio = (effCore == 0) ? 0x32 : 0x30;
    for (int i = 0; i < 3; ++i) {
        int c = candidates[i];
        if (!ProbeCoreSchedulable(c)) continue;
        if (GetPersistentHelper(c).Dispatch(c, prio, fn, arg)) {
            outCores[outCount++] = c;
        }
    }
}

static void WaitHelpers(const int cores[3], int count) {
    for (int i = 0; i < count; ++i) {
        GetPersistentHelper(cores[i]).Wait();
    }
}
#endif

struct Conv1x1RangeArgs {
    const int16_t *inHwc;
    float *out;
    int16_t *outS16;
    const float *scales;
    const float *bias;
    const int8_t *wq;
    const int16_t *wq16;
    int cin, cinPad, nPairs, outHW;
    int coStart, coEnd;
    float xScale, invOutScale, postScale, postBias;
    uint8_t act;
    bool hasPost;
    std::atomic<int> *nextCo;
    int chunkSize;
    int totalCout;
};

static inline void UnpackS8ToS16(const int8_t *src, int16_t *dst, int n) {
    int c = 0;
#ifdef __ARM_FEATURE_SIMD32
    if ((((uintptr_t)src) & 3) == 0 && n >= 4) {
        const uint32_t *s32 = (const uint32_t *)src;
        uint32_t *d32 = (uint32_t *)dst;
        int n4 = n >> 2;
        for (int i = 0; i < n4; ++i) {
            uint32_t v = s32[i];
            uint32_t lo, hi, p0, p1;
            __asm__(
                "sxtb16 %[lo], %[v]\n\t"
                "sxtb16 %[hi], %[v], ror #8\n\t"
                "pkhbt %[p0], %[lo], %[hi], lsl #16\n\t"
                "pkhtb %[p1], %[hi], %[lo], asr #16"
                : [lo] "=&r"(lo), [hi] "=&r"(hi), [p0] "=&r"(p0), [p1] "=r"(p1)
                : [v] "r"(v)
            );
            d32[i * 2 + 0] = p0;
            d32[i * 2 + 1] = p1;
        }
        c = n4 << 2;
    }
#endif
    for (; c < n; ++c) dst[c] = (int16_t)src[c];
}

static inline float ApplyHardSwishOnly(float x) {
    float t = x * (1.0f / 6.0f) + 0.5f;
    return (t <= 0.0f) ? 0.0f : (t >= 1.0f ? x : x * t);
}

static inline float ApplyHardSwishPost(float x, float postScale, float postBias, bool hasPost) {
    float v = ApplyHardSwishOnly(x);
    return hasPost ? (v * postScale + postBias) : v;
}

template <bool Neg0, bool Neg1>
__attribute__((noinline))
static void RunConv1x1HSwishPair(const int16_t *inHwc, const int16_t *w0, const int16_t *w1,
                                 int16_t *out0, int16_t *out1, int p0, int pEnd, int cinPad, int nPairs,
                                 const FixHSwishChan &fp0, const FixHSwishChan &fp1,
                                 int32_t m2, int32_t sh2, int32_t rnd2)
{
    const int32_t m1_0 = fp0.m1, sh1_0 = Neg0 ? -fp0.sh1 : fp0.sh1, r1_0 = fp0.rnd1, b_0 = fp0.bvQ13;
    const int32_t m1_1 = fp1.m1, sh1_1 = Neg1 ? -fp1.sh1 : fp1.sh1, r1_1 = fp1.rnd1, b_1 = fp1.bvQ13;
    int p = p0;
    for (; p + 1 < pEnd; p += 2) {
        int32_t c00, c01, c10, c11;
        DotS16_2x2(inHwc + (size_t)p * cinPad, inHwc + (size_t)(p + 1) * cinPad,
                   w0, w1, nPairs, c00, c01, c10, c11);
        out0[p]     = ApplyFixHSwishFast<Neg0>(c00, m1_0, sh1_0, r1_0, b_0, m2, sh2, rnd2);
        out0[p + 1] = ApplyFixHSwishFast<Neg0>(c01, m1_0, sh1_0, r1_0, b_0, m2, sh2, rnd2);
        out1[p]     = ApplyFixHSwishFast<Neg1>(c10, m1_1, sh1_1, r1_1, b_1, m2, sh2, rnd2);
        out1[p + 1] = ApplyFixHSwishFast<Neg1>(c11, m1_1, sh1_1, r1_1, b_1, m2, sh2, rnd2);
    }
    for (; p < pEnd; ++p) {
        int32_t a0 = DotS16(inHwc + (size_t)p * cinPad, w0, nPairs);
        int32_t a1 = DotS16(inHwc + (size_t)p * cinPad, w1, nPairs);
        out0[p] = ApplyFixHSwishFast<Neg0>(a0, m1_0, sh1_0, r1_0, b_0, m2, sh2, rnd2);
        out1[p] = ApplyFixHSwishFast<Neg1>(a1, m1_1, sh1_1, r1_1, b_1, m2, sh2, rnd2);
    }
}

static void RunConv1x1Range(const Conv1x1RangeArgs *args, uint64_t *unpackTicksOut = nullptr) {
    const int cin = args->cin;
    const int cinPad = args->cinPad;
    const int nPairs = args->nPairs;
    const int outHW = args->outHW;
    const int16_t *inHwc = args->inHwc;
    const float xScale = args->xScale;
    const uint8_t act = args->act;
    const bool hasPost = args->hasPost;
    const float postScale = args->postScale;
    const float postBias = args->postBias;

    std::vector<int16_t> wBlock;
    auto runSlice = [&](int sCo, int eCo) {
        const int nCh = eCo - sCo;
        if (nCh <= 0) return;
        const size_t needW = (size_t)nCh * (size_t)cinPad;
        if (wBlock.size() < needW) wBlock.resize(needW);
        uint64_t tUnpack0 = unpackTicksOut ? NowTicks() : 0;
        for (int i = 0; i < nCh; ++i) {
            int co = sCo + i;
            int16_t *dstW = wBlock.data() + (size_t)i * cinPad;
            if (args->wq16) {
                std::memcpy(dstW, args->wq16 + (size_t)co * cin, (size_t)cin * sizeof(int16_t));
            } else {
                UnpackS8ToS16(args->wq + (size_t)co * cin, dstW, cin);
            }
            for (int c = cin; c < cinPad; ++c) dstW[c] = 0;
        }
        if (unpackTicksOut) *unpackTicksOut += NowTicks() - tUnpack0;

        const FixHSwishLayer layHsw = (args->outS16 && act == ACT_HARDSWISH)
            ? MakeFixHSwishLayer(postScale, postBias, args->invOutScale)
            : FixHSwishLayer{0, 0, 0, 0, false};

        const int kTileP = (cinPad >= 384) ? 6 : (cinPad >= 192) ? 16 : (cinPad >= 96) ? 32 : 64;
        for (int p0 = 0; p0 < outHW; p0 += kTileP) {
            const int pEnd = std::min(outHW, p0 + kTileP);
            int i = 0;
            for (; i + 1 < nCh; i += 2) {
                const int co = sCo + i;
                const int16_t *w0 = wBlock.data() + (size_t)i * cinPad;
                const int16_t *w1 = wBlock.data() + (size_t)(i + 1) * cinPad;
                const float sc0 = args->scales[co] * xScale;
                const float bv0 = args->bias[co];
                const float sc1 = args->scales[co + 1] * xScale;
                const float bv1 = args->bias[co + 1];

                if (args->outS16) {
                    const float invOut = args->invOutScale;
                    int16_t *out0 = args->outS16 + (size_t)co * outHW;
                    int16_t *out1 = args->outS16 + (size_t)(co + 1) * outHW;
                    int p = p0;
                    if (act == ACT_HARDSWISH && layHsw.fastPost) {
                        const FixHSwishChan fp0 = MakeFixHSwishChan(sc0, bv0);
                        const FixHSwishChan fp1 = MakeFixHSwishChan(sc1, bv1);
                        const int32_t m2 = layHsw.m2, sh2 = layHsw.sh2, rnd2 = layHsw.rnd2;
                        if (fp0.sh1 < 0) {
                            if (fp1.sh1 < 0) RunConv1x1HSwishPair<true, true>(inHwc, w0, w1, out0, out1, p0, pEnd, cinPad, nPairs, fp0, fp1, m2, sh2, rnd2);
                            else RunConv1x1HSwishPair<true, false>(inHwc, w0, w1, out0, out1, p0, pEnd, cinPad, nPairs, fp0, fp1, m2, sh2, rnd2);
                        } else {
                            if (fp1.sh1 < 0) RunConv1x1HSwishPair<false, true>(inHwc, w0, w1, out0, out1, p0, pEnd, cinPad, nPairs, fp0, fp1, m2, sh2, rnd2);
                            else RunConv1x1HSwishPair<false, false>(inHwc, w0, w1, out0, out1, p0, pEnd, cinPad, nPairs, fp0, fp1, m2, sh2, rnd2);
                        }
                    } else if (act == ACT_HARDSWISH) {
                        const FixHSwishChan fp0 = MakeFixHSwishChan(sc0, bv0);
                        const FixHSwishChan fp1 = MakeFixHSwishChan(sc1, bv1);
                        for (; p + 1 < pEnd; p += 2) {
                            int32_t c00, c01, c10, c11;
                            DotS16_2x2(inHwc + (size_t)p * cinPad, inHwc + (size_t)(p + 1) * cinPad,
                                       w0, w1, nPairs, c00, c01, c10, c11);
                            out0[p]     = ApplyFixHSwishGeneric(c00, fp0, layHsw);
                            out0[p + 1] = ApplyFixHSwishGeneric(c01, fp0, layHsw);
                            out1[p]     = ApplyFixHSwishGeneric(c10, fp1, layHsw);
                            out1[p + 1] = ApplyFixHSwishGeneric(c11, fp1, layHsw);
                        }
                        for (; p < pEnd; ++p) {
                            int32_t a0 = DotS16(inHwc + (size_t)p * cinPad, w0, nPairs);
                            int32_t a1 = DotS16(inHwc + (size_t)p * cinPad, w1, nPairs);
                            out0[p] = ApplyFixHSwishGeneric(a0, fp0, layHsw);
                            out1[p] = ApplyFixHSwishGeneric(a1, fp1, layHsw);
                        }
                    } else {
                        for (; p + 1 < pEnd; p += 2) {
                            int32_t c00, c01, c10, c11;
                            DotS16_2x2(inHwc + (size_t)p * cinPad, inHwc + (size_t)(p + 1) * cinPad,
                                       w0, w1, nPairs, c00, c01, c10, c11);
                            float v00 = ApplyAct((float)c00 * sc0 + bv0, act);
                            float v01 = ApplyAct((float)c01 * sc0 + bv0, act);
                            float v10 = ApplyAct((float)c10 * sc1 + bv1, act);
                            float v11 = ApplyAct((float)c11 * sc1 + bv1, act);
                            if (hasPost) {
                                v00 = v00 * postScale + postBias;
                                v01 = v01 * postScale + postBias;
                                v10 = v10 * postScale + postBias;
                                v11 = v11 * postScale + postBias;
                            }
                            out0[p] = ClampRoundToS16(v00 * invOut); out0[p + 1] = ClampRoundToS16(v01 * invOut);
                            out1[p] = ClampRoundToS16(v10 * invOut); out1[p + 1] = ClampRoundToS16(v11 * invOut);
                        }
                        for (; p < pEnd; ++p) {
                            int32_t a0 = DotS16(inHwc + (size_t)p * cinPad, w0, nPairs);
                            int32_t a1 = DotS16(inHwc + (size_t)p * cinPad, w1, nPairs);
                            float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                            float v1 = ApplyAct((float)a1 * sc1 + bv1, act);
                            if (hasPost) { v0 = v0 * postScale + postBias; v1 = v1 * postScale + postBias; }
                            out0[p] = ClampRoundToS16(v0 * invOut); out1[p] = ClampRoundToS16(v1 * invOut);
                        }
                    }
                    continue;
                }

                float *out0 = args->out + (size_t)co * outHW;
                float *out1 = args->out + (size_t)(co + 1) * outHW;

                int p = p0;
                if (act == ACT_HARDSWISH && !hasPost) {
                    for (; p + 1 < pEnd; p += 2) {
                        int32_t c00, c01, c10, c11;
                        DotS16_2x2(inHwc + (size_t)p * cinPad, inHwc + (size_t)(p + 1) * cinPad,
                                   w0, w1, nPairs, c00, c01, c10, c11);
                        out0[p]     = ApplyHardSwishOnly((float)c00 * sc0 + bv0);
                        out0[p + 1] = ApplyHardSwishOnly((float)c01 * sc0 + bv0);
                        out1[p]     = ApplyHardSwishOnly((float)c10 * sc1 + bv1);
                        out1[p + 1] = ApplyHardSwishOnly((float)c11 * sc1 + bv1);
                    }
                    for (; p < pEnd; ++p) {
                        int32_t a0 = DotS16(inHwc + (size_t)p * cinPad, w0, nPairs);
                        int32_t a1 = DotS16(inHwc + (size_t)p * cinPad, w1, nPairs);
                        out0[p] = ApplyHardSwishOnly((float)a0 * sc0 + bv0);
                        out1[p] = ApplyHardSwishOnly((float)a1 * sc1 + bv1);
                    }
                } else if (act == ACT_HARDSWISH) {
                    for (; p + 1 < pEnd; p += 2) {
                        int32_t c00, c01, c10, c11;
                        DotS16_2x2(inHwc + (size_t)p * cinPad, inHwc + (size_t)(p + 1) * cinPad,
                                   w0, w1, nPairs, c00, c01, c10, c11);
                        out0[p]     = ApplyHardSwishPost((float)c00 * sc0 + bv0, postScale, postBias, hasPost);
                        out0[p + 1] = ApplyHardSwishPost((float)c01 * sc0 + bv0, postScale, postBias, hasPost);
                        out1[p]     = ApplyHardSwishPost((float)c10 * sc1 + bv1, postScale, postBias, hasPost);
                        out1[p + 1] = ApplyHardSwishPost((float)c11 * sc1 + bv1, postScale, postBias, hasPost);
                    }
                    for (; p < pEnd; ++p) {
                        int32_t a0 = DotS16(inHwc + (size_t)p * cinPad, w0, nPairs);
                        int32_t a1 = DotS16(inHwc + (size_t)p * cinPad, w1, nPairs);
                        out0[p] = ApplyHardSwishPost((float)a0 * sc0 + bv0, postScale, postBias, hasPost);
                        out1[p] = ApplyHardSwishPost((float)a1 * sc1 + bv1, postScale, postBias, hasPost);
                    }
                } else {
                    for (; p + 1 < pEnd; p += 2) {
                        int32_t c00, c01, c10, c11;
                        DotS16_2x2(inHwc + (size_t)p * cinPad, inHwc + (size_t)(p + 1) * cinPad,
                                   w0, w1, nPairs, c00, c01, c10, c11);
                        float v00 = ApplyAct((float)c00 * sc0 + bv0, act);
                        float v01 = ApplyAct((float)c01 * sc0 + bv0, act);
                        float v10 = ApplyAct((float)c10 * sc1 + bv1, act);
                        float v11 = ApplyAct((float)c11 * sc1 + bv1, act);
                        if (hasPost) {
                            v00 = v00 * postScale + postBias;
                            v01 = v01 * postScale + postBias;
                            v10 = v10 * postScale + postBias;
                            v11 = v11 * postScale + postBias;
                        }
                        out0[p] = v00; out0[p + 1] = v01;
                        out1[p] = v10; out1[p + 1] = v11;
                    }
                    for (; p < pEnd; ++p) {
                        int32_t a0 = DotS16(inHwc + (size_t)p * cinPad, w0, nPairs);
                        int32_t a1 = DotS16(inHwc + (size_t)p * cinPad, w1, nPairs);
                        float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                        float v1 = ApplyAct((float)a1 * sc1 + bv1, act);
                        if (hasPost) {
                            v0 = v0 * postScale + postBias;
                            v1 = v1 * postScale + postBias;
                        }
                        out0[p] = v0; out1[p] = v1;
                    }
                }
            }
            for (; i < nCh; ++i) {
                const int co = sCo + i;
                const int16_t *w0 = wBlock.data() + (size_t)i * cinPad;
                const float sc = args->scales[co] * xScale;
                const float bv = args->bias[co];
                if (args->outS16) {
                    const float invOut = args->invOutScale;
                    int16_t *outC = args->outS16 + (size_t)co * outHW;
                    if (act == ACT_HARDSWISH) {
                        const FixHSwishLayer layHsw = MakeFixHSwishLayer(postScale, postBias, invOut);
                        const FixHSwishChan fp0 = MakeFixHSwishChan(sc, bv);
                        for (int p = p0; p < pEnd; ++p) {
                            int32_t acc = DotS16(inHwc + (size_t)p * cinPad, w0, nPairs);
                            outC[p] = layHsw.fastPost
                                ? ApplyFixHSwishFastCh(acc, fp0, layHsw.m2, layHsw.sh2, layHsw.rnd2)
                                : ApplyFixHSwishGeneric(acc, fp0, layHsw);
                        }
                    } else {
                        for (int p = p0; p < pEnd; ++p) {
                            int32_t acc = DotS16(inHwc + (size_t)p * cinPad, w0, nPairs);
                            float v = ApplyAct((float)acc * sc + bv, act);
                            if (hasPost) v = v * postScale + postBias;
                            outC[p] = ClampRoundToS16(v * invOut);
                        }
                    }
                } else {
                    float *outC = args->out + (size_t)co * outHW;
                    for (int p = p0; p < pEnd; ++p) {
                        int32_t acc = DotS16(inHwc + (size_t)p * cinPad, w0, nPairs);
                        float v = ApplyAct((float)acc * sc + bv, act);
                        if (hasPost) v = v * postScale + postBias;
                        outC[p] = v;
                    }
                }
            }
        }
    };

    if (args->nextCo) {
        for (;;) {
            int s = args->nextCo->fetch_add(args->chunkSize, std::memory_order_relaxed);
            if (s >= args->totalCout) break;
            int e = std::min(args->totalCout, s + args->chunkSize);
            runSlice(s, e);
        }
    } else {
        runSlice(args->coStart, args->coEnd);
    }
}

#ifdef __3DS__
static void Conv1x1WorkerEntry(void *arg) {
    RunConv1x1Range((const Conv1x1RangeArgs *)arg);
}
#endif

struct DepthwiseRangeArgs {
    const float *in;
    const int16_t *inS16;
    float *out;
    int16_t *outS16;
    const float *scales;
    const float *bias;
    const int8_t *wq;
    const int16_t *wq16;
    int hin, win, hout, wout, inHW, outHW, coutPad;
    int kh, kw, sh, sw, ph, pw;
    int coStart, coEnd;
    float inScale, invOutScale, postScale, postBias;
    uint8_t act;
    bool hasPost;
    std::atomic<int> *nextCo;
    int chunkSize;
    int totalCout;
};

template <bool IsHardSwish>
static inline float FinishDwPx(int32_t acc, float sc, float bv, uint8_t act,
                               float postScale, float postBias, bool hasPost)
{
    float x = (float)acc * sc + bv;
    if (IsHardSwish) {
        return ApplyHardSwishPost(x, postScale, postBias, hasPost);
    }
    float v = ApplyAct(x, act);
    if (hasPost) v = v * postScale + postBias;
    return v;
}

__attribute__((noinline))
static void AccumDw5RowSw1(const int16_t *row, int32_t *acc, int count,
                           uint32_t wp0, uint32_t wp1, int32_t w4, bool firstRow)
{
    int k = 0;
    if (count >= 2) {
#ifdef __ARM_FEATURE_SIMD32
        int pairs = count >> 1;
        uint32_t p01 = Pack2S16(row[0], row[1]);
        uint32_t p12 = Pack2S16(row[1], row[2]);
        uint32_t p23 = Pack2S16(row[2], row[3]);
        const int16_t *p = row + 4;
        int32_t *dst = acc;
        uint32_t x4, x5;
        int32_t a0;
        if (firstRow) {
            __asm__ volatile(
                "1:\n\t"
                "ldrsh %[x4], [%[p]], #2\n\t"
                "ldrsh %[x5], [%[p]], #2\n\t"
                "smuad %[a0], %[p01], %[wp0]\n\t"
                "smlad %[a0], %[p23], %[wp1], %[a0]\n\t"
                "mla   %[a0], %[x4], %[w4], %[a0]\n\t"
                "str   %[a0], [%[dst]], #4\n\t"
                "mov   %[p01], %[p23]\n\t"
                "lsr   %[p23], %[p23], #16\n\t"
                "smuad %[a0], %[p12], %[wp0]\n\t"
                "pkhbt %[p12], %[p23], %[x4], lsl #16\n\t"
                "smlad %[a0], %[p12], %[wp1], %[a0]\n\t"
                "mla   %[a0], %[x5], %[w4], %[a0]\n\t"
                "pkhbt %[p23], %[x4], %[x5], lsl #16\n\t"
                "str   %[a0], [%[dst]], #4\n\t"
                "subs  %[cnt], %[cnt], #1\n\t"
                "bne   1b\n\t"
                : [p] "+r"(p), [dst] "+r"(dst), [cnt] "+r"(pairs),
                  [p01] "+r"(p01), [p12] "+r"(p12), [p23] "+r"(p23),
                  [x4] "=&r"(x4), [x5] "=&r"(x5), [a0] "=&r"(a0)
                : [wp0] "r"(wp0), [wp1] "r"(wp1), [w4] "r"(w4)
                : "cc", "memory"
            );
        } else {
            __asm__ volatile(
                "2:\n\t"
                "ldr   %[a0], [%[dst]]\n\t"
                "ldrsh %[x4], [%[p]], #2\n\t"
                "ldrsh %[x5], [%[p]], #2\n\t"
                "smlad %[a0], %[p01], %[wp0], %[a0]\n\t"
                "smlad %[a0], %[p23], %[wp1], %[a0]\n\t"
                "mla   %[a0], %[x4], %[w4], %[a0]\n\t"
                "str   %[a0], [%[dst]], #4\n\t"
                "ldr   %[a0], [%[dst]]\n\t"
                "mov   %[p01], %[p23]\n\t"
                "lsr   %[p23], %[p23], #16\n\t"
                "smlad %[a0], %[p12], %[wp0], %[a0]\n\t"
                "pkhbt %[p12], %[p23], %[x4], lsl #16\n\t"
                "smlad %[a0], %[p12], %[wp1], %[a0]\n\t"
                "mla   %[a0], %[x5], %[w4], %[a0]\n\t"
                "pkhbt %[p23], %[x4], %[x5], lsl #16\n\t"
                "str   %[a0], [%[dst]], #4\n\t"
                "subs  %[cnt], %[cnt], #1\n\t"
                "bne   2b\n\t"
                : [p] "+r"(p), [dst] "+r"(dst), [cnt] "+r"(pairs),
                  [p01] "+r"(p01), [p12] "+r"(p12), [p23] "+r"(p23),
                  [x4] "=&r"(x4), [x5] "=&r"(x5), [a0] "=&r"(a0)
                : [wp0] "r"(wp0), [wp1] "r"(wp1), [w4] "r"(w4)
                : "cc", "memory"
            );
        }
        k = (count >> 1) << 1;
#else
        int32_t x0 = row[0], x1 = row[1], x2 = row[2], x3 = row[3];
        uint32_t p01 = Pack2S16(x0, x1);
        uint32_t p12 = Pack2S16(x1, x2);
        uint32_t p23 = Pack2S16(x2, x3);
        const int16_t *p = row + 4;
        if (firstRow) {
            for (; k + 1 < count; k += 2) {
                int32_t x4 = p[0], x5 = p[1];
                p += 2;
                uint32_t p34 = Pack2S16(x3, x4);
                uint32_t p45 = Pack2S16(x4, x5);
                int32_t a0 = Smlad(p01, wp0, 0);
                a0 = Smlad(p23, wp1, a0);
                a0 += x4 * w4;
                int32_t a1 = Smlad(p12, wp0, 0);
                a1 = Smlad(p34, wp1, a1);
                a1 += x5 * w4;
                acc[k]     = a0;
                acc[k + 1] = a1;
                p01 = p23;
                p12 = p34;
                p23 = p45;
                x3 = x5;
            }
        } else {
            for (; k + 1 < count; k += 2) {
                int32_t x4 = p[0], x5 = p[1];
                p += 2;
                uint32_t p34 = Pack2S16(x3, x4);
                uint32_t p45 = Pack2S16(x4, x5);
                int32_t a0 = acc[k];
                int32_t a1 = acc[k + 1];
                a0 = Smlad(p01, wp0, a0);
                a0 = Smlad(p23, wp1, a0);
                a0 += x4 * w4;
                a1 = Smlad(p12, wp0, a1);
                a1 = Smlad(p34, wp1, a1);
                a1 += x5 * w4;
                acc[k]     = a0;
                acc[k + 1] = a1;
                p01 = p23;
                p12 = p34;
                p23 = p45;
                x3 = x5;
            }
        }
#endif
    }
    if (k < count) {
        const int16_t *t = row + k;
        int32_t a = firstRow ? 0 : acc[k];
        a = Smlad(Pack2S16(t[0], t[1]), wp0, a);
        a = Smlad(Pack2S16(t[2], t[3]), wp1, a);
        a += (int32_t)t[4] * w4;
        acc[k] = a;
    }
}

__attribute__((noinline))
static void AccumDw3RowSw1(const int16_t *row, int32_t *acc, int count,
                           uint32_t wp0, int32_t w2, bool firstRow)
{
    int k = 0;
    if (count >= 2) {
#ifdef __ARM_FEATURE_SIMD32
        int pairs = count >> 1;
        uint32_t p01 = Pack2S16(row[0], row[1]);
        uint32_t x1 = (uint32_t)(uint16_t)row[1];
        const int16_t *p = row + 2;
        int32_t *dst = acc;
        uint32_t x2, x3, p12;
        int32_t a0;
        if (firstRow) {
            __asm__ volatile(
                "1:\n\t"
                "ldrsh %[x2], [%[p]], #2\n\t"
                "ldrsh %[x3], [%[p]], #2\n\t"
                "smuad %[a0], %[p01], %[wp0]\n\t"
                "pkhbt %[p12], %[x1], %[x2], lsl #16\n\t"
                "mla   %[a0], %[x2], %[w2], %[a0]\n\t"
                "str   %[a0], [%[dst]], #4\n\t"
                "smuad %[a0], %[p12], %[wp0]\n\t"
                "pkhbt %[p01], %[x2], %[x3], lsl #16\n\t"
                "mla   %[a0], %[x3], %[w2], %[a0]\n\t"
                "mov   %[x1], %[x3]\n\t"
                "str   %[a0], [%[dst]], #4\n\t"
                "subs  %[cnt], %[cnt], #1\n\t"
                "bne   1b\n\t"
                : [p] "+r"(p), [dst] "+r"(dst), [cnt] "+r"(pairs),
                  [p01] "+r"(p01), [x1] "+r"(x1),
                  [x2] "=&r"(x2), [x3] "=&r"(x3), [p12] "=&r"(p12), [a0] "=&r"(a0)
                : [wp0] "r"(wp0), [w2] "r"(w2)
                : "cc", "memory"
            );
        } else {
            __asm__ volatile(
                "2:\n\t"
                "ldr   %[a0], [%[dst]]\n\t"
                "ldrsh %[x2], [%[p]], #2\n\t"
                "ldrsh %[x3], [%[p]], #2\n\t"
                "smlad %[a0], %[p01], %[wp0], %[a0]\n\t"
                "pkhbt %[p12], %[x1], %[x2], lsl #16\n\t"
                "mla   %[a0], %[x2], %[w2], %[a0]\n\t"
                "str   %[a0], [%[dst]], #4\n\t"
                "ldr   %[a0], [%[dst]]\n\t"
                "pkhbt %[p01], %[x2], %[x3], lsl #16\n\t"
                "smlad %[a0], %[p12], %[wp0], %[a0]\n\t"
                "mla   %[a0], %[x3], %[w2], %[a0]\n\t"
                "mov   %[x1], %[x3]\n\t"
                "str   %[a0], [%[dst]], #4\n\t"
                "subs  %[cnt], %[cnt], #1\n\t"
                "bne   2b\n\t"
                : [p] "+r"(p), [dst] "+r"(dst), [cnt] "+r"(pairs),
                  [p01] "+r"(p01), [x1] "+r"(x1),
                  [x2] "=&r"(x2), [x3] "=&r"(x3), [p12] "=&r"(p12), [a0] "=&r"(a0)
                : [wp0] "r"(wp0), [w2] "r"(w2)
                : "cc", "memory"
            );
        }
        k = (count >> 1) << 1;
#else
        int32_t x0 = row[0], x1 = row[1];
        uint32_t p01 = Pack2S16(x0, x1);
        const int16_t *p = row + 2;
        if (firstRow) {
            for (; k + 1 < count; k += 2) {
                int32_t x2 = p[0], x3 = p[1];
                p += 2;
                uint32_t p12 = Pack2S16(x1, x2);
                uint32_t p23 = Pack2S16(x2, x3);
                int32_t a0 = Smlad(p01, wp0, 0) + x2 * w2;
                int32_t a1 = Smlad(p12, wp0, 0) + x3 * w2;
                acc[k]     = a0;
                acc[k + 1] = a1;
                p01 = p23;
                x1 = x3;
            }
        } else {
            for (; k + 1 < count; k += 2) {
                int32_t x2 = p[0], x3 = p[1];
                p += 2;
                uint32_t p12 = Pack2S16(x1, x2);
                uint32_t p23 = Pack2S16(x2, x3);
                int32_t a0 = Smlad(p01, wp0, acc[k]) + x2 * w2;
                int32_t a1 = Smlad(p12, wp0, acc[k + 1]) + x3 * w2;
                acc[k]     = a0;
                acc[k + 1] = a1;
                p01 = p23;
                x1 = x3;
            }
        }
#endif
    }
    if (k < count) {
        const int16_t *t = row + k;
        int32_t a = firstRow ? 0 : acc[k];
        a = Smlad(Pack2S16(t[0], t[1]), wp0, a);
        a += (int32_t)t[2] * w2;
        acc[k] = a;
    }
}

__attribute__((noinline))
static void RunDw3x3RowSw1(const int16_t *r0, const int16_t *r1, const int16_t *r2,
                           int32_t *acc, int count,
                           const uint32_t wp0[3], const int32_t w2[3])
{
    const uint32_t wp0_0 = wp0[0], wp0_1 = wp0[1], wp0_2 = wp0[2];
    const int32_t w2_0 = w2[0], w2_1 = w2[1], w2_2 = w2[2];
    int k = 0;
    for (; k + 1 < count; k += 2) {
        const int16_t *p0 = r0 + k;
        int32_t x0 = p0[0], x1 = p0[1], x2 = p0[2], x3 = p0[3];
        int32_t a0 = Smlad(Pack2S16(x0, x1), wp0_0, 0) + x2 * w2_0;
        int32_t a1 = Smlad(Pack2S16(x1, x2), wp0_0, 0) + x3 * w2_0;

        const int16_t *p1 = r1 + k;
        x0 = p1[0]; x1 = p1[1]; x2 = p1[2]; x3 = p1[3];
        a0 = Smlad(Pack2S16(x0, x1), wp0_1, a0) + x2 * w2_1;
        a1 = Smlad(Pack2S16(x1, x2), wp0_1, a1) + x3 * w2_1;

        const int16_t *p2 = r2 + k;
        x0 = p2[0]; x1 = p2[1]; x2 = p2[2]; x3 = p2[3];
        a0 = Smlad(Pack2S16(x0, x1), wp0_2, a0) + x2 * w2_2;
        a1 = Smlad(Pack2S16(x1, x2), wp0_2, a1) + x3 * w2_2;

        acc[k]     = a0;
        acc[k + 1] = a1;
    }
    if (k < count) {
        const int16_t *p0 = r0 + k;
        const int16_t *p1 = r1 + k;
        const int16_t *p2 = r2 + k;
        int32_t a = Smlad(Pack2S16(p0[0], p0[1]), wp0_0, 0) + (int32_t)p0[2] * w2_0;
        a = Smlad(Pack2S16(p1[0], p1[1]), wp0_1, a) + (int32_t)p1[2] * w2_1;
        a = Smlad(Pack2S16(p2[0], p2[1]), wp0_2, a) + (int32_t)p2[2] * w2_2;
        acc[k] = a;
    }
}

template <bool IsHardSwish>
__attribute__((noinline))
static void RunDwInterior5x5(const int16_t *inQ, float *outRow, int32_t *rowAcc, int win, int iy0,
                             int ox0, int ox1, int sw, int pw,
                             const uint32_t wp0[5], const uint32_t wp1[5], const int32_t w4[5],
                             float sc, float bv, uint8_t act,
                             float postScale, float postBias, bool hasPost)
{
    const int count = ox1 - ox0;
    if (count <= 0) return;

    if (sw == 1) {
        const int base = iy0 * win + ox0 - pw;
        AccumDw5RowSw1(inQ + base + 0 * win, rowAcc, count, wp0[0], wp1[0], w4[0], true);
        AccumDw5RowSw1(inQ + base + 1 * win, rowAcc, count, wp0[1], wp1[1], w4[1], false);
        AccumDw5RowSw1(inQ + base + 2 * win, rowAcc, count, wp0[2], wp1[2], w4[2], false);
        AccumDw5RowSw1(inQ + base + 3 * win, rowAcc, count, wp0[3], wp1[3], w4[3], false);
        AccumDw5RowSw1(inQ + base + 4 * win, rowAcc, count, wp0[4], wp1[4], w4[4], false);
        float *dst = outRow + ox0;
        for (int k = 0; k < count; ++k) {
            dst[k] = FinishDwPx<IsHardSwish>(rowAcc[k], sc, bv, act, postScale, postBias, hasPost);
        }
        return;
    }

    const int16_t *r0 = inQ + (iy0 + 0) * win - pw;
    const int16_t *r1 = inQ + (iy0 + 1) * win - pw;
    const int16_t *r2 = inQ + (iy0 + 2) * win - pw;
    const int16_t *r3 = inQ + (iy0 + 3) * win - pw;
    const int16_t *r4 = inQ + (iy0 + 4) * win - pw;
    for (int ox = ox0; ox < ox1; ++ox) {
        const int sx = ox * sw;
        int32_t acc = 0;
        #define DW5_ROW_1PX(ROW, K) do { \
            const int16_t *p = (ROW) + sx; \
            acc = Smlad(Pack2S16(p[0], p[1]), wp0[K], acc); \
            acc = Smlad(Pack2S16(p[2], p[3]), wp1[K], acc); \
            acc += (int32_t)p[4] * w4[K]; \
        } while (0)
        DW5_ROW_1PX(r0, 0);
        DW5_ROW_1PX(r1, 1);
        DW5_ROW_1PX(r2, 2);
        DW5_ROW_1PX(r3, 3);
        DW5_ROW_1PX(r4, 4);
        #undef DW5_ROW_1PX
        outRow[ox] = FinishDwPx<IsHardSwish>(acc, sc, bv, act, postScale, postBias, hasPost);
    }
}

template <bool IsHardSwish>
__attribute__((noinline))
static void RunDwInterior3x3(const int16_t *inQ, float *outRow, int32_t *rowAcc, int win, int iy0,
                             int ox0, int ox1, int sw, int pw,
                             const uint32_t wp0[3], const int32_t w2[3],
                             float sc, float bv, uint8_t act,
                             float postScale, float postBias, bool hasPost)
{
    const int count = ox1 - ox0;
    if (count <= 0) return;

    if (sw == 1) {
        const int base = iy0 * win + ox0 - pw;
        RunDw3x3RowSw1(inQ + base + 0 * win, inQ + base + 1 * win, inQ + base + 2 * win, rowAcc, count, wp0, w2);
        float *dst = outRow + ox0;
        for (int k = 0; k < count; ++k) {
            dst[k] = FinishDwPx<IsHardSwish>(rowAcc[k], sc, bv, act, postScale, postBias, hasPost);
        }
        return;
    }

    const int16_t *r0 = inQ + (iy0 + 0) * win - pw;
    const int16_t *r1 = inQ + (iy0 + 1) * win - pw;
    const int16_t *r2 = inQ + (iy0 + 2) * win - pw;
    for (int ox = ox0; ox < ox1; ++ox) {
        const int sx = ox * sw;
        int32_t acc = 0;
        #define DW3_ROW_1PX(ROW, K) do { \
            const int16_t *p = (ROW) + sx; \
            acc = Smlad(Pack2S16(p[0], p[1]), wp0[K], acc); \
            acc += (int32_t)p[2] * w2[K]; \
        } while (0)
        DW3_ROW_1PX(r0, 0);
        DW3_ROW_1PX(r1, 1);
        DW3_ROW_1PX(r2, 2);
        #undef DW3_ROW_1PX
        outRow[ox] = FinishDwPx<IsHardSwish>(acc, sc, bv, act, postScale, postBias, hasPost);
    }
}

static void RunDepthwiseRange(const DepthwiseRangeArgs *args) {
    const int hin = args->hin, win = args->win;
    const int hout = args->hout, wout = args->wout;
    const int inHW = args->inHW, outHW = args->outHW;
    const int kh = args->kh, kw = args->kw;
    const int sh = args->sh, sw = args->sw;
    const int ph = args->ph, pw = args->pw;
    const int ksz = kh * kw;
    const float postScale = args->postScale, postBias = args->postBias;
    const uint8_t act = args->act;
    const bool hasPost = args->hasPost;
    const bool isHardSwish = (act == ACT_HARDSWISH);

    const int padH = std::max(hin + 2 * ph, (hout > 0) ? ((hout - 1) * sh + kh) : (hin + 2 * ph));
    const int padW = std::max(win + 2 * pw, (wout > 0) ? ((wout - 1) * sw + kw) : (win + 2 * pw));

    std::vector<int16_t> inPad((size_t)padH * (size_t)padW, 0);
    std::vector<int32_t> rowAcc((size_t)wout);
    std::vector<float> tmpOutRow(args->outS16 ? (size_t)wout : 0);
    int16_t wQ[25];
    auto runSlice = [&](int sCo, int eCo) {
      for (int c = sCo; c < eCo; ++c) {
        float xScale;
        if (args->inS16) {
            xScale = args->inScale;
            const int16_t *inC16 = args->inS16 + (size_t)c * inHW;
            for (int y = 0; y < hin; ++y) {
                std::memcpy(inPad.data() + (y + ph) * padW + pw,
                            inC16 + y * win, (size_t)win * sizeof(int16_t));
            }
        } else {
            const float *inC = args->in + (size_t)c * inHW;
            float maxAbs = 1e-6f;
            int i = 0;
            for (; i + 3 < inHW; i += 4) {
                float a0 = std::fabs(inC[i + 0]);
                float a1 = std::fabs(inC[i + 1]);
                float a2 = std::fabs(inC[i + 2]);
                float a3 = std::fabs(inC[i + 3]);
                if (a0 > maxAbs) maxAbs = a0;
                if (a1 > maxAbs) maxAbs = a1;
                if (a2 > maxAbs) maxAbs = a2;
                if (a3 > maxAbs) maxAbs = a3;
            }
            for (; i < inHW; ++i) {
                float a = std::fabs(inC[i]);
                if (a > maxAbs) maxAbs = a;
            }
            xScale = maxAbs / 2048.0f;
            const float invXScale = 2048.0f / maxAbs;
            for (int y = 0; y < hin; ++y) {
                const float *srcRow = inC + y * win;
                int16_t *dstRow = inPad.data() + (y + ph) * padW + pw;
                for (int x = 0; x < win; ++x) {
                    dstRow[x] = RoundToS16(srcRow[x] * invXScale);
                }
            }
        }

        float *outC = args->out ? (args->out + (size_t)c * outHW) : nullptr;
        if (args->wq16) {
            const int16_t *wC = args->wq16 + (size_t)c * ksz;
            for (int k = 0; k < ksz; ++k) wQ[k] = wC[k];
        } else {
            const int8_t *wC = args->wq + (size_t)c * ksz;
            for (int k = 0; k < ksz; ++k) wQ[k] = (int16_t)wC[k];
        }
        const float sc = args->scales[c] * xScale;
        const float bv = args->bias[c];

        uint32_t wp0_5[5] = {0}, wp1_5[5] = {0};
        int32_t w4_5[5] = {0};
        uint32_t wp0_3[3] = {0};
        int32_t w2_3[3] = {0};
        if (kh == 5 && kw == 5) {
            for (int ky = 0; ky < 5; ++ky) {
                wp0_5[ky] = Pack2S16(wQ[ky * 5 + 0], wQ[ky * 5 + 1]);
                wp1_5[ky] = Pack2S16(wQ[ky * 5 + 2], wQ[ky * 5 + 3]);
                w4_5[ky]  = (int32_t)wQ[ky * 5 + 4];
            }
        } else if (kh == 3 && kw == 3) {
            for (int ky = 0; ky < 3; ++ky) {
                wp0_3[ky] = Pack2S16(wQ[ky * 3 + 0], wQ[ky * 3 + 1]);
                w2_3[ky]  = (int32_t)wQ[ky * 3 + 2];
            }
        }

        const FixHSwishLayer layHsw = (args->outS16 && isHardSwish)
            ? MakeFixHSwishLayer(postScale, postBias, args->invOutScale)
            : FixHSwishLayer{0, 0, 0, 0, false};
        const FixHSwishChan fpHsw = (args->outS16 && isHardSwish)
            ? MakeFixHSwishChan(sc, bv)
            : FixHSwishChan{0, 0, 0, 0};

        for (int oy = 0; oy < hout; ++oy) {
            const int iy0 = oy * sh;
            if (args->outS16 && isHardSwish && sw == 1 && (kh == 5 || kh == 3) && kh == kw) {
                const int base = iy0 * padW;
                if (kh == 5) {
                    AccumDw5RowSw1(inPad.data() + base + 0 * padW, rowAcc.data(), wout, wp0_5[0], wp1_5[0], w4_5[0], true);
                    AccumDw5RowSw1(inPad.data() + base + 1 * padW, rowAcc.data(), wout, wp0_5[1], wp1_5[1], w4_5[1], false);
                    AccumDw5RowSw1(inPad.data() + base + 2 * padW, rowAcc.data(), wout, wp0_5[2], wp1_5[2], w4_5[2], false);
                    AccumDw5RowSw1(inPad.data() + base + 3 * padW, rowAcc.data(), wout, wp0_5[3], wp1_5[3], w4_5[3], false);
                    AccumDw5RowSw1(inPad.data() + base + 4 * padW, rowAcc.data(), wout, wp0_5[4], wp1_5[4], w4_5[4], false);
                } else {
                    RunDw3x3RowSw1(inPad.data() + base + 0 * padW, inPad.data() + base + 1 * padW, inPad.data() + base + 2 * padW,
                                   rowAcc.data(), wout, wp0_3, w2_3);
                }
                const int coutPad = args->coutPad;
                int16_t *dstHwc = args->outS16 + ((size_t)oy * wout) * coutPad + c;
                ApplyFixHSwishRowStrided(rowAcc.data(), dstHwc, wout, coutPad, fpHsw, layHsw);
                continue;
            }
            if (args->outS16 && isHardSwish && (kh == 5 || kh == 3) && kh == kw) {
                const int coutPad = args->coutPad;
                int16_t *dstHwc = args->outS16 + ((size_t)oy * wout) * coutPad + c;
                if (kh == 5) {
                    const int16_t *r0 = inPad.data() + (iy0 + 0) * padW;
                    const int16_t *r1 = inPad.data() + (iy0 + 1) * padW;
                    const int16_t *r2 = inPad.data() + (iy0 + 2) * padW;
                    const int16_t *r3 = inPad.data() + (iy0 + 3) * padW;
                    const int16_t *r4 = inPad.data() + (iy0 + 4) * padW;
                    for (int ox = 0; ox < wout; ++ox) {
                        const int sx = ox * sw;
                        int32_t acc = 0;
                        #define DW5_ROW_S16(ROW, K) do { \
                            const int16_t *p = (ROW) + sx; \
                            acc = Smlad(Pack2S16(p[0], p[1]), wp0_5[K], acc); \
                            acc = Smlad(Pack2S16(p[2], p[3]), wp1_5[K], acc); \
                            acc += (int32_t)p[4] * w4_5[K]; \
                        } while (0)
                        DW5_ROW_S16(r0, 0);
                        DW5_ROW_S16(r1, 1);
                        DW5_ROW_S16(r2, 2);
                        DW5_ROW_S16(r3, 3);
                        DW5_ROW_S16(r4, 4);
                        #undef DW5_ROW_S16
                        dstHwc[(size_t)ox * coutPad] = layHsw.fastPost
                            ? ApplyFixHSwishFastCh(acc, fpHsw, layHsw.m2, layHsw.sh2, layHsw.rnd2)
                            : ApplyFixHSwishGeneric(acc, fpHsw, layHsw);
                    }
                } else {
                    const int16_t *r0 = inPad.data() + (iy0 + 0) * padW;
                    const int16_t *r1 = inPad.data() + (iy0 + 1) * padW;
                    const int16_t *r2 = inPad.data() + (iy0 + 2) * padW;
                    for (int ox = 0; ox < wout; ++ox) {
                        const int sx = ox * sw;
                        int32_t acc = 0;
                        #define DW3_ROW_S16(ROW, K) do { \
                            const int16_t *p = (ROW) + sx; \
                            acc = Smlad(Pack2S16(p[0], p[1]), wp0_3[K], acc); \
                            acc += (int32_t)p[2] * w2_3[K]; \
                        } while (0)
                        DW3_ROW_S16(r0, 0);
                        DW3_ROW_S16(r1, 1);
                        DW3_ROW_S16(r2, 2);
                        #undef DW3_ROW_S16
                        dstHwc[(size_t)ox * coutPad] = layHsw.fastPost
                            ? ApplyFixHSwishFastCh(acc, fpHsw, layHsw.m2, layHsw.sh2, layHsw.rnd2)
                            : ApplyFixHSwishGeneric(acc, fpHsw, layHsw);
                    }
                }
                continue;
            }
            float *outRow = args->outS16 ? tmpOutRow.data() : (outC + oy * wout);
            if (kh == 5 && kw == 5) {
                if (isHardSwish) {
                    RunDwInterior5x5<true>(inPad.data(), outRow, rowAcc.data(), padW, iy0, 0, wout, sw, 0,
                                           wp0_5, wp1_5, w4_5, sc, bv, act, postScale, postBias, hasPost);
                } else {
                    RunDwInterior5x5<false>(inPad.data(), outRow, rowAcc.data(), padW, iy0, 0, wout, sw, 0,
                                            wp0_5, wp1_5, w4_5, sc, bv, act, postScale, postBias, hasPost);
                }
            } else if (kh == 3 && kw == 3) {
                if (isHardSwish) {
                    RunDwInterior3x3<true>(inPad.data(), outRow, rowAcc.data(), padW, iy0, 0, wout, sw, 0,
                                           wp0_3, w2_3, sc, bv, act, postScale, postBias, hasPost);
                } else {
                    RunDwInterior3x3<false>(inPad.data(), outRow, rowAcc.data(), padW, iy0, 0, wout, sw, 0,
                                            wp0_3, w2_3, sc, bv, act, postScale, postBias, hasPost);
                }
            } else {
                for (int ox = 0; ox < wout; ++ox) {
                    const int ix0 = ox * sw;
                    int32_t acc = 0;
                    for (int ky = 0; ky < kh; ++ky) {
                        const int16_t *inRow = inPad.data() + (iy0 + ky) * padW + ix0;
                        const int16_t *wRow = wQ + ky * kw;
                        for (int kx = 0; kx < kw; ++kx) {
                            acc += (int32_t)inRow[kx] * (int32_t)wRow[kx];
                        }
                    }
                    outRow[ox] = FinishDwPx<false>(acc, sc, bv, act, postScale, postBias, hasPost);
                }
            }
            if (args->outS16) {
                const float invOut = args->invOutScale;
                const int coutPad = args->coutPad;
                int16_t *dstHwc = args->outS16 + ((size_t)oy * wout) * coutPad + c;
                for (int ox = 0; ox < wout; ++ox) {
                    dstHwc[(size_t)ox * coutPad] = ClampRoundToS16(outRow[ox] * invOut);
                }
            }
        }
      }
    };

    if (args->nextCo) {
        for (;;) {
            int s = args->nextCo->fetch_add(args->chunkSize, std::memory_order_relaxed);
            if (s >= args->totalCout) break;
            int e = std::min(args->totalCout, s + args->chunkSize);
            runSlice(s, e);
        }
    } else {
        runSlice(args->coStart, args->coEnd);
    }
}

#ifdef __3DS__
static void DepthwiseWorkerEntry(void *arg) {
    RunDepthwiseRange((const DepthwiseRangeArgs *)arg);
}
#endif

struct Im2ColConvArgs {
    const int16_t *inQ;
    float *out;
    int16_t *outS16;
    const float *scales;
    const float *bias;
    const int16_t *wAll;
    int cin, hin, win, cout, hout, wout, inHW, outHW;
    int kh, kw, sh, sw, ph, pw;
    int kDim, kPad, nPairs;
    int oyStart, oyEnd;
    float xScale, invOutScale, postScale, postBias;
    uint8_t act;
    bool hasPost;
    std::atomic<int> *nextOy;
    int chunkSize;
    int totalHout;
};

static void RunIm2ColConvRange(const Im2ColConvArgs *args) {
    const int cin = args->cin, hin = args->hin, win = args->win;
    const int cout = args->cout, wout = args->wout;
    const int inHW = args->inHW, outHW = args->outHW;
    const int kh = args->kh, kw = args->kw;
    const int sh = args->sh, sw = args->sw;
    const int ph = args->ph, pw = args->pw;
    const int kDim = args->kDim, kPad = args->kPad, nPairs = args->nPairs;
    const float xScale = args->xScale;
    const float postScale = args->postScale, postBias = args->postBias;
    const uint8_t act = args->act;
    const bool hasPost = args->hasPost;
    const bool fastNone = (!hasPost && act == ACT_NONE);
    const bool fastRelu = (!hasPost && act == ACT_RELU);
    const int kPerC = kh * kw;

    // Find interior ox range where [ox*sw - pw, ox*sw - pw + kw - 1] is inside [0, win-1]
    const int oxInt0 = std::min(wout, (pw + sw - 1) / sw);
    const int oxInt1 = std::max(oxInt0, std::min(wout, (win + pw - kw) / sw + 1));

    const int kTileX = (kPad >= 512) ? 6 : (kPad >= 256) ? 12 : (kPad >= 128) ? 24 : 48;
    std::vector<int16_t> rowCol((size_t)kTileX * kPad, 0);

    auto runSlice = [&](int sOy, int eOy) {
      for (int oy = sOy; oy < eOy; ++oy) {
        const int iy0 = oy * sh - ph;
        for (int ox0 = 0; ox0 < wout; ox0 += kTileX) {
          const int ox1 = std::min(wout, ox0 + kTileX);
          const int tileW = ox1 - ox0;
          const int tInt0 = std::max(ox0, std::min(ox1, oxInt0));
          const int tInt1 = std::max(tInt0, std::min(ox1, oxInt1));
          std::memset(rowCol.data(), 0, (size_t)tileW * kPad * sizeof(int16_t));
          for (int ci = 0; ci < cin; ++ci) {
              const int16_t *inC = args->inQ + (size_t)ci * inHW;
              const int dstBase = ci * kPerC;
              for (int ky = 0; ky < kh; ++ky) {
                  const int iy = iy0 + ky;
                  if (iy < 0 || iy >= hin) continue;
                  const int16_t *inRow = inC + iy * win;
                  const int dstOff = dstBase + ky * kw;
                  for (int ox = ox0; ox < tInt0; ++ox) {
                      const int ix0 = ox * sw - pw;
                      int16_t *dst = rowCol.data() + (size_t)(ox - ox0) * kPad + dstOff;
                      for (int kx = 0; kx < kw; ++kx) {
                          const int ix = ix0 + kx;
                          if (ix >= 0 && ix < win) dst[kx] = inRow[ix];
                      }
                  }
                  if (kw == 3) {
                      int16_t *dst = rowCol.data() + (size_t)(tInt0 - ox0) * kPad + dstOff;
                      for (int ox = tInt0; ox < tInt1; ++ox, dst += kPad) {
                          const int16_t *src = inRow + (ox * sw - pw);
                          dst[0] = src[0];
                          dst[1] = src[1];
                          dst[2] = src[2];
                      }
                  } else {
                      int16_t *dst = rowCol.data() + (size_t)(tInt0 - ox0) * kPad + dstOff;
                      for (int ox = tInt0; ox < tInt1; ++ox, dst += kPad) {
                          const int16_t *src = inRow + (ox * sw - pw);
                          for (int kx = 0; kx < kw; ++kx) dst[kx] = src[kx];
                      }
                  }
                  for (int ox = tInt1; ox < ox1; ++ox) {
                      const int ix0 = ox * sw - pw;
                      int16_t *dst = rowCol.data() + (size_t)(ox - ox0) * kPad + dstOff;
                      for (int kx = 0; kx < kw; ++kx) {
                          const int ix = ix0 + kx;
                          if (ix >= 0 && ix < win) dst[kx] = inRow[ix];
                      }
                  }
              }
          }

          int co = 0;
          for (; co + 1 < cout; co += 2) {
              const int16_t *w0 = args->wAll + (size_t)co * kPad;
              const int16_t *w1 = args->wAll + (size_t)(co + 1) * kPad;
              const float sc0 = args->scales[co] * xScale;
              const float bv0 = args->bias[co];
              const float sc1 = args->scales[co + 1] * xScale;
              const float bv1 = args->bias[co + 1];
              if (args->outS16) {
                  const float invOut = args->invOutScale;
                  int16_t *out0 = args->outS16 + (size_t)co * outHW + (size_t)oy * wout + ox0;
                  int16_t *out1 = args->outS16 + (size_t)(co + 1) * outHW + (size_t)oy * wout + ox0;
                  int t = 0;
                  if (act == ACT_NONE) {
                      const FixLinearParams fl0 = MakeFixLinear(sc0, bv0, postScale, postBias, invOut);
                      const FixLinearParams fl1 = MakeFixLinear(sc1, bv1, postScale, postBias, invOut);
                      for (; t + 1 < tileW; t += 2) {
                          int32_t c00, c01, c10, c11;
                          DotS16_2x2(rowCol.data() + (size_t)t * kPad,
                                     rowCol.data() + (size_t)(t + 1) * kPad,
                                     w0, w1, nPairs, c00, c01, c10, c11);
                          out0[t]     = ApplyFixLinearToS16(c00, fl0);
                          out0[t + 1] = ApplyFixLinearToS16(c01, fl0);
                          out1[t]     = ApplyFixLinearToS16(c10, fl1);
                          out1[t + 1] = ApplyFixLinearToS16(c11, fl1);
                      }
                      for (; t < tileW; ++t) {
                          int32_t a0 = DotS16(rowCol.data() + (size_t)t * kPad, w0, nPairs);
                          int32_t a1 = DotS16(rowCol.data() + (size_t)t * kPad, w1, nPairs);
                          out0[t] = ApplyFixLinearToS16(a0, fl0);
                          out1[t] = ApplyFixLinearToS16(a1, fl1);
                      }
                      continue;
                  }
                  for (; t + 1 < tileW; t += 2) {
                      int32_t c00, c01, c10, c11;
                      DotS16_2x2(rowCol.data() + (size_t)t * kPad,
                                 rowCol.data() + (size_t)(t + 1) * kPad,
                                 w0, w1, nPairs, c00, c01, c10, c11);
                      float v00 = ApplyAct((float)c00 * sc0 + bv0, act);
                      float v01 = ApplyAct((float)c01 * sc0 + bv0, act);
                      float v10 = ApplyAct((float)c10 * sc1 + bv1, act);
                      float v11 = ApplyAct((float)c11 * sc1 + bv1, act);
                      if (hasPost) {
                          v00 = v00 * postScale + postBias; v01 = v01 * postScale + postBias;
                          v10 = v10 * postScale + postBias; v11 = v11 * postScale + postBias;
                      }
                      out0[t] = ClampRoundToS16(v00 * invOut); out0[t + 1] = ClampRoundToS16(v01 * invOut);
                      out1[t] = ClampRoundToS16(v10 * invOut); out1[t + 1] = ClampRoundToS16(v11 * invOut);
                  }
                  for (; t < tileW; ++t) {
                      int32_t a0 = DotS16(rowCol.data() + (size_t)t * kPad, w0, nPairs);
                      int32_t a1 = DotS16(rowCol.data() + (size_t)t * kPad, w1, nPairs);
                      float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                      float v1 = ApplyAct((float)a1 * sc1 + bv1, act);
                      if (hasPost) { v0 = v0 * postScale + postBias; v1 = v1 * postScale + postBias; }
                      out0[t] = ClampRoundToS16(v0 * invOut); out1[t] = ClampRoundToS16(v1 * invOut);
                  }
                  continue;
              }
              float *out0 = args->out + (size_t)co * outHW + (size_t)oy * wout + ox0;
              float *out1 = args->out + (size_t)(co + 1) * outHW + (size_t)oy * wout + ox0;

              int t = 0;
              if (fastNone) {
                  for (; t + 1 < tileW; t += 2) {
                      int32_t c00, c01, c10, c11;
                      DotS16_2x2(rowCol.data() + (size_t)t * kPad,
                                 rowCol.data() + (size_t)(t + 1) * kPad,
                                 w0, w1, nPairs, c00, c01, c10, c11);
                      out0[t] = (float)c00 * sc0 + bv0; out0[t + 1] = (float)c01 * sc0 + bv0;
                      out1[t] = (float)c10 * sc1 + bv1; out1[t + 1] = (float)c11 * sc1 + bv1;
                  }
              } else if (fastRelu) {
                  for (; t + 1 < tileW; t += 2) {
                      int32_t c00, c01, c10, c11;
                      DotS16_2x2(rowCol.data() + (size_t)t * kPad,
                                 rowCol.data() + (size_t)(t + 1) * kPad,
                                 w0, w1, nPairs, c00, c01, c10, c11);
                      float v00 = (float)c00 * sc0 + bv0;
                      float v01 = (float)c01 * sc0 + bv0;
                      float v10 = (float)c10 * sc1 + bv1;
                      float v11 = (float)c11 * sc1 + bv1;
                      out0[t] = (v00 > 0.0f) ? v00 : 0.0f;
                      out0[t + 1] = (v01 > 0.0f) ? v01 : 0.0f;
                      out1[t] = (v10 > 0.0f) ? v10 : 0.0f;
                      out1[t + 1] = (v11 > 0.0f) ? v11 : 0.0f;
                  }
              } else {
                  for (; t + 1 < tileW; t += 2) {
                      int32_t c00, c01, c10, c11;
                      DotS16_2x2(rowCol.data() + (size_t)t * kPad,
                                 rowCol.data() + (size_t)(t + 1) * kPad,
                                 w0, w1, nPairs, c00, c01, c10, c11);
                      float v00 = ApplyAct((float)c00 * sc0 + bv0, act);
                      float v01 = ApplyAct((float)c01 * sc0 + bv0, act);
                      float v10 = ApplyAct((float)c10 * sc1 + bv1, act);
                      float v11 = ApplyAct((float)c11 * sc1 + bv1, act);
                      if (hasPost) {
                          v00 = v00 * postScale + postBias;
                          v01 = v01 * postScale + postBias;
                          v10 = v10 * postScale + postBias;
                          v11 = v11 * postScale + postBias;
                      }
                      out0[t] = v00; out0[t + 1] = v01;
                      out1[t] = v10; out1[t + 1] = v11;
                  }
              }
              for (; t < tileW; ++t) {
                  int32_t a0 = DotS16(rowCol.data() + (size_t)t * kPad, w0, nPairs);
                  int32_t a1 = DotS16(rowCol.data() + (size_t)t * kPad, w1, nPairs);
                  float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                  float v1 = ApplyAct((float)a1 * sc1 + bv1, act);
                  if (hasPost) {
                      v0 = v0 * postScale + postBias;
                      v1 = v1 * postScale + postBias;
                  }
                  out0[t] = v0;
                  out1[t] = v1;
              }
          }
          for (; co < cout; ++co) {
              const int16_t *w0 = args->wAll + (size_t)co * kPad;
              const float sc0 = args->scales[co] * xScale;
              const float bv0 = args->bias[co];
              if (args->outS16) {
                  const float invOut = args->invOutScale;
                  int16_t *out0 = args->outS16 + (size_t)co * outHW + (size_t)oy * wout + ox0;
                  for (int t = 0; t < tileW; ++t) {
                      int32_t a0 = DotS16(rowCol.data() + (size_t)t * kPad, w0, nPairs);
                      float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                      if (hasPost) v0 = v0 * postScale + postBias;
                      out0[t] = ClampRoundToS16(v0 * invOut);
                  }
              } else {
                  float *out0 = args->out + (size_t)co * outHW + (size_t)oy * wout + ox0;
                  for (int t = 0; t < tileW; ++t) {
                      int32_t a0 = DotS16(rowCol.data() + (size_t)t * kPad, w0, nPairs);
                      float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                      if (hasPost) v0 = v0 * postScale + postBias;
                      out0[t] = v0;
                  }
              }
          }
        }
      }
    };

    if (args->nextOy) {
        for (;;) {
            int s = args->nextOy->fetch_add(args->chunkSize, std::memory_order_relaxed);
            if (s >= args->totalHout) break;
            int e = std::min(args->totalHout, s + args->chunkSize);
            runSlice(s, e);
        }
    } else {
        runSlice(args->oyStart, args->oyEnd);
    }
}

#ifdef __3DS__
static void Im2ColConvWorkerEntry(void *arg) {
    RunIm2ColConvRange((const Im2ColConvArgs *)arg);
}
#endif

struct Im2ColCoArgs {
    const int16_t *rowCol;
    float *out;
    int16_t *outS16;
    const float *scales;
    const float *bias;
    const int8_t *wq;
    const int16_t *wq16;
    int cout, outHW, kDim, kPad, nPairs;
    float xScale, invOutScale, postScale, postBias;
    uint8_t act;
    bool hasPost;
    std::atomic<int> *nextCo;
    int chunkSize;
};

static void RunIm2ColCoRange(const Im2ColCoArgs *args) {
    const int cout = args->cout, outHW = args->outHW;
    const int kDim = args->kDim, kPad = args->kPad, nPairs = args->nPairs;
    const float xScale = args->xScale;
    const float postScale = args->postScale, postBias = args->postBias;
    const uint8_t act = args->act;
    const bool hasPost = args->hasPost;
    const bool fastNone = (!hasPost && act == ACT_NONE);
    const bool fastRelu = (!hasPost && act == ACT_RELU);
    const int16_t *rowCol = args->rowCol;
    std::vector<int16_t> w0Buf(kPad, 0), w1Buf(kPad, 0);
    int16_t *w0 = w0Buf.data(), *w1 = w1Buf.data();
    auto unpackW = [&](int co, int16_t *dst) {
        if (args->wq16) std::memcpy(dst, args->wq16 + (size_t)co * kDim, (size_t)kDim * sizeof(int16_t));
        else UnpackS8ToS16(args->wq + (size_t)co * kDim, dst, kDim);
    };

    for (;;) {
        int sCo = args->nextCo->fetch_add(args->chunkSize, std::memory_order_relaxed);
        if (sCo >= cout) break;
        int eCo = std::min(cout, sCo + args->chunkSize);
        int co = sCo;
        for (; co + 1 < eCo; co += 2) {
            unpackW(co, w0);
            unpackW(co + 1, w1);
            const float sc0 = args->scales[co] * xScale;
            const float bv0 = args->bias[co];
            const float sc1 = args->scales[co + 1] * xScale;
            const float bv1 = args->bias[co + 1];
            if (args->outS16) {
                const float invOut = args->invOutScale;
                int16_t *out0 = args->outS16 + (size_t)co * outHW;
                int16_t *out1 = args->outS16 + (size_t)(co + 1) * outHW;
                int p = 0;
                for (; p + 1 < outHW; p += 2) {
                    int32_t c00, c01, c10, c11;
                    DotS16_2x2(rowCol + (size_t)p * kPad, rowCol + (size_t)(p + 1) * kPad,
                               w0, w1, nPairs, c00, c01, c10, c11);
                    float v00 = ApplyAct((float)c00 * sc0 + bv0, act);
                    float v01 = ApplyAct((float)c01 * sc0 + bv0, act);
                    float v10 = ApplyAct((float)c10 * sc1 + bv1, act);
                    float v11 = ApplyAct((float)c11 * sc1 + bv1, act);
                    if (hasPost) {
                        v00 = v00 * postScale + postBias; v01 = v01 * postScale + postBias;
                        v10 = v10 * postScale + postBias; v11 = v11 * postScale + postBias;
                    }
                    out0[p] = ClampRoundToS16(v00 * invOut); out0[p + 1] = ClampRoundToS16(v01 * invOut);
                    out1[p] = ClampRoundToS16(v10 * invOut); out1[p + 1] = ClampRoundToS16(v11 * invOut);
                }
                for (; p < outHW; ++p) {
                    int32_t a0 = DotS16(rowCol + (size_t)p * kPad, w0, nPairs);
                    int32_t a1 = DotS16(rowCol + (size_t)p * kPad, w1, nPairs);
                    float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                    float v1 = ApplyAct((float)a1 * sc1 + bv1, act);
                    if (hasPost) { v0 = v0 * postScale + postBias; v1 = v1 * postScale + postBias; }
                    out0[p] = ClampRoundToS16(v0 * invOut); out1[p] = ClampRoundToS16(v1 * invOut);
                }
                continue;
            }
            float *out0 = args->out + (size_t)co * outHW;
            float *out1 = args->out + (size_t)(co + 1) * outHW;
            int p = 0;
            if (fastNone) {
                for (; p + 1 < outHW; p += 2) {
                    int32_t c00, c01, c10, c11;
                    DotS16_2x2(rowCol + (size_t)p * kPad, rowCol + (size_t)(p + 1) * kPad,
                               w0, w1, nPairs, c00, c01, c10, c11);
                    out0[p] = (float)c00 * sc0 + bv0; out0[p + 1] = (float)c01 * sc0 + bv0;
                    out1[p] = (float)c10 * sc1 + bv1; out1[p + 1] = (float)c11 * sc1 + bv1;
                }
            } else if (fastRelu) {
                for (; p + 1 < outHW; p += 2) {
                    int32_t c00, c01, c10, c11;
                    DotS16_2x2(rowCol + (size_t)p * kPad, rowCol + (size_t)(p + 1) * kPad,
                               w0, w1, nPairs, c00, c01, c10, c11);
                    float v00 = (float)c00 * sc0 + bv0, v01 = (float)c01 * sc0 + bv0;
                    float v10 = (float)c10 * sc1 + bv1, v11 = (float)c11 * sc1 + bv1;
                    out0[p] = (v00 > 0.0f) ? v00 : 0.0f; out0[p + 1] = (v01 > 0.0f) ? v01 : 0.0f;
                    out1[p] = (v10 > 0.0f) ? v10 : 0.0f; out1[p + 1] = (v11 > 0.0f) ? v11 : 0.0f;
                }
            } else {
                for (; p + 1 < outHW; p += 2) {
                    int32_t c00, c01, c10, c11;
                    DotS16_2x2(rowCol + (size_t)p * kPad, rowCol + (size_t)(p + 1) * kPad,
                               w0, w1, nPairs, c00, c01, c10, c11);
                    float v00 = ApplyAct((float)c00 * sc0 + bv0, act);
                    float v01 = ApplyAct((float)c01 * sc0 + bv0, act);
                    float v10 = ApplyAct((float)c10 * sc1 + bv1, act);
                    float v11 = ApplyAct((float)c11 * sc1 + bv1, act);
                    if (hasPost) {
                        v00 = v00 * postScale + postBias; v01 = v01 * postScale + postBias;
                        v10 = v10 * postScale + postBias; v11 = v11 * postScale + postBias;
                    }
                    out0[p] = v00; out0[p + 1] = v01;
                    out1[p] = v10; out1[p + 1] = v11;
                }
            }
            for (; p < outHW; ++p) {
                int32_t a0 = DotS16(rowCol + (size_t)p * kPad, w0, nPairs);
                int32_t a1 = DotS16(rowCol + (size_t)p * kPad, w1, nPairs);
                float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                float v1 = ApplyAct((float)a1 * sc1 + bv1, act);
                if (hasPost) { v0 = v0 * postScale + postBias; v1 = v1 * postScale + postBias; }
                out0[p] = v0; out1[p] = v1;
            }
        }
        for (; co < eCo; ++co) {
            unpackW(co, w0);
            const float sc0 = args->scales[co] * xScale;
            const float bv0 = args->bias[co];
            if (args->outS16) {
                const float invOut = args->invOutScale;
                int16_t *out0 = args->outS16 + (size_t)co * outHW;
                for (int p = 0; p < outHW; ++p) {
                    int32_t a0 = DotS16(rowCol + (size_t)p * kPad, w0, nPairs);
                    float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                    if (hasPost) v0 = v0 * postScale + postBias;
                    out0[p] = ClampRoundToS16(v0 * invOut);
                }
            } else {
                float *out0 = args->out + (size_t)co * outHW;
                for (int p = 0; p < outHW; ++p) {
                    int32_t a0 = DotS16(rowCol + (size_t)p * kPad, w0, nPairs);
                    float v0 = ApplyAct((float)a0 * sc0 + bv0, act);
                    if (hasPost) v0 = v0 * postScale + postBias;
                    out0[p] = v0;
                }
            }
        }
    }
}

#ifdef __3DS__
static void Im2ColCoWorkerEntry(void *arg) {
    RunIm2ColCoRange((const Im2ColCoArgs *)arg);
}
#endif

struct QuantTransposeArgs {
    const float *in;
    int16_t *inHwc;
    int cin, cinPad, inHW, outHW;
    float invXScale;
    std::atomic<int> *nextP;
    int chunkSize;
};

static void RunQuantTransposeRange(const QuantTransposeArgs *args) {
    const int cin = args->cin, cinPad = args->cinPad;
    const int inHW = args->inHW, outHW = args->outHW;
    const float invXScale = args->invXScale;
    const float *inBase = args->in;
    int16_t *inHwc = args->inHwc;
    for (;;) {
        int s = args->nextP->fetch_add(args->chunkSize, std::memory_order_relaxed);
        if (s >= outHW) break;
        int e = std::min(outHW, s + args->chunkSize);
        int p = s;
        for (; p + 7 < e; p += 8) {
            int16_t *d0 = inHwc + (size_t)(p + 0) * cinPad;
            int16_t *d1 = d0 + cinPad;
            int16_t *d2 = d1 + cinPad;
            int16_t *d3 = d2 + cinPad;
            int16_t *d4 = d3 + cinPad;
            int16_t *d5 = d4 + cinPad;
            int16_t *d6 = d5 + cinPad;
            int16_t *d7 = d6 + cinPad;
            const float *srcRow = inBase + p;
            for (int c = 0; c < cin; ++c) {
                d0[c] = RoundToS16(srcRow[0] * invXScale);
                d1[c] = RoundToS16(srcRow[1] * invXScale);
                d2[c] = RoundToS16(srcRow[2] * invXScale);
                d3[c] = RoundToS16(srcRow[3] * invXScale);
                d4[c] = RoundToS16(srcRow[4] * invXScale);
                d5[c] = RoundToS16(srcRow[5] * invXScale);
                d6[c] = RoundToS16(srcRow[6] * invXScale);
                d7[c] = RoundToS16(srcRow[7] * invXScale);
                srcRow += inHW;
            }
            if (cinPad > cin) {
                for (int c = cin; c < cinPad; ++c) {
                    d0[c] = 0; d1[c] = 0; d2[c] = 0; d3[c] = 0;
                    d4[c] = 0; d5[c] = 0; d6[c] = 0; d7[c] = 0;
                }
            }
        }
        for (; p < e; ++p) {
            int16_t *dst = inHwc + (size_t)p * cinPad;
            const float *src = inBase + p;
            for (int c = 0; c < cin; ++c) {
                dst[c] = RoundToS16(src[(size_t)c * inHW] * invXScale);
            }
            for (int c = cin; c < cinPad; ++c) dst[c] = 0;
        }
    }
}

#ifdef __3DS__
static void QuantTransposeWorkerEntry(void *arg) {
    RunQuantTransposeRange((const QuantTransposeArgs *)arg);
}
#endif

struct MulQuantTransposeArgs {
    const float *in;
    const float *chScale;
    int16_t *inHwc;
    int cin, cinPad, inHW, outHW;
    std::atomic<int> *nextP;
    int chunkSize;
};

static void RunMulQuantTransposeRange(const MulQuantTransposeArgs *args) {
    const int cin = args->cin, cinPad = args->cinPad;
    const int inHW = args->inHW, outHW = args->outHW;
    const float *inBase = args->in;
    const float *chScale = args->chScale;
    int16_t *inHwc = args->inHwc;
    for (;;) {
        int s = args->nextP->fetch_add(args->chunkSize, std::memory_order_relaxed);
        if (s >= outHW) break;
        int e = std::min(outHW, s + args->chunkSize);
        int p = s;
        for (; p + 7 < e; p += 8) {
            int16_t *d0 = inHwc + (size_t)(p + 0) * cinPad;
            int16_t *d1 = d0 + cinPad;
            int16_t *d2 = d1 + cinPad;
            int16_t *d3 = d2 + cinPad;
            int16_t *d4 = d3 + cinPad;
            int16_t *d5 = d4 + cinPad;
            int16_t *d6 = d5 + cinPad;
            int16_t *d7 = d6 + cinPad;
            const float *srcRow = inBase + p;
            for (int c = 0; c < cin; ++c) {
                const float sc = chScale[c];
                d0[c] = RoundToS16(srcRow[0] * sc);
                d1[c] = RoundToS16(srcRow[1] * sc);
                d2[c] = RoundToS16(srcRow[2] * sc);
                d3[c] = RoundToS16(srcRow[3] * sc);
                d4[c] = RoundToS16(srcRow[4] * sc);
                d5[c] = RoundToS16(srcRow[5] * sc);
                d6[c] = RoundToS16(srcRow[6] * sc);
                d7[c] = RoundToS16(srcRow[7] * sc);
                srcRow += inHW;
            }
            if (cinPad > cin) {
                for (int c = cin; c < cinPad; ++c) {
                    d0[c] = 0; d1[c] = 0; d2[c] = 0; d3[c] = 0;
                    d4[c] = 0; d5[c] = 0; d6[c] = 0; d7[c] = 0;
                }
            }
        }
        for (; p < e; ++p) {
            int16_t *dst = inHwc + (size_t)p * cinPad;
            const float *src = inBase + p;
            for (int c = 0; c < cin; ++c) {
                dst[c] = RoundToS16(src[(size_t)c * inHW] * chScale[c]);
            }
            for (int c = cin; c < cinPad; ++c) dst[c] = 0;
        }
    }
}

#ifdef __3DS__
static void MulQuantTransposeWorkerEntry(void *arg) {
    RunMulQuantTransposeRange((const MulQuantTransposeArgs *)arg);
}
#endif

static float MaxAbsRange(const float *in, int s, int e) {
    float maxAbs = 1e-6f;
    int i = s;
    for (; i + 3 < e; i += 4) {
        float a0 = std::fabs(in[i + 0]);
        float a1 = std::fabs(in[i + 1]);
        float a2 = std::fabs(in[i + 2]);
        float a3 = std::fabs(in[i + 3]);
        if (a0 > maxAbs) maxAbs = a0;
        if (a1 > maxAbs) maxAbs = a1;
        if (a2 > maxAbs) maxAbs = a2;
        if (a3 > maxAbs) maxAbs = a3;
    }
    for (; i < e; ++i) {
        float a = std::fabs(in[i]);
        if (a > maxAbs) maxAbs = a;
    }
    return maxAbs;
}

#ifdef __3DS__
struct MaxAbsArgs {
    const float *in;
    int totalIn;
    int chunkSize;
    std::atomic<int> *nextIdx;
    std::atomic<int> *nextSlot;
    float *partialMax;
};

static void MaxAbsWorkerRun(const MaxAbsArgs *args) {
    int slot = args->nextSlot->fetch_add(1, std::memory_order_relaxed);
    float m = 1e-6f;
    for (;;) {
        int s = args->nextIdx->fetch_add(args->chunkSize, std::memory_order_relaxed);
        if (s >= args->totalIn) break;
        int e = std::min(args->totalIn, s + args->chunkSize);
        float lm = MaxAbsRange(args->in, s, e);
        if (lm > m) m = lm;
    }
    if (slot < 8) args->partialMax[slot] = m;
}

static void MaxAbsWorkerEntry(void *arg) {
    MaxAbsWorkerRun((const MaxAbsArgs *)arg);
}
#endif

static float ComputeMaxAbs(const float *in, int totalIn, int effCore) {
#ifdef __3DS__
    if (effCore >= 0 && totalIn >= 16384) {
        std::atomic<int> nextIdx(0);
        std::atomic<int> nextSlot(0);
        float partialMax[8] = {1e-6f, 1e-6f, 1e-6f, 1e-6f, 1e-6f, 1e-6f, 1e-6f, 1e-6f};
        MaxAbsArgs mArgs{in, totalIn, 8192, &nextIdx, &nextSlot, partialMax};
        int activeCores[3], activeCount = 0;
        DispatchHelpers(effCore, MaxAbsWorkerEntry, &mArgs, activeCores, activeCount);
        MaxAbsWorkerRun(&mArgs);
        if (activeCount > 0) WaitHelpers(activeCores, activeCount);
        int nSlots = std::min(8, nextSlot.load(std::memory_order_relaxed));
        float maxAbs = 1e-6f;
        for (int s = 0; s < nSlots; ++s) {
            if (partialMax[s] > maxAbs) maxAbs = partialMax[s];
        }
        return maxAbs;
    }
#endif
    (void)effCore;
    return MaxAbsRange(in, 0, totalIn);
}

#ifdef __3DS__
struct MulMaxAbsArgs {
    const float *slot1;
    const float *seScales;
    int C, hw, chunkSize;
    std::atomic<int> *nextC;
    std::atomic<int> *nextSlot;
    float *partialMax;
};

static void MulMaxAbsWorkerRun(const MulMaxAbsArgs *args) {
    int slot = args->nextSlot->fetch_add(1, std::memory_order_relaxed);
    float m = 1e-6f;
    for (;;) {
        int s = args->nextC->fetch_add(args->chunkSize, std::memory_order_relaxed);
        if (s >= args->C) break;
        int e = std::min(args->C, s + args->chunkSize);
        for (int c = s; c < e; ++c) {
            float cm = MaxAbsRange(args->slot1 + (size_t)c * args->hw, 0, args->hw) * std::fabs(args->seScales[c]);
            if (cm > m) m = cm;
        }
    }
    if (slot < 8) args->partialMax[slot] = m;
}

static void MulMaxAbsWorkerEntry(void *arg) {
    MulMaxAbsWorkerRun((const MulMaxAbsArgs *)arg);
}
#endif

static float ComputeMulMaxAbs(const float *slot1, const float *seScales, int C, int hw, int effCore) {
#ifdef __3DS__
    if (effCore >= 0 && C >= 32 && C * hw >= 8192) {
        std::atomic<int> nextC(0);
        std::atomic<int> nextSlot(0);
        float partialMax[8] = {1e-6f, 1e-6f, 1e-6f, 1e-6f, 1e-6f, 1e-6f, 1e-6f, 1e-6f};
        MulMaxAbsArgs mArgs{slot1, seScales, C, hw, 16, &nextC, &nextSlot, partialMax};
        int activeCores[3], activeCount = 0;
        DispatchHelpers(effCore, MulMaxAbsWorkerEntry, &mArgs, activeCores, activeCount);
        MulMaxAbsWorkerRun(&mArgs);
        if (activeCount > 0) WaitHelpers(activeCores, activeCount);
        int nSlots = std::min(8, nextSlot.load(std::memory_order_relaxed));
        float maxAbs = 1e-6f;
        for (int s = 0; s < nSlots; ++s) {
            if (partialMax[s] > maxAbs) maxAbs = partialMax[s];
        }
        return maxAbs;
    }
#endif
    (void)effCore;
    float maxAbs = 1e-6f;
    for (int c = 0; c < C; ++c) {
        float cm = MaxAbsRange(slot1 + (size_t)c * hw, 0, hw) * std::fabs(seScales[c]);
        if (cm > maxAbs) maxAbs = cm;
    }
    return maxAbs;
}

struct QuantFlatArgs {
    const float *in;
    int16_t *out;
    int totalIn;
    int chunkSize;
    float invXScale;
    std::atomic<int> *nextIdx;
};

static void QuantFlatRange(const QuantFlatArgs *args) {
    const float invXScale = args->invXScale;
    const float *in = args->in;
    int16_t *out = args->out;
    for (;;) {
        int s = args->nextIdx->fetch_add(args->chunkSize, std::memory_order_relaxed);
        if (s >= args->totalIn) break;
        int e = std::min(args->totalIn, s + args->chunkSize);
        int i = s;
        for (; i + 3 < e; i += 4) {
            out[i + 0] = RoundToS16(in[i + 0] * invXScale);
            out[i + 1] = RoundToS16(in[i + 1] * invXScale);
            out[i + 2] = RoundToS16(in[i + 2] * invXScale);
            out[i + 3] = RoundToS16(in[i + 3] * invXScale);
        }
        for (; i < e; ++i) {
            out[i] = RoundToS16(in[i] * invXScale);
        }
    }
}

#ifdef __3DS__
static void QuantFlatWorkerEntry(void *arg) {
    QuantFlatRange((const QuantFlatArgs *)arg);
}
#endif

static void QuantizeFlatToS16(const float *in, int16_t *out, int totalIn, float invXScale, int effCore) {
    std::atomic<int> nextIdx(0);
    QuantFlatArgs qArgs{in, out, totalIn, 8192, invXScale, &nextIdx};
#ifdef __3DS__
    if (effCore >= 0 && totalIn >= 16384) {
        int activeCores[3], activeCount = 0;
        DispatchHelpers(effCore, QuantFlatWorkerEntry, &qArgs, activeCores, activeCount);
        QuantFlatRange(&qArgs);
        if (activeCount > 0) WaitHelpers(activeCores, activeCount);
        return;
    }
#endif
    (void)effCore;
    QuantFlatRange(&qArgs);
}

static void RunConv2D(
    const float *in, int cin, int hin, int win,
    float *out, int cout, int hout, int wout,
    int kh, int kw, int sh, int sw, int ph, int pw, int groups,
    const float *scales, const float *bias, const int8_t *wq,
    uint8_t act, float postScale, float postBias, bool allowDualCore = true, int helperCore = 2,
    const int16_t *wq16 = nullptr, std::vector<int16_t> *scratchInHwc = nullptr,
    OcrProfile *prof = nullptr, int opIdx = -1,
    TensorLayout inLayout = LAY_F32_CHW, float inScale = 0.0f,
    TensorLayout outLayout = LAY_F32_CHW, float outScale = 0.0f,
    const int16_t *inS16 = nullptr, int16_t *outS16 = nullptr)
{
    const bool hasPost = (postScale != 1.0f || postBias != 0.0f);
    const int outHW = hout * wout;
    const int inHW = hin * win;
    const int effCore = allowDualCore ? helperCore : -1;
    const float invOutScale = (outLayout != LAY_F32_CHW && outScale > 0.0f && outS16 != nullptr) ? (1.0f / outScale) : 0.0f;

    if (groups == 1 && kh == 1 && kw == 1 && sh == 1 && sw == 1 && ph == 0 && pw == 0) {
        // Fast contiguous HWC INT16 + 2x2 SMLAD 1x1 pointwise convolution (multi-core on N3DS)
        uint64_t tQuant0 = prof ? NowTicks() : 0;
        const int cinPad = (cin + 1) & ~1;
        const int nPairs = cinPad / 2;
        float xScale = inScale;
        const int16_t *inHwcPtr = nullptr;
        std::vector<int16_t> localInHwc;

        if (inLayout == LAY_S16_HWC && inS16 != nullptr) {
            inHwcPtr = inS16;
            if (prof) prof->conv1x1QuantTicks += NowTicks() - tQuant0;
        } else {
            float maxAbs = ComputeMaxAbs(in, cin * inHW, effCore);
            xScale = maxAbs / 2048.0f;
            const float invXScale = 2048.0f / maxAbs;
            uint64_t tMaxEnd = prof ? NowTicks() : 0;
            if (prof) prof->quantMaxAbsTicks += tMaxEnd - tQuant0;

            std::vector<int16_t> &inHwc = scratchInHwc ? *scratchInHwc : localInHwc;
            const size_t needInHwc = (size_t)outHW * (size_t)cinPad;
            if (inHwc.size() < needInHwc) inHwc.resize(needInHwc);

            std::atomic<int> nextP(0);
            QuantTransposeArgs qtArgs{in, inHwc.data(), cin, cinPad, inHW, outHW, invXScale, &nextP, 64};
            uint64_t tQt0 = prof ? NowTicks() : 0;
            uint64_t tQtWait = 0;
#ifdef __3DS__
            if (effCore >= 0 && outHW >= 128 && (size_t)cin * (size_t)outHW >= 8192) {
                int qtCores[3], qtCount = 0;
                DispatchHelpers(effCore, QuantTransposeWorkerEntry, &qtArgs, qtCores, qtCount);
                RunQuantTransposeRange(&qtArgs);
                if (qtCount > 0) {
                    uint64_t tw0 = prof ? NowTicks() : 0;
                    WaitHelpers(qtCores, qtCount);
                    if (prof) tQtWait = NowTicks() - tw0;
                }
            } else {
                RunQuantTransposeRange(&qtArgs);
            }
#else
            RunQuantTransposeRange(&qtArgs);
#endif
            uint64_t tQuantEnd = prof ? NowTicks() : 0;
            if (prof) {
                prof->conv1x1QuantTicks += tQuantEnd - tQuant0;
                prof->quantTransposeTicks += (tQuantEnd - tQt0) - tQtWait;
                prof->quantWaitTicks += tQtWait;
            }
            inHwcPtr = inHwc.data();
        }
        uint64_t qOpTicks = prof ? (NowTicks() - tQuant0) : 0;

        uint64_t tGemm0 = prof ? NowTicks() : 0;
        uint64_t gWaitTicks = 0;
        Conv1x1RangeArgs mainArgs{
            inHwcPtr, (outLayout == LAY_F32_CHW) ? out : nullptr,
            (outLayout == LAY_S16_CHW) ? outS16 : nullptr,
            scales, bias, wq, wq16,
            cin, cinPad, nPairs, outHW, 0, cout,
            xScale, invOutScale, postScale, postBias, act, hasPost,
            nullptr, 0, cout
        };
        bool ranMt = false;
#ifdef __3DS__
        if (effCore >= 0 && cout >= 32 &&
            ((size_t)cout * (size_t)outHW >= 4096 || (size_t)cin * (size_t)cout * (size_t)outHW >= 32768)) {
            std::atomic<int> nextCo(0);
            mainArgs.nextCo = &nextCo;
            mainArgs.chunkSize = (cout >= 256) ? 8 : (cout >= 64) ? 4 : 2;
            int activeCores[3], activeCount = 0;
            DispatchHelpers(effCore, Conv1x1WorkerEntry, &mainArgs, activeCores, activeCount);
            if (activeCount > 0) {
                ranMt = true;
                RunConv1x1Range(&mainArgs, prof ? &prof->gemmUnpackTicks : nullptr);
                uint64_t tw0 = prof ? NowTicks() : 0;
                WaitHelpers(activeCores, activeCount);
                if (prof) gWaitTicks = NowTicks() - tw0;
            } else {
                mainArgs.nextCo = nullptr;
            }
        }
#endif
        if (!ranMt) {
            RunConv1x1Range(&mainArgs, prof ? &prof->gemmUnpackTicks : nullptr);
        }
        if (prof) {
            uint64_t gOpTicks = NowTicks() - tGemm0;
            prof->conv1x1GemmTicks += gOpTicks;
            prof->gemmWaitTicks += gWaitTicks;
            if (ranMt) {
                prof->gemmMultiThreadCalls += 1;
            } else {
                prof->gemmSingleThreadCalls += 1;
                prof->gemmSingleThreadTicks += gOpTicks;
            }
            if (opIdx >= 0 && opIdx < 64) {
                Conv1x1OpStat &st = prof->conv1x1Ops[opIdx];
                st.cin = (uint16_t)cin;
                st.cout = (uint16_t)cout;
                st.act = act;
                st.isS16 = (wq16 != nullptr) ? 1 : 0;
                st.calls += 1;
                st.sumOutHW += (uint64_t)outHW;
                st.totalMacs += (uint64_t)cin * (uint64_t)cout * (uint64_t)outHW;
                st.quantTicks += qOpTicks;
                st.gemmTicks += gOpTicks;
                st.waitTicks += gWaitTicks;
            }
        }
        return;
    }

    if (groups == cin && groups == cout) {
        // Integer Depthwise convolution (3x3 or 5x5) with branchless interior & dual-core split
        uint64_t tDw0 = prof ? NowTicks() : 0;
        const int coutPad = (cout + 1) & ~1;
        DepthwiseRangeArgs mainArgs{
            (inLayout == LAY_F32_CHW) ? in : nullptr,
            (inLayout == LAY_S16_CHW) ? inS16 : nullptr,
            (outLayout == LAY_F32_CHW) ? out : nullptr,
            (outLayout == LAY_S16_HWC) ? outS16 : nullptr,
            scales, bias, wq, wq16,
            hin, win, hout, wout, inHW, outHW, coutPad,
            kh, kw, sh, sw, ph, pw,
            0, cout, inScale, invOutScale, postScale, postBias, act, hasPost,
            nullptr, 0, cout
        };
#ifdef __3DS__
        if (effCore >= 0 && cout >= 16 && (size_t)cout * (size_t)outHW >= 4096) {
            std::atomic<int> nextCo(0);
            mainArgs.nextCo = &nextCo;
            mainArgs.chunkSize = (cout >= 32) ? 8 : 4;
            int activeCores[3], activeCount = 0;
            DispatchHelpers(effCore, DepthwiseWorkerEntry, &mainArgs, activeCores, activeCount);
            if (activeCount > 0) {
                RunDepthwiseRange(&mainArgs);
                WaitHelpers(activeCores, activeCount);
                if (prof) prof->convDwTicks += NowTicks() - tDw0;
                return;
            }
            mainArgs.nextCo = nullptr;
        }
#endif
        RunDepthwiseRange(&mainArgs);
        if (prof) prof->convDwTicks += NowTicks() - tDw0;
        return;
    }

    if (groups == 1) {
        // Row-tiled im2col + SMLAD 2x2 for non-1x1 standard Convs
        uint64_t tIm0 = prof ? NowTicks() : 0;
        const int kDim = cin * kh * kw;
        const int kPad = (kDim + 1) & ~1;
        const int nPairs = kPad / 2;
        const int totalIn = cin * inHW;

        float maxAbs = ComputeMaxAbs(in, totalIn, effCore);
        const float xScale = maxAbs / 2048.0f;
        const float invXScale = 2048.0f / maxAbs;
        std::vector<int16_t> inQ((size_t)totalIn);
        QuantizeFlatToS16(in, inQ.data(), totalIn, invXScale, effCore);

#ifdef __3DS__
        if (effCore >= 0 && hout < 4 && cout >= 8 && (size_t)cout * (size_t)outHW * (size_t)kDim >= 65536) {
            std::vector<int16_t> fullCol((size_t)outHW * kPad, 0);
            const int kPerC = kh * kw;
            for (int oy = 0; oy < hout; ++oy) {
                const int iy0 = oy * sh - ph;
                for (int ci = 0; ci < cin; ++ci) {
                    const int16_t *inC = inQ.data() + (size_t)ci * inHW;
                    const int dstBase = ci * kPerC;
                    for (int ky = 0; ky < kh; ++ky) {
                        const int iy = iy0 + ky;
                        if (iy < 0 || iy >= hin) continue;
                        const int16_t *inRow = inC + iy * win;
                        const int dstOff = dstBase + ky * kw;
                        for (int ox = 0; ox < wout; ++ox) {
                            const int ix0 = ox * sw - pw;
                            int16_t *dst = fullCol.data() + ((size_t)oy * wout + ox) * kPad + dstOff;
                            for (int kx = 0; kx < kw; ++kx) {
                                const int ix = ix0 + kx;
                                if (ix >= 0 && ix < win) dst[kx] = inRow[ix];
                            }
                        }
                    }
                }
            }
            std::atomic<int> nextCo(0);
            Im2ColCoArgs coArgs{
                fullCol.data(),
                (outLayout == LAY_F32_CHW) ? out : nullptr,
                (outLayout == LAY_S16_CHW) ? outS16 : nullptr,
                scales, bias, wq, wq16,
                cout, outHW, kDim, kPad, nPairs,
                xScale, invOutScale, postScale, postBias, act, hasPost,
                &nextCo, 4
            };
            int activeCores[3], activeCount = 0;
            DispatchHelpers(effCore, Im2ColCoWorkerEntry, &coArgs, activeCores, activeCount);
            if (activeCount > 0) {
                RunIm2ColCoRange(&coArgs);
                WaitHelpers(activeCores, activeCount);
                if (prof) prof->convIm2ColTicks += NowTicks() - tIm0;
                return;
            }
        }
#endif

        std::vector<int16_t> wAll((size_t)cout * kPad, 0);
        for (int co = 0; co < cout; ++co) {
            int16_t *dstW = wAll.data() + (size_t)co * kPad;
            if (wq16) {
                std::memcpy(dstW, wq16 + (size_t)co * kDim, (size_t)kDim * sizeof(int16_t));
            } else {
                const int8_t *ws = wq + (size_t)co * kDim;
                UnpackS8ToS16(ws, dstW, kDim);
            }
        }

        Im2ColConvArgs mainArgs{
            inQ.data(),
            (outLayout == LAY_F32_CHW) ? out : nullptr,
            (outLayout == LAY_S16_CHW) ? outS16 : nullptr,
            scales, bias, wAll.data(),
            cin, hin, win, cout, hout, wout, inHW, outHW,
            kh, kw, sh, sw, ph, pw,
            kDim, kPad, nPairs,
            0, hout, xScale, invOutScale, postScale, postBias, act, hasPost,
            nullptr, 0, hout
        };
#ifdef __3DS__
        if (effCore >= 0 && hout >= 4 && (size_t)cout * (size_t)outHW * (size_t)kDim >= 65536) {
            std::atomic<int> nextOy(0);
            mainArgs.nextOy = &nextOy;
            mainArgs.chunkSize = 2;
            int activeCores[3], activeCount = 0;
            DispatchHelpers(effCore, Im2ColConvWorkerEntry, &mainArgs, activeCores, activeCount);
            if (activeCount > 0) {
                RunIm2ColConvRange(&mainArgs);
                WaitHelpers(activeCores, activeCount);
                if (prof) prof->convIm2ColTicks += NowTicks() - tIm0;
                return;
            }
            mainArgs.nextOy = nullptr;
        }
#endif
        RunIm2ColConvRange(&mainArgs);
        if (prof) prof->convIm2ColTicks += NowTicks() - tIm0;
        return;
    }

    // General grouped / standard conv (e.g. stem 3x3 or 1x3 head convs)
    uint64_t tGen0 = prof ? NowTicks() : 0;
    const int cinPerG = cin / groups;
    const int kPerCo = cinPerG * kh * kw;
    for (int co = 0; co < cout; ++co) {
        const int g = (groups == 1) ? 0 : (co * groups / cout);
        const int ciStart = g * cinPerG;
        const int8_t *wCo = wq ? (wq + (size_t)co * kPerCo) : nullptr;
        const int16_t *wCo16 = wq16 ? (wq16 + (size_t)co * kPerCo) : nullptr;
        const float sc = scales[co];
        const float bv = bias[co];
        float *outC = out + (size_t)co * outHW;

        for (int oy = 0; oy < hout; ++oy) {
            const int iy0 = oy * sh - ph;
            for (int ox = 0; ox < wout; ++ox) {
                const int ix0 = ox * sw - pw;
                float acc = 0.0f;
                for (int ci = 0; ci < cinPerG; ++ci) {
                    const float *inC = in + (size_t)(ciStart + ci) * inHW;
                    for (int ky = 0; ky < kh; ++ky) {
                        const int iy = iy0 + ky;
                        if (iy < 0 || iy >= hin) continue;
                        const float *inRow = inC + iy * win;
                        const size_t wOff = (size_t)ci * kh * kw + (size_t)ky * kw;
                        for (int kx = 0; kx < kw; ++kx) {
                            const int ix = ix0 + kx;
                            if (ix < 0 || ix >= win) continue;
                            float wv = wCo16 ? (float)wCo16[wOff + kx] : (float)wCo[wOff + kx];
                            acc += inRow[ix] * wv;
                        }
                    }
                }
                float v = acc * sc + bv;
                v = ApplyAct(v, act);
                if (hasPost) v = v * postScale + postBias;
                outC[oy * wout + ox] = v;
            }
        }
    }
    if (prof) prof->convIm2ColTicks += NowTicks() - tGen0;
}

static void RunLayerNorm(const float *in, float *out, int T, int dim,
                         const float *gamma, const float *beta, float eps)
{
    const float invDim = 1.0f / (float)dim;
    for (int t = 0; t < T; ++t) {
        const float *x = in + (size_t)t * dim;
        float *y = out + (size_t)t * dim;
        float sum = 0.0f;
        for (int i = 0; i < dim; ++i) sum += x[i];
        const float mean = sum * invDim;
        float vsum = 0.0f;
        for (int i = 0; i < dim; ++i) {
            float d = x[i] - mean;
            vsum += d * d;
        }
        const float invStd = 1.0f / std::sqrt(vsum * invDim + eps);
        for (int i = 0; i < dim; ++i) {
            y[i] = (x[i] - mean) * invStd * gamma[i] + beta[i];
        }
    }
}

struct LinearRangeArgs {
    const int16_t *in16;
    float *out;
    const float *scales;
    const float *bias;
    const int8_t *wq;
    int T, inDim, outDim, inPad, nPairs;
    float xScale;
    std::atomic<int> *nextO;
    int chunkSize;
};

static void RunLinearRange(const LinearRangeArgs *args) {
    const int T = args->T, inDim = args->inDim, outDim = args->outDim;
    const int inPad = args->inPad, nPairs = args->nPairs;
    const float xScale = args->xScale;
    std::vector<int16_t> w0(inPad, 0), w1(inPad, 0);

    for (;;) {
        int s = args->nextO->fetch_add(args->chunkSize, std::memory_order_relaxed);
        if (s >= outDim) break;
        int e = std::min(outDim, s + args->chunkSize);
        int o = s;
        for (; o + 1 < e; o += 2) {
            const int8_t *ws0 = args->wq + (size_t)o * inDim;
            const int8_t *ws1 = args->wq + (size_t)(o + 1) * inDim;
            UnpackS8ToS16(ws0, w0.data(), inDim);
            UnpackS8ToS16(ws1, w1.data(), inDim);
            const float sc0 = args->scales[o] * xScale, bv0 = args->bias[o];
            const float sc1 = args->scales[o + 1] * xScale, bv1 = args->bias[o + 1];
            int t = 0;
            for (; t + 1 < T; t += 2) {
                int32_t c00, c01, c10, c11;
                DotS16_2x2(args->in16 + (size_t)t * inPad,
                           args->in16 + (size_t)(t + 1) * inPad,
                           w0.data(), w1.data(), nPairs, c00, c01, c10, c11);
                args->out[(size_t)t * outDim + o]           = (float)c00 * sc0 + bv0;
                args->out[(size_t)(t + 1) * outDim + o]     = (float)c01 * sc0 + bv0;
                args->out[(size_t)t * outDim + o + 1]       = (float)c10 * sc1 + bv1;
                args->out[(size_t)(t + 1) * outDim + o + 1] = (float)c11 * sc1 + bv1;
            }
            for (; t < T; ++t) {
                int32_t a0 = DotS16(args->in16 + (size_t)t * inPad, w0.data(), nPairs);
                int32_t a1 = DotS16(args->in16 + (size_t)t * inPad, w1.data(), nPairs);
                args->out[(size_t)t * outDim + o]     = (float)a0 * sc0 + bv0;
                args->out[(size_t)t * outDim + o + 1] = (float)a1 * sc1 + bv1;
            }
        }
        for (; o < e; ++o) {
            const int8_t *ws0 = args->wq + (size_t)o * inDim;
            UnpackS8ToS16(ws0, w0.data(), inDim);
            const float sc0 = args->scales[o] * xScale, bv0 = args->bias[o];
            for (int t = 0; t < T; ++t) {
                int32_t acc = DotS16(args->in16 + (size_t)t * inPad, w0.data(), nPairs);
                args->out[(size_t)t * outDim + o] = (float)acc * sc0 + bv0;
            }
        }
    }
}

#ifdef __3DS__
static void LinearWorkerEntry(void *arg) {
    RunLinearRange((const LinearRangeArgs *)arg);
}
#endif

static void RunLinear(const float *in, float *out, int T, int inDim, int outDim,
                      const float *scales, const float *bias, const int8_t *wq,
                      bool allowDualCore = false, int helperCore = 2)
{
    const int inPad = (inDim + 1) & ~1;
    const int nPairs = inPad / 2;
    float maxAbs = 1e-6f;
    for (int i = 0; i < T * inDim; ++i) {
        float v = in[i];
        if (v < 0.0f) v = -v;
        if (v > maxAbs) maxAbs = v;
    }
    const float xScale = maxAbs / 2048.0f;
    const float invXScale = 2048.0f / maxAbs;

    std::vector<int16_t> in16((size_t)T * inPad, 0);
    for (int t = 0; t < T; ++t) {
        const float *src = in + (size_t)t * inDim;
        int16_t *dst = in16.data() + (size_t)t * inPad;
        for (int i = 0; i < inDim; ++i) {
            float q = src[i] * invXScale;
            int32_t qi = (int32_t)(q >= 0.0f ? (q + 0.5f) : (q - 0.5f));
            if (qi > 32767) qi = 32767;
            else if (qi < -32767) qi = -32767;
            dst[i] = (int16_t)qi;
        }
    }

    std::atomic<int> nextO(0);
    LinearRangeArgs args{in16.data(), out, scales, bias, wq, T, inDim, outDim, inPad, nPairs, xScale, &nextO, 16};
#ifdef __3DS__
    const int effCore = allowDualCore ? helperCore : -1;
    if (effCore >= 0 && outDim >= 64 && T >= 8) {
        int activeCores[3], activeCount = 0;
        DispatchHelpers(effCore, LinearWorkerEntry, &args, activeCores, activeCount);
        RunLinearRange(&args);
        if (activeCount > 0) WaitHelpers(activeCores, activeCount);
        return;
    }
#endif
    RunLinearRange(&args);
}

static uint32_t Utf8FirstCodepoint(const std::string &s) {
    if (s.empty()) return 0;
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && s.size() >= 2)
        return ((uint32_t)(c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && s.size() >= 3)
        return ((uint32_t)(c & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F);
    if ((c & 0xF8) == 0xF0 && s.size() >= 4)
        return ((uint32_t)(c & 0x07) << 18) | (((uint8_t)s[1] & 0x3F) << 12) | (((uint8_t)s[2] & 0x3F) << 6) | ((uint8_t)s[3] & 0x3F);
    return '?';
}

} // namespace

int GetActiveCoreCount() {
#ifdef __3DS__
    return 2 + (ProbeCoreSchedulable(3) ? 1 : 0) + (ProbeCoreSchedulable(1) ? 1 : 0);
#else
    return 1;
#endif
}

void GetAndResetHelperProfile(uint64_t outBusyTicks[4], uint32_t &outDispatches) {
    outDispatches = 0;
    for (int i = 0; i < 4; ++i) outBusyTicks[i] = 0;
#ifdef __3DS__
    for (int i = 0; i < 4; ++i) {
        PersistentHelper &h = GetPersistentHelper(i);
        outBusyTicks[i] = h.busyTicks.exchange(0, std::memory_order_relaxed);
        outDispatches += h.dispatches.exchange(0, std::memory_order_relaxed);
    }
#endif
}

bool CheckModelFile(const char *modelPath, std::string *outInfo) {
    OfflineFile f;
    if (!f.OpenRead(modelPath)) {
        if (outInfo) *outInfo = "Model file not found";
        return false;
    }
    uint8_t hdr[20];
    uint64_t fsz = f.GetSize();
    bool okRead = f.ReadExact(hdr, sizeof(hdr));
    f.Close();
    if (!okRead || std::memcmp(hdr, "PPOCRV4I", 8) != 0) {
        if (outInfo) *outInfo = "Invalid model header";
        return false;
    }
    uint32_t ver = ReadU32(hdr + 8);
    uint32_t ops = ReadU32(hdr + 12);
    uint32_t voc = ReadU32(hdr + 16);
    if (outInfo) {
        *outInfo = "v" + std::to_string(ver) + ", " + std::to_string(ops) + " ops, "
                 + std::to_string(voc) + " chars (" + std::to_string(fsz / 1024) + " KB)";
    }
    return (ver == 1 || ver == 2) && ops > 0 && voc > 0;
}

bool LoadModel(const char *modelPath, LoadedModel &outModel, std::string &err)
{
    outModel = LoadedModel();
#ifdef __3DS__
    osSetSpeedupEnable(true);
#endif
    OfflineFile f;
    if (!f.OpenRead(modelPath)) {
        err = "Missing " + std::string(modelPath);
        return false;
    }
    uint64_t fsz = f.GetSize();
    if (fsz <= 20 || fsz > 6 * 1024 * 1024) {
        f.Close();
        err = "Invalid OCR model size";
        return false;
    }

    outModel.modelBuf.resize((size_t)fsz);
    if (!f.ReadExact(outModel.modelBuf.data(), (size_t)fsz)) {
        f.Close();
        outModel.modelBuf.clear();
        err = "Failed to read OCR model";
        return false;
    }
    f.Close();

    const uint8_t *buf = outModel.modelBuf.data();
    const size_t len = outModel.modelBuf.size();
    if (std::memcmp(buf, "PPOCRV4I", 8) != 0) {
        outModel.modelBuf.clear();
        err = "Bad OCR model magic";
        return false;
    }
    size_t pos = 8;
    uint32_t ver = ReadU32(buf + pos); pos += 4;
    outModel.numOps = ReadU32(buf + pos); pos += 4;
    uint32_t vocabSz = ReadU32(buf + pos); pos += 4;
    if ((ver != 1 && ver != 2) || vocabSz == 0 || vocabSz > 16384) {
        outModel.modelBuf.clear();
        err = "Unsupported OCR model version";
        return false;
    }

    outModel.vocab.resize(vocabSz);
    for (uint32_t i = 0; i < vocabSz; ++i) {
        if (pos >= len) { outModel.modelBuf.clear(); err = "Truncated vocab"; return false; }
        uint8_t slen = buf[pos++];
        if (pos + slen > len) { outModel.modelBuf.clear(); err = "Truncated vocab str"; return false; }
        outModel.vocab[i].assign((const char *)(buf + pos), slen);
        pos += slen;
    }
    if (ver == 2) {
        if (pos + (size_t)outModel.numOps * 4 > len) {
            outModel.modelBuf.clear();
            err = "Truncated activation scales";
            return false;
        }
        outModel.actOutScales.resize(outModel.numOps);
        std::memcpy(outModel.actOutScales.data(), buf + pos, (size_t)outModel.numOps * 4);
        pos += (size_t)outModel.numOps * 4;
    }
    outModel.opsOffset = pos;
    outModel.loaded = true;
    return true;
}

struct CtcRangeArgs {
    const int16_t *x16;
    const float *sc;
    const float *bv;
    const int8_t *wq;
    float *bestLogits;
    uint32_t *bestIds;
    int T, id, inPad, nPairs;
    uint32_t oStart, oEnd;
    float xScale;
    std::atomic<uint32_t> *nextO;
    uint32_t chunkSize;
    uint32_t totalOd;
};

static void RunCtcRange(const CtcRangeArgs *args) {
    const int T = args->T, id = args->id, nPairs = args->nPairs;
    const int inPad = args->inPad;
    const float xScale = args->xScale;
    int16_t w0[256] = {0}, w1[256] = {0};

    auto runSlice = [&](uint32_t sO, uint32_t eO) {
        uint32_t o = sO;
        for (; o + 1 < eO; o += 2) {
            const int8_t *ws0 = args->wq + (size_t)o * id;
            const int8_t *ws1 = args->wq + (size_t)(o + 1) * id;
            UnpackS8ToS16(ws0, w0, id);
            UnpackS8ToS16(ws1, w1, id);
            const float s0 = args->sc[o] * xScale, b0 = args->bv[o];
            const float s1 = args->sc[o + 1] * xScale, b1 = args->bv[o + 1];

            int t = 0;
            for (; t + 1 < T; t += 2) {
                int32_t c00, c01, c10, c11;
                DotS16_2x2(args->x16 + (size_t)t * inPad,
                           args->x16 + (size_t)(t + 1) * inPad,
                           w0, w1, nPairs, c00, c01, c10, c11);
                float l00 = (float)c00 * s0 + b0;
                float l01 = (float)c01 * s0 + b0;
                float l10 = (float)c10 * s1 + b1;
                float l11 = (float)c11 * s1 + b1;
                if (l00 > args->bestLogits[t])     { args->bestLogits[t] = l00;     args->bestIds[t] = o; }
                if (l01 > args->bestLogits[t + 1]) { args->bestLogits[t + 1] = l01; args->bestIds[t + 1] = o; }
                if (l10 > args->bestLogits[t])     { args->bestLogits[t] = l10;     args->bestIds[t] = o + 1; }
                if (l11 > args->bestLogits[t + 1]) { args->bestLogits[t + 1] = l11; args->bestIds[t + 1] = o + 1; }
            }
            for (; t < T; ++t) {
                int32_t a0 = DotS16(args->x16 + (size_t)t * inPad, w0, nPairs);
                int32_t a1 = DotS16(args->x16 + (size_t)t * inPad, w1, nPairs);
                float l0 = (float)a0 * s0 + b0;
                float l1 = (float)a1 * s1 + b1;
                if (l0 > args->bestLogits[t]) { args->bestLogits[t] = l0; args->bestIds[t] = o; }
                if (l1 > args->bestLogits[t]) { args->bestLogits[t] = l1; args->bestIds[t] = o + 1; }
            }
        }
        for (; o < eO; ++o) {
            const int8_t *ws0 = args->wq + (size_t)o * id;
            UnpackS8ToS16(ws0, w0, id);
            const float s0 = args->sc[o] * xScale, b0 = args->bv[o];
            for (int t = 0; t < T; ++t) {
                int32_t a0 = DotS16(args->x16 + (size_t)t * inPad, w0, nPairs);
                float l0 = (float)a0 * s0 + b0;
                if (l0 > args->bestLogits[t]) { args->bestLogits[t] = l0; args->bestIds[t] = o; }
            }
        }
    };

    if (args->nextO) {
        for (;;) {
            uint32_t s = args->nextO->fetch_add(args->chunkSize, std::memory_order_relaxed);
            if (s >= args->totalOd) break;
            uint32_t e = std::min(args->totalOd, s + args->chunkSize);
            runSlice(s, e);
        }
    } else {
        runSlice(args->oStart, args->oEnd);
    }
}

#ifdef __3DS__
static void CtcWorkerEntry(void *arg) {
    RunCtcRange((const CtcRangeArgs *)arg);
}
#endif

bool RunPreprocessed(const float *nchw, int width, const LoadedModel &model,
                     OcrLine &outLine, std::string &err,
                     bool allowDualCore, const std::atomic<bool> *abortFlag, int helperCore,
                     RecScratch *scratch, OcrProfile *prof, float *calMaxAbsOut)
{
    outLine.text.clear();
    outLine.chars.clear();
    if (!model.loaded || model.modelBuf.empty()) {
        err = "OCR model not loaded";
        return false;
    }
    if (!nchw || width <= 0 || width > 320 || (width % 8) != 0) {
        err = "Invalid preprocessed OCR tensor dimensions";
        return false;
    }
#ifdef __3DS__
    osSetSpeedupEnable(true);
#endif

    const uint8_t *buf = model.modelBuf.data();
    const size_t len = model.modelBuf.size();
    size_t pos = model.opsOffset;
    const uint32_t numOps = model.numOps;
    const std::vector<std::string> &vocab = model.vocab;

    // Peak activation: in ver=2 (compactS16), saveSlot==0 ops need <= 384*W floats;
    // saveSlot==1 ops write directly to slot1. Free before grow to avoid double-allocation.
    const bool compactS16 = (!calMaxAbsOut && !model.actOutScales.empty());
    const size_t maxActFloats = (compactS16 ? (size_t)400 : (size_t)768) * (size_t)width + 1024;
    RecScratch localScratch;
    RecScratch &sc = scratch ? *scratch : localScratch;
    if (sc.bufA.size() < maxActFloats) { std::vector<float>().swap(sc.bufA); sc.bufA.resize(maxActFloats); }
    if (sc.bufB.size() < maxActFloats) { std::vector<float>().swap(sc.bufB); sc.bufB.resize(maxActFloats); }
    std::vector<float> &slot1 = sc.slot1;
    std::vector<float> &slot2 = sc.slot2;

    float *curPtr = sc.bufA.data();
    float *nxtPtr = sc.bufB.data();
    TensorLayout curLayout = LAY_F32_CHW;
    float curScale = 0.0f;
    int curC = 3, curH = 48, curW = width;
    std::memcpy(curPtr, nchw, (size_t)3 * 48 * (size_t)width * sizeof(float));

    int slot1C = 0, slot1H = 0, slot1W = 0;
    int slot2C = 0, slot2H = 0, slot2W = 0;
    int seqT = 0, seqC = 0;

    for (uint32_t opIdx = 0; opIdx < numOps; ++opIdx) {
        if (abortFlag && abortFlag->load(std::memory_order_relaxed)) {
            err = "aborted";
            return false;
        }
        if (pos >= len) { err = "Truncated op stream"; return false; }
        uint64_t opT0 = prof ? NowTicks() : 0;
        uint8_t opType = buf[pos++];

        if (opType == OP_CONV || opType == OP_CONV_S16) {
            if (pos + 24 > len) { err = "Truncated conv"; return false; }
            uint16_t cout = ReadU16(buf + pos);
            uint16_t cin  = ReadU16(buf + pos + 2);
            uint16_t kh   = ReadU16(buf + pos + 4);
            uint16_t kw   = ReadU16(buf + pos + 6);
            uint8_t sh    = buf[pos + 8];
            uint8_t sw    = buf[pos + 9];
            uint8_t ph    = buf[pos + 10];
            uint8_t pw    = buf[pos + 11];
            uint16_t grp  = ReadU16(buf + pos + 12);
            uint8_t act   = buf[pos + 14];
            uint8_t saveSlot = buf[pos + 15];
            float postScale = ReadF32(buf + pos + 16);
            float postBias  = ReadF32(buf + pos + 20);
            pos += 24;

            std::vector<float> &scalesVec = sc.scalesVec;
            std::vector<float> &biasVec = sc.biasVec;
            ReadF32Vec(buf, pos, cout, scalesVec);
            ReadF32Vec(buf, pos, cout, biasVec);
            const size_t wCnt = (size_t)cout * (cin / grp) * kh * kw;
            const size_t wBytes = (opType == OP_CONV_S16) ? (wCnt * 2) : wCnt;
            if (pos + wBytes > len) { err = "Truncated conv weights"; return false; }

            const int hout = (curH + 2 * ph - kh) / sh + 1;
            const int wout = (curW + 2 * pw - kw) / sw + 1;
            TensorLayout outLayout = LAY_F32_CHW;
            float outScale = 0.0f;
            int16_t *outS16 = nullptr;
            const int16_t *inS16 = (curLayout == LAY_S16_HWC) ? sc.inHwc.data() : (const int16_t *)curPtr;
            if (!calMaxAbsOut && opIdx < model.actOutScales.size() &&
                model.actOutScales[opIdx] > 0.0f && saveSlot == 0) {
                outScale = model.actOutScales[opIdx];
                if (grp == curC && grp == cout) {
                    outLayout = LAY_S16_HWC;
                    const int coutPad = (cout + 1) & ~1;
                    const size_t needHwc = (size_t)hout * (size_t)wout * (size_t)coutPad;
                    if (sc.inHwc.size() < needHwc) { std::vector<int16_t>().swap(sc.inHwc); sc.inHwc.resize(needHwc); }
                    if (coutPad > cout) std::fill(sc.inHwc.begin(), sc.inHwc.begin() + needHwc, 0);
                    outS16 = sc.inHwc.data();
                } else {
                    outLayout = LAY_S16_CHW;
                    outS16 = (int16_t *)nxtPtr;
                }
            }

            float *convOut = nxtPtr;
            if (saveSlot == 1) {
                const size_t needSlot1 = (size_t)cout * (size_t)hout * (size_t)wout;
                if (slot1.size() < needSlot1) { std::vector<float>().swap(slot1); slot1.resize(needSlot1); }
                convOut = slot1.data();
            }

            if (opType == OP_CONV_S16) {
                std::vector<int16_t> w16Vec(wCnt);
                if (wCnt > 0) std::memcpy(w16Vec.data(), buf + pos, wBytes);
                pos += wBytes;
                RunConv2D(curPtr, curC, curH, curW, convOut, cout, hout, wout,
                          kh, kw, sh, sw, ph, pw, grp, scalesVec.data(), biasVec.data(), nullptr,
                          act, postScale, postBias, allowDualCore, helperCore, w16Vec.data(), &sc.inHwc, prof, (int)opIdx,
                          curLayout, curScale, outLayout, outScale, inS16, outS16);
            } else {
                const int8_t *wq = (const int8_t *)(buf + pos);
                pos += wBytes;
                RunConv2D(curPtr, curC, curH, curW, convOut, cout, hout, wout,
                          kh, kw, sh, sw, ph, pw, grp, scalesVec.data(), biasVec.data(), wq,
                          act, postScale, postBias, allowDualCore, helperCore, nullptr, &sc.inHwc, prof, (int)opIdx,
                          curLayout, curScale, outLayout, outScale, inS16, outS16);
            }
            if (saveSlot == 1) {
                curPtr = slot1.data();
                nxtPtr = sc.bufA.data();
                slot1C = cout; slot1H = hout; slot1W = wout;
            } else if (outLayout != LAY_S16_HWC) {
                std::swap(curPtr, nxtPtr);
            }
            curLayout = outLayout;
            curScale = outScale;
            curC = cout; curH = hout; curW = wout;

            if (calMaxAbsOut && opIdx < 64 && curLayout == LAY_F32_CHW) {
                float m = MaxAbsRange(curPtr, 0, curC * curH * curW);
                if (m > calMaxAbsOut[opIdx]) calMaxAbsOut[opIdx] = m;
            }
        } else if (opType == OP_GAP) {
            const int hw = curH * curW;
            const float invHW = 1.0f / (float)hw;
            for (int c = 0; c < curC; ++c) {
                const float *p = curPtr + (size_t)c * hw;
                float s = 0.0f;
                for (int i = 0; i < hw; ++i) s += p[i];
                nxtPtr[c] = s * invHW;
            }
            curPtr = nxtPtr;
            nxtPtr = (curPtr == sc.bufA.data()) ? sc.bufB.data() : sc.bufA.data();
            curH = 1; curW = 1;
        } else if (opType == OP_MUL_SLOT) {
            const int hw = slot1H * slot1W;
            bool nextIs1x1 = false;
            if (!calMaxAbsOut && opIdx + 1 < model.numOps && pos + 25 <= len) {
                uint8_t nextType = buf[pos];
                if (nextType == OP_CONV || nextType == OP_CONV_S16) {
                    const uint8_t *np = buf + pos + 1;
                    uint16_t nKh = ReadU16(np + 4), nKw = ReadU16(np + 6);
                    uint8_t nSh = np[8], nSw = np[9], nPh = np[10], nPw = np[11];
                    uint16_t nGrp = ReadU16(np + 12);
                    if (nGrp == 1 && nKh == 1 && nKw == 1 && nSh == 1 && nSw == 1 && nPh == 0 && nPw == 0) {
                        nextIs1x1 = true;
                    }
                }
            }
            if (nextIs1x1) {
                uint64_t tQuant0 = prof ? NowTicks() : 0;
                float maxAbs = ComputeMulMaxAbs(slot1.data(), curPtr, slot1C, hw, allowDualCore ? helperCore : -1);
                const float xScale = maxAbs / 2048.0f;
                const float invXScale = 2048.0f / maxAbs;
                if (prof) prof->quantMaxAbsTicks += NowTicks() - tQuant0;

                std::vector<float> chScale(slot1C);
                for (int c = 0; c < slot1C; ++c) chScale[c] = curPtr[c] * invXScale;

                const int cinPad = (slot1C + 1) & ~1;
                const size_t needHwc = (size_t)hw * (size_t)cinPad;
                if (sc.inHwc.size() < needHwc) { std::vector<int16_t>().swap(sc.inHwc); sc.inHwc.resize(needHwc); }

                std::atomic<int> nextP(0);
                MulQuantTransposeArgs mqArgs{slot1.data(), chScale.data(), sc.inHwc.data(), slot1C, cinPad, hw, hw, &nextP, 32};
                uint64_t tQt0 = prof ? NowTicks() : 0;
                uint64_t tQtWait = 0;
#ifdef __3DS__
                if (allowDualCore && helperCore >= 0 && hw >= 64) {
                    int qtCores[3], qtCount = 0;
                    DispatchHelpers(helperCore, MulQuantTransposeWorkerEntry, &mqArgs, qtCores, qtCount);
                    RunMulQuantTransposeRange(&mqArgs);
                    if (qtCount > 0) {
                        uint64_t tw0 = prof ? NowTicks() : 0;
                        WaitHelpers(qtCores, qtCount);
                        if (prof) tQtWait = NowTicks() - tw0;
                    }
                } else {
                    RunMulQuantTransposeRange(&mqArgs);
                }
#else
                RunMulQuantTransposeRange(&mqArgs);
#endif
                if (prof) {
                    uint64_t tQtEnd = NowTicks();
                    prof->quantWaitTicks += tQtWait;
                    prof->quantTransposeTicks += (tQtEnd - tQt0) - tQtWait;
                    prof->conv1x1QuantTicks += tQtEnd - tQuant0;
                }
                curLayout = LAY_S16_HWC;
                curScale = xScale;
                curC = slot1C; curH = slot1H; curW = slot1W;
                std::vector<float>().swap(slot1);
            } else {
                for (int c = 0; c < slot1C; ++c) {
                    const float s = curPtr[c];
                    const float *src = slot1.data() + (size_t)c * hw;
                    float *dst = nxtPtr + (size_t)c * hw;
                    for (int i = 0; i < hw; ++i) dst[i] = src[i] * s;
                }
                std::swap(curPtr, nxtPtr);
                curLayout = LAY_F32_CHW;
                curScale = 0.0f;
                curC = slot1C; curH = slot1H; curW = slot1W;
                std::vector<float>().swap(slot1);
            }
        } else if (opType == OP_AVGPOOL) {
            uint8_t kh = buf[pos], kw = buf[pos + 1], sh = buf[pos + 2], sw = buf[pos + 3], saveSlot = buf[pos + 4];
            pos += 5;
            const int ho = (curH - kh) / sh + 1;
            const int wo = (curW - kw) / sw + 1;
            const float invK = 1.0f / (float)(kh * kw);
            const int inHW = curH * curW;
            const int outHW = ho * wo;
            for (int c = 0; c < curC; ++c) {
                const float *inC = curPtr + (size_t)c * inHW;
                float *outC = nxtPtr + (size_t)c * outHW;
                for (int oy = 0; oy < ho; ++oy) {
                    for (int ox = 0; ox < wo; ++ox) {
                        float acc = 0.0f;
                        for (int ky = 0; ky < kh; ++ky) {
                            const float *row = inC + (oy * sh + ky) * curW + ox * sw;
                            for (int kx = 0; kx < kw; ++kx) acc += row[kx];
                        }
                        outC[oy * wo + ox] = acc * invK;
                    }
                }
            }
            std::swap(curPtr, nxtPtr);
            curH = ho; curW = wo;
            if (saveSlot == 2) {
                slot2C = curC; slot2H = curH; slot2W = curW;
                slot2.assign(curPtr, curPtr + (size_t)curC * curH * curW);
            }
        } else if (opType == OP_CONCAT_POOL) {
            const size_t n2 = (size_t)slot2C * slot2H * slot2W;
            const size_t nc = (size_t)curC * curH * curW;
            std::memcpy(nxtPtr, slot2.data(), n2 * sizeof(float));
            std::memcpy(nxtPtr + n2, curPtr, nc * sizeof(float));
            std::swap(curPtr, nxtPtr);
            curC = slot2C + curC;
            std::vector<float>().swap(slot2);
        } else if (opType == OP_SVTR_BLOCK) {
            uint16_t dim = ReadU16(buf + pos);
            uint8_t nHeads = buf[pos + 2];
            float eps1 = ReadF32(buf + pos + 3);
            float eps2 = ReadF32(buf + pos + 7);
            pos += 11;

            std::vector<float> ln1G, ln1B, ln2G, ln2B;
            ReadF32Vec(buf, pos, dim, ln1G);
            ReadF32Vec(buf, pos, dim, ln1B);
            ReadF32Vec(buf, pos, dim, ln2G);
            ReadF32Vec(buf, pos, dim, ln2B);

            struct Lin { uint16_t od, id; std::vector<float> sc, bv; const int8_t *wq; } lins[4];
            for (int l = 0; l < 4; ++l) {
                lins[l].od = ReadU16(buf + pos);
                lins[l].id = ReadU16(buf + pos + 2);
                pos += 4;
                ReadF32Vec(buf, pos, lins[l].od, lins[l].sc);
                ReadF32Vec(buf, pos, lins[l].od, lins[l].bv);
                lins[l].wq = (const int8_t *)(buf + pos); pos += (size_t)lins[l].od * lins[l].id;
            }

            if (curH > 0 && seqT == 0) {
                // Transpose [C, 1, W] -> [T=W, C]
                seqT = curW;
                seqC = curC;
                for (int t = 0; t < seqT; ++t) {
                    for (int c = 0; c < seqC; ++c) {
                        nxtPtr[t * seqC + c] = curPtr[c * seqT + t];
                    }
                }
                std::swap(curPtr, nxtPtr);
                curH = 0;
            }

            const int T = seqT;
            const int C = dim;
            const int hdim = C / nHeads;
            const float qScale = 1.0f / std::sqrt((float)hdim);

            std::vector<float> tmp((size_t)T * C * 3);
            std::vector<float> attnCtx((size_t)T * C);
            std::vector<float> scores((size_t)T);

            uint64_t tSvtr0 = prof ? NowTicks() : 0;
            uint64_t tLin = 0, tL0 = 0;

            // LN1 -> QKV
            RunLayerNorm(curPtr, nxtPtr, T, C, ln1G.data(), ln1B.data(), eps1);
            tL0 = prof ? NowTicks() : 0;
            RunLinear(nxtPtr, tmp.data(), T, C, C * 3, lins[0].sc.data(), lins[0].bv.data(), lins[0].wq, allowDualCore, helperCore);
            if (prof) tLin += NowTicks() - tL0;

            // Multi-head self attention
            for (int h = 0; h < nHeads; ++h) {
                for (int ti = 0; ti < T; ++ti) {
                    const float *q = tmp.data() + ((size_t)ti * 3 * C) + h * hdim;
                    float maxS = -1e30f;
                    for (int tj = 0; tj < T; ++tj) {
                        const float *k = tmp.data() + ((size_t)tj * 3 * C) + C + h * hdim;
                        float dot = 0.0f;
                        for (int d = 0; d < hdim; ++d) dot += q[d] * k[d];
                        float s = dot * qScale;
                        scores[tj] = s;
                        if (s > maxS) maxS = s;
                    }
                    float sumExp = 0.0f;
                    for (int tj = 0; tj < T; ++tj) {
                        scores[tj] = FastExpf(scores[tj] - maxS);
                        sumExp += scores[tj];
                    }
                    const float invSum = 1.0f / sumExp;
                    float *outH = attnCtx.data() + (size_t)ti * C + h * hdim;
                    for (int d = 0; d < hdim; ++d) outH[d] = 0.0f;
                    for (int tj = 0; tj < T; ++tj) {
                        const float w = scores[tj] * invSum;
                        const float *v = tmp.data() + ((size_t)tj * 3 * C) + 2 * C + h * hdim;
                        for (int d = 0; d < hdim; ++d) outH[d] += w * v[d];
                    }
                }
            }

            // Proj + residual
            tL0 = prof ? NowTicks() : 0;
            RunLinear(attnCtx.data(), nxtPtr, T, C, C, lins[1].sc.data(), lins[1].bv.data(), lins[1].wq, allowDualCore, helperCore);
            if (prof) tLin += NowTicks() - tL0;
            for (int i = 0; i < T * C; ++i) curPtr[i] += nxtPtr[i];

            // LN2 -> FFN1 -> Swish -> FFN2 + residual
            RunLayerNorm(curPtr, nxtPtr, T, C, ln2G.data(), ln2B.data(), eps2);
            tL0 = prof ? NowTicks() : 0;
            RunLinear(nxtPtr, tmp.data(), T, C, lins[2].od, lins[2].sc.data(), lins[2].bv.data(), lins[2].wq, allowDualCore, helperCore);
            if (prof) tLin += NowTicks() - tL0;
            for (int i = 0; i < T * lins[2].od; ++i) tmp[i] = ApplyAct(tmp[i], ACT_SWISH);
            tL0 = prof ? NowTicks() : 0;
            RunLinear(tmp.data(), nxtPtr, T, lins[2].od, C, lins[3].sc.data(), lins[3].bv.data(), lins[3].wq, allowDualCore, helperCore);
            if (prof) tLin += NowTicks() - tL0;
            for (int i = 0; i < T * C; ++i) curPtr[i] += nxtPtr[i];
            if (prof) {
                uint64_t totSvtr = NowTicks() - tSvtr0;
                prof->svtrLinTicks += tLin;
                prof->svtrAttnTicks += (totSvtr > tLin) ? (totSvtr - tLin) : 0;
            }
        } else if (opType == OP_LAYERNORM) {
            uint64_t tOther0 = prof ? NowTicks() : 0;
            uint16_t dim = ReadU16(buf + pos);
            float eps = ReadF32(buf + pos + 2);
            pos += 6;
            std::vector<float> g, b;
            ReadF32Vec(buf, pos, dim, g);
            ReadF32Vec(buf, pos, dim, b);
            RunLayerNorm(curPtr, nxtPtr, seqT, dim, g.data(), b.data(), eps);
            // Transpose back [T, C] -> [C, 1, W=T]
            for (int c = 0; c < dim; ++c) {
                for (int t = 0; t < seqT; ++t) {
                    curPtr[c * seqT + t] = nxtPtr[t * dim + c];
                }
            }
            curC = dim; curH = 1; curW = seqT;
            seqT = 0;
            if (prof) prof->otherTicks += NowTicks() - tOther0;
        } else if (opType == OP_CTC_HEAD) {
            uint64_t tCtc0 = prof ? NowTicks() : 0;
            uint16_t od = ReadU16(buf + pos);
            uint16_t id = ReadU16(buf + pos + 2);
            pos += 4;
            std::vector<float> sc, bv;
            ReadF32Vec(buf, pos, od, sc);
            ReadF32Vec(buf, pos, od, bv);
            const int8_t *wq = (const int8_t *)(buf + pos); pos += (size_t)od * id;

            // curPtr is [C=120, 1, W=T] -> argmax CTC directly per timestep t using DotS16
            const int T = curW;
            const int inPad = (id + 1) & ~1;
            const int nPairs = inPad / 2;
            float maxAbs = 1e-6f;
            for (int i = 0; i < id * T; ++i) {
                float v = curPtr[i];
                if (v < 0.0f) v = -v;
                if (v > maxAbs) maxAbs = v;
            }
            const float xScale = maxAbs / 2048.0f;
            const float invXScale = 2048.0f / maxAbs;

            std::vector<int16_t> x16((size_t)T * inPad, 0);
            for (int t = 0; t < T; ++t) {
                int16_t *dst = x16.data() + (size_t)t * inPad;
                for (int c = 0; c < id; ++c) {
                    float q = curPtr[(size_t)c * T + t] * invXScale;
                    int32_t qi = (int32_t)(q >= 0.0f ? (q + 0.5f) : (q - 0.5f));
                    if (qi > 32767) qi = 32767;
                    else if (qi < -32767) qi = -32767;
                    dst[c] = (int16_t)qi;
                }
            }

            std::vector<float> bestLogits((size_t)T, -1e30f);
            std::vector<uint32_t> bestIds((size_t)T, 0);
            CtcRangeArgs mainCtc{
                x16.data(), sc.data(), bv.data(), wq,
                bestLogits.data(), bestIds.data(),
                T, (int)id, inPad, nPairs, 0, od, xScale,
                nullptr, 0, od
            };
#ifdef __3DS__
            const int effCore = allowDualCore ? helperCore : -1;
            if (effCore >= 0 && od >= 256) {
                std::atomic<uint32_t> nextO(0);
                const int candidates[3] = { (effCore == 0) ? 0 : 2, 3, 1 };
                const int prio = (effCore == 0) ? 0x32 : 0x30;
                std::vector<float> wLogits[3];
                std::vector<uint32_t> wIds[3];
                CtcRangeArgs workerCtc[3];
                int activeCores[3];
                int activeSlots[3];
                int activeCount = 0;
                for (int i = 0; i < 3; ++i) {
                    int c = candidates[i];
                    if (!ProbeCoreSchedulable(c)) continue;
                    wLogits[i].assign((size_t)T, -1e30f);
                    wIds[i].assign((size_t)T, 0);
                    workerCtc[i] = mainCtc;
                    workerCtc[i].bestLogits = wLogits[i].data();
                    workerCtc[i].bestIds = wIds[i].data();
                    workerCtc[i].nextO = &nextO;
                    workerCtc[i].chunkSize = 128;
                    if (GetPersistentHelper(c).Dispatch(c, prio, CtcWorkerEntry, &workerCtc[i])) {
                        activeCores[activeCount] = c;
                        activeSlots[activeCount] = i;
                        ++activeCount;
                    }
                }
                if (activeCount > 0) {
                    mainCtc.nextO = &nextO;
                    mainCtc.chunkSize = 128;
                    RunCtcRange(&mainCtc);
                    WaitHelpers(activeCores, activeCount);
                    for (int k = 0; k < activeCount; ++k) {
                        int s = activeSlots[k];
                        for (int t = 0; t < T; ++t) {
                            if (wLogits[s][t] > bestLogits[t]) {
                                bestLogits[t] = wLogits[s][t];
                                bestIds[t] = wIds[s][t];
                            }
                        }
                    }
                } else {
                    mainCtc.nextO = nullptr;
                    RunCtcRange(&mainCtc);
                }
            } else {
                RunCtcRange(&mainCtc);
            }
#else
            RunCtcRange(&mainCtc);
#endif

            uint32_t prevId = 0;
            for (int t = 0; t < T; ++t) {
                uint32_t bestId = bestIds[t];
                if (bestId != 0 && bestId != prevId && bestId < vocab.size()) {
                    const std::string &tok = vocab[bestId];
                    outLine.text += tok;
                    CharSpan sp;
                    sp.cp = Utf8FirstCodepoint(tok);
                    sp.xStartRatio = (float)t / (float)T;
                    sp.xEndRatio = (float)(t + 1) / (float)T;
                    outLine.chars.push_back(sp);
                }
                prevId = bestId;
            }
            if (prof) prof->ctcTicks += NowTicks() - tCtc0;
        } else {
            err = "Unknown OCR op";
            return false;
        }
        if (prof && opIdx < 64) prof->opTicks[opIdx] += NowTicks() - opT0;
    }

    return true;
}

// Bilinear resize of RGB24 crop [x1..x2, y1..y2] into normalized NCHW [1, 3, 48, outW]
static void PreprocessCropToNchw(const uint8_t *rgb, int imgW, int imgH,
                                  int x1, int y1, int x2, int y2,
                                  std::vector<float> &outNchw, int &outW,
                                  int maxOutW = 256, float widthScale = 1.0f)
{
    int cw = std::max(1, x2 - x1);
    int ch = std::max(1, y2 - y1);
    int targetW = (int)std::round((float)cw * 48.0f * widthScale / (float)ch);
    targetW = std::max(16, std::min(maxOutW, targetW));
    outW = ((targetW + 7) / 8) * 8;
    if (outW > maxOutW) outW = maxOutW;

    outNchw.assign((size_t)3 * 48 * outW, 0.0f);
    const int hw = 48 * outW;

    for (int dy = 0; dy < 48; ++dy) {
        float sy = y1 + ((float)dy + 0.5f) * (float)ch / 48.0f - 0.5f;
        int iy0 = std::max(0, std::min(imgH - 1, (int)std::floor(sy)));
        int iy1 = std::max(0, std::min(imgH - 1, iy0 + 1));
        float fy = sy - (float)iy0;
        if (fy < 0.0f) fy = 0.0f;
        if (fy > 1.0f) fy = 1.0f;

        for (int dx = 0; dx < targetW; ++dx) {
            float sx = x1 + ((float)dx + 0.5f) * (float)cw / (float)targetW - 0.5f;
            int ix0 = std::max(0, std::min(imgW - 1, (int)std::floor(sx)));
            int ix1 = std::max(0, std::min(imgW - 1, ix0 + 1));
            float fx = sx - (float)ix0;
            if (fx < 0.0f) fx = 0.0f;
            if (fx > 1.0f) fx = 1.0f;

            const uint8_t *p00 = rgb + ((size_t)iy0 * imgW + ix0) * 3;
            const uint8_t *p01 = rgb + ((size_t)iy0 * imgW + ix1) * 3;
            const uint8_t *p10 = rgb + ((size_t)iy1 * imgW + ix0) * 3;
            const uint8_t *p11 = rgb + ((size_t)iy1 * imgW + ix1) * 3;

            const float w00 = (1.0f - fx) * (1.0f - fy);
            const float w01 = fx * (1.0f - fy);
            const float w10 = (1.0f - fx) * fy;
            const float w11 = fx * fy;

            const int outIdx = dy * outW + dx;
            for (int c = 0; c < 3; ++c) {
                int srcC = 2 - c; // BGR order to match PaddleOCR PP-OCRv4
                float px = p00[srcC] * w00 + p01[srcC] * w01 + p10[srcC] * w10 + p11[srcC] * w11;
                outNchw[(size_t)c * hw + outIdx] = (px / 255.0f - 0.5f) / 0.5f;
            }
        }
    }
}

// Compute horizontal edge/contrast projection to tighten a text band around clickY
static void TightenBandY(const uint8_t *rgb, int imgW, int imgH,
                         int x1, int x2, int centerY, int halfSearch,
                         int &outY1, int &outY2)
{
    int top = std::max(0, centerY - halfSearch);
    int bot = std::min(imgH - 1, centerY + halfSearch);
    int len = bot - top + 1;
    std::vector<float> rowEnergy(len, 0.0f);

    for (int y = top; y <= bot; ++y) {
        float e = 0.0f;
        const uint8_t *row = rgb + (size_t)y * imgW * 3;
        for (int x = std::max(1, x1); x < std::min(imgW - 1, x2); ++x) {
            int l0 = row[(x - 1) * 3] + row[(x - 1) * 3 + 1] + row[(x - 1) * 3 + 2];
            int l1 = row[x * 3] + row[x * 3 + 1] + row[x * 3 + 2];
            int d = l1 - l0;
            if (d < 0) d = -d;
            if (d > 60) e += 1.0f;
        }
        rowEnergy[y - top] = e;
    }

    float maxE = 0.0f;
    for (float v : rowEnergy) if (v > maxE) maxE = v;
    if (maxE < 3.0f) {
        outY1 = std::max(0, centerY - 12);
        outY2 = std::min(imgH, centerY + 12);
        return;
    }

    float thresh = maxE * 0.18f;
    int ci = centerY - top;
    int yUp = ci;
    while (yUp > 0 && (rowEnergy[yUp - 1] >= thresh || (ci - yUp) < 6)) --yUp;
    int yDn = ci;
    while (yDn + 1 < len && (rowEnergy[yDn + 1] >= thresh || (yDn - ci) < 6)) ++yDn;

    outY1 = std::max(0, top + yUp - 3);
    outY2 = std::min(imgH, top + yDn + 4);
    if (outY2 - outY1 < 16) {
        int mid = (outY1 + outY2) / 2;
        outY1 = std::max(0, mid - 9);
        outY2 = std::min(imgH, mid + 9);
    }
    if (outY2 - outY1 > 34) {
        outY1 = std::max(0, centerY - 15);
        outY2 = std::min(imgH, centerY + 15);
    }
}

static void TrimCropX(const uint8_t *rgb, int imgW, int imgH,
                      int y1, int y2, int clickX, int &x1, int &x2)
{
    int w = x2 - x1;
    if (w <= 24) return;
    std::vector<float> colE(w, 0.0f);
    float maxE = 0.0f;
    for (int x = x1; x < x2; ++x) {
        float e = 0.0f;
        for (int y = std::max(1, y1); y < std::min(imgH - 1, y2); ++y) {
            const uint8_t *p0 = rgb + ((size_t)(y - 1) * imgW + x) * 3;
            const uint8_t *p1 = rgb + ((size_t)y * imgW + x) * 3;
            int d = std::abs((p1[0] + p1[1] + p1[2]) - (p0[0] + p0[1] + p0[2]));
            if (x > 0) {
                const uint8_t *pl = p1 - 3;
                int dl = std::abs((p1[0] + p1[1] + p1[2]) - (pl[0] + pl[1] + pl[2]));
                if (dl > d) d = dl;
            }
            if (d > 48) e += 1.0f;
        }
        colE[x - x1] = e;
        if (e > maxE) maxE = e;
    }
    if (maxE < 2.0f) return;
    float thresh = maxE * 0.15f;
    int ci = std::max(0, std::min(w - 1, clickX - x1));

    int left = ci;
    int blank = 0;
    for (int i = ci; i >= 0; --i) {
        if (colE[i] >= thresh) {
            left = i;
            blank = 0;
        } else if (++blank >= 14) {
            break;
        }
    }
    int right = ci;
    blank = 0;
    for (int i = ci; i < w; ++i) {
        if (colE[i] >= thresh) {
            right = i;
            blank = 0;
        } else if (++blank >= 14) {
            break;
        }
    }
    int nx1 = std::max(x1, x1 + left - 4);
    int nx2 = std::min(x2, x1 + right + 5);
    if (nx2 - nx1 >= 16) {
        x1 = nx1;
        x2 = nx2;
    }
}

bool RunPreprocessed(const float *nchw, int width, const char *modelPath,
                     OcrLine &outLine, std::string &err)
{
    LoadedModel model;
    if (!LoadModel(modelPath, model, err)) return false;
    return RunPreprocessed(nchw, width, model, outLine, err, true, nullptr);
}

static int FindCleanSplitX(const uint8_t *rgb, int imgW, int imgH,
                           int by1, int by2, int targetX, int minX, int maxX)
{
    int lo = std::max(std::max(1, minX), targetX - 24);
    int hi = std::min(std::min(imgW - 1, maxX), targetX + 16);
    if (lo > hi) return std::max(minX, std::min(maxX, targetX));

    int y0 = std::max(1, by1 + 1);
    int y1 = std::min(imgH - 1, by2 - 1);
    int bestX = targetX;
    float bestScore = 1e30f;

    for (int x = lo; x <= hi; ++x) {
        float energy = 0.0f;
        for (int y = y0; y < y1; ++y) {
            const uint8_t *p  = rgb + ((size_t)y * imgW + x) * 3;
            const uint8_t *pu = rgb + ((size_t)(y - 1) * imgW + x) * 3;
            const uint8_t *pl = p - 3;
            int lc  = (int)p[0]  + (int)p[1]  + (int)p[2];
            int lu  = (int)pu[0] + (int)pu[1] + (int)pu[2];
            int ll  = (int)pl[0] + (int)pl[1] + (int)pl[2];
            energy += (float)(std::abs(lc - lu) + std::abs(lc - ll));
        }
        float score = energy + 0.01f * (float)std::abs(x - targetX);
        if (score < bestScore) {
            bestScore = score;
            bestX = x;
        }
    }
    return bestX;
}

void DetectScreenTextLines(const uint8_t *rgb, int imgW, int imgH, std::vector<TextBox> &outBoxes)
{
    outBoxes.clear();
    if (!rgb || imgW <= 8 || imgH <= 8) return;

    std::vector<float> rowEnergy(imgH, 0.0f);
    float maxRE = 0.0f;
    for (int y = 1; y < imgH - 1; ++y) {
        float e = 0.0f;
        const uint8_t *row = rgb + (size_t)y * imgW * 3;
        for (int x = 1; x < imgW - 1; ++x) {
            int l0 = row[(x - 1) * 3] + row[(x - 1) * 3 + 1] + row[(x - 1) * 3 + 2];
            int l1 = row[x * 3] + row[x * 3 + 1] + row[x * 3 + 2];
            int d = l1 - l0;
            if (d < 0) d = -d;
            if (d > 55) e += 1.0f;
        }
        rowEnergy[y] = e;
        if (e > maxRE) maxRE = e;
    }
    if (maxRE < 3.0f) return;

    float rThresh = std::max(3.0f, maxRE * 0.08f);
    struct Band { int y0, y1; };
    std::vector<Band> bands;
    int y = 0;
    while (y < imgH) {
        while (y < imgH && rowEnergy[y] < rThresh) ++y;
        if (y >= imgH) break;
        int ys = y;
        while (y < imgH && rowEnergy[y] >= rThresh) ++y;
        int ye = y;
        if (ye - ys < 7) continue;
        // Split tall multi-line bands at local minima
        while (ye - ys > 32) {
            int splitY = ys + 16;
            float bestMin = 1e9f;
            int searchEnd = std::min(ye - 8, ys + 30);
            for (int sy = ys + 10; sy <= searchEnd; ++sy) {
                if (rowEnergy[sy] < bestMin) {
                    bestMin = rowEnergy[sy];
                    splitY = sy;
                }
            }
            bands.push_back({ys, splitY});
            ys = splitY + 1;
        }
        if (ye - ys >= 7) bands.push_back({ys, ye});
    }

    for (const Band &b : bands) {
        int by1 = std::max(0, b.y0 - 2);
        int by2 = std::min(imgH, b.y1 + 3);
        int bh = by2 - by1;
        if (bh < 8 || bh > 36) continue;

        std::vector<float> colE(imgW, 0.0f);
        float maxCE = 0.0f;
        for (int x = 1; x < imgW - 1; ++x) {
            float e = 0.0f;
            for (int yy = by1 + 1; yy < by2 - 1; ++yy) {
                const uint8_t *p0 = rgb + ((size_t)(yy - 1) * imgW + x) * 3;
                const uint8_t *p1 = rgb + ((size_t)yy * imgW + x) * 3;
                int d = std::abs((p1[0] + p1[1] + p1[2]) - (p0[0] + p0[1] + p0[2]));
                if (d > 55) e += 1.0f;
            }
            colE[x] = e;
            if (e > maxCE) maxCE = e;
        }
        if (maxCE < 2.0f) continue;

        float cThresh = std::max(1.5f, maxCE * 0.14f);
        int x = 0;
        while (x < imgW) {
            while (x < imgW && colE[x] < cThresh) ++x;
            if (x >= imgW) break;
            int xs = x;
            int blank = 0;
            int lastActive = x;
            while (x < imgW) {
                if (colE[x] >= cThresh) {
                    lastActive = x;
                    blank = 0;
                } else if (++blank >= 18) {
                    break;
                }
                ++x;
            }
            int xe = lastActive + 1;
            int bw = xe - xs;
            if (bw >= 16) {
                int bx1 = std::max(0, xs - 4);
                int bx2 = std::min(imgW, xe + 5);
                // Split wide lines along character-gap valleys so no glyph is sliced mid-character
                int maxChunkW = std::max(160, (int)((float)bh * (320.0f / 48.0f)));
                int cx = bx1;
                while (cx < bx2) {
                    if (bx2 - cx <= maxChunkW + 24) {
                        if (bx2 - cx >= 16) {
                            TextBox tb;
                            tb.x1 = cx; tb.y1 = by1; tb.x2 = bx2; tb.y2 = by2;
                            outBoxes.push_back(tb);
                        }
                        break;
                    }
                    int splitX = FindCleanSplitX(rgb, imgW, imgH, by1, by2, cx + maxChunkW, cx + 32, bx2 - 16);
                    int cx2 = std::min(bx2, splitX + 1);
                    if (cx2 - cx >= 16) {
                        TextBox tb;
                        tb.x1 = cx; tb.y1 = by1; tb.x2 = cx2; tb.y2 = by2;
                        outBoxes.push_back(tb);
                    }
                    cx = std::max(cx + 16, splitX);
                }
            }
        }
    }
}

static inline uint16_t DetRdU16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t DetRdU32(const uint8_t *p) {
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

bool LoadDetModel(const char *detModelPath, LoadedDetModel &outModel, std::string &err)
{
    outModel = LoadedDetModel();
    OfflineFile fp;
    if (!fp.OpenRead(detModelPath)) {
        err = "Missing " + std::string(detModelPath);
        return false;
    }
    uint64_t fsz = fp.GetSize();
    if (fsz < 28 || fsz > 16ULL * 1024ULL * 1024ULL) {
        err = "Invalid det model size";
        return false;
    }
    outModel.modelBuf.resize((size_t)fsz);
    if (!fp.ReadExact(outModel.modelBuf.data(), (size_t)fsz)) {
        outModel.modelBuf.clear();
        err = "Failed to read det model";
        return false;
    }
    fp.Close();

    const uint8_t *mb = outModel.modelBuf.data();
    if (std::memcmp(mb, "OCRDET01", 8) != 0) {
        outModel.modelBuf.clear();
        err = "Bad det model magic";
        return false;
    }
    uint32_t ver = DetRdU32(mb + 8);
    uint32_t numOps = DetRdU32(mb + 12);
    outModel.numSlots = DetRdU32(mb + 16);
    outModel.inputSlot = DetRdU32(mb + 20);
    outModel.outputSlot = DetRdU32(mb + 24);
    if (ver != 1 || numOps == 0 || outModel.numSlots == 0) {
        outModel.modelBuf.clear();
        err = "Unsupported det model version";
        return false;
    }

    outModel.filePath = detModelPath;
    size_t p = 28;
    outModel.ops.reserve(numOps);
    for (uint32_t i = 0; i < numOps; ++i) {
        if (p >= fsz) { err = "Truncated det model"; return false; }
        uint8_t opType = mb[p++];
        DetOp op;
        op.type = opType;
        if (op.type == 1) { // DET_OP_CONV2D
            if (p + 32 > fsz) { err = "Truncated det conv"; return false; }
            const uint8_t *tmp = mb + p;
            op.in0 = DetRdU16(tmp + 0);
            op.out = DetRdU16(tmp + 2);
            op.inC = DetRdU16(tmp + 4);
            op.outC = DetRdU16(tmp + 6);
            op.kH = DetRdU16(tmp + 8);
            op.kW = DetRdU16(tmp + 10);
            op.sH = DetRdU16(tmp + 12);
            op.sW = DetRdU16(tmp + 14);
            op.pH = DetRdU16(tmp + 16);
            op.pW = DetRdU16(tmp + 18);
            op.groups = DetRdU16(tmp + 20);
            op.act = DetRdU16(tmp + 22);
            std::memcpy(&op.postScale, tmp + 24, 4);
            std::memcpy(&op.postBias, tmp + 28, 4);
            p += 32;
            op.dataOffset = p;
            size_t cinG = op.inC / std::max<uint16_t>(1, op.groups);
            op.dataSize = (size_t)op.outC * 8 + (size_t)op.outC * cinG * op.kH * op.kW;
            if (p + op.dataSize > fsz) { err = "Truncated det conv weights"; return false; }
            p += op.dataSize;
        } else if (op.type == 2) { // DET_OP_CONV_T2X2
            if (p + 10 > fsz) { err = "Truncated det convT"; return false; }
            const uint8_t *tmp = mb + p;
            op.in0 = DetRdU16(tmp + 0);
            op.out = DetRdU16(tmp + 2);
            op.inC = DetRdU16(tmp + 4);
            op.outC = DetRdU16(tmp + 6);
            op.act = DetRdU16(tmp + 8);
            p += 10;
            op.dataOffset = p;
            op.dataSize = (size_t)op.outC * 4 + (size_t)op.inC * op.outC * 16;
            if (p + op.dataSize > fsz) { err = "Truncated det convT weights"; return false; }
            p += op.dataSize;
        } else if (op.type == 3 || op.type == 4) { // ADD / MUL_CH
            if (p + 6 > fsz) { err = "Truncated det binary"; return false; }
            const uint8_t *tmp = mb + p;
            op.in0 = DetRdU16(tmp + 0);
            op.in1 = DetRdU16(tmp + 2);
            op.out = DetRdU16(tmp + 4);
            p += 6;
        } else if (op.type == 5) { // GAP
            if (p + 4 > fsz) { err = "Truncated det GAP"; return false; }
            const uint8_t *tmp = mb + p;
            op.in0 = DetRdU16(tmp + 0);
            op.out = DetRdU16(tmp + 2);
            p += 4;
        } else if (op.type == 6) { // RESIZE_NEAREST
            if (p + 6 > fsz) { err = "Truncated det resize"; return false; }
            const uint8_t *tmp = mb + p;
            op.in0 = DetRdU16(tmp + 0);
            op.out = DetRdU16(tmp + 2);
            op.scale = DetRdU16(tmp + 4);
            p += 6;
        } else if (op.type == 7) { // CONCAT4
            if (p + 10 > fsz) { err = "Truncated det concat"; return false; }
            const uint8_t *tmp = mb + p;
            op.in0 = DetRdU16(tmp + 0);
            op.in1 = DetRdU16(tmp + 2);
            op.in2 = DetRdU16(tmp + 4);
            op.in3 = DetRdU16(tmp + 6);
            op.out = DetRdU16(tmp + 8);
            p += 10;
        } else {
            err = "Unknown det op";
            return false;
        }
        outModel.ops.push_back(op);
    }
    outModel.loaded = true;
    return true;
}

struct ConvT2x2Args {
    const int16_t *inHwc;
    const int16_t *wQ;
    const float *wScales;
    const float *bias;
    float *out;
    float xScale;
    int cinPad, nPairs, inH, inW, outC, wout;
    size_t outHW;
    uint8_t act;
    bool splitByY;
    std::atomic<int> *nextIdx;
    int chunkSize;
    int totalCount;
};

static void RunConvT2x2Range(const ConvT2x2Args *args) {
    const int cinPad = args->cinPad, nPairs = args->nPairs;
    const int inH = args->inH, inW = args->inW;
    const int outC = args->outC, wout = args->wout;
    const size_t outHW = args->outHW;
    const uint8_t act = args->act;
    const float xScale = args->xScale;

    auto runOcRows = [&](int oc, int y0, int y1) {
        const int16_t *wk0 = args->wQ + ((size_t)oc * 4 + 0) * cinPad;
        const int16_t *wk1 = args->wQ + ((size_t)oc * 4 + 1) * cinPad;
        const int16_t *wk2 = args->wQ + ((size_t)oc * 4 + 2) * cinPad;
        const int16_t *wk3 = args->wQ + ((size_t)oc * 4 + 3) * cinPad;
        const float sc = args->wScales[oc] * xScale;
        const float bv = args->bias[oc];
        float *outCh = args->out + (size_t)oc * outHW;
        for (int y = y0; y < y1; ++y) {
            const int16_t *inRow = args->inHwc + ((size_t)y * inW) * cinPad;
            float *oRow0 = outCh + (size_t)(y * 2) * wout;
            float *oRow1 = oRow0 + wout;
            int x = 0;
            for (; x + 1 < inW; x += 2) {
                const int16_t *px0 = inRow + (size_t)x * cinPad;
                const int16_t *px1 = px0 + cinPad;
                int32_t c00, c01, c10, c11;
                DotS16_2x2(px0, px1, wk0, wk1, nPairs, c00, c01, c10, c11);
                oRow0[x * 2 + 0] = ApplyAct((float)c00 * sc + bv, act);
                oRow0[x * 2 + 1] = ApplyAct((float)c10 * sc + bv, act);
                oRow0[x * 2 + 2] = ApplyAct((float)c01 * sc + bv, act);
                oRow0[x * 2 + 3] = ApplyAct((float)c11 * sc + bv, act);
                DotS16_2x2(px0, px1, wk2, wk3, nPairs, c00, c01, c10, c11);
                oRow1[x * 2 + 0] = ApplyAct((float)c00 * sc + bv, act);
                oRow1[x * 2 + 1] = ApplyAct((float)c10 * sc + bv, act);
                oRow1[x * 2 + 2] = ApplyAct((float)c01 * sc + bv, act);
                oRow1[x * 2 + 3] = ApplyAct((float)c11 * sc + bv, act);
            }
            if (x < inW) {
                const int16_t *px0 = inRow + (size_t)x * cinPad;
                oRow0[x * 2 + 0] = ApplyAct((float)DotS16(px0, wk0, nPairs) * sc + bv, act);
                oRow0[x * 2 + 1] = ApplyAct((float)DotS16(px0, wk1, nPairs) * sc + bv, act);
                oRow1[x * 2 + 0] = ApplyAct((float)DotS16(px0, wk2, nPairs) * sc + bv, act);
                oRow1[x * 2 + 1] = ApplyAct((float)DotS16(px0, wk3, nPairs) * sc + bv, act);
            }
        }
    };

    for (;;) {
        int s = args->nextIdx->fetch_add(args->chunkSize, std::memory_order_relaxed);
        if (s >= args->totalCount) break;
        int e = std::min(args->totalCount, s + args->chunkSize);

        if (!args->splitByY) {
            for (int oc = s; oc < e; ++oc) runOcRows(oc, 0, inH);
        } else {
            for (int oc = 0; oc < outC; ++oc) runOcRows(oc, s, e);
        }
    }
}

#ifdef __3DS__
static void ConvT2x2WorkerEntry(void *arg) {
    RunConvT2x2Range((const ConvT2x2Args *)arg);
}
#endif

bool DetectScreenTextLinesDbNet(const uint8_t *rgb, int imgW, int imgH,
                                const LoadedDetModel &detModel,
                                std::vector<TextBox> &outBoxes, std::string &err,
                                bool allowDualCore, const std::atomic<bool> *abortFlag,
                                int helperCore, DetProfile *detProf)
{
    outBoxes.clear();
    if (!detModel.loaded || detModel.modelBuf.empty() || !rgb || imgW <= 8 || imgH <= 8) {
        err = "Det model not loaded";
        return false;
    }

    const uint8_t *mb = detModel.modelBuf.data();
    const size_t mbSize = detModel.modelBuf.size();

    const int detW = (imgW > 340) ? 256 : 224;
    const int detH = 160;

    struct DetTensor {
        int c{0}, h{0}, w{0};
        std::vector<float> data;
    };
    std::vector<DetTensor> slots(detModel.numSlots);
    OcrProfile detConvProf;
    if (detProf) {
        detProf->numOps = (uint32_t)detModel.ops.size();
    }

    // Preprocess RGB -> BGR NCHW normalized with mean=0.5, std=0.5
    uint64_t tPre0 = detProf ? NowTicks() : 0;
    {
        DetTensor &inT = slots[detModel.inputSlot];
        inT.c = 3; inT.h = detH; inT.w = detW;
        const size_t hw = (size_t)detH * detW;
        inT.data.resize(3 * hw);
        const float sx = (float)imgW / (float)detW;
        const float sy = (float)imgH / (float)detH;
        for (int dy = 0; dy < detH; ++dy) {
            float srcY = (dy + 0.5f) * sy - 0.5f;
            if (srcY < 0.0f) srcY = 0.0f;
            if (srcY > imgH - 1.0f) srcY = imgH - 1.0f;
            int iy0 = (int)srcY;
            int iy1 = std::min(imgH - 1, iy0 + 1);
            float fy = srcY - iy0;
            for (int dx = 0; dx < detW; ++dx) {
                float srcX = (dx + 0.5f) * sx - 0.5f;
                if (srcX < 0.0f) srcX = 0.0f;
                if (srcX > imgW - 1.0f) srcX = imgW - 1.0f;
                int ix0 = (int)srcX;
                int ix1 = std::min(imgW - 1, ix0 + 1);
                float fx = srcX - ix0;

                const uint8_t *p00 = rgb + ((size_t)iy0 * imgW + ix0) * 3;
                const uint8_t *p01 = rgb + ((size_t)iy0 * imgW + ix1) * 3;
                const uint8_t *p10 = rgb + ((size_t)iy1 * imgW + ix0) * 3;
                const uint8_t *p11 = rgb + ((size_t)iy1 * imgW + ix1) * 3;
                const float w00 = (1.0f - fx) * (1.0f - fy);
                const float w01 = fx * (1.0f - fy);
                const float w10 = (1.0f - fx) * fy;
                const float w11 = fx * fy;

                const size_t outIdx = (size_t)dy * detW + dx;
                for (int c = 0; c < 3; ++c) {
                    int srcC = 2 - c; // BGR
                    float px = p00[srcC] * w00 + p01[srcC] * w01 + p10[srcC] * w10 + p11[srcC] * w11;
                    inT.data[(size_t)c * hw + outIdx] = (px / 255.0f - 0.5f) / 0.5f;
                }
            }
        }
    }
    if (detProf) detProf->preprocTicks += NowTicks() - tPre0;

    // Liveness analysis so intermediate tensors are freed immediately after last use
    std::vector<int> lastUse(detModel.numSlots, -1);
    for (int oi = 0; oi < (int)detModel.ops.size(); ++oi) {
        const DetOp &op = detModel.ops[oi];
        if (op.type == 1 || op.type == 2 || op.type == 5 || op.type == 6) {
            lastUse[op.in0] = oi;
        } else if (op.type == 3 || op.type == 4) {
            lastUse[op.in0] = oi;
            lastUse[op.in1] = oi;
        } else if (op.type == 7) {
            lastUse[op.in0] = oi;
            lastUse[op.in1] = oi;
            lastUse[op.in2] = oi;
            lastUse[op.in3] = oi;
        }
    }
    lastUse[detModel.outputSlot] = (int)detModel.ops.size();

    auto freeIfDead = [&](uint16_t slot, int oi) {
        if (slot < detModel.numSlots && lastUse[slot] == oi) {
            std::vector<float>().swap(slots[slot].data);
        }
    };

    for (int oi = 0; oi < (int)detModel.ops.size(); ++oi) {
        if (abortFlag && abortFlag->load(std::memory_order_acquire)) {
            err = "Aborted";
            return false;
        }
        uint64_t opT0 = detProf ? NowTicks() : 0;
        const DetOp &op = detModel.ops[oi];
        if (op.type == 1) { // CONV2D
            const DetTensor &inT = slots[op.in0];
            int hout = (inT.h + 2 * op.pH - op.kH) / op.sH + 1;
            int wout = (inT.w + 2 * op.pW - op.kW) / op.sW + 1;
            DetTensor &outT = slots[op.out];
            outT.c = op.outC; outT.h = hout; outT.w = wout;
            outT.data.resize((size_t)op.outC * hout * wout);
            uint64_t tIo0 = detProf ? NowTicks() : 0;
            if (op.dataOffset + op.dataSize > mbSize) {
                err = "Det weight out of bounds";
                return false;
            }
            const uint8_t *buf = mb + op.dataOffset;
            std::vector<float> scales(op.outC), bias(op.outC);
            std::memcpy(scales.data(), buf, (size_t)op.outC * 4);
            std::memcpy(bias.data(), buf + (size_t)op.outC * 4, (size_t)op.outC * 4);
            if (detProf) detProf->ioTicks += NowTicks() - tIo0;
            const int8_t *wq = (const int8_t *)(buf + (size_t)op.outC * 8);
            RunConv2D(inT.data.data(), inT.c, inT.h, inT.w,
                      outT.data.data(), op.outC, hout, wout,
                      op.kH, op.kW, op.sH, op.sW, op.pH, op.pW, op.groups,
                      scales.data(), bias.data(), wq, (uint8_t)op.act, op.postScale, op.postBias,
                      allowDualCore, helperCore, nullptr, nullptr, detProf ? &detConvProf : nullptr);
            freeIfDead(op.in0, oi);
        } else if (op.type == 2) { // CONV_T2X2
            const DetTensor &inT = slots[op.in0];
            const int inH = inT.h, inW = inT.w;
            const int hout = inH * 2;
            const int wout = inW * 2;
            uint64_t tIo0 = detProf ? NowTicks() : 0;
            if (op.dataOffset + op.dataSize > mbSize) {
                err = "Det convT out of bounds";
                return false;
            }
            const uint8_t *buf = mb + op.dataOffset;
            std::vector<float> bias(op.outC);
            std::vector<float> w((size_t)op.inC * op.outC * 4);
            std::memcpy(bias.data(), buf, (size_t)op.outC * 4);
            std::memcpy(w.data(), buf + (size_t)op.outC * 4, w.size() * sizeof(float));
            if (detProf) detProf->ioTicks += NowTicks() - tIo0;
            uint64_t tCt0 = detProf ? NowTicks() : 0;
            const size_t inHW = (size_t)inH * inW;
            const size_t outHW = (size_t)hout * wout;
            const int effCore = allowDualCore ? helperCore : -1;
            const int cinPad = (op.inC + 1) & ~1;
            const int nPairs = cinPad / 2;

            float maxAbs = ComputeMaxAbs(inT.data.data(), (int)(op.inC * inHW), effCore);
            const float xScale = maxAbs / 2048.0f;
            const float invXScale = 2048.0f / maxAbs;
            std::vector<int16_t> inHwc(inHW * (size_t)cinPad, 0);
            {
                std::atomic<int> nextP(0);
                QuantTransposeArgs qtArgs{inT.data.data(), inHwc.data(), op.inC, cinPad, (int)inHW, (int)inHW, invXScale, &nextP, 64};
#ifdef __3DS__
                if (effCore >= 0 && inHW >= 256) {
                    int qtCores[3], qtCount = 0;
                    DispatchHelpers(effCore, QuantTransposeWorkerEntry, &qtArgs, qtCores, qtCount);
                    RunQuantTransposeRange(&qtArgs);
                    if (qtCount > 0) WaitHelpers(qtCores, qtCount);
                } else {
                    RunQuantTransposeRange(&qtArgs);
                }
#else
                RunQuantTransposeRange(&qtArgs);
#endif
            }
            freeIfDead(op.in0, oi);

            DetTensor &outT = slots[op.out];
            outT.c = op.outC; outT.h = hout; outT.w = wout;
            outT.data.resize((size_t)op.outC * hout * wout);

            std::vector<int16_t> wQ((size_t)op.outC * 4 * cinPad, 0);
            std::vector<float> wScales(op.outC);
            for (int oc = 0; oc < op.outC; ++oc) {
                float maxW = 1e-6f;
                for (int ic = 0; ic < op.inC; ++ic) {
                    const float *kw = w.data() + ((size_t)ic * op.outC + oc) * 4;
                    for (int q = 0; q < 4; ++q) {
                        float aw = std::fabs(kw[q]);
                        if (aw > maxW) maxW = aw;
                    }
                }
                wScales[oc] = maxW / 4096.0f;
                const float invW = 4096.0f / maxW;
                for (int q = 0; q < 4; ++q) {
                    int16_t *dst = wQ.data() + ((size_t)oc * 4 + q) * cinPad;
                    for (int ic = 0; ic < op.inC; ++ic) {
                        dst[ic] = RoundToS16(w[((size_t)ic * op.outC + oc) * 4 + q] * invW);
                    }
                }
            }

            const bool splitByY = (op.outC < 4);
            std::atomic<int> nextIdx(0);
            ConvT2x2Args ctArgs{
                inHwc.data(), wQ.data(), wScales.data(), bias.data(), outT.data.data(),
                xScale, cinPad, nPairs, inH, inW, op.outC, wout,
                outHW, (uint8_t)op.act, splitByY,
                &nextIdx, splitByY ? 8 : 4, splitByY ? inH : (int)op.outC
            };
#ifdef __3DS__
            if (allowDualCore && helperCore >= 0 && (size_t)op.inC * op.outC * inHW >= 8192) {
                int activeCores[3], activeCount = 0;
                DispatchHelpers(helperCore, ConvT2x2WorkerEntry, &ctArgs, activeCores, activeCount);
                RunConvT2x2Range(&ctArgs);
                if (activeCount > 0) WaitHelpers(activeCores, activeCount);
            } else {
                RunConvT2x2Range(&ctArgs);
            }
#else
            RunConvT2x2Range(&ctArgs);
#endif
            if (detProf) detProf->convT2x2Ticks += NowTicks() - tCt0;
        } else if (op.type == 3) { // ADD
            uint64_t tOp0 = detProf ? NowTicks() : 0;
            const DetTensor &a = slots[op.in0];
            const DetTensor &b = slots[op.in1];
            DetTensor &outT = slots[op.out];
            outT.c = a.c; outT.h = a.h; outT.w = a.w;
            const size_t n = a.data.size();
            outT.data.resize(n);
            for (size_t i = 0; i < n; ++i) outT.data[i] = a.data[i] + b.data[i];
            if (detProf) detProf->addTicks += NowTicks() - tOp0;
            freeIfDead(op.in0, oi);
            freeIfDead(op.in1, oi);
        } else if (op.type == 4) { // MUL_CH
            uint64_t tOp0 = detProf ? NowTicks() : 0;
            const DetTensor *fm = &slots[op.in0];
            const DetTensor *vec = &slots[op.in1];
            if (fm->h * fm->w < vec->h * vec->w) std::swap(fm, vec);
            DetTensor &outT = slots[op.out];
            outT.c = fm->c; outT.h = fm->h; outT.w = fm->w;
            const size_t hw = (size_t)fm->h * fm->w;
            outT.data.resize((size_t)fm->c * hw);
            for (int c = 0; c < fm->c; ++c) {
                float s = vec->data[c];
                const float *src = fm->data.data() + (size_t)c * hw;
                float *dst = outT.data.data() + (size_t)c * hw;
                for (size_t i = 0; i < hw; ++i) dst[i] = src[i] * s;
            }
            if (detProf) detProf->mulChTicks += NowTicks() - tOp0;
            freeIfDead(op.in0, oi);
            freeIfDead(op.in1, oi);
        } else if (op.type == 5) { // GAP
            uint64_t tOp0 = detProf ? NowTicks() : 0;
            const DetTensor &inT = slots[op.in0];
            DetTensor &outT = slots[op.out];
            outT.c = inT.c; outT.h = 1; outT.w = 1;
            outT.data.resize(inT.c);
            const size_t hw = (size_t)inT.h * inT.w;
            const float inv = 1.0f / (float)std::max<size_t>(1, hw);
            for (int c = 0; c < inT.c; ++c) {
                const float *src = inT.data.data() + (size_t)c * hw;
                float sum = 0.0f;
                for (size_t i = 0; i < hw; ++i) sum += src[i];
                outT.data[c] = sum * inv;
            }
            if (detProf) detProf->gapTicks += NowTicks() - tOp0;
            freeIfDead(op.in0, oi);
        } else if (op.type == 6) { // RESIZE_NEAREST
            uint64_t tOp0 = detProf ? NowTicks() : 0;
            const DetTensor &inT = slots[op.in0];
            int sc = op.scale;
            int hout = inT.h * sc;
            int wout = inT.w * sc;
            DetTensor &outT = slots[op.out];
            outT.c = inT.c; outT.h = hout; outT.w = wout;
            outT.data.resize((size_t)inT.c * hout * wout);
            const size_t inHW = (size_t)inT.h * inT.w;
            const size_t outHW = (size_t)hout * wout;
            for (int c = 0; c < inT.c; ++c) {
                const float *srcC = inT.data.data() + (size_t)c * inHW;
                float *dstC = outT.data.data() + (size_t)c * outHW;
                for (int dy = 0; dy < hout; ++dy) {
                    const float *srcRow = srcC + (size_t)(dy / sc) * inT.w;
                    float *dstRow = dstC + (size_t)dy * wout;
                    for (int dx = 0; dx < wout; ++dx) {
                        dstRow[dx] = srcRow[dx / sc];
                    }
                }
            }
            if (detProf) detProf->resizeTicks += NowTicks() - tOp0;
            freeIfDead(op.in0, oi);
        } else if (op.type == 7) { // CONCAT4
            uint64_t tOp0 = detProf ? NowTicks() : 0;
            uint16_t ins[4] = {op.in0, op.in1, op.in2, op.in3};
            int totC = 0;
            for (int k = 0; k < 4; ++k) totC += slots[ins[k]].c;
            int h = slots[op.in0].h;
            int w = slots[op.in0].w;
            DetTensor &outT = slots[op.out];
            outT.c = totC; outT.h = h; outT.w = w;
            outT.data.resize((size_t)totC * h * w);
            size_t off = 0;
            for (int k = 0; k < 4; ++k) {
                const auto &src = slots[ins[k]].data;
                std::memcpy(outT.data.data() + off, src.data(), src.size() * sizeof(float));
                off += src.size();
            }
            if (detProf) detProf->concatTicks += NowTicks() - tOp0;
            for (int k = 0; k < 4; ++k) freeIfDead(ins[k], oi);
        }
        if (detProf && oi < 160) detProf->opTicks[oi] += NowTicks() - opT0;
    }

    // Post-process DBNet probability map [1, detH, detW]
    uint64_t tPost0 = detProf ? NowTicks() : 0;
    const DetTensor &prob = slots[detModel.outputSlot];
    if (prob.h != detH || prob.w != detW || prob.data.size() < (size_t)detH * detW) {
        err = "Unexpected det output shape";
        return false;
    }

    const float thresh = 0.30f;
    const float boxThresh = 0.45f;
    std::vector<uint8_t> visited((size_t)detH * detW, 0);
    std::vector<int> qx, qy;
    qx.reserve(512);
    qy.reserve(512);

    const float scaleX = (float)imgW / (float)detW;
    const float scaleY = (float)imgH / (float)detH;

    for (int y = 0; y < detH; ++y) {
        for (int x = 0; x < detW; ++x) {
            size_t idx = (size_t)y * detW + x;
            if (visited[idx] || prob.data[idx] <= thresh) continue;

            qx.clear(); qy.clear();
            qx.push_back(x); qy.push_back(y);
            visited[idx] = 1;

            int minX = x, maxX = x, minY = y, maxY = y;
            double probSum = 0.0;
            size_t head = 0;
            while (head < qx.size()) {
                int cx = qx[head], cy = qy[head];
                ++head;
                probSum += prob.data[(size_t)cy * detW + cx];
                if (cx < minX) minX = cx;
                if (cx > maxX) maxX = cx;
                if (cy < minY) minY = cy;
                if (cy > maxY) maxY = cy;

                const int dx4[4] = {1, -1, 0, 0};
                const int dy4[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; ++d) {
                    int nx = cx + dx4[d], ny = cy + dy4[d];
                    if (nx >= 0 && nx < detW && ny >= 0 && ny < detH) {
                        size_t nidx = (size_t)ny * detW + nx;
                        if (!visited[nidx] && prob.data[nidx] > thresh) {
                            visited[nidx] = 1;
                            qx.push_back(nx);
                            qy.push_back(ny);
                        }
                    }
                }
            }

            int area = (int)qx.size();
            if (area < 4) continue;
            float meanProb = (float)(probSum / (double)area);
            if (meanProb < boxThresh) continue;

            int bw = maxX - minX + 1;
            int bh = maxY - minY + 1;
            float d = ((float)area * 1.5f) / std::max(1.0f, 2.0f * (float)(bw + bh));
            int padX = std::max(2, std::min(12, (int)std::round(d * 0.65f)));
            int padY = std::max(2, std::min(10, (int)std::round(d * 0.55f)));

            int bx1 = std::max(0, (int)std::floor((minX - padX) * scaleX));
            int by1 = std::max(0, (int)std::floor((minY - padY) * scaleY) - 1);
            int bx2 = std::min(imgW, (int)std::ceil((maxX + 1 + padX) * scaleX));
            int by2 = std::min(imgH, (int)std::ceil((maxY + 1 + padY) * scaleY) + 1);
            if (bx2 - bx1 < 10 || by2 - by1 < 7) continue;

            int origBh = by2 - by1;
            int maxChunkW = std::max(160, (int)((float)origBh * (320.0f / 48.0f)));
            int cx = bx1;
            while (cx < bx2) {
                if (bx2 - cx <= maxChunkW + 24) {
                    if (bx2 - cx >= 10) {
                        TextBox tb;
                        tb.x1 = cx; tb.y1 = by1; tb.x2 = bx2; tb.y2 = by2;
                        outBoxes.push_back(tb);
                    }
                    break;
                }
                int splitX = FindCleanSplitX(rgb, imgW, imgH, by1, by2, cx + maxChunkW, cx + 32, bx2 - 16);
                int cx2 = std::min(bx2, splitX + 1);
                if (cx2 - cx >= 10) {
                    TextBox tb;
                    tb.x1 = cx; tb.y1 = by1; tb.x2 = cx2; tb.y2 = by2;
                    outBoxes.push_back(tb);
                }
                cx = std::max(cx + 16, splitX);
            }
        }
    }

    std::sort(outBoxes.begin(), outBoxes.end(), [](const TextBox &a, const TextBox &b) {
        int ra = a.y1 / 12, rb = b.y1 / 12;
        if (ra != rb) return ra < rb;
        return a.x1 < b.x1;
    });
    if (detProf) {
        detProf->postprocTicks += NowTicks() - tPost0;
        detProf->conv1x1QuantTicks += detConvProf.conv1x1QuantTicks;
        detProf->conv1x1GemmTicks += detConvProf.conv1x1GemmTicks;
        detProf->convDwTicks += detConvProf.convDwTicks;
        detProf->convIm2ColTicks += detConvProf.convIm2ColTicks;
    }
    return true;
}

static void TrimBoxEdgeBorders(const uint8_t *rgb, int imgW, int imgH,
                                 int &x1, int y1, int &x2, int y2)
{
    int w = x2 - x1;
    int h = y2 - y1;
    if (w < 24 || h < 8) return;

    int ry0 = std::min(y2 - 1, y1 + 2);
    int ry1 = std::max(ry0 + 1, y2 - 2);
    std::vector<int> colE((size_t)w, 0);
    int maxBoxE = 0;
    for (int x = x1; x < x2; ++x) {
        int e = 0;
        for (int y = ry0; y < ry1; ++y) {
            const uint8_t *p = rgb + ((size_t)y * imgW + x) * 3;
            int l = (int)p[0] * 3 + (int)p[1] * 6 + (int)p[2];
            int d = 0;
            if (x > x1) {
                const uint8_t *pl = p - 3;
                d = std::max(d, std::abs(l - ((int)pl[0] * 3 + (int)pl[1] * 6 + (int)pl[2])));
            }
            if (y > ry0) {
                const uint8_t *pu = p - (size_t)imgW * 3;
                d = std::max(d, std::abs(l - ((int)pu[0] * 3 + (int)pu[1] * 6 + (int)pu[2])));
            }
            if (d > 180) ++e;
        }
        colE[(size_t)(x - x1)] = e;
        if (e > maxBoxE) maxBoxE = e;
    }
    if (maxBoxE < 4) return;

    int maxScan = std::min(12, w / 4);
    int nx1 = x1;
    for (int g = 0; g < maxScan; ++g) {
        if (colE[(size_t)g] == 0) {
            int gEnd = g;
            int lim = std::min(w, maxScan + 8);
            while (gEnd < lim && colE[(size_t)gEnd] == 0) ++gEnd;
            if (g == 0) {
                if (gEnd >= 6 && gEnd < w - 10) {
                    nx1 = x1 + std::max(0, gEnd - 2);
                    break;
                }
            } else if (gEnd - g >= 4 && gEnd < w - 10) {
                int maxEdgeE = 0;
                for (int k = 0; k < g; ++k) {
                    if (colE[(size_t)k] > maxEdgeE) maxEdgeE = colE[(size_t)k];
                }
                if (maxEdgeE * 2 < maxBoxE) {
                    nx1 = x1 + std::max(0, gEnd - 2);
                }
                break;
            }
            g = gEnd - 1;
        }
    }

    int nx2 = x2;
    for (int r = 0; r < maxScan; ++r) {
        if (colE[(size_t)(w - 1 - r)] == 0) {
            int rEnd = r;
            int lim = std::min(w, maxScan + 8);
            while (rEnd < lim && colE[(size_t)(w - 1 - rEnd)] == 0) ++rEnd;
            if (r == 0) {
                if (rEnd >= 6 && w - rEnd > 10) {
                    nx2 = x2 - std::max(0, rEnd - 2);
                    break;
                }
            } else if (rEnd - r >= 4 && w - rEnd > 10) {
                int maxEdgeE = 0;
                for (int k = w - r; k < w; ++k) {
                    if (colE[(size_t)k] > maxEdgeE) maxEdgeE = colE[(size_t)k];
                }
                if (maxEdgeE * 2 < maxBoxE) {
                    nx2 = x2 - std::max(0, rEnd - 2);
                }
                break;
            }
            r = rEnd - 1;
        }
    }

    if (nx2 - nx1 >= 16) {
        x1 = nx1;
        x2 = nx2;
    }
}

static int ColContrastEnergy(const uint8_t *rgb, int imgW, int x, int ry0, int ry1) {
    int e = 0;
    for (int y = ry0; y < ry1; ++y) {
        const uint8_t *p = rgb + ((size_t)y * imgW + x) * 3;
        int l = (int)p[0] * 3 + (int)p[1] * 6 + (int)p[2];
        int d = 0;
        if (x > 0) {
            const uint8_t *pl = p - 3;
            d = std::max(d, std::abs(l - ((int)pl[0] * 3 + (int)pl[1] * 6 + (int)pl[2])));
        }
        if (y > ry0) {
            const uint8_t *pu = p - (size_t)imgW * 3;
            d = std::max(d, std::abs(l - ((int)pu[0] * 3 + (int)pu[1] * 6 + (int)pu[2])));
        }
        if (d > 180) ++e;
    }
    return e;
}

static void ExpandClippedBoxEdges(const uint8_t *rgb, int imgW, int imgH,
                                  int &x1, int y1, int &x2, int y2,
                                  bool allowLeft, bool allowRight)
{
    if (y2 - y1 < 8) return;
    int ry0 = std::min(y2 - 1, y1 + 2);
    int ry1 = std::max(ry0 + 1, y2 - 2);
    if (allowLeft && x1 > 0 && ColContrastEnergy(rgb, imgW, x1, ry0, ry1) >= 2) {
        int lim = std::max(0, x1 - 8);
        int nx1 = x1;
        int z = 0;
        for (int x = x1 - 1; x >= lim; --x) {
            if (ColContrastEnergy(rgb, imgW, x, ry0, ry1) == 0) {
                if (++z >= 2) { nx1 = x; break; }
            } else {
                z = 0;
                nx1 = x;
            }
        }
        x1 = std::max(0, nx1 - 2);
    }
    if (allowRight && x2 < imgW && ColContrastEnergy(rgb, imgW, x2 - 1, ry0, ry1) >= 2) {
        int lim = std::min(imgW - 1, x2 + 8);
        int nx2 = x2;
        int z = 0;
        for (int x = x2; x <= lim; ++x) {
            if (ColContrastEnergy(rgb, imgW, x, ry0, ry1) == 0) {
                if (++z >= 2) { nx2 = x; break; }
            } else {
                z = 0;
                nx2 = x + 1;
            }
        }
        x2 = std::min(imgW, nx2 + 2);
    }
}

bool RunBox(const uint8_t *rgb, int imgW, int imgH, const TextBox &box,
            const LoadedModel &model, OcrLine &outLine, std::string &err,
            bool allowDualCore, const std::atomic<bool> *abortFlag, int helperCore,
            RecScratch *scratch, OcrProfile *prof)
{
    int bx1 = box.x1;
    int bx2 = box.x2;
    TrimBoxEdgeBorders(rgb, imgW, imgH, bx1, box.y1, bx2, box.y2);
    ExpandClippedBoxEdges(rgb, imgW, imgH, bx1, box.y1, bx2, box.y2,
                          bx1 == box.x1, bx2 == box.x2);

    std::vector<float> localNchw;
    std::vector<float> &nchw = scratch ? scratch->nchw : localNchw;
    int outW = 0;
    uint64_t tPre0 = prof ? NowTicks() : 0;
    PreprocessCropToNchw(rgb, imgW, imgH, bx1, box.y1, bx2, box.y2, nchw, outW, 224, 0.65f);
    if (prof) {
        prof->preprocTicks += NowTicks() - tPre0;
        prof->boxCount += 1;
        prof->totalOutW += (uint32_t)outW;
    }
    if (!RunPreprocessed(nchw.data(), outW, model, outLine, err, allowDualCore, abortFlag, helperCore, scratch, prof)) return false;

    outLine.cropX1 = bx1;
    outLine.cropY1 = box.y1;
    outLine.cropX2 = bx2;
    outLine.cropY2 = box.y2;

    int ch = std::max(1, box.y2 - box.y1);
    int cw = std::max(1, bx2 - bx1);
    int targetW = std::max(16, std::min(224, (int)std::round((float)cw * 48.0f * 0.65f / (float)ch)));
    float unpadScale = (float)outW / (float)targetW;
    for (auto &cs : outLine.chars) {
        cs.xStartRatio = std::min(1.0f, cs.xStartRatio * unpadScale);
        cs.xEndRatio = std::min(1.0f, cs.xEndRatio * unpadScale);
    }
    return true;
}

bool RunBoxesBatch(const uint8_t *rgb, int imgW, int imgH,
                   const std::vector<TextBox> &boxes,
                   const LoadedModel &model,
                   std::vector<OcrLine> &outLines,
                   std::vector<uint8_t> &doneMask,
                   std::string &err,
                   std::atomic<int> *progressCur,
                   const std::atomic<bool> *abortFlag,
                   int helperCore,
                   OcrProfile *prof)
{
    const size_t n = boxes.size();
    outLines.resize(n);
    doneMask.resize(n, 0);

    RecScratch scratch;
    for (size_t i = 0; i < n; ++i) {
        if (doneMask[i]) continue;
        if (abortFlag && abortFlag->load(std::memory_order_relaxed)) {
            err = "aborted";
            return false;
        }
        OcrLine line;
        if (RunBox(rgb, imgW, imgH, boxes[i], model, line, err, true, abortFlag, helperCore, &scratch, prof)) {
            outLines[i] = std::move(line);
            doneMask[i] = 1;
            if (progressCur) progressCur->fetch_add(1, std::memory_order_release);
        } else if (abortFlag && abortFlag->load(std::memory_order_relaxed)) {
            return false;
        } else {
            doneMask[i] = 1;
            if (progressCur) progressCur->fetch_add(1, std::memory_order_release);
        }
    }
    return true;
}

bool RunWord(const uint8_t *rgb, int imgW, int imgH, int clickX, int clickY,
             const LoadedModel &model, OcrLine &outLine, int &outCharIdx, std::string &err,
             bool allowDualCore, const std::atomic<bool> *abortFlag, int helperCore)
{
    int searchX1 = std::max(0, clickX - 24);
    int searchX2 = std::min(imgW, clickX + 24);
    int y1 = 0, y2 = 0;
    TightenBandY(rgb, imgW, imgH, searchX1, searchX2, clickY, 16, y1, y2);

    // Keep crop aspect ratio <= 224/(48*0.65) so characters match Full OCR's 0.65x width scale
    int ch = std::max(1, y2 - y1);
    int halfW = (int)((float)ch * (224.0f / (48.0f * 0.65f)) * 0.5f);
    halfW = std::max(48, std::min(112, halfW));
    int x1 = std::max(0, clickX - halfW);
    int x2 = std::min(imgW, clickX + halfW);
    TrimCropX(rgb, imgW, imgH, y1, y2, clickX, x1, x2);
    {
        int tx1 = x1, tx2 = x2;
        TrimBoxEdgeBorders(rgb, imgW, imgH, tx1, y1, tx2, y2);
        if (clickX >= tx1 && clickX < tx2) {
            x1 = tx1;
            x2 = tx2;
        }
    }

    std::vector<float> nchw;
    int outW = 0;
    PreprocessCropToNchw(rgb, imgW, imgH, x1, y1, x2, y2, nchw, outW, 224, 0.65f);

    if (!RunPreprocessed(nchw.data(), outW, model, outLine, err, allowDualCore, abortFlag, helperCore)) return false;
    outLine.cropX1 = x1; outLine.cropY1 = y1;
    outLine.cropX2 = x2; outLine.cropY2 = y2;

    float clickRatio = (float)(clickX - x1) / (float)std::max(1, x2 - x1);
    // Account for right padding ratio
    int targetW = (int)std::round((float)(x2 - x1) * 48.0f * 0.65f / (float)std::max(1, y2 - y1));
    targetW = std::max(16, std::min(224, targetW));
    float scaledClickRatio = clickRatio * ((float)targetW / (float)outW);

    outCharIdx = 0;
    float bestDist = 1e9f;
    for (size_t i = 0; i < outLine.chars.size(); ++i) {
        float mid = 0.5f * (outLine.chars[i].xStartRatio + outLine.chars[i].xEndRatio);
        float d = std::fabs(mid - scaledClickRatio);
        if (d < bestDist) {
            bestDist = d;
            outCharIdx = (int)i;
        }
    }
    return true;
}

bool RunWord(const uint8_t *rgb, int imgW, int imgH, int clickX, int clickY,
             const char *modelPath, OcrLine &outLine, int &outCharIdx, std::string &err)
{
    LoadedModel model;
    if (!LoadModel(modelPath, model, err)) return false;
    return RunWord(rgb, imgW, imgH, clickX, clickY, model, outLine, outCharIdx, err, true, nullptr);
}

bool RunRegion(const uint8_t *rgb, int imgW, int imgH,
               int x1, int y1, int x2, int y2,
               const LoadedModel &model, std::vector<OcrLine> &outLines, std::string &err,
               bool allowDualCore, const std::atomic<bool> *abortFlag, int helperCore)
{
    outLines.clear();
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(imgW, x2); y2 = std::min(imgH, y2);
    if (x2 - x1 < 4 || y2 - y1 < 4) {
        err = "Selection box too small";
        return false;
    }

    // Detect horizontal text lines via row contrast projection
    int bh = y2 - y1;
    std::vector<float> rowEnergy(bh, 0.0f);
    float maxE = 0.0f;
    for (int y = y1; y < y2; ++y) {
        float e = 0.0f;
        const uint8_t *row = rgb + (size_t)y * imgW * 3;
        for (int x = x1 + 1; x < x2; ++x) {
            int l0 = row[(x - 1) * 3] + row[(x - 1) * 3 + 1] + row[(x - 1) * 3 + 2];
            int l1 = row[x * 3] + row[x * 3 + 1] + row[x * 3 + 2];
            int d = l1 - l0;
            if (d < 0) d = -d;
            if (d > 50) e += 1.0f;
        }
        rowEnergy[y - y1] = e;
        if (e > maxE) maxE = e;
    }

    struct Band { int y0, y1; };
    std::vector<Band> bands;
    if (maxE >= 2.0f && bh > 28) {
        float thresh = maxE * 0.12f;
        int i = 0;
        while (i < bh) {
            while (i < bh && rowEnergy[i] < thresh) ++i;
            if (i >= bh) break;
            int s = i;
            while (i < bh && rowEnergy[i] >= thresh) ++i;
            int e = i;
            if (e - s >= 5) {
                if (!bands.empty() && s - bands.back().y1 <= 2) {
                    bands.back().y1 = e;
                } else {
                    bands.push_back({s, e});
                }
            }
        }
    }

    if (bands.empty()) {
        bands.push_back({0, bh});
    }

    RecScratch scratch;
    for (const Band &b : bands) {
        int ly1 = std::max(y1, y1 + b.y0 - 2);
        int ly2 = std::min(y2, y1 + b.y1 + 3);
        int outW = 0;
        PreprocessCropToNchw(rgb, imgW, imgH, x1, ly1, x2, ly2, scratch.nchw, outW, 256, 0.65f);
        OcrLine line;
        if (!RunPreprocessed(scratch.nchw.data(), outW, model, line, err, allowDualCore, abortFlag, helperCore, &scratch)) return false;
        line.cropX1 = x1; line.cropY1 = ly1;
        line.cropX2 = x2; line.cropY2 = ly2;
        if (!line.text.empty()) {
            outLines.push_back(std::move(line));
        }
    }
    return true;
}

bool RunRegion(const uint8_t *rgb, int imgW, int imgH,
               int x1, int y1, int x2, int y2,
               const char *modelPath, std::vector<OcrLine> &outLines, std::string &err)
{
    LoadedModel model;
    if (!LoadModel(modelPath, model, err)) return false;
    return RunRegion(rgb, imgW, imgH, x1, y1, x2, y2, model, outLines, err, true, nullptr);
}

} // namespace OfflineOcr
