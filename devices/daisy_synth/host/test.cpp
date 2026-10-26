// Desktop check of the synth before it goes on the kit:
//   1. the box's MIDI-CI initiator (lib/midici_initiator) scans the device over a simulated MIDI 1.0 cable, byte by
//      byte through the same parser the firmware uses; every resource it reads is written to build/ci/*.json
//   2. CCs and pitch bend as the box sends them land on the right parameters, the kit's controls send theirs
//   3. MPE: the configuration message, one voice per channel, per-channel bend, the manager's bend for all
//   4. a demo phrase is rendered to build/demo.wav (48 kHz stereo 16-bit), with the time the engine took
//
//   host/build.sh && host/build/test
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../engine/device.h"
#include "airmidi/midici_initiator.h"

using namespace syn;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), failures++; \
    } while (0)

// ---- the cable -------------------------------------------------------------------------------------------------------

static std::vector<uint8_t> toDevice, fromDevice;
static uint32_t nowMs = 0;

static size_t wireBytes = 0;
static void deviceOut(void *, const uint8_t *b, size_t n) {
    fromDevice.insert(fromDevice.end(), b, b + n);
    wireBytes += n;
}

static void initiatorSend(void *, const uint8_t *sx, size_t n) {
    toDevice.push_back(0xF0);
    toDevice.insert(toDevice.end(), sx, sx + n);
    toDevice.push_back(0xF7);
}

static std::vector<std::pair<std::string, std::string>> got;  // name, json
static midici_done_t doneResult;
static bool done = false;

static bool onDevice(void *, uint32_t, const midici_discovery_t *d) {
    CHECK(d->ident.model == 3);
    CHECK(d->categories & MIDICI_CAT_PE);
    return true;
}

static void onResource(void *, const char *res, const char *id, int status, const uint8_t *data, size_t len) {
    CHECK(status == 200);
    std::string name = *id ? std::string(res) + "-" + id : res;
    got.push_back({name, std::string((const char *)data, len)});
    printf("  %-22s status %d, %zu bytes\n", name.c_str(), status, len);
}

static void onDone(void *, midici_done_t r) { doneResult = r, done = true; }

static int reportResult = -1, reportTypes = 0;
static void onReport(void *, midici_report_done_t r, uint8_t types) { reportResult = (int)r, reportTypes = types; }

// Hand the device's bytes to the initiator: whole SysEx messages only, channel messages are counted.
static int channelMsgs = 0;
static void pump(midici_initiator_t *in, Device &dev) {
    for (uint8_t b : toDevice) dev.receive(b);
    toDevice.clear();
    std::vector<uint8_t> sx;
    bool open = false;
    for (uint8_t b : fromDevice) {
        if (b == 0xF0) open = true, sx.clear();
        else if (b == 0xF7 && open) open = false, midici_initiator_receive(in, sx.data(), sx.size(), nowMs);
        else if (open) sx.push_back(b);
        else if (b & 0x80) channelMsgs++;
    }
    fromDevice.clear();
}

static float echoMem[Synth::kEchoFloats], roomMem[Synth::kRoomFloats];
static Synth synth;
static Device dev;

static void scan() {
    printf("MIDI-CI scan:\n");
    static uint8_t buf[16 * 1024];
    midici_initiator_t in;
    midici_initiator_cb_t cb = {nullptr, initiatorSend, onDevice, onResource, onDone, onReport};
    const midici_ident_t box = {{0x7D, 0, 0}, 0, 1, {0, 1, 0, 0}};
    midici_initiator_init(&in, midici_muid(0x1234567), &box, &cb, buf, sizeof buf);
    midici_initiator_start(&in, nowMs);
    for (int i = 0; i < 1000 && !done; i++) {
        wireBytes += toDevice.size();
        pump(&in, dev);
        nowMs += 10;
        midici_initiator_tick(&in, nowMs);
    }
    CHECK(done && doneResult == MIDICI_DONE_OK);
    printf("  done: %d, %zu device replies, %.0f ms of 31250-baud wire\n", doneResult, (size_t)dev.ciReplies(),
           wireBytes * 0.32);
    system("mkdir -p build/ci");
    for (auto &[name, json] : got) {
        FILE *f = fopen(("build/ci/" + name + ".json").c_str(), "w");
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
    }
    CHECK(got.size() == 3 + kMaps);  // ResourceList, DeviceInfo, AllCtrlList, the maps

    // MIDI Message Report: the knobs' current values, not the control list's defaults
    printf("MIDI Message Report:\n");
    CHECK(midici_initiator_can_report(&in));
    synth.setParam(kAttack, ccToValue(kAttack, 77));  // as if its knob had been turned
    CHECK(midici_initiator_report(&in, MIDICI_REPORT_PITCHBEND | MIDICI_REPORT_CC | MIDICI_REPORT_PROGRAM, nowMs));
    for (uint8_t b : toDevice) dev.receive(b);
    toDevice.clear();
    std::vector<std::vector<uint8_t>> msgs;  // what the synth sent, in order: SysEx contents or channel messages
    std::vector<uint8_t> cur;
    bool open = false;
    for (uint8_t b : fromDevice) {
        if (b == 0xF0) open = true, cur.clear();
        else if (b == 0xF7 && open) open = false, msgs.push_back(cur), midici_initiator_receive(&in, cur.data(), cur.size(), nowMs);
        else if (open) cur.push_back(b);
        else if (b & 0x80) msgs.push_back({b});
        else if (!msgs.empty()) msgs.back().push_back(b);
    }
    fromDevice.clear();
    CHECK(reportResult == MIDICI_REPORT_OK);
    CHECK(reportTypes == (MIDICI_REPORT_PITCHBEND | MIDICI_REPORT_CC));  // no Program Change on this synth
    CHECK(msgs.size() == 2 + 1 + kParams);  // Reply, pitch bend, a CC per parameter, End
    CHECK(msgs.front().size() > 3 && msgs.front()[3] == MIDICI_REPORT_REPLY);
    CHECK(msgs.back().size() > 3 && msgs.back()[3] == MIDICI_REPORT_END);
    CHECK(msgs[1] == (std::vector<uint8_t>{0xE0, 0, 64}));  // bend at the centre
    int lastCc = -1, attack = -1;
    for (size_t i = 2; i + 1 < msgs.size(); i++) {
        CHECK(msgs[i].size() == 3 && msgs[i][0] == 0xB0 && msgs[i][1] > lastCc);  // ascending CC numbers
        lastCc = msgs[i][1];
        if (msgs[i][1] == kControls[kAttack].cc) attack = msgs[i][2];
    }
    CHECK(attack == 77);
    printf("  %zu messages: Reply, bend, %d CCs, End; attack reported as %d\n", msgs.size(), kParams, attack);
    synth.setParam(kAttack, ccToValue(kAttack, kControls[kAttack].def));
}

static void controls() {
    printf("controls:\n");
    fromDevice.clear();
    // the box scales the page's 32-bit values to 7 bits by keeping the top 7 (M2-104)
    auto boxCc = [](uint8_t cc, uint32_t v32) {
        const uint8_t m[3] = {0xB0, cc, (uint8_t)(v32 >> 25)};
        for (uint8_t b : m) dev.receive(b);
    };
    boxCc(74, 0xFFFFFFFF);
    CHECK(synth.param(kCutoff) == 1.f);
    for (int k = 0; k < kWaves; k++) {
        boxCc(70, (uint32_t)((uint64_t)k * 0x100000000ull / kWaves));
        CHECK(waveIndex(synth.param(kWave)) == k);
    }
    boxCc(65, 0xFFFFFFFF);
    CHECK(synth.param(kGlide) == 1.f);
    CHECK(dev.takeRemote() == (1u << kCutoff | 1u << kWave | 1u << kGlide));
    CHECK(fromDevice.empty());  // nothing echoed back

    // running status, a real-time byte in the middle
    const uint8_t rs[] = {0xB0, 71, 0, 0xF8, 71, 127};
    for (uint8_t b : rs) dev.receive(b);
    CHECK(synth.param(kReso) == 1.f);

    // a value the kit sends for each wave lands on the same option in the page (nearest of k * 2^32 / 3)
    for (int k = 0; k < kWaves; k++) {
        uint8_t cc = valueToCc(kWave, (k + 0.5f) / kWaves);
        double v = cc / 127.0 * 4294967295.0;
        int best = 0;
        for (int j = 0; j < kWaves; j++)
            if (std::abs(v - j * 4294967296.0 / kWaves) < std::abs(v - best * 4294967296.0 / kWaves)) best = j;
        CHECK(best == k);
    }

    dev.localParam(kSpace, 0.5f);
    CHECK(fromDevice.size() == 3 && fromDevice[0] == 0xB0 && fromDevice[1] == 91 && fromDevice[2] == 64);
    dev.localParam(kSpace, 0.501f);  // same 7-bit value: nothing sent
    CHECK(fromDevice.size() == 3);
    printf("  %s\n", failures ? "failed" : "ok");

    // back to the defaults for the render
    for (int p = 0; p < kParams; p++) synth.setParam(p, ccToValue(p, kControls[p].def));
}

// ---- MPE ------------------------------------------------------------------------------------------------------------

static void send(std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) dev.receive(b);
}

static void bendTo(uint8_t ch, float semis, float range) {
    int v = (int)(8192 + semis / range * 8192 + 0.5f);
    v = v < 0 ? 0 : v > 16383 ? 16383 : v;
    send({(uint8_t)(0xE0 | ch), (uint8_t)(v & 0x7F), (uint8_t)(v >> 7)});
}

static void runMs(int ms) {
    float l[48], r[48];
    for (int i = 0; i < ms; i++) synth.process(l, r, 48);
}

// The voice playing note on channel ch with its gate open, or -1.
static int voiceOf(int note, int ch) {
    for (int i = 0; i < Synth::kVoices; i++)
        if (synth.voiceNote(i) == note && synth.voiceChannel(i) == ch && synth.voiceGate(i)) return i;
    return -1;
}

static void mpe() {
    printf("mpe:\n");
    // what the app sends on connect: MPE configuration (lower zone, 3 members), then ±48 on each member
    send({0xB0, 101, 0, 0xB0, 100, 6, 0xB0, 6, 3, 0xB0, 101, 127, 0xB0, 100, 127});
    CHECK(dev.mpeMembers() == 3 && synth.mpe());
    for (uint8_t ch = 1; ch <= 3; ch++)
        send({(uint8_t)(0xB0 | ch), 101, 0, (uint8_t)(0xB0 | ch), 100, 0, (uint8_t)(0xB0 | ch), 6, 48,
              (uint8_t)(0xB0 | ch), 38, 0});

    // the app repeats its setup while connected: the same zone again keeps the ranges and cuts nothing
    send({0x91, 40, 100});
    runMs(1);
    send({0xB0, 101, 0, 0xB0, 100, 6, 0xB0, 6, 3, 0xB0, 101, 127, 0xB0, 100, 127});
    runMs(1);
    CHECK(voiceOf(40, 1) >= 0);
    send({0x81, 40, 0});

    // two strips on the same note (d string at the 5th fret, open g): two voices, each with its own bend
    bendTo(1, 0.2f, 48), send({0x91, 55, 100});
    bendTo(2, 0.f, 48), send({0x92, 55, 100});
    runMs(1);
    int a = voiceOf(55, 1), b = voiceOf(55, 2);
    CHECK(a >= 0 && b >= 0 && a != b);
    if (a < 0 || b < 0) return;
    CHECK(std::abs(synth.voicePitch(a) - 55.2f) < 0.005f);  // started at its bend, no scoop
    CHECK(std::abs(synth.voicePitch(b) - 55.f) < 0.005f);

    // a pedal: strip 1 bends up a whole tone, strip 2 holds
    bendTo(1, 2.f, 48);
    runMs(30);
    CHECK(std::abs(synth.voicePitch(a) - 57.f) < 0.01f);
    CHECK(std::abs(synth.voicePitch(b) - 55.f) < 0.005f);

    // a note off on channel 2 releases only channel 2's note
    send({0x82, 55, 0});
    runMs(1);
    CHECK(synth.voiceGate(a) && !synth.voiceGate(b));

    // the manager channel's bend (±2) moves every voice: the bar
    bendTo(0, 1.f, 2);
    runMs(30);
    CHECK(std::abs(synth.voicePitch(a) - 58.f) < 0.01f);
    bendTo(0, 0.f, 2);
    send({0x81, 55, 0});

    // a reset synth (MPE off, ±2) hears the repeated setup and is back in MPE with ±48
    send({0xB0, 101, 0, 0xB0, 100, 6, 0xB0, 6, 0});
    CHECK(dev.mpeMembers() == 0);
    send({0xB0, 101, 0, 0xB0, 100, 6, 0xB0, 6, 3, 0xB0, 101, 127, 0xB0, 100, 127});
    CHECK(dev.mpeMembers() == 3 && synth.mpe());
    bendTo(1, 7.f, 48), send({0x91, 50, 100});
    runMs(1);
    int r = voiceOf(50, 1);
    CHECK(r >= 0 && std::abs(synth.voicePitch(r) - 57.f) < 0.01f);  // a fifth, not a 24th of one
    send({0x81, 50, 0});

    // MPE off again: one bend for everything, from any channel, ±2 (the kit's behaviour before MPE)
    send({0xB0, 101, 0, 0xB0, 100, 6, 0xB0, 6, 0});
    CHECK(dev.mpeMembers() == 0 && !synth.mpe());
    runMs(1);
    send({0x90, 60, 100});
    bendTo(4, 1.f, 2);
    runMs(30);
    int c = voiceOf(60, 0);
    CHECK(c >= 0 && std::abs(synth.voicePitch(c) - 61.f) < 0.01f);
    bendTo(4, 0.f, 2);
    send({0x80, 60, 0});
    runMs(3000);  // silent again for the render
    printf("  %s\n", failures ? "failed" : "ok");
}

// ---- render ---------------------------------------------------------------------------------------------------------

static void writeWav(const char *path, const std::vector<float> &l, const std::vector<float> &r, int sr) {
    FILE *f = fopen(path, "wb");
    uint32_t n = (uint32_t)l.size(), data = n * 4, riff = 36 + data, fmtLen = 16, rate = (uint32_t)sr,
             bps = rate * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmtLen, 4, 1, f);
    fwrite(&pcm, 2, 1, f), fwrite(&ch, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&bps, 4, 1, f);
    fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < n; i++) {
        int16_t s[2] = {(int16_t)(l[i] * 32767.f), (int16_t)(r[i] * 32767.f)};
        fwrite(s, 2, 2, f);
    }
    fclose(f);
}

static void render() {
    const int sr = 48000, block = 48;
    std::vector<float> L, R;
    float bl[block], br[block];
    double engineSec = 0;
    float peak = 0;
    auto run = [&](float sec) {
        for (int b = 0; b < (int)(sec * sr / block); b++) {
            auto t0 = std::chrono::steady_clock::now();
            synth.process(bl, br, block);
            engineSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            for (int i = 0; i < block; i++) {
                L.push_back(bl[i]), R.push_back(br[i]);
                peak = std::max(peak, std::max(std::abs(bl[i]), std::abs(br[i])));
            }
        }
    };
    auto chord = [&](std::initializer_list<int> notes, float sec, float gate) {
        for (int n : notes) synth.noteOn((uint8_t)n, 96);
        run(sec * gate);
        for (int n : notes) synth.noteOff((uint8_t)n);
        run(sec * (1 - gate));
    };
    // pads with a slow attack, then a plucked bass line, then a glide lead
    synth.setParam(kAttack, 0.55f), synth.setParam(kRelease, 0.7f), synth.setParam(kSpace, 0.6f);
    chord({48, 55, 63, 67}, 2.f, 0.8f);
    chord({44, 51, 60, 67}, 2.f, 0.8f);
    synth.setParam(kAttack, 0.f), synth.setParam(kDecay, 0.4f), synth.setParam(kSustain, 0.f);
    synth.setParam(kRelease, 0.35f), synth.setParam(kReso, 0.6f), synth.setParam(kCutoff, 0.35f);
    synth.setParam(kSpace, 0.25f), synth.setParam(kEcho, 0.4f);
    for (int n : {36, 36, 48, 36, 39, 36, 46, 43}) chord({n}, 0.25f, 0.5f);
    synth.setParam(kWave, 0.5f), synth.setParam(kGlide, 1.f), synth.setParam(kSustain, 0.8f);
    synth.setParam(kCutoff, 0.6f), synth.setParam(kReso, 0.3f), synth.setParam(kVibrato, 0.5f);
    for (int n : {72, 75, 79, 77, 75}) chord({n}, 0.4f, 0.95f);
    run(2.f);
    writeWav("build/demo.wav", L, R, sr);
    double audioSec = (double)L.size() / sr;
    printf("render: build/demo.wav, %.1f s, peak %.2f, engine %.0fx real time on this machine\n", audioSec, peak,
           audioSec / engineSec);
    CHECK(peak < 1.f && peak > 0.1f);
}

// ---- filter envelope -------------------------------------------------------------------------------------------------

// The amp and the filter each follow their own envelope: a slow filter attack under a fast amp attack.
static void filterEnv() {
    printf("filter envelope:\n");
    send({0xB0, 86, 100});  // Filter Attack 0.55 s (attackSec(100 / 127))
    send({0xB0, 73, 0});    // Amp Attack 1 ms
    send({0xB0, 89, 0});    // Filter Sustain 0
    runMs(50);              // the parameters' smoothing settles
    send({0x90, 64, 100});
    runMs(100);
    int v = voiceOf(64, 0);
    CHECK(v >= 0);
    float amp = synth.voiceLevel(v), flt = synth.voiceFilterLevel(v);
    printf("  100 ms in: amp %.2f, filter %.2f\n", amp, flt);
    CHECK(amp > 0.5f);                  // decaying from its peak toward 0.71
    CHECK(flt > 0.12f && flt < 0.24f);  // 0.1 / 0.55 of the way up
    runMs(1500);
    flt = synth.voiceFilterLevel(v);
    CHECK(flt < 0.5f);  // past its peak, decaying toward its own sustain of 0
    send({0x80, 64, 0});
    runMs(3000);
    CHECK(voiceOf(64, 0) < 0 && synth.voiceFilterLevel(v) == 0.f);
    for (int p : {kFAttack, kAttack, kFSustain}) synth.setParam(p, ccToValue(p, kControls[p].def));
    printf("  %s\n", failures ? "failed" : "ok");
}

int main() {
    synth.init(48000.f, echoMem, roomMem);
    CHECK(dev.init(&synth, 0x2A2A2A2, deviceOut, nullptr));
    scan();
    controls();
    mpe();
    filterEnv();
    render();
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
