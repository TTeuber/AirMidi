# The desktop MIDI engine for Windows and Linux (macOS builds it through macos/midi_engine.podspec): engine.cpp
# with libremidi header-only on the platform's MIDI API.
cmake_minimum_required(VERSION 3.14)
project(midi_engine_library VERSION 0.1.0 LANGUAGES CXX)

add_library(midi_engine SHARED "engine.cpp")
set_target_properties(midi_engine PROPERTIES PUBLIC_HEADER engine.h OUTPUT_NAME "midi_engine")
target_compile_features(midi_engine PRIVATE cxx_std_20)
target_include_directories(midi_engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libremidi/include")
target_compile_definitions(midi_engine PRIVATE LIBREMIDI_HEADER_ONLY)

if(WIN32)
  # WinMM: every Windows; Windows MIDI Services (virtual ports, MIDI 2.0) to add when the Windows build starts
  target_compile_definitions(midi_engine PRIVATE LIBREMIDI_WINMM)
  target_link_libraries(midi_engine PRIVATE winmm)
else()
  find_package(ALSA REQUIRED)
  find_package(Threads REQUIRED)
  target_compile_definitions(midi_engine PRIVATE LIBREMIDI_ALSA)
  target_link_libraries(midi_engine PRIVATE ALSA::ALSA Threads::Threads)
endif()
