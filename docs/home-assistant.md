# Home Assistant integration

`wallview-agent` (Rust, `agent/`) makes the panel a device in Home Assistant through the **ESPHome native API**, the same open protocol the Open Home Foundation uses for [linux-voice-assistant](https://github.com/OHF-Voice/linux-voice-assistant). No MQTT broker and no YAML are needed. MQTT discovery remains an optional add-on.

## Adding the panel

0. Wi-Fi with [Improv](https://www.improv-wifi.com/) over Bluetooth LE: without a configured network the agent starts the Improv service right away (with a network that gives no address within 120 s, then too). Home Assistant (Improv BLE integration, also through ESPHome Bluetooth proxies) or the Improv web page shows "WallView xxxx"; entering the Wi-Fi password is the only step. The panel writes `/data/wallview/wpa_supplicant.conf` (SSID in hex, PSK as PBKDF2 hash), restarts the network and answers with its address. "Identify" blinks the backlight three times. The service stops 30 s after a successful connection.
1. The panel announces itself over mDNS as `_esphomelib._tcp` (`wallview-xxxxxx`), advertising `api_encryption_supported` and `api_provisioning=zero-psk` while it has no encryption key.
2. Home Assistant shows it as a discovered ESPHome device. "Add" is the only step.
3. Home Assistant first connects without encryption (the panel accepts that only while it has no key), reads the device info and creates the device. It then sees `api_encryption_provisionable`, opens a second connection with the well-known all-zero Noise key, generates a key and sends it base64-encoded (`NoiseEncryptionSetKeyRequest`). The panel stores it in `/data/wallview/api.key`; from then on it accepts only that key, answers unencrypted clients with the ESPHome reject frame (`0x01`, "Bad indicator byte") and advertises `api_encryption`.
4. With the WallView integration installed (see [Kiosk login](#kiosk-login)), Home Assistant also shows "Kiosk sign-in for WallView xxxx" as discovered; "Submit" signs in the kiosk. This adds no second device: the panel is only a device of the ESPHome integration.

Dashboard URL: as long as `/data/wallview/url` does not exist, the agent looks for Home Assistant over mDNS (`_home-assistant._tcp`, 10 s, again every minute) and writes its `internal_url`, or `http://<IPv4>:<port>` when that is empty; the kiosk then restarts with it. The rootfs has no mDNS name resolution, so `homeassistant.local` (the fallback default) does not resolve on the panel. Tested with Home Assistant 2026.11.3 in the build VM.

To hand the panel to another Home Assistant, delete `/data/wallview/api.key` and restart the agent.

The panel identifies itself honestly: manufacturer "WallView", model "Sonoff NSPanel Pro", project "WallView.WallView OS" with the agent version, platform `HOST`. ESPHome firmware updates do not apply to it; WallView OS updates will be offered through an `update` entity of the same API (phase 6).

## Entities

| Entity | Type | Source / action |
|---|---|---|
| Display | light (brightness) | `/sys/class/backlight/backlight` |
| Presence | binary sensor (occupancy) | proximity sensor above a threshold (hand in front of the panel) |
| Illuminance | sensor, lx | light sensor (`stk3310`, raw × scale) |
| Proximity | sensor, diagnostic | raw proximity value |
| CPU temperature | sensor, °C, diagnostic | `thermal_zone0` |
| Free memory | sensor, MB, diagnostic | `MemAvailable` |
| Uptime | sensor, s, diagnostic | `/proc/uptime` |
| Wi-Fi signal | sensor, dBm, diagnostic | `iw dev wlan0 link` |
| IP address, WallView version | text sensors, diagnostic | |
| Restart | button | `reboot` |
| Restart kiosk | button | restarts Cog |
| Sign in again | button, config | signs the kiosk in again: with the WallView integration a new automatic sign-in, otherwise the sign-in screen with the keyboard |
| `esphome.wallview_xxxxxx_pair` | action (`secret`) | used by the WallView integration, starts the automatic sign-in |
| Dashboard URL | text, config | `/data/wallview/url`, the kiosk restarts with the new URL |

Presence changes are sent immediately, sensors every 15 s, commands answer with the new state.

## Protocol details

- `agent/proto/api.proto` and `api_options.proto` come unchanged from aioesphomeapi (MIT); Rust code is generated at build time with `prost` and `protox` (no `protoc` needed).
- Noise `Noise_NNpsk0_25519_ChaChaPoly_SHA256`, panel is the responder, prologue `NoiseAPIInit\0\0`; server hello carries protocol `0x01`, node name and MAC; frames `0x01, length (u16 BE), ciphertext`, plaintext `type (u16 BE), length (u16 BE), protobuf`.
- API version 1.10, TCP port 6053.

Tested 2026-11-26 against the panel with aioesphomeapi 46.31 (the library Home Assistant uses): zero-key connection, key provisioning, zero key rejected afterwards, reconnection with the new key, all 13 entities, states, brightness commands. Tested the same day with a fresh Home Assistant 2026.11.3 (agent in the build VM with a dummy `wlan0`): discovery, adding without a key, key provisioning, encrypted reconnection, entities and the action.

## Optional: MQTT

If `/data/wallview/mqtt.conf` exists, the agent also publishes MQTT discovery (display light, illuminance, proximity, CPU temperature, restart, dashboard URL):

```
host=192.168.1.10
port=1883
username=wallview
password=secret
```

## Kiosk login

WPE WebKit has no on-screen keyboard, so the Home Assistant login page cannot be used on the panel. There are two ways to sign in the kiosk.

### Automatic: WallView integration (recommended)

`custom_components/wallview` is a small Home Assistant integration, installed once through HACS (this repository, category Integration). It needs no password and no typing on the panel:

1. The integration discovers each panel through the same mDNS record as ESPHome (`_esphomelib._tcp`, `project_name=WallView*`) and shows "WallView xxxx". "Submit" approves it.
2. As soon as the panel's ESPHome action `pair` exists (the panel is also added in ESPHome, in either order), the integration creates a one-time code (random, valid for 10 minutes, older codes of the panel are dropped) and calls `esphome.<device_name>_pair`, with the device name taken from the ESPHome config entry of the same MAC address. The code travels over the encrypted ESPHome connection.
3. The agent writes it to `/data/wallview/pairing` and restarts the kiosk. The kiosk script passes it once to the start page and deletes the file.
4. The start page sends the code and the client ID `<origin>/` to `<Home Assistant>/api/wallview/pair` (POST). The integration accepts it only from the panel's own IP address (host of the ESPHome config entry) and only once; a code that leaked to another device is useless. The client ID must be the Home Assistant origin the request was sent to (`http(s)://<host>/`), the one the frontend uses. It creates a refresh token for the user "WallView" (created on first use: no administrator rights, local network only). The token has Home Assistant's normal expiry: it stays valid while it is used and expires after 90 days without use; the kiosk refreshes it constantly, a panel that was off longer signs in again with "Sign in again".
5. The start page opens `<Home Assistant>/api/wallview/pair#tokens=…&next=<dashboard>`. That page comes from the Home Assistant origin and takes the tokens from the URL fragment (never sent to a server or logged). It stores them in the frontend's `hassTokens`, exactly like a normal login with "Keep me logged in", and opens the dashboard.
6. If the code is rejected (expired, Home Assistant restarted, other Home Assistant), the start page shows the touch keyboard sign-in with the reason.

Each panel has its own refresh token (named after the panel, visible on the WallView user's profile). Pairing again replaces it, and removing the panel from the integration revokes it; removing the last panel also removes the "WallView" user. A panel approved before it is added in ESPHome gets a repair issue "Add … in ESPHome", which disappears once the first code is delivered. Dashboards shown on the panel must be visible to the "WallView" user.

Signing in again: the button "Sign in again" on the panel's ESPHome device makes the agent fire the event `esphome.wallview_sign_in_requested` (ESPHome `HomeassistantActionRequest` with `is_event`, sent on every connection that subscribed to Home Assistant actions; Home Assistant forwards device events without the "allow actions" option and adds the device ID). The integration maps the device ID to the ESPHome device, its MAC address and the approved WallView entry and sends a new one-time code as above. Events for unknown devices are ignored. A device that fakes another panel's device ID can only make that panel receive a code it alone can redeem.

One device per panel: since Home Assistant 2026 a device belongs to exactly one config entry, and devices of different integrations with the same MAC address are no longer merged. The WallView integration therefore has no device and no entities of its own (`integration_type: service`); everything about the panel lives on its ESPHome device. Config entries from version 0.1 of the integration (with a "Sign in kiosk" button on a second device) are migrated: the button and the extra device are removed. This follows a review of how comparable projects do it: devices that speak the ESPHome API (for example linux-voice-assistant) live only in ESPHome; none of them sign a kiosk in, which is the one thing that needs code inside Home Assistant.

After the panel stores its encryption key, the agent closes every open API connection, including unencrypted ones opened before, as ESPHome firmware does.

Tested 2026-11-26 with Home Assistant 2026.11.3 in the build VM: migration from 0.1 (button and second device removed, only the ESPHome device left), "Sign in again" on the ESPHome device delivering a new code to `/data/wallview/pairing`, an event with an unknown device ID ignored. Earlier the same day with the agent and the start page in Chrome. Covered: discovery, approval before and after adding in ESPHome, key provisioning with a single encrypted connection left, code delivery, rejection from another IP address with fallback to the keyboard, success from the panel address, reuse rejected, token expiry, the token page signing the frontend in as "WallView", token replaced on re-pairing. An earlier version was reviewed by Fable 5.2; its findings are fixed in this design.

### Development and quality

The integration follows the Home Assistant core conventions so a later core submission stays possible: docstrings, strict typing (`mypy --strict`), Ruff with core's rule set (`pyproject.toml`), translated exceptions and repair issues, `quality_scale.yaml` (Bronze rules done or exempt with a reason), brand images in `custom_components/wallview/brand/`, no dependency on Home Assistant internals (the client ID check is our own because `indieauth.verify_client_id` changed its signature between 2026.10 and 2026.11). Tests use `pytest-homeassistant-custom-component` with 100 % coverage:

```
pip install -r requirements_test.txt
ruff format --check && ruff check && mypy && pytest --cov
```

`.github/workflows/integration.yml` runs these plus hassfest and the HACS validation on every change. Core acceptance itself is not likely yet: new integrations go to the Marketplace first and need an established user base, and no core integration creates users or issues tokens, which would need an architecture discussion. The core-friendly alternative is a sign-in on the panel side with Home Assistant's standard authorization flow (QR code), planned for later.

### Manual: touch keyboard

Without the integration, the start page signs in itself:

1. Once the dashboard URL answers and the panel is not signed in to that Home Assistant, it shows "Sign in to Home Assistant" with its own touch keyboard (letters, numbers, symbols).
2. It runs the Home Assistant `login_flow` (`/auth/providers`, `/auth/login_flow`, provider `homeassistant`, client ID `<origin>/`, two-factor code supported). Cog runs with `--allow-universal-access-from-file-urls` so the local page may call Home Assistant without CORS.
3. With the authorization code it opens the dashboard as `<dashboard>?auth_callback=1&code=…&state=base64({hassUrl, clientId})&storeToken=true`. The Home Assistant frontend exchanges the code for tokens and keeps them in its local storage under `/data/xdg`, exactly as after a normal login with "Keep me logged in".
4. The launcher remembers in its own local storage that it is signed in to this origin.

The "Sign in again" button (ESPHome device) increments `/data/wallview/sign-in` and restarts the kiosk; the launcher then shows the sign-in screen again (for example after the tokens were revoked or to change the user).
