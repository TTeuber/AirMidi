// Connect one of macOS's RTP-MIDI sessions (Audio MIDI Setup's MIDI Network window, "My Sessions") to a host, from
// the command line: by setting the session's peers in the `apple.midirtp.session` property of its entity on
// CoreMIDI's "Network" device. MIDINetworkSession, the public API, does nothing on macOS 26 from a command-line
// tool (it stays disabled). The peer format is the one the driver keeps after Audio MIDI Setup connects:
// {name, address: "\t<service>\tlocal."}; macOS adds `state = connected`. An address with a port is dropped.
//
// macOS ends a session that no CoreMIDI client has open after a few seconds (BY to the peer), so keep the
// session's port open while it should stay connected.
//
//   swiftc -O tools/hil/rtpconnect.swift -o tools/hil/build/rtpconnect
//   tools/hil/build/rtpconnect <session> <service>     e.g. "RTP Session 3" airmidi-7530
//   tools/hil/build/rtpconnect --remove <session>      no peers: macOS ends the session (BY)
//   tools/hil/build/rtpconnect --show
import CoreMIDI
import Foundation

func props(_ o: MIDIObjectRef) -> [String: Any] {
    var p: Unmanaged<CFPropertyList>?
    MIDIObjectGetProperties(o, &p, true)
    return p?.takeRetainedValue() as? [String: Any] ?? [:]
}

let key = "apple.midirtp.session" as CFString

func sessions() -> [(String, MIDIEntityRef)] {
    var out: [(String, MIDIEntityRef)] = []
    for i in 0..<MIDIGetNumberOfDevices() where (props(MIDIGetDevice(i))["name"] as? String) == "Network" {
        let d = MIDIGetDevice(i)
        for e in 0..<MIDIDeviceGetNumberOfEntities(d) {
            let ent = MIDIDeviceGetEntity(d, e)
            out.append((props(ent)["name"] as? String ?? "?", ent))
        }
    }
    return out
}

func session(_ ent: MIDIEntityRef) -> [String: Any] {
    var d: Unmanaged<CFDictionary>?
    MIDIObjectGetDictionaryProperty(ent, key, &d)
    return d?.takeRetainedValue() as? [String: Any] ?? [:]
}

func setPeers(_ ent: MIDIEntityRef, _ peers: [[String: Any]]) {
    var s = session(ent)
    s["peers"] = peers
    let err = MIDIObjectSetDictionaryProperty(ent, key, s as CFDictionary)
    if err != noErr { print("could not set the session: \(err)") }
}

var args = Array(CommandLine.arguments.dropFirst())
if args.first == "--show" {
    for (name, ent) in sessions() { print(name, session(ent)) }
    exit(0)
}
let remove = args.first == "--remove"
if remove { args.removeFirst() }
guard let name = args.first, let ent = sessions().first(where: { $0.0 == name })?.1, remove || args.count == 2 else {
    print("usage: rtpconnect <session> <service> | --remove <session> | --show")
    exit(2)
}
setPeers(ent, [])  // a fresh connection: writing an unchanged peer list does nothing
if !remove {
    RunLoop.current.run(until: Date().addingTimeInterval(0.5))
    setPeers(ent, [["name": args[1], "address": "\t\(args[1])\tlocal."]])
    RunLoop.current.run(until: Date().addingTimeInterval(2))
}
print(session(ent)["peers"] ?? "")
