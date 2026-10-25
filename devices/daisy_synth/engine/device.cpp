#include "device.h"

namespace syn {

namespace {
constexpr float kBendRange = 2.f;        // semitones, the MIDI default
constexpr float kMemberBendRange = 48.f;  // semitones, the MPE default for member channels
}

bool Device::init(Synth *synth, uint32_t muid, Out out, void *ctx) {
    synth_ = synth;
    out_ = out;
    ctx_ = ctx;
    for (int p = 0; p < kParams; p++) lastCc_[p] = kControls[p].def;
    for (int c = 0; c < 16; c++) rpnMsb_[c] = rpnLsb_[c] = 127, bendRange_[c] = kBendRange;

    bool ok = buildCtrlList(ctrlList_, sizeof ctrlList_) > 0;
    int n = 0;
    res_[n++] = {"DeviceInfo", nullptr, kDeviceInfo};
    res_[n++] = {"AllCtrlList", nullptr, ctrlList_};
    for (int i = 0; i < kMaps; i++) {
        ok &= buildCtrlMap(kMapIds[i], maps_[i], sizeof maps_[i]) > 0;
        res_[n++] = {"CtrlMapList", kMapIds[i], maps_[i]};
    }
    // manufacturer 7D 00 00 (educational / development), family 0, model 3 (the Mac stand-in is model 2), v0.1
    const midici_ident_t id = {{0x7D, 0, 0}, 0, 3, {0, 1, 0, 0}};
    ok &= midici_responder_init(&responder_, midici_muid(muid), &id, res_, n, sendSysex, this);
    return ok;
}

void Device::sendSysex(void *ctx, const uint8_t *sx, size_t n) {
    Device *d = static_cast<Device *>(ctx);
    const uint8_t f0 = 0xF0, f7 = 0xF7;
    d->out_(d->ctx_, &f0, 1);
    d->out_(d->ctx_, sx, n);
    d->out_(d->ctx_, &f7, 1);
    d->ciReplies_++;
}

void Device::send3(uint8_t st, uint8_t d1, uint8_t d2) {
    const uint8_t m[3] = {st, d1, d2};
    out_(ctx_, m, 3);
}

void Device::receive(uint8_t b) {
    if (b >= 0xF8) return;  // real time: may sit anywhere, even inside a SysEx
    if (b == 0xF0) {
        inSysex_ = true;
        sysexOverflow_ = false;
        sysexLen_ = 0;
        status_ = 0;
        return;
    }
    if (b == 0xF7) {
        if (inSysex_ && !sysexOverflow_) midici_responder_receive(&responder_, sysex_, sysexLen_);
        inSysex_ = false;
        return;
    }
    if (b & 0x80) {           // any other status ends a SysEx (an unterminated one is dropped)
        inSysex_ = false;
        status_ = b < 0xF0 ? b : 0;  // system common cancels running status; its data bytes are skipped
        n_ = 0;
        return;
    }
    if (inSysex_) {
        if (sysexLen_ < sizeof sysex_)
            sysex_[sysexLen_++] = b;
        else
            sysexOverflow_ = true;
        return;
    }
    if (!status_) return;
    data_[n_++] = b;
    uint8_t type = status_ & 0xF0;
    if (n_ < (type == 0xC0 || type == 0xD0 ? 1 : 2)) return;
    n_ = 0;  // running status: the next data bytes reuse status_
    dispatch();
}

// Every channel: the page sends on channel 1, a keyboard behind the box on whatever it is set to. Without MPE
// every channel plays the same voices and bends all of them; with MPE each channel keeps its own.
void Device::dispatch() {
    const uint8_t type = status_ & 0xF0, ch = status_ & 0x0F, d1 = data_[0], d2 = data_[1];
    const bool mpe = mpeMembers_ > 0;
    switch (type) {
        case 0x90:
            synth_->noteOn(d1, d2, ch);
            break;
        case 0x80:
            synth_->noteOff(d1, ch);
            break;
        case 0xB0:
            controlChange(ch, d1, d2);
            break;
        case 0xE0:
            synth_->bend(((d2 << 7 | d1) - 8192) * (bendRange_[ch] / 8192.f), mpe ? ch : 0);
            break;
    }
}

void Device::controlChange(uint8_t ch, uint8_t cc, uint8_t v) {
    switch (cc) {
        case 101:  // RPN select
            rpnMsb_[ch] = v;
            return;
        case 100:
            rpnLsb_[ch] = v;
            return;
        case 99:  // an NRPN select deselects the RPN
        case 98:
            rpnMsb_[ch] = rpnLsb_[ch] = 127;
            return;
        case 6:
            dataEntry(ch, true, v);
            return;
        case 38:
            dataEntry(ch, false, v);
            return;
        case 64:
            synth_->sustain(v >= 64);
            return;
        case 120:
        case 123:
            synth_->allNotesOff();
            return;
    }
    int p = paramForCc(cc);
    if (p < 0) return;
    synth_->setParam(p, ccToValue(p, v));
    lastCc_[p] = v;
    remote_ |= 1u << p;
}

// RPN 0 (pitch bend range: semitones in the MSB, cents in the LSB) on any channel; RPN 6 (MPE configuration) on
// channel 1 only.
void Device::dataEntry(uint8_t ch, bool msb, uint8_t v) {
    if (rpnMsb_[ch] != 0) return;
    if (rpnLsb_[ch] == 0) {
        bendRange_[ch] = msb ? v : (float)(int)bendRange_[ch] + v * 0.01f;
    } else if (rpnLsb_[ch] == 6 && ch == 0 && msb) {
        // The same zone again changes nothing: the app repeats its setup every few seconds, so a synth that was
        // reset picks MPE back up, and the repeat must not cut what is sounding.
        int members = v > 15 ? 15 : v;
        if (synth_->mpe() == (members > 0) && members == mpeMembers_) return;
        mpeMembers_ = members;
        synth_->allNotesOff();
        bendRange_[0] = kBendRange;
        for (int c = 1; c < 16; c++) {
            bendRange_[c] = c <= mpeMembers_ ? kMemberBendRange : kBendRange;
            synth_->bend(0.f, c);
        }
        synth_->bend(0.f, 0);
        synth_->setMpe(mpeMembers_ > 0);
    }
}

void Device::localParam(int p, float v) {
    synth_->setParam(p, v);
    uint8_t cc = valueToCc(p, v);
    if (cc == lastCc_[p]) return;
    lastCc_[p] = cc;
    send3(0xB0, kControls[p].cc, cc);
}

void Device::localNote(uint8_t note, bool on) {
    if (on) {
        synth_->noteOn(note, 100);
        send3(0x90, note, 100);
    } else {
        synth_->noteOff(note);
        send3(0x80, note, 0);
    }
}

}  // namespace syn
