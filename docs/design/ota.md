# Updates: signed images, self-test, rollback (`ota`, `webui`)

## Flash layout (`firmware/partitions.csv`, 4 MB)

| Name | Offset | Size | Why |
|---|---|---|---|
| nvs | 0x9000 | 24 KB | settings and Wi-Fi networks; same offset as the default layout, so settings survived the switch |
| otadata | 0xF000 | 8 KB | which slot boots, and each image's state |
| phy_init | 0x11000 | 4 KB | radio calibration |
| ota_0 / ota_1 | 0x20000 / 0x1F0000 | 1.81 MB each | two app slots: one runs, the other receives |
| coredump | 0x3C0000 | 64 KB | the last crash (ELF), for download |
| storage | 0x3D0000 | 192 KB | LittleFS, for MIDI-CI profiles (phase 5; a control list is a few KB) |

The layout is frozen from the first release on (an update cannot move partitions), so it was settled before one:
with HTTPS for release downloads the image reached 1.63 MB, so the build optimises for size (`-Os`, 1.45 MB), uses
the common-CA certificate bundle, leaves out BLE roles not used yet, and storage gave 128 KB to the app slots.
About 20% of a slot is free for phases 4-6.

## An update, step by step

1. **Upload** (`POST /api/update`): the image streams into the slot that is not running, 4 KB at a time; erasing
   happens as it goes (`OTA_WITH_SEQUENTIAL_WRITES`) instead of 1.75 MB up front.
2. **Verify** (`esp_ota_end`): image format, SHA-256, and the **signature**. Images are signed at build time
   (Secure Boot V2 format, RSA-3072); the running firmware accepts only images signed with the key it was itself
   signed with. A bad image is refused with 400 and the box carries on as before.
3. **Switch**: the new slot becomes the boot slot and the box restarts. The image boots in *pending verify*.
4. **Self-test** (`ota_confirm_after`, 10 s): settings readable, BLE host synced, web server up, nothing dropped
   on the bus. Pass: the image is marked valid. Fail: it is marked invalid and the box restarts into the previous
   image. A crash before the verdict has the same effect: the bootloader sees an image still pending verify and
   falls back.

## Signatures without burning eFuses

`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`: the app checks
signatures of updates; nothing is written to eFuses. That protects against installing a wrong or tampered image
over the air, which is the threat for a box with an unauthenticated web page on a LAN. It does not protect against
someone with a USB cable flashing anything they like; that is what hardware Secure Boot (irreversible eFuses) and
flash encryption are for, and they are left off on purpose: board #1 is the only C6, and a mistake there bricks it.

RSA-3072 rather than ECDSA because the S3 supports only RSA for this format, and one scheme for both boards keeps
the release pipeline simple. Keys: `firmware/keys/dev_signing_key.pem` for dev builds (gitignored, made by
`build.sh` on first use); release images are signed in CI with a key held as a GitHub Actions secret.

### Found on the rig: a leftover signature

The first `ota_test.py` run accepted an *unsigned* image. Sequential writes erase only the sectors written, and the
verifier reads the signature block from the 4 KB sector after the image data. The previous (rejected, corrupted)
upload had left a signed copy's signature block there, and the unsigned image had the same content, so the
leftover signature matched. Not exploitable (it needs a genuine signature for exactly those bytes) but wrong:
`ota_end()` now erases that sector before verifying, so only a signature that came with the upload counts. The
test uploads an unsigned image whose content was never signed.

## Tests (`tools/hil/ota_test.py`, 13 checks on the C6)

It builds four images (`build.sh` with `BUILD_DIR`, `PROJECT_VER` and `EXTRA_DEFAULTS=test/sdkconfig.*`) and
uploads them through the web API:

| Upload | Expected | Result |
|---|---|---|
| corrupted (64 bytes flipped) | refused, box unchanged | 400 |
| unsigned | refused | 400 |
| signed with another key | refused | 400 |
| good | restarts into it, pending verify, valid after 10 s | ota_0 -> ota_1, valid |
| self-test fails (`CONFIG_AIRMIDI_TEST_SELFTEST_FAIL`) | boots pending verify, rolls back | back on ota_1, ota_0 marked rolled back |
| crashes 3 s after boot (`CONFIG_AIRMIDI_TEST_CRASH`) | rolls back, core dump kept | back on ota_1, 13.7 KB dump from task esp_timer |

A 1.48 MB image uploads in about 16.5 s (89 KB/s over Wi-Fi with modem sleep).

## Over BLE (`ble_update`)

For a box that is not on Wi-Fi (Play mode), the same signed images go over BLE: a GATT service next to BLE MIDI with
a control characteristic (begin with the size, end, abort; progress and the verdict come back as notifications), a
data characteristic (write without response) and the running version (read). `ota_end()` checks the signature as for
an upload, so anyone in BLE range can start an upload but nothing unsigned installs. Flash writes run in their own
task, so the BLE host task never waits for them; that task has 8 KB of stack, because checking the RSA-3072
signature overflowed 3 KB (in SHA-512).

Each data write carries its offset. BLE delivers what reaches the box in order, but writes without response can be
lost before they get there: CoreBluetooth drops the ones its queue has no room for, and bleak 3.0 does not wait for
room (`tools/hil/ble_ota.py` asks CoreBluetooth itself). So the box takes only the piece it expects next and, after
a gap, asks once for a resend from there (go-back-N). The client keeps at most 16 KB beyond the last progress
report, as flash writes are slower than BLE at times.

`ble_ota_test.py`, 9 checks in Play mode: an unsigned and a corrupted image are refused, and the box keeps
running; the good image (1.6 MB) goes over in 69 s (23 KB/s), the box restarts into it, passes its self-test, and
the web API shows it valid.

## Releases (`.github/workflows/release.yml`)

A `vX.Y.Z` tag builds both boards with the release key (repository secret `AIRMIDI_SIGNING_KEY`, written to
`firmware/keys/release_signing_key.pem` and selected by `firmware/sdkconfig.release`) and publishes, per board, the
OTA image (`airmidi-c6.bin`, `airmidi-s3.bin`), a zip for a first install over USB (`esptool.py write_flash
@flash_args`), and one `manifest.json`. A box reads `<base>/manifest.json` from `AIRMIDI_UPDATE_BASE_URL`
(GitHub's `releases/latest/download`, which redirects to the newest release's assets), compares the version with
its own, and on request downloads its board's image through the same `ota_begin/write/end` path as an upload, so
the signature check, the sector erase and the self-test all apply.

A box only accepts images signed with the key its own image was signed with: a box running a dev build (dev key)
updates only to dev builds, and moves to release builds with one install of the USB zip.

Tested (`tools/hil/release_test.py`, 8 checks): a local web server publishes `v0.1.0-test`; the box reports it as
newer, reports a missing manifest as an error, downloads with progress, restarts into it, confirms it after the
self-test and then reports itself up to date. HTTPS with GitHub's redirects was checked against real GitHub URLs
(the TLS handshake and redirect succeed; the manifest is a 404 until a release exists).
