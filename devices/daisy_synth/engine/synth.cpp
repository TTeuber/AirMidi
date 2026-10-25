#include "synth.h"

namespace syn {

namespace {
constexpr int kSub = 16;  // samples per control update: filter cutoff, pitch, envelope rates
constexpr float kVoiceGain = 0.3f;
// Bends arrive as steps (every 10-15 ms over BLE); a 3 ms time constant per sub-block hides the steps without
// lagging a slide.
constexpr float kBendSmooth = 0.105f;  // 1 - exp(-(16 / 48000) / 0.003)

inline float frac(float x) { return x >= 1.f ? x - 1.f : x; }

// One oscillator sample at phase ph (0..1) and increment dt. tri is the triangle's integrator state.
inline float osc(int wave, float ph, float dt, float &tri) {
    if (wave == 0) return 2.f * ph - 1.f - polyblep(ph, dt);
    float sq = (ph < 0.5f ? 1.f : -1.f) + polyblep(ph, dt) - polyblep(frac(ph + 0.5f), dt);
    if (wave == 1) return 0.8f * sq;
    // triangle: the band-limited square, integrated (leaky, so no DC builds up)
    tri = 0.9998f * tri + 4.f * dt * sq;
    return tri;
}
}  // namespace

void Synth::init(float sr, float *echoMem, float *roomMem) {
    sr_ = sr;
    for (int p = 0; p < kParams; p++) param_[p] = sm_[p] = ccToValue(p, kControls[p].def);
    chorus_.init(sr);
    echo_.init(sr, echoMem);
    room_.init(sr, roomMem);
}

// ---- main loop side -------------------------------------------------------------------------------------------------

void Synth::push(uint8_t type, uint8_t note, uint8_t vel, uint8_t ch) {
    uint32_t w = cmdW_.load(std::memory_order_relaxed);
    if (w - cmdR_.load(std::memory_order_acquire) >= kCmds) return;  // full: dropped (64 events in one block)
    cmd_[w % kCmds] = {type, note, vel, ch};
    cmdW_.store(w + 1, std::memory_order_release);
}

void Synth::noteOn(uint8_t note, uint8_t vel, uint8_t ch) { vel ? push(kOn, note, vel, ch) : push(kOff, note, 0, ch); }
void Synth::noteOff(uint8_t note, uint8_t ch) { push(kOff, note, 0, ch); }
void Synth::sustain(bool down) { push(down ? kSustainOn : kSustainOff); }
void Synth::allNotesOff() { push(kAllOff); }

// ---- audio side -----------------------------------------------------------------------------------------------------

void Synth::apply(const Cmd &c) {
    switch (c.type) {
        case kOn:
            start(c.note, c.vel, c.ch);
            break;
        case kOff:
            release(c.note, c.ch);
            break;
        case kSustainOn:
            sustain_ = true;
            break;
        case kSustainOff:
            sustain_ = false;
            for (Voice &v : v_)
                if (v.held) v.held = false, v.stage = v.stage ? 3 : 0;
            break;
        case kAllOff:
            sustain_ = false;
            for (Voice &v : v_) {
                v.gate = v.held = false;
                if (v.stage) v.stage = 3;
            }
            break;
    }
}

void Synth::start(uint8_t note, uint8_t vel, uint8_t ch) {
    // the same note still sounding (on the same channel, in MPE) takes the new hit; otherwise a silent voice, else
    // the oldest released one, else the oldest held by the pedal, else the oldest playing one
    const bool mpe = mpe_;
    Voice *pick = nullptr;
    for (Voice &v : v_)
        if (v.stage && v.note == note && (!mpe || v.ch == ch)) pick = &v;
    if (!pick) {
        uint64_t best = UINT64_MAX;
        for (Voice &v : v_) {
            uint64_t rank = !v.stage ? 0 : (uint64_t)(v.gate ? 3 : v.held ? 2 : 1) << 32 | v.age;
            if (rank < best) best = rank, pick = &v;
        }
    }
    Voice &v = *pick;
    bool glide = !mpe && param_[kGlide] >= 0.5f && lastPitch_ >= 0.f;
    if (!v.stage) {
        v.ph1 = 0.f;
        v.ph2 = 0.37f;  // the second oscillator starts out of phase: no doubled attack while detune is low
        v.tri1 = -1.f, v.tri2 = 4.f * 0.37f - 1.f;  // where a triangle is at those phases
        v.f.reset();
        v.env = 0.f;
    }
    v.pitch = glide ? lastPitch_ : (float)note;
    v.target = (float)note;
    v.note = note;
    v.ch = mpe ? ch : 0;
    v.bend = bendFor(v.ch);  // the bend sent just before the note on: the note starts at its pitch
    v.vel = vel * (1.f / 127.f);
    v.gate = true;
    v.held = false;
    v.stage = 1;  // the attack starts from the current level, so a retriggered or stolen voice does not click
    v.age = ++clock_;
    lastPitch_ = (float)note;
}

void Synth::release(uint8_t note, uint8_t ch) {
    const bool mpe = mpe_;
    for (Voice &v : v_) {
        if (v.note != note || !v.gate || (mpe && v.ch != ch)) continue;
        v.gate = false;
        if (sustain_)
            v.held = true;
        else
            v.stage = 3;
    }
}

void Synth::process(float *outL, float *outR, size_t n) {
    uint32_t r = cmdR_.load(std::memory_order_relaxed);
    while (r != cmdW_.load(std::memory_order_acquire)) apply(cmd_[r++ % kCmds]);
    cmdR_.store(r, std::memory_order_release);

    echo_.set(sm_[kEcho]);
    room_.set(0.35f + 0.65f * sm_[kSpace], 3500.f + 3500.f * (1.f - sm_[kSpace]));
    for (size_t i = 0; i < n; i += kSub) render(outL + i, outR + i, n - i < kSub ? (int)(n - i) : kSub);
}

void Synth::render(float *outL, float *outR, int n) {
    for (int p = 0; p < kParams; p++) sm_[p] += 0.12f * (param_[p] - sm_[p]);  // ~2.5 ms at 48 kHz
    const int wave = waveIndex(param_[kWave]);
    const bool glide = param_[kGlide] >= 0.5f;
    const float gk = 1.f - tauCoef(glideSec(sm_[kGlideTime]) / 3.f, sr_ / kSub);

    const float atkInc = 1.f / (attackSec(sm_[kAttack]) * sr_);
    const float decCoef = tauCoef(timeSec(sm_[kDecay]) / 3.f, sr_);  // the time reads as "down to 5 %"
    const float relCoef = tauCoef(timeSec(sm_[kRelease]) / 3.f, sr_);
    const float sus = sm_[kSustain];

    lfo_ += kSub * 5.5f / sr_;
    if (lfo_ >= 1.f) lfo_ -= 1.f;
    const float tri = lfo_ < 0.5f ? 4.f * lfo_ - 1.f : 3.f - 4.f * lfo_;
    const float vib = sm_[kVibrato] * sm_[kVibrato] * 0.6f * tri;
    const float cents = 22.f * sm_[kDetune];
    const float up = exp2f2(cents / 1200.f), down = 1.f / up;

    const float reso = sm_[kReso];
    const float q = 0.6f + 11.f * reso * reso;
    const float gIn = 1.f / (1.f + 0.8f * reso);  // resonance boosts the peak; keep the level roughly even
    const float base = cutoffHz(sm_[kCutoff]);
    const float envOct = 7.f * sm_[kEnvAmt];

    float mono[kSub] = {};
    for (Voice &v : v_) {
        if (!v.stage) continue;
        v.pitch = glide ? v.pitch + gk * (v.target - v.pitch) : v.target;
        v.bend += kBendSmooth * (bendFor(v.ch) - v.bend);
        const float played = v.pitch + v.bend;
        float hz = 440.f * exp2f2((played + vib - 69.f) * (1.f / 12.f));
        float dt1 = clampf(hz * down / sr_, 0.f, 0.45f), dt2 = clampf(hz * up / sr_, 0.f, 0.45f);
        float oct = envOct * v.env * (0.6f + 0.4f * v.vel) + 0.5f * (played - 60.f) * (1.f / 12.f);
        v.f.set(base * exp2f2(oct), q, sr_);
        const float amp = kVoiceGain * (0.35f + 0.65f * v.vel);

        for (int s = 0; s < n; s++) {
            switch (v.stage) {
                case 1:
                    v.env += atkInc;
                    if (v.env >= 1.f) v.env = 1.f, v.stage = 2;
                    break;
                case 2:
                    v.env = sus + (v.env - sus) * decCoef;
                    break;
                case 3:
                    v.env *= relCoef;
                    break;
            }
            float x = 0.5f * (osc(wave, v.ph1, dt1, v.tri1) + osc(wave, v.ph2, dt2, v.tri2));
            mono[s] += v.f.lp(x * gIn) * v.env * amp;
            v.ph1 = frac(v.ph1 + dt1);
            v.ph2 = frac(v.ph2 + dt2);
        }
        if (v.stage == 3 && v.env < 1e-4f) {
            v.stage = 0;
            v.env = 0.f;
            v.note = -1;
            v.held = false;
        }
    }

    const float send = 0.75f * sm_[kSpace];
    for (int s = 0; s < n; s++) {
        float cl, cr, el, er, rl, rr;
        chorus_.process(mono[s], cl, cr);
        echo_.process(mono[s], el, er);
        room_.process(send * (mono[s] + 0.5f * (el + er)), rl, rr);
        outL[s] = 0.92f * softclip(cl + el + 0.7f * rl);
        outR[s] = 0.92f * softclip(cr + er + 0.7f * rr);
    }
}

}  // namespace syn
