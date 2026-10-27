// Loopback through the engine's own virtual port: a scheduled stream out of it, read back on an input opened on
// that port. Checks order, SysEx, and how close each arrival is to its due time.
//   cmake -S . -B build && cmake --build build && ./build/engine_test
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"

static std::atomic<int> wakes{0};
static int failures = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), failures++; \
    } while (0)

int main() {
    CHECK(ame_start([] { wakes++; }) == 0);
    const int out = ame_open_virtual(1, "airmidi engine test");
    CHECK(out > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // CoreMIDI announces the new source
    ame_refresh();
    int src = -1;
    for (int i = 0; i < ame_port_count(0); i++) {
        printf("  input %d: %s (type %d)\n", i, ame_port_name(0, i), ame_port_type(0, i));
        if (std::string(ame_port_name(0, i)).find("airmidi engine test") != std::string::npos) src = i;
    }
    CHECK(src >= 0);
    // the observer told us about the new port (kind 2)
    {
        int h, n, ports = 0;
        int64_t t;
        uint8_t b[16];
        for (int kind; (kind = ame_read(&h, b, sizeof b, &n, &t));) ports += kind == 2;
        CHECK(ports > 0);
    }
    const int in = ame_open(0, src);
    CHECK(in > 0);

    // 400 note ons 5 ms apart, queued all at once, the first 100 ms from now; in between a SysEx
    const int N = 400;
    const int64_t t0 = ame_now_ns() + 100'000'000, step = 5'000'000;
    for (int k = 0; k < N; k++) {
        uint8_t m[3] = {0x90, (uint8_t)(k % 128), (uint8_t)(1 + k / 128)};
        ame_send(out, m, 3, t0 + k * step);
    }
    const uint8_t sx[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x70, 0x02, 1, 2, 3, 4, 0xF7};
    ame_send(out, sx, sizeof sx, t0 + 1);  // right after the first note

    std::vector<int64_t> err;
    int got = 0, sysex = 0, order = 0, k = 0;
    const int64_t end = t0 + N * step + 500'000'000;
    while (ame_now_ns() < end && got < N) {
        int h, n;
        int64_t t;
        uint8_t b[64];
        int kind = ame_read(&h, b, sizeof b, &n, &t);
        if (!kind) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        if (kind != 1 || h != in) continue;
        if (b[0] == 0xF0) {
            sysex += n == (int)sizeof sx && !memcmp(b, sx, n);
            continue;
        }
        const int idx = b[1] + 128 * (b[2] - 1);
        order += idx == k;
        k = idx + 1;
        err.push_back(t - (t0 + idx * step));
        got++;
    }
    CHECK(got == N);
    CHECK(order == N);
    CHECK(sysex == 1);
    CHECK(wakes > 0);
    std::sort(err.begin(), err.end());
    if (!err.empty()) {
        double mean = 0;
        for (auto e : err) mean += e;
        mean /= err.size();
        printf("  %d messages back in order; arrival - due: min %.3f, median %.3f, p99 %.3f, max %.3f ms, mean %.3f\n",
               got, err.front() / 1e6, err[err.size() / 2] / 1e6, err[err.size() * 99 / 100] / 1e6, err.back() / 1e6,
               mean / 1e6);
        printf("  send thread late: max %.3f ms, mean %.3f ms over %lld messages\n", ame_late_max_ns() / 1e6,
               ame_late_sum_ns() / 1e6 / std::max<int64_t>(1, ame_sent()), (long long)ame_sent());
        CHECK(err[err.size() * 99 / 100] < 2'000'000);  // 99 % within 2 ms of due
    }
    // clear: queued and dropped, nothing arrives
    for (int k = 0; k < 10; k++) {
        uint8_t m[3] = {0x80, 1, 0};
        ame_send(out, m, 3, ame_now_ns() + 200'000'000);
    }
    CHECK(ame_clear(out) == 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int late = 0;
    { int h, n; int64_t t; uint8_t b[16]; for (int kind; (kind = ame_read(&h, b, sizeof b, &n, &t));) late += kind == 1; }
    CHECK(late == 0);
    // routes: a second virtual pair; what arrives on the test input is routed to it, filtered and remapped
    const int out2 = ame_open_virtual(1, "airmidi engine test 2");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ame_refresh();
    int src2 = -1;
    for (int i = 0; i < ame_port_count(0); i++)
        if (std::string(ame_port_name(0, i)).find("airmidi engine test 2") != std::string::npos) src2 = i;
    const int in2 = ame_open(0, src2);
    CHECK(in2 > 0);
    { int h, n; int64_t t; uint8_t b[16]; while (ame_read(&h, b, sizeof b, &n, &t)) {} }
    uint8_t map[16];
    for (int c = 0; c < 16; c++) map[c] = (uint8_t)c;
    map[0] = 2;  // channel 1 -> 3
    // channels 1 and 10 pass; notes 36..72 only, up an octave; notes and CCs, no bend
    CHECK(ame_route_set(7, in, out2, (1u << 0) | (1u << 9), map, 12, 36, 72, AME_T_NOTES | AME_T_CC) == 0);
    const uint8_t sendme[][3] = {
        {0x90, 60, 100},  // ch1 note 60 -> ch3 note 72
        {0x91, 60, 100},  // ch2: blocked
        {0x99, 36, 90},   // ch10 kick -> 48, stays ch10
        {0x90, 80, 100},  // above the range: dropped
        {0xE0, 0, 64},    // bend: not in types
        {0xB0, 7, 99},    // CC 7 -> ch3
    };
    for (auto &m : sendme) ame_send(out, m, 3, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::vector<std::vector<uint8_t>> routed;
    { int h, n; int64_t t; uint8_t b[16]; for (int kind; (kind = ame_read(&h, b, sizeof b, &n, &t));) if (kind == 1 && h == in2) routed.emplace_back(b, b + n); }
    // what a route adds: arrival on the source input to arrival after the routed hop, 200 notes
    {
        std::vector<int64_t> a(128, 0), d;
        for (int k = 0; k < 200; k++) {
            uint8_t m[3] = {0x90, (uint8_t)(40 + k % 30), 100};
            ame_send(out, m, 3, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int h, n;
        int64_t t;
        uint8_t b[16];
        for (int kind; (kind = ame_read(&h, b, sizeof b, &n, &t));) {
            if (kind != 1) continue;
            if (h == in) a[b[1]] = t;
            if (h == in2 && a[b[1] - 12]) d.push_back(t - a[b[1] - 12]);
        }
        std::sort(d.begin(), d.end());
        if (!d.empty())
            printf("  routed hop: %zu notes, median %.3f ms, max %.3f ms\n", d.size(), d[d.size() / 2] / 1e6, d.back() / 1e6);
        CHECK(d.size() == 200);
    }
    CHECK(routed.size() == 3);
    if (routed.size() == 3) {
        CHECK((routed[0] == std::vector<uint8_t>{0x92, 72, 100}));
        CHECK((routed[1] == std::vector<uint8_t>{0x99, 48, 90}));
        CHECK((routed[2] == std::vector<uint8_t>{0xB2, 7, 99}));
    }
    CHECK(ame_route_count(7) == 203);
    printf("  route: %zu of 6 messages through, filtered and remapped\n", routed.size());
    ame_route_remove(7);
    CHECK(ame_route_count(7) == -1);
    ame_close(in2);
    ame_close(out2);
    ame_close(in);
    ame_close(out);
    ame_stop();
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
