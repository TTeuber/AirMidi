// The desktop MIDI engine (engine.h). libremidi is used header-only (third_party/libremidi), with the platform's
// own backend: CoreMIDI, Windows MIDI, ALSA.
#include "engine.h"

#include <libremidi/libremidi.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace {

// The send thread gets real-time scheduling where the platform allows it without privileges: a plain thread's
// timed wait wakes ~1 ms late on macOS (timer coalescing), a time-constraint thread within tens of microseconds.
void realtime() {
#if defined(__APPLE__)
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    auto abs = [&](double ms) { return (uint32_t)(ms * 1e6 * tb.denom / tb.numer); };
    thread_time_constraint_policy_data_t p;
    p.period = abs(1.0);  // a hint: we wake about this often while a stream plays
    p.computation = abs(0.1);
    p.constraint = abs(0.5);
    p.preemptible = 1;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#elif defined(_WIN32)
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    // Linux: SCHED_FIFO needs rtkit or privileges; to look at with the Linux build
}

int64_t now_ns() {
    // libremidi's SystemMonotonic timestamps use the same clock (on macOS: CoreMIDI host time = CLOCK_UPTIME_RAW,
    // which is what libc++'s steady_clock reads)
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Event {
    int kind;  // 1 MIDI, 2 ports changed
    int handle;
    int64_t t;
    std::vector<uint8_t> bytes;
};

struct Due {
    int64_t at;
    uint64_t seq;  // ties keep the order they were queued in
    int handle;
    std::vector<uint8_t> bytes;
    bool operator>(const Due &o) const { return at != o.at ? at > o.at : seq > o.seq; }
};

struct Port {
    std::unique_ptr<libremidi::midi_in> in;
    std::unique_ptr<libremidi::midi_out> out;
};

struct Route {
    int id, in, out;
    uint16_t mask;
    uint8_t map[16];
    int transpose, lo, hi;
    uint32_t types;
    int64_t count = 0;
};

// The route's verdict on one message: false = dropped; otherwise *out is what to send.
bool routeMessage(const Route &r, const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
    if (in.empty()) return false;
    const uint8_t s = in[0];
    if (s >= 0xF0) {
        uint32_t t = s == 0xF0 ? AME_T_SYSEX
                     : (s == 0xF8 || s == 0xFA || s == 0xFB || s == 0xFC || s == 0xF2) ? AME_T_CLOCK
                                                                                        : AME_T_OTHER;
        if (!(r.types & t)) return false;
        out = in;
        return true;
    }
    const int ch = s & 0x0F, kind = s & 0xF0;
    if (!(r.mask & (1u << ch))) return false;
    uint32_t t = kind == 0x80 || kind == 0x90 || kind == 0xA0 ? AME_T_NOTES
                 : kind == 0xB0                               ? AME_T_CC
                 : kind == 0xC0                               ? AME_T_PROGRAM
                 : kind == 0xD0                               ? AME_T_PRESSURE
                                                              : AME_T_BEND;
    if (!(r.types & t)) return false;
    out = in;
    out[0] = (uint8_t)(kind | (r.map[ch] & 0x0F));
    if (t == AME_T_NOTES && out.size() > 1) {
        if (in[1] < r.lo || in[1] > r.hi) return false;
        const int n = in[1] + r.transpose;
        if (n < 0 || n > 127) return false;
        out[1] = (uint8_t)n;
    }
    return true;
}

struct Engine {
    void (*wake)() = nullptr;

    // events for Dart
    std::mutex evMu;
    std::deque<Event> events;

    // routes, checked on every input's thread
    std::mutex routeMu;
    std::vector<Route> routes;

    // open ports
    std::mutex portMu;
    std::map<int, Port> ports;
    int nextHandle = 1;

    // the port lists Dart sees
    std::mutex listMu;
    std::vector<libremidi::input_port> inputs;
    std::vector<libremidi::output_port> outputs;
    std::vector<std::string> inNames, outNames;
    std::unique_ptr<libremidi::observer> observer;
#if defined(__APPLE__)
    // CoreMIDI calls a client's notifications on the run loop of the thread that made it: the observer lives on a
    // thread of its own that runs one (the Dart thread does not)
    std::thread observerThread;
    CFRunLoopRef observerLoop = nullptr;
#endif

    // the send thread
    std::mutex dueMu;
    std::condition_variable dueCv;
    std::priority_queue<Due, std::vector<Due>, std::greater<>> due;
    uint64_t seq = 0;
    bool running = false;
    std::thread sender;
    std::atomic<int64_t> lateMax{0}, lateSum{0}, sent{0};

    void post(Event e) {
        bool wasEmpty;
        {
            std::lock_guard<std::mutex> l(evMu);
            wasEmpty = events.empty();
            events.push_back(std::move(e));
        }
        if (wasEmpty && wake) wake();
    }

    void sendLoop() {
        realtime();
        std::unique_lock<std::mutex> l(dueMu);
        while (running) {
            if (due.empty()) {
                dueCv.wait(l);
                continue;
            }
            const int64_t at = due.top().at;
            const int64_t now = now_ns();
            if (at > now) {
                // condition_variable waits on steady_clock, the clock the times are in
                dueCv.wait_until(l, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(at)));
                continue;
            }
            Due d = due.top();
            due.pop();
            l.unlock();
            {
                std::lock_guard<std::mutex> pl(portMu);
                auto it = ports.find(d.handle);
                if (it != ports.end() && it->second.out) it->second.out->send_message(d.bytes.data(), d.bytes.size());
            }
            const int64_t late = now_ns() - d.at;
            if (late > lateMax.load()) lateMax = late;
            lateSum += late;
            sent++;
            l.lock();
        }
    }

    libremidi::input_configuration inputConfig(int handle) {
        libremidi::input_configuration c{};
        c.on_message = [this, handle](libremidi::message &&m) {
            int64_t t = m.timestamp, now = now_ns();
            if (t <= 0 || t > now + 1'000'000'000 || t < now - 1'000'000'000) t = now;  // no usable stamp: arrival
            std::vector<uint8_t> bytes(m.bytes.begin(), m.bytes.end());
            route(handle, bytes);
            post({1, handle, t, std::move(bytes)});
        };
        c.ignore_sysex = false;   // MIDI-CI
        c.ignore_timing = false;  // clock, for following another sequencer
        c.ignore_sensing = true;
        c.timestamps = libremidi::timestamp_mode::SystemMonotonic;
        return c;
    }

    // Straight out on this thread: no queue, no wait for Dart.
    void route(int handle, const std::vector<uint8_t> &bytes) {
        std::vector<std::pair<int, std::vector<uint8_t>>> sends;
        {
            std::lock_guard<std::mutex> l(routeMu);
            for (auto &r : routes) {
                if (r.in != handle) continue;
                std::vector<uint8_t> out;
                if (!routeMessage(r, bytes, out)) continue;
                r.count++;
                sends.emplace_back(r.out, std::move(out));
            }
        }
        if (sends.empty()) return;
        std::lock_guard<std::mutex> pl(portMu);
        for (auto &[h, b] : sends) {
            auto it = ports.find(h);
            if (it != ports.end() && it->second.out) it->second.out->send_message(b.data(), b.size());
        }
    }

    int add(Port p) {
        std::lock_guard<std::mutex> l(portMu);
        int h = nextHandle++;
        ports[h] = std::move(p);
        return h;
    }
};

Engine *E = nullptr;

}  // namespace

extern "C" {

int ame_start(void (*wake)(void)) {
    if (E) return 0;
    E = new Engine();
    E->wake = wake;
    libremidi::observer_configuration oc{};
    auto changed = [](const auto &) { E->post({2, 0, now_ns(), {}}); };
    oc.input_added = changed;
    oc.input_removed = changed;
    oc.output_added = changed;
    oc.output_removed = changed;
    oc.track_hardware = true;
    oc.track_virtual = true;
    oc.track_network = true;
    oc.track_any = true;
    oc.notify_in_constructor = false;
#if defined(__APPLE__)
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    E->observerThread = std::thread([&, oc] {
        E->observer = std::make_unique<libremidi::observer>(oc);
        E->observerLoop = CFRunLoopGetCurrent();
        CFRunLoopSourceContext ctx{};  // a source keeps CFRunLoopRun from returning while there is nothing to do
        ctx.perform = [](void *) {};
        CFRunLoopSourceRef keep = CFRunLoopSourceCreate(nullptr, 0, &ctx);
        CFRunLoopAddSource(E->observerLoop, keep, kCFRunLoopDefaultMode);
        {
            std::lock_guard<std::mutex> l(m);
            ready = true;
        }
        cv.notify_one();
        CFRunLoopRun();
        CFRunLoopRemoveSource(E->observerLoop, keep, kCFRunLoopDefaultMode);
        CFRelease(keep);
        E->observer.reset();
    });
    {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return ready; });
    }
#else
    E->observer = std::make_unique<libremidi::observer>(oc);
#endif
    E->running = true;
    E->sender = std::thread([] { E->sendLoop(); });
    ame_refresh();
    return 0;
}

void ame_stop(void) {
    if (!E) return;
    {
        std::lock_guard<std::mutex> l(E->dueMu);
        E->running = false;
    }
    E->dueCv.notify_all();
    E->sender.join();
    {
        std::lock_guard<std::mutex> l(E->portMu);
        E->ports.clear();
    }
#if defined(__APPLE__)
    CFRunLoopStop(E->observerLoop);
    E->observerThread.join();
#else
    E->observer.reset();
#endif
    delete E;
    E = nullptr;
}

int64_t ame_now_ns(void) { return now_ns(); }

int ame_refresh(void) {
    if (!E) return -1;
    std::lock_guard<std::mutex> l(E->listMu);
    E->inputs = E->observer->get_input_ports();
    E->outputs = E->observer->get_output_ports();
    E->inNames.clear();
    E->outNames.clear();
    for (auto &p : E->inputs) E->inNames.push_back(p.display_name.empty() ? p.port_name : p.display_name);
    for (auto &p : E->outputs) E->outNames.push_back(p.display_name.empty() ? p.port_name : p.display_name);
    return (int)(E->inputs.size() + E->outputs.size());
}

int ame_port_count(int out) {
    if (!E) return 0;
    std::lock_guard<std::mutex> l(E->listMu);
    return (int)(out ? E->outputs.size() : E->inputs.size());
}

const char *ame_port_name(int out, int i) {
    if (!E) return "";
    std::lock_guard<std::mutex> l(E->listMu);
    auto &v = out ? E->outNames : E->inNames;
    return i >= 0 && i < (int)v.size() ? v[i].c_str() : "";
}

int ame_port_type(int out, int i) {
    if (!E) return 0;
    std::lock_guard<std::mutex> l(E->listMu);
    if (out) return i >= 0 && i < (int)E->outputs.size() ? E->outputs[i].type : 0;
    return i >= 0 && i < (int)E->inputs.size() ? E->inputs[i].type : 0;
}

int ame_open(int out, int i) {
    if (!E) return -1;
    Port p;
    if (out) {
        libremidi::output_port port;
        {
            std::lock_guard<std::mutex> l(E->listMu);
            if (i < 0 || i >= (int)E->outputs.size()) return -1;
            port = E->outputs[i];
        }
        p.out = std::make_unique<libremidi::midi_out>(libremidi::output_configuration{});
        if (p.out->open_port(port, "airmidi") != stdx::error{}) return -1;
        return E->add(std::move(p));
    }
    libremidi::input_port port;
    {
        std::lock_guard<std::mutex> l(E->listMu);
        if (i < 0 || i >= (int)E->inputs.size()) return -1;
        port = E->inputs[i];
    }
    int h;
    {
        std::lock_guard<std::mutex> l(E->portMu);
        h = E->nextHandle++;
    }
    p.in = std::make_unique<libremidi::midi_in>(E->inputConfig(h));
    if (p.in->open_port(port, "airmidi") != stdx::error{}) return -1;
    std::lock_guard<std::mutex> l(E->portMu);
    E->ports[h] = std::move(p);
    return h;
}

int ame_open_virtual(int out, const char *name) {
    if (!E) return -1;
    Port p;
    if (out) {
        p.out = std::make_unique<libremidi::midi_out>(libremidi::output_configuration{});
        if (p.out->open_virtual_port(name) != stdx::error{}) return -1;
        return E->add(std::move(p));
    }
    int h;
    {
        std::lock_guard<std::mutex> l(E->portMu);
        h = E->nextHandle++;
    }
    p.in = std::make_unique<libremidi::midi_in>(E->inputConfig(h));
    if (p.in->open_virtual_port(name) != stdx::error{}) return -1;
    std::lock_guard<std::mutex> l(E->portMu);
    E->ports[h] = std::move(p);
    return h;
}

void ame_close(int handle) {
    if (!E) return;
    std::unique_ptr<libremidi::midi_in> in;
    std::unique_ptr<libremidi::midi_out> out;
    {
        std::lock_guard<std::mutex> l(E->portMu);
        auto it = E->ports.find(handle);
        if (it == E->ports.end()) return;
        in = std::move(it->second.in);
        out = std::move(it->second.out);
        E->ports.erase(it);
    }
    {
        std::lock_guard<std::mutex> l(E->routeMu);
        E->routes.erase(std::remove_if(E->routes.begin(), E->routes.end(),
                                       [&](const Route &r) { return r.in == handle || r.out == handle; }),
                        E->routes.end());
    }
    // closed outside the lock: an input's callback may be posting at this moment
}

int ame_send(int handle, const uint8_t *bytes, int n, int64_t at_ns) {
    if (!E || n <= 0) return -1;
    {
        std::lock_guard<std::mutex> l(E->portMu);
        auto it = E->ports.find(handle);
        if (it == E->ports.end() || !it->second.out) return -1;
    }
    // split into messages: CoreMIDI wants one message (or one SysEx) per send
    std::vector<std::vector<uint8_t>> msgs;
    for (int i = 0; i < n;) {
        int len;
        const uint8_t s = bytes[i];
        if (s == 0xF0) {
            len = 1;
            while (i + len < n && bytes[i + len] != 0xF7) len++;
            len = std::min(len + 1, n - i);
        } else if (s >= 0xF8) {
            len = 1;
        } else {
            const uint8_t hi = s & 0xF0;
            len = (hi == 0xC0 || hi == 0xD0 || s == 0xF1 || s == 0xF3) ? 2 : (s == 0xF6 ? 1 : 3);
            if (s < 0x80) len = 1;  // stray data byte: passed on alone
            len = std::min(len, n - i);
        }
        msgs.emplace_back(bytes + i, bytes + i + len);
        i += len;
    }
    {
        std::lock_guard<std::mutex> l(E->dueMu);
        for (auto &m : msgs) E->due.push({at_ns, E->seq++, handle, std::move(m)});
    }
    E->dueCv.notify_one();
    return 0;
}

int ame_clear(int handle) {
    if (!E) return 0;
    std::lock_guard<std::mutex> l(E->dueMu);
    std::vector<Due> keep;
    int dropped = 0;
    while (!E->due.empty()) {
        Due d = E->due.top();
        E->due.pop();
        if (d.handle == handle)
            dropped++;
        else
            keep.push_back(std::move(d));
    }
    for (auto &d : keep) E->due.push(std::move(d));
    return dropped;
}

int ame_route_set(int id, int in_handle, int out_handle, uint16_t mask, const uint8_t *map, int transpose, int lo,
                  int hi, uint32_t types) {
    if (!E) return -1;
    Route r{id, in_handle, out_handle, mask, {}, transpose, lo, hi, types};
    for (int c = 0; c < 16; c++) r.map[c] = map ? map[c] : (uint8_t)c;
    std::lock_guard<std::mutex> l(E->routeMu);
    for (auto &x : E->routes)
        if (x.id == id) {
            r.count = x.count;
            x = r;
            return 0;
        }
    E->routes.push_back(r);
    return 0;
}

void ame_route_remove(int id) {
    if (!E) return;
    std::lock_guard<std::mutex> l(E->routeMu);
    E->routes.erase(std::remove_if(E->routes.begin(), E->routes.end(), [&](const Route &r) { return r.id == id; }),
                    E->routes.end());
}

int64_t ame_route_count(int id) {
    if (!E) return -1;
    std::lock_guard<std::mutex> l(E->routeMu);
    for (auto &r : E->routes)
        if (r.id == id) return r.count;
    return -1;
}

int ame_read(int *handle, uint8_t *bytes, int cap, int *n, int64_t *t_ns) {
    if (!E) return 0;
    std::lock_guard<std::mutex> l(E->evMu);
    if (E->events.empty()) return 0;
    Event e = std::move(E->events.front());
    E->events.pop_front();
    *handle = e.handle;
    *t_ns = e.t;
    *n = (int)std::min<size_t>(e.bytes.size(), (size_t)cap);
    if (*n) std::memcpy(bytes, e.bytes.data(), *n);
    return e.kind;
}

int64_t ame_late_max_ns(void) { return E ? E->lateMax.load() : 0; }
int64_t ame_late_sum_ns(void) { return E ? E->lateSum.load() : 0; }
int64_t ame_sent(void) { return E ? E->sent.load() : 0; }

}  // extern "C"
