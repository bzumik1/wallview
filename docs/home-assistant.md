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
| Presence | binary sensor (occupancy) | proximity sensor rises above its baseline (hand or person in front of the panel), see [Presence](#presence) |
| Presence sensitivity | number, %, config | `/data/wallview/presence`: 1–100, default 50 |
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
| Radio mode | select, config | `/data/wallview/radio-mode`: Off (default) or Zigbee coordinator, see [Zigbee and Thread](#zigbee-and-thread) |
| Radio client connected | binary sensor (connectivity), diagnostic | a host (ZHA, Zigbee2MQTT) is connected to the radio |
| Bluetooth proxy | switch, config | `/data/wallview/bluetooth-proxy`: off (default) or on, see [Bluetooth proxy](#bluetooth-proxy) |

Presence changes are sent immediately, sensors every 15 s, commands answer with the new state.

## Presence

The agent samples the proximity channel of the `stk3310` every 100 ms. The raw value is compared with a baseline, not with a fixed number, because the reflection off the cover glass differs from panel to panel and drifts with temperature: the baseline follows a lower reading at once and a higher one slowly (about 10 s time constant) while nobody is near, and stays put while Presence is on.

- Presence turns on when the reading rises above the baseline by the rise that **Presence sensitivity** sets: 1000 counts at 1 %, 2 counts at 100 %, on a logarithmic scale (about 46 at the default 50 %; before, presence was a fixed raw value of 100 with a baseline of about 3).
- It turns off once the reading has been below half of that rise for 1 s, so a quick wave is still seen by Home Assistant (the API loop sends states every 500 ms) and the state does not flicker at the edge.
- To calibrate, watch the diagnostic **Proximity** sensor (raw value) with nothing in front and at the distance that should count; a slow drift is absorbed into the baseline, a person in front of the panel is not for the first 10 minutes; after 10 minutes of continuous presence the baseline starts to follow again, so an object left in front of the panel releases Presence. Longer delays (turn the screen off after a minute without presence) belong in a Home Assistant automation.

## Zigbee and Thread

The onboard EFR32MG21 (SM-044, UART5, `/dev/ttyS5`) runs one mode at a time, chosen with the **Radio mode** select on the panel's ESPHome device. The radio itself is always used by an integration in Home Assistant; the select only decides what the panel offers.

| Mode | Radio firmware | Panel | Home Assistant |
|---|---|---|---|
| Off | unchanged | UART closed | – |
| Zigbee coordinator | factory EZSP NCP (115200, software flow control) | raw TCP bridge on port 6638, mDNS `_zigbee-coordinator._tcp` | ZHA offers "WallView xxxx" automatically (`socket://<panel>:6638`, radio type EZSP); Zigbee2MQTT: `port: tcp://<panel>:6638`, `adapter: ember` (check the EZSP version first; ember rejects old firmware) |
| Zigbee router, Thread border router | planned: open-source router or OpenThread RCP image, flashed by the agent | planned: `otbr-agent` for Thread | planned |

The bridge serves one host at a time and refuses further connections while it is connected, like ser2net. A second host taking over could make the radio leave its Zigbee network (Zigbee2MQTT's ember adapter forms a new network when its configuration does not match), and a port scan would cut ZHA off. A host that disappears without closing the connection is detected by TCP keepalive within about 25 s. The UART runs without flow control; the radio's XON/XOFF bytes are passed to the host, which ignores them, so a lost XON cannot stall the bridge. The TCP port is not encrypted or authenticated, like ser2net or other network Zigbee coordinators; keep the panel on a trusted network. Switching the mode to Off closes the port and stops the advertisement; the network stays on the radio and ZHA reconnects when the mode is switched back.

- ZHA allows one coordinator per Home Assistant, so with several panels only one can be in Zigbee coordinator mode.
- ZHA stores the panel's IP address (`socket://<IP>:6638`), not its name, and does not follow a changed address. Give the panel a DHCP reservation. The ESPHome serial proxy (planned) removes this limitation.
- Over Wi-Fi, a stall of several seconds makes bellows reset the ASH connection; ZHA reconnects by itself.

Tested 2026-11-27 in the build VM against a pseudo terminal that answers like the panel's NCP (`WALLVIEW_RADIO_SERIAL` overrides the UART path): ASH reset answered through the bridge, a second host refused while the first is connected and accepted after it left, select and sensor states with aioesphomeapi, mDNS record with `radio_type=ezsp` and `serial_number`. Not yet tested on the panel with ZHA.

## Bluetooth proxy

The panel can be an [ESPHome Bluetooth proxy](https://esphome.io/components/bluetooth_proxy/): with the **Bluetooth proxy** switch on, Home Assistant receives the BLE advertisements the panel hears, as from an ESP32 proxy (BTHome, Xiaomi, Shelly BLU and Govee sensors, room presence with Bermuda or Private BLE Device, Improv for other devices). Active connections (SwitchBot, locks) are not supported yet.

Bluetooth is off unless something needs it: with the switch off and Wi-Fi working, the agent powers the adapter down and soft-blocks it with rfkill, so the RTL8723DS, which shares its antenna with Wi-Fi, neither transmits nor listens. Improv turns it on by itself when there is no Wi-Fi and off again after provisioning; without Wi-Fi Bluetooth always comes on for Improv, so the switch cannot lock the panel out.

How it works (`agent/src/bluetooth/`):
- One owner decides the adapter's power from the holds (Improv) and the switch, and scans only while the switch is on and Home Assistant is subscribed to advertisements.
- Scanning is passive: a BlueZ advertisement monitor (flags patterns as Home Assistant uses on Linux) makes the kernel scan passively with the controller's duplicate filter off. The RTL8723DS has no Microsoft extension, so the kernel does not filter by the patterns (bluetoothd does) and reports every advertisement on the management socket, which is what the agent forwards. Without the monitor API (BlueZ without experimental interfaces) the agent falls back to BlueZ discovery, logged as "scanning with discovery": active scanning, but the controller's duplicate filter stays on, so each device is reported only about once per discovery cycle, too rarely for presence.
- The owner checks the adapter every 5 s: with nothing needing Bluetooth it switches it off again if anything turned it on (bluetoothd powers an adapter on when it appears or is unblocked), and with the switch on it powers it on again if it lost power. Improv retries every 5 s while there is no Wi-Fi, keeping Bluetooth on. `/etc/bluetooth/main.conf` turns on the experimental interfaces and sets a scan window of 30 ms every 320 ms (ESPHome's default), to leave the shared antenna to Wi-Fi most of the time.
- The raw advertisement data (address, address type, RSSI, up to 62 bytes cut at an AD structure) comes from the kernel's management socket (`Device Found` and `Advertisement Monitor Device Found` events), because BlueZ's D-Bus API only offers parsed and merged device properties. It is sent to Home Assistant in batches of up to 16 every 100 ms (`BluetoothLERawAdvertisementsResponse`); a subscriber that falls more than 256 advertisements behind loses the newest ones.
- The device info reports the proxy features (passive scan, raw advertisements) and the adapter's address only while the switch is on; switching closes the API connection so Home Assistant reconnects and reads them again.

Tested 2026-11-27 on the development panel (kernel 6.18.63, BlueZ 5.79, agent from `/tmp`, `main.conf` bind-mounted):
- 50 power off/on cycles of the RTL8723DS (30 with rfkill, 20 with `bluetoothctl` only): one short system-wide stall (panfrost timeout, SDIO interrupt latency, then `hci0: command 0x0c45 tx timeout`) with a Wi-Fi re-association of about 5 s on cycle 10; the other 49 and a 2-minute idle control without errors or lost pings. Probably a rare stall that a power-on overlapped; the agent switches Bluetooth only on state changes.
- Switch off: Bluetooth off (rfkill soft 1, unpowered) within 9 s of the agent's start, stays off, Home Assistant stays connected.
- Switch on: "scanning passively with an advertisement monitor" (no discovery fallback; `Adv Monitor Manager created with supported features:0x00000000`, so bluetoothd filters), Home Assistant reconnects within 13 s and lists the panel as a Bluetooth scanner; about 26 Bluetooth UART interrupts per second; `bluetoothd`, `dbus-daemon` and the agent at 0 % CPU.
- Ping over Wi-Fi (60 packets, avg/max): 5.1/11.5 ms with the switch off, 6.7/33.4 ms with it on, no loss.
- Switch off again: Bluetooth off within 1 s.
- BlueZ 5.79 warns `Unknown key ScanIntervalAdvMonitor` and `ScanWindowAdvMonitor` (its list of known keys spells them `…AdvMonitoring`), but its parser reads exactly these names, so the values apply. The `Unable set contents for /var/lib/bluetooth/…` errors come from the read-only root, independent of the proxy.
- Not yet checked: the number of advertisements Home Assistant shows for the panel, and Improv without Wi-Fi (needs someone at the panel).

Way back on the panel: `echo 0 > /sys/class/rfkill/rfkillN/soft` (the Bluetooth one) and `rm /data/wallview/bluetooth-proxy`; nothing keeps the rfkill state across a reboot.

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
