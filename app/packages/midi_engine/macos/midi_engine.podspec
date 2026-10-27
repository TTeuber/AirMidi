# The desktop MIDI engine for macOS: src/engine.cpp (through Classes/engine.cpp, as podspecs cannot reach outside
# their folder) with libremidi header-only on CoreMIDI.
Pod::Spec.new do |s|
  s.name             = 'midi_engine'
  s.version          = '0.1.0'
  s.summary          = 'airmidi desktop MIDI engine (libremidi over dart:ffi)'
  s.description      = 'Ports, timestamped input and scheduled output for the airmidi desktop app.'
  s.homepage         = 'https://github.com/TTeuber/AirMidi'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Tyler Teuber' => 'https://github.com/TTeuber' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'
  s.dependency 'FlutterMacOS'
  s.platform = :osx, '11.0'
  s.frameworks = 'CoreMIDI', 'CoreAudio', 'CoreFoundation'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++20',
    'CLANG_CXX_LIBRARY' => 'libc++',
    'HEADER_SEARCH_PATHS' => '"${PODS_TARGET_SRCROOT}/../third_party/libremidi/include" "${PODS_TARGET_SRCROOT}/../src"',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) LIBREMIDI_HEADER_ONLY=1 LIBREMIDI_COREMIDI=1',
  }
  s.swift_version = '5.0'
end
