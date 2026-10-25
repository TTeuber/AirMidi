# Settings and Wi-Fi setup over BLE

The phone app (`app/`, plan in `docs/app.md`) sets the box up over BLE, in Play mode, without joining the box's
access point. Three pieces: one API for both transports, a GATT service that carries it, and Wi-Fi setup that works
outside Network mode.

## One API, two transports (`components/api`)

`api_call(method, path, body)` returns an HTTP status and JSON text. The web server (`webui`) registers `/api/*` and
hands every JSON request to it; the BLE config service does the same. So a browser over Wi-Fi and a phone over BLE
get the same answers from the same code, and the web regression test (`tools/hil/web_test.py`) checks both paths'
handlers. Firmware uploads and the core dump download stay HTTP only (binary, megabytes); BLE has its own update
service (`ble_update`).

A reply can carry `then`: work that runs after the reply has gone out. A mode switch takes the network (or BLE) away,
so the answer must leave first.

Alternative considered: ESP-IDF's `wifi_provisioning` (protocomm over BLE). It brings its own BLE setup, which
conflicts with ours (NimBLE already runs BLE MIDI, the central and the update service), covers Wi-Fi only, and
would leave settings, modes and the rest without a BLE path.

## The GATT service (`components/ble_config`, framing in `lib/blecfg.c`)

Two characteristics: requests are written (with response), replies are notified. A message is split into pieces of
at most MTU - 3 bytes; each starts with a message id and a flags byte (LAST on the final piece). Requests are
`METHOD PATH\n` + body, replies `STATUS\n` + JSON, with the request's id. A piece with a new id starts a new message,
so a client that gave up on one simply sends the next. Requests up to 8 KB (the buffer exists only while a phone uses
the service); a longer one is answered 413 once its last piece arrives. Host-tested in `test/test_blecfg.c`, and
the app's Dart side against the same cases (`app/test/config_test.dart`).

Requests run on a worker task, not in the NimBLE host task: some handlers take a while (status builds ~2.5 KB of
JSON, settings write NVS), and the host task must keep BLE MIDI moving meanwhile. A reply piece that finds NimBLE out
of buffers waits 5 ms and tries again (up to 1 s).

## Who may use it

The request characteristic needs an encrypted link: the phone bonds ("just works", the box has no display). That
alone would let anyone in range change the box's settings, so the box also keeps a list of up to four trusted phones
(their identity addresses, in NVS). A phone outside the list gets ATT error 0x08 (insufficient authorization), unless
the pairing window is open, in which case it joins the list:

- in Setup mode (an 8 s button press: physical access);
- for 2 minutes after a power-on or a reset by hand, not after the box's own restarts (a mode or settings change
  restarts it, and the LAN web page can cause that);
- for 2 minutes when a trusted phone (or the web page) asks, to add another (`POST /api/phones/window`).

`DELETE /api/phones` forgets them all. Rev B's BLE button ("hold = pair") will open the window too. The web page on
the LAN and the Setup access point remain open, as before; BLE is now the stricter path.

## Wi-Fi setup outside Network mode (`components/net`)

`POST /api/wifi/scan` and `POST /api/wifi/try {ssid, pass}` work in Play and Setup mode. In Play mode the radio
comes on as a station for the scan or the attempt and goes off again; in Setup mode the station is added next to the
access point (APSTA, and it stays so: switching back would restart the access point under a browser using it). The
box lists only what it can join (2.4 GHz), strongest first, one entry per name. A try that joins stores the network
(newest first, so Network mode tries it first) and leaves again; one that fails says why in words (not found, wrong
password, unsupported security). Network mode answers 409: the box is busy joining there. A mode change during a
probe cancels it.

Caveat: in APSTA the access point follows the station's channel, so a browser on the Setup access point may drop for a
moment while the box tries a network on another channel. Phones on BLE are not affected.

## Measured (2026-10-24, Pixel 9a, C6 box)

- Pairing from the app: Android's "Pair with airmidi?" dialog once; the box page then showed the status, the stored
  networks and the phone as trusted (1 of 4). Response times not measured yet.
- Scan in Setup mode over BLE: the list was there within 7 s (3 networks). A wrong password was reported as such; the
  right one joined and was stored, both within 15 s; Network mode then joined it after the restart. The web
  regression test passed over HTTP against the refactored API (14/14).
- The phone stayed trusted across software restarts with the window closed.
- Connection interval: Android moves to 48.75 ms right after the link is encrypted. The box's 7.5-15 ms request at
  connect collided with that change (HCI 0x23, LL procedure collision), which is not a refusal: the box now asks again
  when the interval drifts above 15 ms (up to three times per connection) and only treats a real refusal as Apple's.
  Result: 15 ms after pairing, as before pairing.
