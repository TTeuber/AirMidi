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
    ame_close(in);
    ame_close(out);
    ame_stop();
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
