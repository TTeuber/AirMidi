#include "controls.h"

#include <cstdio>
#include <cstring>

#include "dsp.h"

namespace syn {

// Sound Controllers where GM2 has one (74 brightness, 71 timbre, 73 attack, 75 decay, 72 release, 70 variation),
// 91/94 effect depths, 65/5 portamento, 1 mod wheel; undefined CCs (79, 85, 92 is tremolo, so 93 for echo; 86, 87, 89,
// 90 for the filter envelope) for the rest.
const Control kControls[kParams] = {
    {"Cutoff", "/filter/cutoff", 74, 64, 1, nullptr, "cutoff"},
    {"Resonance", "/filter/resonance", 71, 30, 1, nullptr, nullptr},
    {"Env Amt", "/filter/envAmount", 79, 64, 1, nullptr, nullptr},
    {"Amp Attack", "/amp/env/attack", 73, 0, 1, nullptr, "attack"},
    {"Amp Decay", "/amp/env/decay", 75, 70, 1, nullptr, "time"},
    {"Amp Sustain", "/amp/env/sustain", 85, 90, 1, nullptr, nullptr},
    {"Amp Release", "/amp/env/release", 72, 64, 1, nullptr, "time"},
    {"Space", "/fx/space", 91, 40, 1, nullptr, nullptr},
    {"Detune", "/osc/detune", 94, 40, 2, nullptr, nullptr},
    {"Vibrato", "/osc/vibrato", 1, 0, 2, nullptr, nullptr},
    {"Glide Time", "/osc/glideTime", 5, 40, 2, nullptr, nullptr},
    {"Echo", "/fx/echo", 93, 0, 2, nullptr, nullptr},
    {"Filter Attack", "/filter/env/attack", 86, 0, 2, nullptr, "attack"},
    {"Filter Decay", "/filter/env/decay", 87, 70, 2, nullptr, "time"},
    {"Filter Sustain", "/filter/env/sustain", 89, 90, 2, nullptr, nullptr},
    {"Filter Release", "/filter/env/release", 90, 64, 2, nullptr, "time"},
    {"Wave", "/osc/wave", 70, 0, 1, "valueSelect", "wave"},
    {"Glide", "/osc/glide", 65, 0, 1, "toggle", nullptr},
};

const char *const kDeviceInfo =
    "{\"manufacturerId\":[125,0,0],\"manufacturer\":\"Daisy\",\"familyId\":[0,0],\"family\":\"airmidi demo\","
    "\"modelId\":[3,0],\"model\":\"Poly Synth\",\"versionId\":[0,2,0,0],\"version\":\"0.2\"}";

const char *const kMapIds[kMaps] = {"wave", "cutoff", "attack", "time"};

int paramForCc(uint8_t cc) {
    for (int p = 0; p < kParams; p++)
        if (kControls[p].cc == cc) return p;
    return -1;
}

int waveIndex(float v) {
    int i = (int)(v * kWaves);
    return i < 0 ? 0 : (i >= kWaves ? kWaves - 1 : i);
}

float ccToValue(int p, uint8_t cc) {
    if (p == kWave) {
        // the page sends step k at k * 2^32 / 3, which arrives as k * 128 / 3 rounded down: 0, 42, 85
        int i = (cc * kWaves + 64) >> 7;
        if (i >= kWaves) i = kWaves - 1;
        return (i + 0.5f) / kWaves;
    }
    if (p == kGlide) return cc >= 64 ? 1.f : 0.f;
    return cc * (1.f / 127.f);
}

uint8_t valueToCc(int p, float v) {
    if (p == kWave) return (uint8_t)((waveIndex(v) * 128 + kWaves - 1) / kWaves);  // 0, 43, 86
    if (p == kGlide) return v >= 0.5f ? 127 : 0;
    return (uint8_t)(clampf(v, 0.f, 1.f) * 127.f + 0.5f);
}

float cutoffHz(float x) { return 20.f * exp2f2(10.f * x); }
float attackSec(float x) { return 0.001f * exp2f2(11.55f * x); }
float timeSec(float x) { return 0.005f * exp2f2(10.23f * x); }
float glideSec(float x) { return 0.005f * exp2f2(7.64f * x); }

namespace {

// A 0..1 parameter as the 32-bit value the page works in.
uint32_t to32(float x) {
    x = clampf(x, 0.f, 1.f);
    return x >= 1.f ? 0xFFFFFFFFu : (uint32_t)(x * 4294967296.0);
}

uint32_t default32(int p) {
    const Control &c = kControls[p];
    if (p == kWave) return (uint32_t)((uint64_t)waveIndex(ccToValue(p, c.def)) * 0x100000000ull / kWaves);
    if (p == kGlide) return c.def >= 64 ? 0xFFFFFFFFu : 0;
    return to32(c.def / 127.f);
}

// x where curve(x) = y, by bisection (the curves rise monotonically); avoids log2 from libm.
float invert(float (*curve)(float), float y) {
    float lo = 0.f, hi = 1.f;
    for (int i = 0; i < 30; i++) {
        float mid = 0.5f * (lo + hi);
        (curve(mid) < y ? lo : hi) = mid;
    }
    return 0.5f * (lo + hi);
}

// Appends with snprintf; n turns into cap + 1 once anything did not fit.
struct Out {
    char *s;
    size_t cap, n = 0;
    template <typename... A>
    void put(const char *fmt, A... a) {
        if (n >= cap) return;
        int k = snprintf(s + n, cap - n, fmt, a...);
        n = (k < 0 || (size_t)k >= cap - n) ? cap + 1 : n + (size_t)k;
    }
    size_t done() const { return n < cap ? n : 0; }
};

void namedPoints(Out &o, float (*curve)(float), const int *pts, int npts, bool hz) {
    o.put("[");
    for (int i = 0; i < npts; i++) {
        int v = pts[i];
        uint32_t at = i == 0 ? 0 : (i == npts - 1 ? 0xFFFFFFFFu : to32(invert(curve, hz ? (float)v : v * 1e-3f)));
        o.put("%s{\"value\":%lu,\"title\":\"", i ? "," : "", (unsigned long)at);
        // short: the page's tiles hold about five characters
        if (hz && v >= 1000)
            v % 1000 ? o.put("%d.%dk", v / 1000, v % 1000 / 100) : o.put("%dk", v / 1000);
        else if (hz)
            o.put("%dHz", v);
        else if (v >= 1000)
            o.put("%ds", v / 1000);
        else
            o.put("%dms", v);
        o.put("\"}");
    }
    o.put("]");
}

const int kCutoffPts[] = {20, 30, 50, 80, 120, 200, 300, 500, 800, 1200, 2000, 3000, 5000, 8000, 12000, 20000};
const int kAttackPts[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 3000};  // ms
const int kTimePts[] = {5, 10, 20, 50, 100, 200, 500, 1000, 2000, 4000, 6000};     // ms

}  // namespace

size_t buildCtrlList(char *out, size_t cap) {
    Out o{out, cap};
    o.put("[");
    for (int p = 0; p < kParams; p++) {
        const Control &c = kControls[p];
        o.put("%s{\"title\":\"%s\",\"channel\":1,\"ctrlType\":\"cc\",\"ctrlIndex\":[%d],\"priority\":%d,"
              "\"default\":%lu,\"paramPath\":\"%s\"",
              p ? "," : "", c.title, c.cc, c.priority, (unsigned long)default32(p), c.path);
        if (c.hint) o.put(",\"typeHint\":\"%s\"", c.hint);
        if (c.mapId) o.put(",\"ctrlMapId\":\"%s\"", c.mapId);
        o.put("}");
    }
    o.put(",{\"title\":\"Bend\",\"channel\":1,\"ctrlType\":\"pBend\",\"priority\":2,\"default\":2147483648,"
          "\"paramPath\":\"/osc/bend\"}]");
    return o.done();
}

size_t buildCtrlMap(const char *mapId, char *out, size_t cap) {
    Out o{out, cap};
    if (!strcmp(mapId, "wave"))
        o.put("[{\"value\":0,\"title\":\"Saw\"},{\"value\":1431655765,\"title\":\"Square\"},"
              "{\"value\":2863311530,\"title\":\"Triangle\"}]");
    else if (!strcmp(mapId, "cutoff"))
        namedPoints(o, cutoffHz, kCutoffPts, sizeof kCutoffPts / sizeof *kCutoffPts, true);
    else if (!strcmp(mapId, "attack"))
        namedPoints(o, attackSec, kAttackPts, sizeof kAttackPts / sizeof *kAttackPts, false);
    else if (!strcmp(mapId, "time"))
        namedPoints(o, timeSec, kTimePts, sizeof kTimePts / sizeof *kTimePts, false);
    else
        return 0;
    return o.done();
}

}  // namespace syn
