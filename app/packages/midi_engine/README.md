# midi_engine

The desktop app's MIDI engine (`docs/desktop.md`, D1): the computer's MIDI ports through
[libremidi](https://github.com/celtera/libremidi) (header-only, `third_party/libremidi`, v5.4.3), reached from Dart
over `dart:ffi`. Android does not use it (it has `MainActivity.kt`).

- `src/engine.h`, `src/engine.cpp`: the C API. Input is stamped on arrival and queued; Dart drains the queue after a
  wake-up (`NativeCallable.listener`). Output is queued with a due time; a real-time thread sends each message then.
- `lib/midi_engine.dart`: the Dart side (`MidiEngine.instance`).
- macOS builds through `macos/midi_engine.podspec`, Windows and Linux through `src/CMakeLists.txt`.

Test without Flutter (a loopback through the engine's own virtual port, with timing):

```
cmake -S test_native -B test_native/build -G Ninja && cmake --build test_native/build && test_native/build/engine_test
```

Measured on the Mac (M-series, macOS 26), 400 notes 5 ms apart queued at once: all back in order with a SysEx in
between; arrival minus due time median 0.07 ms, max 0.19 ms. Before the send thread had real-time scheduling it
woke ~1 ms late (median 1.02, max 5.5 ms).
