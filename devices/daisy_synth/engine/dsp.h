// Small DSP helpers for the synth. Plain C++17 with no platform headers, so the same code runs on the Seed and in the
// host renderer. No libm: the Seed's 128 KB of internal flash is mostly libDaisy, and powf/expf/tanf with their
// range reduction cost ~7 KB.
#pragma once
#include <cstdint>

namespace syn {

constexpr float kPi = 3.14159265f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline float floorf2(float x) {
    float t = (float)(int32_t)x;
    return t > x ? t - 1.f : t;
}

// 2^x to ~1e-4 relative error (0.2 cent as a pitch).
inline float exp2f2(float x) {
    x = clampf(x, -60.f, 60.f);
    float fl = floorf2(x), f = x - fl;
    float p = 1.f + f * (0.69583356f + f * (0.22606716f + f * 0.078024521f));
    union {
        float f;
        int32_t i;
    } u;
    u.f = p;
    u.i += (int32_t)fl << 23;
    return u.f;
}

// e^x. Envelope coefficients sit just below 1 (e^-0.00003), where a relative error of 1e-4 would change a 5 s
// release into a much longer one, so small arguments use a Taylor series (error < 3e-9).
inline float expf2(float x) {
    if (x > -0.05f && x < 0.05f) return 1.f + x * (1.f + x * (0.5f + x * (0.16666667f + x * 0.041666668f)));
    return exp2f2(x * 1.44269504f);
}

// Multiplier per sample for a time constant (to 1/e) of sec seconds.
inline float tauCoef(float sec, float sr) { return sec <= 0.f ? 0.f : expf2(-1.f / (sec * sr)); }

// tan(x) for 0 <= x < pi/2 (a filter's prewarped cutoff): sin/cos by Taylor series, error < 1e-6 up to 0.45 * fs.
inline float tanf2(float x) {
    float x2 = x * x;
    float s = x * (1.f + x2 * (-0.16666667f + x2 * (0.0083333333f + x2 * (-0.00019841270f + x2 * 2.7557319e-6f))));
    float c = 1.f + x2 * (-0.5f + x2 * (0.041666667f + x2 * (-0.0013888889f + x2 * (2.4801587e-5f - x2 * 2.7557319e-7f))));
    return s / c;
}

// Rational tanh, exact at 0 and saturating at +-1 beyond |x| = 3.
inline float softclip(float x) {
    x = clampf(x, -3.f, 3.f);
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

// PolyBLEP: the band-limited step correction around a discontinuity at phase 0, t = phase, dt = increment.
inline float polyblep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.f;
    }
    if (t > 1.f - dt) {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}

// Two-pole state-variable filter, Simper's trapezoidal form: stays stable when the cutoff moves every few samples.
struct Svf {
    float a1 = 0, a2 = 0, a3 = 0, k = 1, ic1 = 0, ic2 = 0;
    void set(float hz, float q, float sr) {
        hz = clampf(hz, 10.f, 0.45f * sr);
        float g = tanf2(kPi * hz / sr);
        k = 1.f / q;
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    float lp(float v0) {
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        return v2;
    }
    void reset() { ic1 = ic2 = 0.f; }
};

struct OnePole {
    float a = 1.f, z = 0.f;
    void setHz(float hz, float sr) { a = 1.f - expf2(-2.f * kPi * hz / sr); }
    float lp(float x) { return z += a * (x - z); }
    float hp(float x) { return x - lp(x); }
};

}  // namespace syn
