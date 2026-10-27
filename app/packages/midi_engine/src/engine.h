// The desktop MIDI engine (docs/desktop.md): the computer's MIDI ports through libremidi, for Dart over dart:ffi.
// What Dart cannot do on time is done here:
//   - input: every message is stamped with the monotonic clock (ame_now_ns) as it arrives and queued; Dart reads
//     the queue whenever it gets to it (ame_read) after a wake-up call;
//   - output: Dart hands over messages with the time they are due (ame_send); a thread of ours sends each one then.
// Plain MIDI 1.0 bytes both ways. Thread-safe: every function may be called from any thread.
#pragma once
#include <stdint.h>

#if defined(_WIN32)
#define AME_API __declspec(dllexport)
#else
#define AME_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Starts the engine (the send thread, the port observer). wake is called from any thread when something is waiting
// for ame_read: input, or the port list changed. It is called again only after ame_read has emptied the queue.
// Returns 0, or -1 if no MIDI system is available.
AME_API int ame_start(void (*wake)(void));
AME_API void ame_stop(void);

// The monotonic clock every timestamp uses, in ns.
AME_API int64_t ame_now_ns(void);

// The port lists, as of the last ame_refresh (call it after a "ports" event). out: 0 = inputs (we receive), 1 =
// outputs (we send). Strings stay valid until the next ame_refresh.
AME_API int ame_refresh(void);
AME_API int ame_port_count(int out);
AME_API const char *ame_port_name(int out, int i);
AME_API int ame_port_type(int out, int i);  // libremidi transport_type bits: 2 software, 8 hw, 16 usb, 32 ble, 128 network

// Opens port i of the last refresh, or a virtual port others see under name (an input others send to, or an
// output others receive from). Returns a handle > 0, or -1.
AME_API int ame_open(int out, int i);
AME_API int ame_open_virtual(int out, const char *name);
AME_API void ame_close(int handle);

// Queues n bytes (one or more whole messages) for an output, due at at_ns (ame_now_ns time; 0 or the past = now).
// Messages due at the same time go out in the order they were queued. Returns 0, or -1 for a bad handle.
AME_API int ame_send(int handle, const uint8_t *bytes, int n, int64_t at_ns);

// Drops everything still queued for an output (a sequencer stopping); returns how many messages were dropped.
AME_API int ame_clear(int handle);

// Takes the oldest waiting event: kind 1 = MIDI (handle, bytes, *t_ns its arrival time), kind 2 = the port list
// changed. Returns the kind (0 = nothing waiting); *n is the message length, cut to cap if longer.
AME_API int ame_read(int *handle, uint8_t *bytes, int cap, int *n, int64_t *t_ns);

// For the timing test: how late the send thread was, over all messages sent so far (ns).
AME_API int64_t ame_late_max_ns(void);
AME_API int64_t ame_late_sum_ns(void);
AME_API int64_t ame_sent(void);

#ifdef __cplusplus
}
#endif
