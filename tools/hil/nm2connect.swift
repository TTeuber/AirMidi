// Connect macOS to a Network MIDI 2.0 host from the command line, the way Audio MIDI Setup's MIDI Network window
// does: by adding the host to the "peers" of a session on CoreMIDI's "UMP Network" device. The format of the
// `midi2.udp.session` property is not documented; it was read back from a session made in Audio MIDI Setup
// (spike S1): peers = [{address: "\t<service>\tlocal.", name: <service>}].
//
//   swiftc -O tools/hil/nm2connect.swift -o tools/hil/build/nm2connect
//   tools/hil/build/nm2connect <service name>      e.g. airmidi-7530 (the mDNS instance name)
//   tools/hil/build/nm2connect --show
import CoreMIDI
import Foundation

func stringProp(_ obj: MIDIObjectRef, _ key: CFString) -> String {
    var s: Unmanaged<CFString>?
    MIDIObjectGetStringProperty(obj, key, &s)
    return (s?.takeRetainedValue() as String?) ?? ""
}

func umpNetworkSession() -> MIDIEntityRef? {
    for i in 0..<MIDIGetNumberOfDevices() {
        let d = MIDIGetDevice(i)
        guard stringProp(d, kMIDIPropertyName) == "UMP Network" else { continue }
        for e in 0..<MIDIDeviceGetNumberOfEntities(d) {
            let ent = MIDIDeviceGetEntity(d, e)
            var dict: Unmanaged<CFDictionary>?
            if MIDIObjectGetDictionaryProperty(ent, "midi2.udp.session" as CFString, &dict) == noErr { return ent }
        }
    }
    return nil
}

guard let ent = umpNetworkSession() else {
    FileHandle.standardError.write("no Network MIDI 2.0 session on the UMP Network device (make one in Audio MIDI Setup)\n".data(using: .utf8)!)
    exit(1)
}
var dict: Unmanaged<CFDictionary>?
MIDIObjectGetDictionaryProperty(ent, "midi2.udp.session" as CFString, &dict)
let session = (dict!.takeRetainedValue() as NSDictionary).mutableCopy() as! NSMutableDictionary
let args = CommandLine.arguments
if args.count < 2 || args[1] == "--show" {
    print(session)
    exit(0)
}
let service = args[1]
var peers = (session["peers"] as? [NSDictionary]) ?? []
peers.removeAll { ($0["name"] as? String) == service }
peers.append(["address": "\t\(service)\tlocal.", "name": service])
session["peers"] = peers
let r = MIDIObjectSetDictionaryProperty(ent, "midi2.udp.session" as CFString, session as CFDictionary)
print(r == noErr ? "asked macOS to connect to \(service)" : "MIDIObjectSetDictionaryProperty: \(r)")
exit(r == noErr ? 0 : 1)
