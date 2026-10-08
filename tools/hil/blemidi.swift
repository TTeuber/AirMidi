// Hand a BLE MIDI peripheral to CoreMIDI from the command line, the way Audio MIDI Setup's Bluetooth window does,
// so test scripts can use the receiver as an ordinary CoreMIDI port (and macOS picks its own MIDI connection
// parameters). Follows the steps in CoreMIDI/MIDIBluetoothConnection.h: scan with CoreBluetooth, connect, call
// MIDIBluetoothDriverActivateAllConnections, wait for the MIDI device to show up, then let go of our connection.
//
//   swiftc -O tools/hil/blemidi.swift -o tools/hil/build/blemidi
//   tools/hil/build/blemidi [-v] connect <name> prints the CoreMIDI device name once it is online
//   tools/hil/build/blemidi disconnect <name>
//   tools/hil/build/blemidi list                 MIDI devices and whether they are online
import CoreBluetooth
import CoreMIDI
import Foundation

let midiService = CBUUID(string: "03B80E5A-EDE8-4B33-A751-6CE34EC4C700")

func stringProp(_ obj: MIDIObjectRef, _ key: CFString) -> String? {
    var s: Unmanaged<CFString>?
    return MIDIObjectGetStringProperty(obj, key, &s) == noErr ? s?.takeRetainedValue() as String? : nil
}

func intProp(_ obj: MIDIObjectRef, _ key: CFString) -> Int32 {
    var v: Int32 = 0
    MIDIObjectGetIntegerProperty(obj, key, &v)
    return v
}

func midiDevices() -> [(name: String, online: Bool, dev: MIDIDeviceRef)] {
    (0..<MIDIGetNumberOfDevices()).map { i in
        let d = MIDIGetDevice(i)
        return (stringProp(d, kMIDIPropertyName) ?? "?", intProp(d, kMIDIPropertyOffline) == 0, d)
    }
}

let midiChar = CBUUID(string: "7772E5DB-3868-4112-A1A9-F2669D106BF3")

final class Connector: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    let target: String
    let disconnect: Bool
    var central: CBCentralManager!
    var peripheral: CBPeripheral?

    init(target: String, disconnect: Bool) {
        self.target = target
        self.disconnect = disconnect
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        guard c.state == .poweredOn else {
            if c.state == .unauthorized || c.state == .unsupported { fail("Bluetooth state \(c.state.rawValue)") }
            return
        }
        // Already connected (e.g. owned by CoreMIDI)? It does not advertise then; look it up instead.
        for p in c.retrieveConnectedPeripherals(withServices: [midiService]) where p.name == target {
            found(p)
            return
        }
        c.scanForPeripherals(withServices: [midiService])
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData ad: [String: Any],
                        rssi: NSNumber) {
        let name = ad[CBAdvertisementDataLocalNameKey] as? String ?? p.name
        if name == target { c.stopScan(); found(p) }
    }

    func found(_ p: CBPeripheral) {
        log("found \(target) (\(p.identifier), state \(p.state.rawValue))")
        peripheral = p
        if disconnect {
            let r = MIDIBluetoothDriverDisconnect(p.identifier.uuidString as CFString)
            print(r == noErr ? "disconnected \(target)" : "MIDIBluetoothDriverDisconnect: \(r)")
            exit(r == noErr ? 0 : 1)
        }
        central.connect(p)
    }

    // Connect, then find the MIDI characteristic and subscribe like a MIDI client would, and only then hand the
    // connection to CoreMIDI.
    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        log("connected to \(p.identifier), discovering the MIDI service")
        p.delegate = self
        p.discoverServices([midiService])
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = p.services?.first(where: { $0.uuid == midiService }) else { fail("no MIDI service") }
        p.discoverCharacteristics([midiChar], for: s)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        guard let ch = s.characteristics?.first(where: { $0.uuid == midiChar }) else { fail("no MIDI characteristic") }
        p.setNotifyValue(true, for: ch)
    }

    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor ch: CBCharacteristic, error: Error?) {
        log("subscribed (\(error?.localizedDescription ?? "ok")), activating")
        let r = MIDIBluetoothDriverActivateAllConnections()
        if r != noErr { fail("MIDIBluetoothDriverActivateAllConnections: \(r)") }
        poll(tries: 100)
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        fail("connect failed: \(error?.localizedDescription ?? "?")")
    }

    func poll(tries: Int) {
        if let d = midiDevices().first(where: { $0.name.hasPrefix(target) && $0.online }) {
            central.cancelPeripheralConnection(peripheral!)  // CoreMIDI keeps its own connection
            print(d.name)
            exit(0)
        }
        if tries == 0 { fail("\(target) did not come online in CoreMIDI") }
        if tries % 10 == 0 { MIDIBluetoothDriverActivateAllConnections() }  // ask again now and then
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { self.poll(tries: tries - 1) }
    }
}

var verbose = CommandLine.arguments.contains("-v")
func log(_ msg: String) { if verbose { FileHandle.standardError.write((msg + "\n").data(using: .utf8)!) } }

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
    exit(1)
}

let args = CommandLine.arguments.filter { $0 != "-v" }
var keep: Connector?
switch (args.count > 1 ? args[1] : "", args.count > 2 ? args[2] : nil) {
case ("list", _):
    for d in midiDevices() { print("\(d.online ? "online " : "offline") \(d.name)") }
    exit(0)
case ("connect", let name?):
    keep = Connector(target: name, disconnect: false)
case ("disconnect", let name?):
    keep = Connector(target: name, disconnect: true)
default:
    fail("usage: blemidi connect|disconnect <name> | list")
}
DispatchQueue.main.asyncAfter(deadline: .now() + 30) { fail("timed out") }
RunLoop.main.run()
