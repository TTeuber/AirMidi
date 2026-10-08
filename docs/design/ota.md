# Updates: signed images, self-test, rollback (`ota`, `webui`)

## Flash layout (`firmware/partitions.csv`, 4 MB)

| Name | Offset | Size | Why |
|---|---|---|---|
| nvs | 0x9000 | 24 KB | settings and Wi-Fi networks; same offset as the default layout, so settings survived the switch |
| otadata | 0xF000 | 8 KB | which slot boots, and each image's state |
| phy_init | 0x11000 | 4 KB | radio calibration |
| ota_0 / ota_1 | 0x20000 / 0x1E0000 | 1.75 MB each | two app slots: one runs, the other receives |
| coredump | 0x3A0000 | 64 KB | the last crash (ELF), for download |
| storage | 0x3B0000 | 320 KB | LittleFS, for MIDI-CI profiles (phase 5) |

The app is about 1.48 MB with BLE, Wi-Fi, the HTTP server and the web UI, so a slot has ~18% headroom.

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
