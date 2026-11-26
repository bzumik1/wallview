# Home Assistant integration

`wallview-agent` (Rust, `agent/`) makes the panel a device in Home Assistant through the **ESPHome native API**, the same open protocol the Open Home Foundation uses for [linux-voice-assistant](https://github.com/OHF-Voice/linux-voice-assistant). No MQTT broker and no YAML are needed. MQTT discovery remains an optional add-on.

## Adding the panel

1. The panel announces itself over mDNS as `_esphomelib._tcp` (`wallview-xxxxxx`), advertising `api_encryption_supported` and `api_provisioning=zero-psk` while it has no encryption key.
2. Home Assistant shows it as a discovered ESPHome device. "Add" is the only step.
3. Home Assistant connects with the well-known all-zero Noise key, sees `api_encryption_provisionable`, generates a key and sends it (`NoiseEncryptionSetKeyRequest`). The panel stores it in `/data/wallview/api.key` and from then on only accepts that key and advertises `api_encryption`.

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
| Dashboard URL | text, config | `/data/wallview/url`, the kiosk restarts with the new URL |

Presence changes are sent immediately, sensors every 15 s, commands answer with the new state.

## Protocol details

- `agent/proto/api.proto` and `api_options.proto` come unchanged from aioesphomeapi (MIT); Rust code is generated at build time with `prost` and `protox` (no `protoc` needed).
- Noise `Noise_NNpsk0_25519_ChaChaPoly_SHA256`, panel is the responder, prologue `NoiseAPIInit\0\0`; server hello carries protocol `0x01`, node name and MAC; frames `0x01, length (u16 BE), ciphertext`, plaintext `type (u16 BE), length (u16 BE), protobuf`.
- API version 1.10, TCP port 6053.

Tested 2026-11-26 against the panel with aioesphomeapi 46.31 (the library Home Assistant uses): zero-key connection, key provisioning, zero key rejected afterwards, reconnection with the new key, all 13 entities, states, brightness commands.

## Optional: MQTT

If `/data/wallview/mqtt.conf` exists, the agent also publishes MQTT discovery (display light, illuminance, proximity, CPU temperature, restart, dashboard URL):

```
host=192.168.1.10
port=1883
username=wallview
password=secret
```

## Kiosk login

Improv (BLE/USB) only transfers Wi-Fi credentials. The dashboard login will be either a one-time login on the panel's own setup screen (touch keyboard, Home Assistant `login_flow` API, code handed to the frontend) or, with no action on the panel, a small WallView integration in Home Assistant that creates access for the kiosk and sends it over the ESPHome API.
