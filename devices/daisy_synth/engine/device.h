// The synth as a MIDI device: MIDI 1.0 bytes from the DIN input in (notes, CCs, pitch bend, RPNs, and SysEx for the
// MIDI-CI responder from lib/), MIDI 1.0 bytes for the DIN output out (the responder's replies, and the notes and
// CCs of the kit's own buttons, pots and toggles, so the box's page follows the hardware).
//
// MIDI-CI MIDI Message Report (M2-101 9): asked for current values, it sends channel 1's pitch bend and every
// parameter's CC with its current value (ascending CC number, as the spec orders them), so the box's page and the
// phone show where the knobs are rather than the defaults the control list carries.
//
// MPE: the MPE Configuration Message (RPN 6 on channel 1) turns a lower zone on with that many member channels
// (0 turns it off again); member channels then bend ±48 semitones and the manager ±2 until RPN 0 says otherwise.
// Upper zones (manager on channel 16) are not supported.
//
// Platform-free: the firmware feeds it UART bytes, the host test feeds it the initiator's messages.
#pragma once
#include <cstddef>
#include <cstdint>

#include "airmidi/midici_responder.h"
#include "controls.h"
#include "synth.h"

namespace syn {

class Device {
  public:
    using Out = void (*)(void *ctx, const uint8_t *bytes, size_t n);

    // muid: a fresh random one at every power-up (M2-101). False if a resource did not fit its buffer.
    bool init(Synth *synth, uint32_t muid, Out out, void *ctx);

    void receive(uint8_t byte);  // one byte from MIDI IN

    // The kit's controls: change a parameter (sends its CC when the 7-bit value changes), play a note.
    void localParam(int p, float v);
    void localNote(uint8_t note, bool on);

    // Parameters a CC from MIDI IN has set since the last call (bit p), so a pot stops holding that one.
    uint32_t takeRemote() {
        uint32_t m = remote_;
        remote_ = 0;
        return m;
    }

    uint32_t muid() const { return responder_.muid; }
    int mpeMembers() const { return mpeMembers_; }  // 0 = MPE off
    uint32_t ciReplies() const { return ciReplies_; }  // MIDI-CI messages sent (for the log)

  private:
    static void sendSysex(void *ctx, const uint8_t *sx, size_t n);
    static void report(void *ctx, int channel, uint8_t types, bool changedOnly);
    void dispatch();
    void send3(uint8_t st, uint8_t d1, uint8_t d2);
    void controlChange(uint8_t ch, uint8_t cc, uint8_t v);
    void dataEntry(uint8_t ch, bool msb, uint8_t v);

    Synth *synth_ = nullptr;
    Out out_ = nullptr;
    void *ctx_ = nullptr;

    // MIDI 1.0 parser
    uint8_t status_ = 0, data_[2] = {}, n_ = 0;
    bool inSysex_ = false, sysexOverflow_ = false;
    uint8_t sysex_[MIDICI_RESPONDER_OUT];
    size_t sysexLen_ = 0;

    uint8_t lastCc_[kParams] = {};  // last value sent or received per parameter

    // RPNs per channel: the selected parameter (127/127 = none) and each channel's bend range
    uint8_t rpnMsb_[16], rpnLsb_[16];
    float bendRange_[16];
    int mpeMembers_ = 0;
    uint32_t remote_ = 0, ciReplies_ = 0;

    midici_responder_t responder_;
    midici_resource_t res_[2 + kMaps];
    char ctrlList_[3072];
    char maps_[kMaps][1024];
};

}  // namespace syn
