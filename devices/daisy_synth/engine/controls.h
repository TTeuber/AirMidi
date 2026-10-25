// The synth's parameters, the one table everything else reads: which CC sets each one, what the knobs on the kit
// edit, and what the box's web page draws (the MIDI-CI AllCtrlList and its CtrlMapLists are generated from it).
#pragma once
#include <cstddef>
#include <cstdint>

namespace syn {

enum Param : uint8_t {
    // the kit's pots VR1-VR8, in order
    kCutoff,
    kReso,
    kEnvAmt,
    kAttack,
    kDecay,
    kSustain,
    kRelease,
    kSpace,
    // only on the page (or any controller sending the CC)
    kDetune,
    kVibrato,
    kGlideTime,
    kEcho,
    // switches: kWave = SW18 (three positions), kGlide = SW17
    kWave,
    kGlide,
    kParams
};

constexpr int kPots = 8;
constexpr int kWaves = 3;  // saw, square, triangle

struct Control {
    const char *title;
    const char *path;   // M2-117 paramPath
    uint8_t cc;         // channel 1 on the page; the synth listens on every channel
    uint8_t def;        // default, 0-127
    uint8_t priority;   // the page sorts by it
    const char *hint;   // M2-117 typeHint, or nullptr for a fader
    const char *mapId;  // CtrlMapList resId, or nullptr
};

extern const Control kControls[kParams];

// -1 if no parameter listens to this CC.
int paramForCc(uint8_t cc);

// A CC value as the parameter's 0..1 value, and back. Stepped and on/off parameters use the spacing the page uses
// (step k at k / steps of the range, on at the top half).
float ccToValue(int p, uint8_t cc);
uint8_t valueToCc(int p, float v);
int waveIndex(float v);  // kWave's value -> 0..kWaves-1

// Parameter curves (0..1 in), shared by the synth and the value names on the page.
float cutoffHz(float x);    // 20 Hz .. 20 kHz
float attackSec(float x);   // 1 ms .. 3 s
float timeSec(float x);     // decay and release: 5 ms .. 6 s
float glideSec(float x);    // 5 ms .. 1 s

// MIDI-CI resources (M2-117 JSON). Each build* writes a terminated string and returns its length, 0 if cap is too
// small.
extern const char *const kDeviceInfo;
size_t buildCtrlList(char *out, size_t cap);
size_t buildCtrlMap(const char *mapId, char *out, size_t cap);
constexpr int kMaps = 4;
extern const char *const kMapIds[kMaps];

}  // namespace syn
