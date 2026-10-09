// A BLE MIDI peripheral on the Mac, standing in for a BLE MIDI keyboard or controller: the device the box's BLE
// central connects to. It advertises the BLE MIDI service under a name, notifies what it is told to send, and
// prints what the box writes to it, each with the Mac's clock (seconds since 1970) for latency measurements.
//   swiftc -O tools/hil/blepeer.swift -o tools/hil/build/blepeer
//   tools/hil/build/blepeer [name]          default "airmidi-peer"
// stdin:  "send <hex>"   one MIDI message, e.g. "send 903c64"
// stdout: "ready", "subscribed", "unsubscribed", "tx <time> <hex>", "rx <time> <hex>" (the MIDI bytes of a packet)
import CoreBluetooth
import Foundation

let midiService = CBUUID(string: "03B80E5A-EDE8-4B33-A751-6CE34EC4C700")
let midiChar = CBUUID(string: "7772E5DB-3868-4112-A1A9-F2669D106BF3")
let name = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "airmidi-peer"

func now() -> String { String(format: "%.6f", Date().timeIntervalSince1970) }
func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }
func say(_ s: String) {
    print(s)
    fflush(stdout)
}

// Strip a BLE MIDI packet's header and timestamps (enough for the box's packets: no SysEx continuations needed).
func midiBytes(_ p: Data) -> Data {
    var out = Data()
    let b = [UInt8](p)
    var i = 1
    var lastWasTs = false
    while i < b.count {
        if b[i] & 0x80 != 0 && !lastWasTs {
            lastWasTs = true  // a timestamp byte
        } else {
            out.append(b[i])
            lastWasTs = false
        }
        i += 1
    }
    return out
}

final class Peer: NSObject, CBPeripheralManagerDelegate {
    var pm: CBPeripheralManager!
    var ch: CBMutableCharacteristic!
    var pending: [Data] = []

    override init() {
        super.init()
        pm = CBPeripheralManager(delegate: self, queue: DispatchQueue.main)
    }

    func peripheralManagerDidUpdateState(_ p: CBPeripheralManager) {
        guard p.state == .poweredOn else { return }
        ch = CBMutableCharacteristic(type: midiChar, properties: [.read, .writeWithoutResponse, .notify], value: nil,
                                     permissions: [.readable, .writeable])
        let s = CBMutableService(type: midiService, primary: true)
        s.characteristics = [ch]
        p.add(s)
    }

    func peripheralManager(_ p: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        p.startAdvertising([CBAdvertisementDataLocalNameKey: name, CBAdvertisementDataServiceUUIDsKey: [midiService]])
    }

    func peripheralManagerDidStartAdvertising(_ p: CBPeripheralManager, error: Error?) {
        say(error == nil ? "ready" : "error \(error!)")
    }

    func peripheralManager(_ p: CBPeripheralManager, central: CBCentral, didSubscribeTo c: CBCharacteristic) {
        say("subscribed")
    }

    func peripheralManager(_ p: CBPeripheralManager, central: CBCentral, didUnsubscribeFrom c: CBCharacteristic) {
        say("unsubscribed")
    }

    func peripheralManager(_ p: CBPeripheralManager, didReceiveRead r: CBATTRequest) {
        r.value = Data()  // BLE MIDI: a read returns no payload
        p.respond(to: r, withResult: .success)
    }

    func peripheralManager(_ p: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        let t = now()
        for r in requests { if let v = r.value { say("rx \(t) \(hex(midiBytes(v)))") } }
    }

    func send(_ midi: Data) {
        let ms = UInt64(Date().timeIntervalSince1970 * 1000) & 0x1FFF  // BLE MIDI timestamps: 13 bits of ms
        var pkt = Data([0x80 | UInt8(ms >> 7 & 0x3F), 0x80 | UInt8(ms & 0x7F)])
        pkt.append(midi)
        pending.append(pkt)
        flush()
    }

    func flush() {
        while let pkt = pending.first {
            guard pm.updateValue(pkt, for: ch, onSubscribedCentrals: nil) else { return }  // resumed when ready
            pending.removeFirst()
            say("tx \(now()) \(hex(midiBytes(pkt)))")
        }
    }

    func peripheralManagerIsReady(toUpdateSubscribers p: CBPeripheralManager) { flush() }
}

let peer = Peer()
DispatchQueue.global().async {
    while let line = readLine() {
        let parts = line.split(separator: " ")
        guard parts.count == 2, parts[0] == "send" else { continue }
        var d = Data()
        var s = Substring(parts[1])
        while s.count >= 2 {
            d.append(UInt8(s.prefix(2), radix: 16)!)
            s = s.dropFirst(2)
        }
        DispatchQueue.main.async { peer.send(d) }
    }
    exit(0)
}
RunLoop.main.run()
