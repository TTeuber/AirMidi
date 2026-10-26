package dev.tteuber.airmidi

import android.bluetooth.BluetoothManager
import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.media.midi.MidiManager
import android.media.midi.MidiOutputPort
import android.media.midi.MidiReceiver
import android.os.Handler
import android.os.Looper
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

// Android's MIDI service, for what is not an airmidi box (lib/link/android_midi.dart): BLE MIDI devices such as a
// Mac (Android's driver asks for the fast connection interval and the big MTU, which the BLE plugin cannot), USB MIDI
// devices, and the USB peripheral port (the phone as a USB MIDI device on a computer, USB mode "MIDI"). One open
// device = its first input port (we send) and first output port (we receive); bytes only, the parsing is in Dart.
class MainActivity : FlutterActivity() {
    private val main = Handler(Looper.getMainLooper())
    private lateinit var midi: MidiManager
    private val open = mutableMapOf<Int, Opened>()
    private var nextHandle = 1
    private var sink: EventChannel.EventSink? = null

    private class Opened(val device: MidiDevice, val input: MidiInputPort?, val output: MidiOutputPort?)

    override fun configureFlutterEngine(engine: FlutterEngine) {
        super.configureFlutterEngine(engine)
        midi = getSystemService(Context.MIDI_SERVICE) as MidiManager
        midi.registerDeviceCallback(object : MidiManager.DeviceCallback() {
            override fun onDeviceAdded(info: MidiDeviceInfo) = changed()
            override fun onDeviceRemoved(info: MidiDeviceInfo) = changed()
        }, main)

        MethodChannel(engine.dartExecutor.binaryMessenger, "airmidi/midi").setMethodCallHandler { call, result ->
            when (call.method) {
                "devices" -> result.success(devices())
                "openBluetooth" -> {
                    val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
                    val bt = adapter.getRemoteDevice(call.argument<String>("address"))
                    midi.openBluetoothDevice(bt, { d -> opened(d, result) }, main)
                }
                "open" -> {
                    val id = call.argument<Int>("id")
                    @Suppress("DEPRECATION") // getDevicesForTransport is API 33; this app starts at 31
                    val info = midi.devices.firstOrNull { it.id == id }
                    if (info == null) result.error("gone", "no MIDI device $id", null)
                    else midi.openDevice(info, { d -> opened(d, result) }, main)
                }
                "send" -> {
                    val o = open[call.argument<Int>("handle")]
                    val bytes = call.argument<ByteArray>("bytes")!!
                    if (o?.input == null) result.error("closed", "not open for sending", null)
                    else {
                        o.input.send(bytes, 0, bytes.size)
                        result.success(null)
                    }
                }
                "close" -> {
                    open.remove(call.argument<Int>("handle"))?.let { o ->
                        o.input?.close()
                        o.output?.close()
                        o.device.close()
                    }
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        }

        EventChannel(engine.dartExecutor.binaryMessenger, "airmidi/midi/events").setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(args: Any?, events: EventChannel.EventSink) {
                    sink = events
                }

                override fun onCancel(args: Any?) {
                    sink = null
                }
            })
    }

    private fun changed() {
        sink?.success(mapOf("event" to "devices"))
    }

    // Bluetooth devices are left out: they show up here only once opened, and the BLE scan already lists them.
    private fun devices(): List<Map<String, Any?>> {
        @Suppress("DEPRECATION")
        return midi.devices.filter { it.type != MidiDeviceInfo.TYPE_BLUETOOTH }.map { info ->
            mapOf(
                "id" to info.id,
                "name" to (info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "MIDI device ${info.id}"),
                "type" to when (info.type) {
                    MidiDeviceInfo.TYPE_USB -> "usb"
                    MidiDeviceInfo.TYPE_VIRTUAL -> "virtual"
                    else -> "other"
                },
                "inputs" to info.inputPortCount,
                "outputs" to info.outputPortCount,
            )
        }
    }

    // The open callback gets null when the device could not be opened (a BLE device out of reach, say).
    private fun opened(d: MidiDevice?, result: MethodChannel.Result) {
        if (d == null) {
            result.error("open", "could not open the MIDI device", null)
            return
        }
        val handle = nextHandle++
        val input = if (d.info.inputPortCount > 0) d.openInputPort(0) else null
        val output = if (d.info.outputPortCount > 0) d.openOutputPort(0) else null
        output?.connect(object : MidiReceiver() {
            override fun onSend(msg: ByteArray, offset: Int, count: Int, timestamp: Long) {
                val bytes = msg.copyOfRange(offset, offset + count)
                main.post { sink?.success(mapOf("event" to "midi", "handle" to handle, "bytes" to bytes)) }
            }
        })
        open[handle] = Opened(d, input, output)
        result.success(mapOf("handle" to handle, "name" to (d.info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "")))
    }
}
