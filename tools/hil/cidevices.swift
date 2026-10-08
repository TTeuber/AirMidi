// List the MIDI-CI devices macOS has discovered (the MIDI server runs Discovery on every endpoint by itself),
// with the UMP endpoint each one sits behind. Waits up to <seconds> (default 10) for one to appear on an endpoint
// whose name contains <filter>, so a test can ask "did macOS find the device behind the receiver?".
//
//   swiftc -O tools/hil/cidevices.swift -o tools/hil/build/cidevices
//   tools/hil/build/cidevices [filter] [seconds]
import CoreMIDI
import Foundation

let args = CommandLine.arguments
let filter = args.count > 1 ? args[1] : ""
let seconds = args.count > 2 ? Double(args[2]) ?? 10 : 10

func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02X", $0) }.joined(separator: " ") }

func describe(_ d: MIDICIDevice) -> String {
    let m = d.deviceInfo.manufacturerID.sysExIDByte
    return "MUID \(String(format: "%07X", d.muid)), manufacturer \(hex([m.0, m.1, m.2])), "
        + "family \(d.deviceInfo.family), model \(d.deviceInfo.modelNumber), type \(d.deviceType.rawValue), "
        + "PE \(d.supportsPropertyExchange), profiles \(d.supportsProfileConfiguration), max SysEx \(d.maxSysExSize)"
}

// Legacy MIDI 1.0 devices (BLE, DIN) are listed by the manager but are not reachable through umpEndpoints, so
// both lists are printed; the filter matches endpoint names, or the "airmidi" family/model when there is none.
func report() -> Bool {
    var hit = false
    for d in MIDICIDeviceManager.shared.discoveredCIDevices {
        print("discovered: \(describe(d))")
        if !filter.isEmpty && d.deviceInfo.manufacturerID.sysExIDByte.0 == 0x7D { hit = true }
    }
    for ep in MIDIUMPEndpointManager.shared.umpEndpoints {
        for fb in ep.functionBlocks {
            guard let d = fb.midiCIDevice else { continue }
            print("\(ep.name) / block \(fb.functionBlockID) \"\(fb.name)\": \(describe(d))")
            if filter.isEmpty || ep.name.localizedCaseInsensitiveContains(filter) { hit = true }
        }
    }
    return hit
}

let start = Date()
_ = MIDICIDeviceManager.shared  // the first access copies the server's cache and starts notifications
func poll() {
    if report() && !filter.isEmpty { exit(0) }
    if Date().timeIntervalSince(start) > seconds {
        print("total discovered: \(MIDICIDeviceManager.shared.discoveredCIDevices.count)")
        exit(filter.isEmpty ? 0 : 1)
    }
    print("--")
    DispatchQueue.main.asyncAfter(deadline: .now() + 1) { poll() }
}
DispatchQueue.main.async { poll() }
RunLoop.main.run()
