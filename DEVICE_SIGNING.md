# Device-side firmware signature verification

Device half of the Ed25519 firmware-signing scheme. The server half (signing at
upload, serving `signature` / `signature_alg` on `/api/ota/check`) lives in the
**energy-pebble-api** repo — see its `FIRMWARE_SIGNING.md` (branch
`worktree-signed-firmware`).

> ## ⚠️ DO NOT PUSH ANY OTA UNTIL THE BOARD QUESTION IS RESOLVED
>
> `energy_pebble.yaml` targets `esp32-s3-devkitc-1`, but the README says
> Lolin **C3** and old build dirs contain both `energy-pebble` and
> `lolin-c3-led-ring` targets. A wrong-architecture binary **bricks** the
> device. Inventory the fleet's actual silicon and gate `/api/ota/check` on a
> board/variant match before releasing anything — including the first
> signature-capable firmware. All code in this change is architecture-agnostic
> (portable C/C++), so it compiles for either board once the target is known.

## What the device does now

`components/ota.yaml` → `perform_ota_update`:

1. Fetch `/api/ota/check`; read `download_url`, `md5_checksum`, and the new
   `signature` (base64) + `signature_alg` fields.
2. **Fail closed** — the update is refused (device keeps running its current
   firmware, nothing is flashed) when the signature is missing, the algorithm
   is not `ed25519`, the signature is malformed base64 / not 64 bytes, the
   download fails, or the signature does not verify.
3. Otherwise the `ota_signature` component **streams the firmware image**
   (1 KB chunks, no RAM buffering) and verifies the Ed25519 signature over the
   exact downloaded bytes against the public key embedded at build time, while
   also computing the MD5 of those bytes.
4. On success, the stock `ota.http_request.flash` performs the actual flash —
   with its MD5 check **pinned to the MD5 of the verified bytes**, not the
   server-asserted MD5. The image is downloaded twice (once to verify, once to
   flash); pinning the second download to the first defeats a server/MITM that
   swaps the image between passes (that would require an MD5 second-preimage
   of a fixed, signed image — not a practical attack).
5. On refusal: a distinct LED error state (center + full ring blink **magenta**
   6×, a color the normal green/yellow/red price display never uses, then the
   price display is restored), the `Update Status` sensor shows
   `Update REFUSED: <reason>`, and the device POSTs
   `status: rejected_<reason>` to `/api/ota/status/{mac}` so the server can
   see refusals. Reasons: `unsigned_firmware`, `unsupported_signature_alg`,
   `invalid_signature`.

The 12-hour check interval, version comparison, and success reporting are
unchanged.

## The component

`esphome/components/ota_signature/` — ESPHome external component (registered
in `energy_pebble.yaml` under `external_components`), configured in
`components/ota.yaml`:

```yaml
ota_signature:
  id: fw_sig_verifier
  http_request_id: http_request_data
  public_key: !secret firmware_signing_pubkey
```

C++ API used from YAML lambdas:

- `verify_url(url, signature_b64) -> bool` — stream + verify; feeds the
  watchdog during the download.
- `verified_md5()` — lowercase-hex MD5 of the verified bytes (empty unless the
  last verify succeeded).
- `last_error()` — human-readable failure reason.

### Signature contract

- Algorithm: **Ed25519** (RFC 8032, pure — not Ed25519ph).
- Signed message: the **raw firmware `.bin` bytes** exactly as flashed.
- Wire format: base64 of the raw 64-byte signature, in `/api/ota/check`'s
  `signature` field; `signature_alg` is `"ed25519"`.
- Public key: 32 raw bytes, configured as 64 hex chars.

Verification is `SHA-512(R ‖ A ‖ M)` streamed over `M`, reduced mod L, then
`crypto_eddsa_check_equation` — Monocypher public API only.

### Vendored crypto

[Monocypher](https://monocypher.org) **4.0.2** by Loup Vaillant, vendored
verbatim: `monocypher.{c,h}`, `monocypher-ed25519.{c,h}` from
`https://github.com/LoupVaillant/Monocypher/tree/4.0.2/src`, plus its
`LICENCE.md`. **Dual-licensed CC0-1.0 OR BSD-2-Clause** (SPDX headers in each
file). Portable C99, no architecture-specific code — builds identically for
ESP32-S3 and ESP32-C3. SHA-512 vendored file hashes:

```
02174117935699d418443c75a558a287deb06ef8cf7c1adced61d9047d2f323d  monocypher.c
97d581639dfa72be08a6d57deb7d79b736be001cb416819cab196d22559d242b  monocypher-ed25519.c
```

## Setting the public key

1. `python firmware_signing.py keygen --out-dir ~/keys` (energy-pebble-api
   repo, run **offline**) prints `FIRMWARE_SIGNING_PUBKEY=<64 hex chars>`.
2. Local builds: put it in `secrets.yaml` **and** `components/secrets.yaml`
   as `firmware_signing_pubkey` (template in `secrets_template.yaml`).
3. CI: set repo secrets `FIRMWARE_SIGNING_PUBKEY` (public key, hex — baked
   into the firmware) and `FIRMWARE_SIGNING_KEY` (private key PEM — signs the
   binary before upload). Both are wired into
   `.github/workflows/build-firmware.yml`.

The documented placeholder is all zeros. A placeholder build **refuses every
update** (fail closed, loud warnings at build time and in `dump_config`), so
never release one: fielded devices receive the first signature-capable
firmware over the *current unverified* OTA path (trust-on-first-update, see
the server doc) and a placeholder build would strand them — they'd keep
working but could never take another OTA without physical access.

## Why `verify_ssl` is still `false`

`verify_ssl: true` is a hard config error for ESP32 + `framework: arduino` on
ESPHome ≤ 2025.x ("ESPHome supports certificate verification only via
ESP-IDF"), and CI builds with ESPHome 2025.6.0 / Arduino. The Ed25519
signature is exactly the control that does not trust the transport: a MITM on
the OTA path can serve stale JSON or garbage, but cannot produce firmware the
device will flash. Remaining exposure of the unverified channel: the API
*payloads* (color codes, version info) and downgrade/withholding games — worth
closing eventually. Follow-up: migrate to `framework: esp-idf` (or ESPHome ≥
2026.x where ESP32/Arduino uses the IDF HTTP client and verification works),
then set `verify_ssl: true`.

## Verified locally (no hardware involved)

- `esphome config` and full `esphome compile` with ESPHome **2025.6.0** (the
  CI-pinned version), `esp32-s3-devkitc-1` / Arduino: **success**, all
  `ota_signature` sources compiled.
- Host-side crypto tests: a signature produced by Python `cryptography` (the
  server's signer) verifies through the component's exact streaming sequence
  (1024-byte and 333-byte chunks); tampered message and tampered signature are
  rejected; RFC 8032 TEST 3 vector passes streamed one byte at a time.
- The component's base64 decoder passes 12 strict-decoding edge cases
  (bad padding, invalid chars, truncation, overflow guard).
- Note: `esphome config` with ESPHome 2026.2.4 fails on a **pre-existing**
  issue unrelated to this change (`rmt_channel` option removed from
  `esp32_rmt_led_strip`) — the repo currently requires ESPHome ≤ 2025.x.

## Still needs on-device testing

Nothing here has run on hardware. Before any fleet rollout, on a bench device
(flashed by cable):

1. **Happy path:** signed release on the server → device verifies (log:
   `Ed25519 signature VALID over N bytes`), flashes, reboots, reports
   `updated`.
2. **Unsigned release** → refused with `unsigned_firmware`, magenta blink,
   `rejected_unsigned_firmware` reported, device keeps running.
3. **Tampered image or wrong key** (e.g. re-sign with a different key) →
   `invalid_signature` refusal; confirm no flash happened.
4. **Placeholder-key build** refuses a correctly signed update.
5. **Resource behavior:** heap headroom and watchdog during the ~1–2 MB
   streaming pass (SHA-512 + MD5 on the fly), and that the LED alert +
   restored price display look right on the 25-LED ring.
6. **Timing:** the extra verification download roughly doubles OTA transfer
   time; confirm the 15 s inactivity timeout is comfortable on real Wi-Fi.
7. Cross-check one signature end-to-end: `firmware_signing.py sign` output
   verifies on-device (same bytes, same key).
