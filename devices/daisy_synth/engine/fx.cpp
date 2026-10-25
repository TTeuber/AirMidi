#include "fx.h"

namespace syn {

// ---- chorus ---------------------------------------------------------------------------------------------------------

void Chorus::init(float sr) {
    sr_ = sr;
    inc_ = 0.45f / sr;  // sweep rate, Hz
    for (float &x : buf_) x = 0.f;
}

float Chorus::read(float d) const {
    float p = (float)pos_ - d;
    if (p < 0.f) p += kLen;
    int i = (int)p;
    float f = p - (float)i;
    int j = i + 1 < kLen ? i + 1 : 0;
    return buf_[i] + f * (buf_[j] - buf_[i]);
}

void Chorus::process(float in, float &l, float &r) {
    buf_[pos_] = in;
    ph_ += inc_;
    if (ph_ >= 1.f) ph_ -= 1.f;
    float tri = ph_ < 0.5f ? 4.f * ph_ - 1.f : 3.f - 4.f * ph_;  // -1..1
    float centre = 0.0065f * sr_, depth = 0.0028f * sr_;
    float a = read(centre + depth * tri), b = read(centre - depth * tri);
    l = 0.75f * in + 0.55f * a;
    r = 0.75f * in + 0.55f * b;
    if (++pos_ >= kLen) pos_ = 0;
}

// ---- echo -----------------------------------------------------------------------------------------------------------

void Echo::init(float sr, float *mem) {
    for (size_t i = 0; i < kMemFloats; i++) mem[i] = 0.f;
    line_[0] = mem;
    line_[1] = mem + kLineLen;
    len_ = (size_t)(0.36f * sr);  // one repeat every 360 ms, alternating sides
    if (len_ > kLineLen) len_ = kLineLen;
    pos_ = 0;
    for (int k = 0; k < 2; k++) lp_[k].setHz(3200.f, sr), hp_[k].setHz(180.f, sr);
    set(0.f);
}

void Echo::set(float a) {
    fb_ = 0.25f + 0.45f * a;
    wet_ = 0.55f * a;
}

void Echo::process(float in, float &l, float &r) {
    float a = line_[0][pos_], b = line_[1][pos_];
    line_[0][pos_] = in * (wet_ > 0.f ? 1.f : 0.f) + fb_ * hp_[0].hp(lp_[0].lp(b));
    line_[1][pos_] = fb_ * hp_[1].hp(lp_[1].lp(a));
    if (++pos_ >= len_) pos_ = 0;
    l = wet_ * a;
    r = wet_ * b;
}

// ---- room -----------------------------------------------------------------------------------------------------------

namespace {
// Line lengths at 48 kHz (37..97 ms), mutually prime so the echoes do not line up.
const int kLens[Room::kLines] = {1777, 2129, 2539, 2903, 3313, 3769, 4153, 4651};
const int kPreMax = 2048, kAp0Max = 1024, kAp1Max = 1024;
}  // namespace

void Room::init(float sr, float *mem) {
    sr_ = sr;
    for (size_t i = 0; i < kMemFloats; i++) mem[i] = 0.f;
    for (int i = 0; i < kLines; i++) {
        line_[i] = mem + i * kLineLen;
        int n = (int)(kLens[i] * sr / 48000.f);
        len_[i] = n < kLineLen ? n : kLineLen - 1;
    }
    float *extra = mem + kLines * kLineLen;
    pre_ = extra;
    preLen_ = (int)(0.02f * sr);  // 20 ms predelay keeps the attack clear of the tail
    ap_[0] = extra + kPreMax;
    apLen_[0] = (int)(331 * sr / 48000.f);
    ap_[1] = extra + kPreMax + kAp0Max;
    apLen_[1] = (int)(797 * sr / 48000.f);
    if (preLen_ > kPreMax - 1) preLen_ = kPreMax - 1;
    if (apLen_[0] > kAp0Max - 1) apLen_[0] = kAp0Max - 1;
    if (apLen_[1] > kAp1Max - 1) apLen_[1] = kAp1Max - 1;
    inHp_.setHz(200.f, sr);
    set(0.5f, 5000.f);
}

void Room::set(float size01, float dampHz) {
    float rt60 = 0.8f + 4.2f * size01 * size01;
    for (int i = 0; i < kLines; i++) gain_[i] = exp2f2(-60.f * 0.16609640f * len_[i] / (sr_ * rt60));
    lpa_ = 1.f - expf2(-2.f * kPi * dampHz / sr_);
}

void Room::process(float in, float &l, float &r) {
    float x = pre_[prePos_];
    pre_[prePos_] = inHp_.hp(in);
    if (++prePos_ >= preLen_) prePos_ = 0;

    for (int k = 0; k < 2; k++) {  // diffusers
        float *b = ap_[k];
        float d = b[apPos_[k]], o = d - 0.6f * x;
        b[apPos_[k]] = x + 0.6f * o;
        if (++apPos_[k] >= apLen_[k]) apPos_[k] = 0;
        x = o;
    }

    float h[kLines];
    for (int i = 0; i < kLines; i++) {
        lpz_[i] += lpa_ * (line_[i][pos_[i]] - lpz_[i]);
        h[i] = lpz_[i];
    }
    l = (h[0] - h[2] + h[4] - h[6]) * 0.5f;
    r = (h[1] - h[3] + h[5] - h[7]) * 0.5f;

    // fast Walsh-Hadamard transform: an orthogonal (lossless) mix of all lines
    for (int w = 1; w < kLines; w <<= 1)
        for (int i = 0; i < kLines; i += w << 1)
            for (int j = i; j < i + w; j++) {
                float a = h[j], b = h[j + w];
                h[j] = a + b;
                h[j + w] = a - b;
            }
    const float norm = 0.35355339f;  // 1/sqrt(8)
    for (int i = 0; i < kLines; i++) {
        line_[i][pos_[i]] = (i & 1 ? -x : x) * 0.35f + gain_[i] * h[i] * norm;
        if (++pos_[i] >= len_[i]) pos_[i] = 0;
    }
}

}  // namespace syn
