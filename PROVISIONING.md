# Energy Pebble – Wi-Fi Provisioning

How a user gets a fresh (or reset) Energy Pebble onto their home Wi-Fi.
There are **two paths**; both are always compiled in. The device ships with
no Wi-Fi credentials — on first boot it advertises Improv BLE **and** opens
the "Energy Pebble" setup network at the same time.

## The two paths

| Path | Component(s) | Works on | UX |
|---|---|---|---|
| **Captive portal** (universal) | `wifi.ap` + `captive_portal` | **Every** phone/laptop: iOS Safari, Android, Firefox, desktop | Join open Wi-Fi network "Energy Pebble" → portal page auto-opens (else `192.168.4.1`) → pick SSID, enter password |
| **Improv BLE** (slick) | `esp32_ble_server` + `esp32_improv` | Chrome/Edge on Android & desktop (Web Bluetooth). **Not** iOS Safari, not Firefox | Open the setup page → tap "Connect Energy Pebble" → pick network in a dialog. No network switching needed |
| **Improv Serial** (factory/bench) | `improv_serial` | Chrome/Edge via WebSerial, or ESPHome tooling, over USB | Provision while flashing at the bench |

Web Bluetooth is Chromium-only by design — that is why the captive portal
stays and remains the primary documented path for iPhone users.

Config lives in:

- `components/provisioning.yaml` – Improv BLE + Serial (new)
- `components/connectivity.yaml` – Wi-Fi, open AP, captive portal, web server
- `components/boot.yaml` – status LED state machine (new behavior)
- `setup/` – the companion setup web page (see below)

## LED state map (center LED)

Replaces the old "yellow pulse for everything" boot animation. All fades are
≥ 1 s — no strobing (reduced-motion friendly). Implemented as a polling state
machine in `components/boot.yaml` (script keeps the historical id
`boot_led_sequence`).

| Light | State | Detected by |
|---|---|---|
| **Blue**, slow pulse | Setup mode: no credentials, waiting for user | captive portal active **and** no stored SSID |
| **Yellow**, slow pulse | Connecting: trying stored or just-received credentials | not connected, not in AP fallback; or Improv `on_provisioning` |
| **Red**, slow blink (fades to near-off) | Can't connect: credentials exist but network unreachable / wrong password. Setup network is up again for re-entry | captive portal active **and** stored SSID present |
| **Green**, slow pulse | Wi-Fi connected, waiting for first price data (usually a few seconds) | `wifi.is_connected()` |
| Normal price colors | Online and painting API data | first successful API paint ends the loop |

Note: after a *wrong password* via Improv, the first connection attempt shows
yellow (Improv flag), then falls back to red once the AP fallback re-engages
(`ap_timeout`, default 1 min). Improv itself also reports the failure back to
the browser dialog.

## Framework / flash decision

**Decision: stay on Arduino framework.** Reasons:

1. The config is Arduino-dependent throughout: `ESP.getEfuseMac()` lambdas in
   `api.yaml`, `ota.yaml`, `01_interface_order.yaml`, `connectivity.yaml`, and
   `rmt_channel:` in `leds.yaml` (invalid under ESP-IDF). Switching would mean
   touching every shared file — colliding with the parallel
   `thomaskgb/device-fw-signing` branch.
2. Fielded devices receive this build **over OTA**. Keeping the same
   framework and partition layout avoids any partition/NVS surprises;
   ESPHome-saved Wi-Fi credentials in NVS survive.
3. It fits (measured, see below).

Measured with the CI-pinned toolchain (`esphome==2025.6.0`,
board `esp32-s3-devkitc-1`, default ~1.79 MB app partition):

| Build | Flash | RAM |
|---|---|---|
| Baseline (main, no BLE) | 1,032,205 B (56.3%) | 42,604 B (13.0%) |
| With Improv BLE + Serial | 1,552,781 B (**84.6%**) | 62,272 B (19.0%) |

BLE costs ~520 KB flash / ~20 KB RAM. **Headroom: ~282 KB (15.4%).** That is
enough for the Ed25519 OTA-signing work (~small), but the budget is now
tight — watch CI size output on every merge. If it ever overflows, the
escape hatch is switching to ESP-IDF (smaller binaries), which then requires
porting the Arduino-only lambdas — plan it as its own change.

Required side-change: `wifi.power_save_mode` moved from `none` to `light` in
`connectivity.yaml` — BLE coexistence rejects `none` at config time. Slightly
higher Wi-Fi latency; irrelevant for a 15-minute polling cadence.

### ⚠️ Board ambiguity gate — NO OTA ROLLOUT until resolved

`energy_pebble.yaml` targets **esp32-s3-devkitc-1**, but the README and old
build dirs reference a **Lolin C3 Mini** (ESP32-C3, 4 MB flash). The numbers
above are S3 numbers. The C3 has a different BLE stack size and the same
~1.79 MB default app partition — it will be similarly tight but was **not
measured**. Flashing an S3-built image OTA onto a C3 (or vice versa) will
not boot. **Resolve which board the fielded devices actually are, compile
for that board, and only then tag a release.** First firmware reaches
devices over the current trust-on-first-update OTA path — there is no
recovery except physical USB access.

## Companion setup page (`setup/`)

One static page, `setup/index.html`, serving **both** platforms:

- **Chrome/Edge (Android, desktop):** shows a "Connect Energy Pebble"
  button using the **self-hosted** Improv BLE SDK vendored at
  `setup/vendor/improv-wifi-sdk/` (from npm `improv-wifi-sdk@1.4.1`,
  `dist/web/` bundle — no requests to improv-wifi.com or any CDN).
- **iOS Safari / Firefox / everything else:** the Bluetooth card is hidden
  automatically (`navigator.bluetooth` feature detection) and the page shows
  the captive-portal steps. The steps are always visible as fallback even on
  Chrome.
- Includes the LED legend in tenant-friendly wording.

**Hosting:** copy the `setup/` folder to any static host under HTTPS —
intended target `https://energypebble.tdlx.nl/setup/`. Web Bluetooth
requires a secure context; the page makes no external requests. To update
the SDK, re-vendor `dist/web/*` from the npm package.

**Branding:** plain CSS in the file head; colors match the LED palette.

## Device identity & account pairing (QR flow)

Decided design (page side implemented here; **backend not built yet**):

- Every Pebble gets a factory **QR sticker** encoding
  `https://energypebble.tdlx.nl/setup/?d=<device-id>&s=<per-device-secret>`.
  Scanning with the normal camera app opens the setup page with the device
  pre-identified — no in-page QR scanner needed. The secret (not the MAC,
  which is enumerable) is what proves possession for claiming.
- `name_add_mac_suffix: true` is enabled: devices advertise/host as
  `energy-pebble-<last 6 MAC hex>`.
- **The BLE connect step is gated on identity**: the button stays locked
  until the user identifies the device — by opening the page from the QR
  deep-link, scanning the sticker with the **in-page scanner** ("Scan QR
  code": getUserMedia + vendored `jsqr@1.4.0`, lazy-loaded, decoded locally;
  this path also captures the claim secret), or typing the device ID (full
  12-hex MAC or last-6 suffix, colons/case tolerated) into the "Which
  Pebble is this?" field. The page then calls `requestDevice` itself, filtering on the Improv
  service **and the exact name** `energy-pebble-<suffix>` — the browser
  picker never offers neighboring Pebbles or arbitrary Improv/ESP32 devices.
  (The SDK's stock launch button, which lists every Improv device, is no
  longer used — only its provisioning dialog is; `launch-button.js` was
  dropped from the vendored files.) Consequence: pre-suffix firmware
  (plain `energy-pebble`) will not match the filter — bench-test devices
  must run this branch's firmware.
  ⚠️ Side effect on fielded devices after OTA: hostname/mDNS changes from
  `energy-pebble.local` to `energy-pebble-xxxxxx.local` (AP SSID and backend
  X-Device-ID identity are unchanged; OTA is pull-based and unaffected).
- Intended full flow once the backend ships (stub marked in
  `setup/index.html`): login → claim (`POST /api/devices/claim` with id +
  secret) → provision Wi-Fi (BLE or captive portal — claiming needs no
  Bluetooth, so iPhone works too) → page polls a device-status endpoint
  until the device phones home → "connected and linked to your account".
  Claiming stays a **skippable step**, not a wall: re-provisioning after a
  router change or setup-by-a-caretaker must keep working without login.
- Backend work (other repo): per-device secrets at manufacturing, claim
  endpoint, device-status endpoint, sticker printing.

ESPHome's Improv has no device-side `next_url` option; the dashboard link on
the page is the hand-off.

## Security notes

- The setup AP is open (no password) and the Improv BLE service has no
  authorizer (no button on the device). Both are only reachable while the
  device has no Wi-Fi connection, which bounds the exposure window to
  setup/outage periods. This matches the pre-existing open-AP behavior.
- `improv_serial` requires physical USB access.

## What still needs on-device testing

Nothing below has run on hardware — this branch was compile-checked only
(`esphome==2025.6.0`, exit 0, sizes above):

1. **Improv BLE end-to-end** on Android Chrome and desktop Chrome/Edge via
   `setup/index.html` (serve over HTTPS or localhost): connect button is
   locked until QR scan / manual ID entry; after unlocking, the picker
   shows **only** `energy-pebble-<suffix>`; credentials apply and the
   success screen shows the device URL. Specifically verify the exact-name
   `requestDevice` filter matches the 20-char advertised name (shortened
   BLE advertisement names could make the device invisible — if so, fall
   back to `namePrefix` + post-pick suffix check).
2. **Captive portal on iOS Safari**: portal auto-opens after joining
   "Energy Pebble", provisioning works, phone falls back to home Wi-Fi.
   Also test the in-page QR scanner on iOS Safari and Android Chrome
   (camera permission prompt, decode speed, permission-denied fallback to
   manual entry).
3. **LED state transitions**: fresh device → blue; during connect → yellow;
   wrong password → red after ap_timeout (~1 min); success → green → price
   colors. Verify `has_sta()` / `captive_portal->is_active()` heuristics
   behave as expected on real Wi-Fi timing.
4. **Improv + wrong password**: browser dialog shows the error; device
   returns to provisionable state.
5. **BLE/Wi-Fi coexistence** under `power_save_mode: light`: no watchdog
   resets, HTTPS API calls still reliable, OTA still works.
6. **Flash fit on the real board** once the S3-vs-C3 question is resolved
   (rebuild + re-measure for C3 if that's the fielded board).
7. **OTA upgrade path**: update one fielded device from current firmware to
   this build and confirm saved Wi-Fi credentials survive (NVS preferences).
