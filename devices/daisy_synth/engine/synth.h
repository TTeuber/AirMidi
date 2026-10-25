// An eight-voice subtractive synth: two detuned oscillators (saw, square or triangle), a resonant low-pass with
// keyboard tracking, one ADSR for the filter and the amp, glide, vibrato, then chorus, echo and reverb.
//
// Threads: noteOn/noteOff/sustain/allNotesOff/setParam/bend/setMpe are called from one thread (the main loop);
// process() runs in the audio callback. Note events cross over through a single-producer ring, parameters and
// bends as plain floats.
//
// MPE (setMpe(true)): every voice belongs to the channel its note came on; a note off only releases its own
// channel's note, each voice follows its channel's bend plus the manager channel's (channel index 0), and glide
// between voices is off (each strip of the app is its own channel and moves by bend).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "controls.h"
#include "dsp.h"
#include "fx.h"

namespace syn {

class Synth {
  public:
    static constexpr int kVoices = 8;
    static constexpr size_t kEchoFloats = Echo::kMemFloats;
    static constexpr size_t kRoomFloats = Room::kMemFloats;

    // echoMem and roomMem hold kEchoFloats and kRoomFloats (cleared here; the Seed puts them in SDRAM).
    void init(float sr, float *echoMem, float *roomMem);

    // ch: 0-based MIDI channel; it only matters in MPE mode
    void noteOn(uint8_t note, uint8_t velocity, uint8_t ch = 0);  // velocity 0 = note off
    void noteOff(uint8_t note, uint8_t ch = 0);
    void sustain(bool down);
    void allNotesOff();
    void setParam(int p, float v) { param_[p] = clampf(v, 0.f, 1.f); }
    float param(int p) const { return param_[p]; }
    void bend(float semitones, uint8_t ch = 0) { bend_[ch & 15] = semitones; }
    float bendOf(uint8_t ch) const { return bend_[ch & 15]; }
    void setMpe(bool on) { mpe_ = on; }
    bool mpe() const { return mpe_; }

    void process(float *outL, float *outR, size_t n);

    // For the LEDs: the note voice i plays (-1 if silent) and its envelope level.
    int voiceNote(int i) const { return v_[i].note; }
    float voiceLevel(int i) const { return v_[i].env; }
    // For the tests: the pitch voice i sounds now (note + glide + bend, no vibrato) and its channel.
    float voicePitch(int i) const { return v_[i].pitch + v_[i].bend; }
    int voiceChannel(int i) const { return v_[i].ch; }
    bool voiceGate(int i) const { return v_[i].gate; }

  private:
    struct Voice {
        int note = -1;
        uint8_t ch = 0;
        float vel = 0.f;
        float bend = 0.f;  // semitones, smoothed toward its channel's bend
        bool gate = false, held = false;
        uint32_t age = 0;
        float pitch = 60.f, target = 60.f;  // semitones
        float ph1 = 0.f, ph2 = 0.f, tri1 = 0.f, tri2 = 0.f;
        int stage = 0;  // 0 idle, 1 attack, 2 decay/sustain, 3 release
        float env = 0.f;
        Svf f;
    };

    struct Cmd {
        uint8_t type, note, vel, ch;
    };
    enum { kOn, kOff, kSustainOn, kSustainOff, kAllOff };
    static constexpr uint32_t kCmds = 64;

    void push(uint8_t type, uint8_t note = 0, uint8_t vel = 0, uint8_t ch = 0);
    void apply(const Cmd &c);
    void start(uint8_t note, uint8_t vel, uint8_t ch);
    void release(uint8_t note, uint8_t ch);
    float bendFor(uint8_t ch) const { return mpe_ && ch ? bend_[0] + bend_[ch] : bend_[0]; }
    void render(float *l, float *r, int n);

    float sr_ = 48000.f;
    Voice v_[kVoices];
    uint32_t clock_ = 0;
    float lastPitch_ = -1.f;
    bool sustain_ = false;
    volatile float param_[kParams] = {};
    volatile float bend_[16] = {};  // semitones per channel; without MPE only [0] is used
    volatile bool mpe_ = false;
    float sm_[kParams] = {};  // smoothed copies, per sub-block
    float lfo_ = 0.f;

    Cmd cmd_[kCmds];
    std::atomic<uint32_t> cmdW_{0}, cmdR_{0};

    Chorus chorus_;
    Echo echo_;
    Room room_;
};

}  // namespace syn
