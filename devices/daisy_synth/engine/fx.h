// The master effects: chorus (always on, for width), a ping-pong echo and a reverb.
#pragma once
#include <cstddef>

#include "dsp.h"

namespace syn {

// Two delay taps swept in opposite directions by one slow triangle, the way a Juno's chorus widens a mono voice.
class Chorus {
   public:
    static constexpr int kLen = 1024;  // 21 ms at 48 kHz
    void init(float sr);
    void process(float in, float &l, float &r);

   private:
    float buf_[kLen] = {};
    int pos_ = 0;
    float sr_ = 48000.f, ph_ = 0.f, inc_ = 0.f;
    float read(float delaySamples) const;
};

// Ping-pong echo: the input enters the left line, each line feeds the other, the feedback is band-limited.
class Echo {
   public:
    static constexpr size_t kLineLen = 24000;  // 0.5 s at 48 kHz
    static constexpr size_t kMemFloats = 2 * kLineLen;
    void init(float sr, float *mem);  // mem: kMemFloats, cleared here (the Seed puts it in SDRAM)
    void set(float amount);           // 0..1: wet level and feedback together
    void process(float in, float &l, float &r);

   private:
    float *line_[2] = {};
    size_t len_ = 0, pos_ = 0;
    float fb_ = 0.f, wet_ = 0.f;
    OnePole lp_[2], hp_[2];
};

// Eight-line feedback delay network (Hadamard mix, one-pole damping per line, two input diffusers): a soft hall.
class Room {
   public:
    static constexpr int kLines = 8;
    static constexpr int kLineLen = 8192;
    static constexpr size_t kMemFloats = kLines * kLineLen + 4096;  // + predelay and diffusers
    void init(float sr, float *mem);                                // mem: kMemFloats, cleared here
    void set(float size01, float dampHz);
    void process(float in, float &l, float &r);

   private:
    float sr_ = 48000.f;
    float *line_[kLines] = {};
    int len_[kLines] = {}, pos_[kLines] = {};
    float gain_[kLines] = {}, lpz_[kLines] = {};
    float lpa_ = 0.5f;
    float *pre_ = nullptr;
    int preLen_ = 0, prePos_ = 0;
    float *ap_[2] = {};
    int apLen_[2] = {}, apPos_[2] = {};
    OnePole inHp_;
};

}  // namespace syn
