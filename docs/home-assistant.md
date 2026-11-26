# Home Assistant integration

`wallview-agent` (Rust, `agent/`) connects the panel to Home Assistant through MQTT discovery. No Home Assistant configuration is needed besides a working MQTT broker (for example the Mosquitto add-on) with the MQTT integration.

## Configuration on the panel

`/data/wallview/mqtt.conf`:

```
host=192.168.1.10
port=1883
username=wallview
password=secret
```

The agent starts automatically (`/etc/init.d/S60wallview-agent`) when this file exists and restarts if it exits. Without the file it does not run.

## Entities

The device appears as `WallView XXXX` (last four hex digits of the Wi-Fi MAC address), model "Sonoff NSPanel Pro".

| Entity | Type | Source / action |
|---|---|---|
| Display | light (brightness) | `/sys/class/backlight/backlight`: `brightness`, `bl_power` |
| Illuminance | sensor, lx | light sensor (`stk3310`, raw × scale) |
| Proximity | sensor | proximity raw value |
| CPU temperature | sensor, °C, diagnostic | `thermal_zone0` |
| Restart | button | `reboot` |
| Dashboard URL | text, config | `/data/wallview/url`, the kiosk restarts with the new URL |

Topics: `wallview/<id>/…`, discovery under `homeassistant/<component>/wallview_<id>/<entity>/config`. Availability `wallview/<id>/availability` (`online`/`offline`, last will). Sensors are published every 15 s.

## Building

The Buildroot post-build script builds the agent with the VM's `cargo` (the VM is arm64 with glibc 2.39, the root file system has glibc 2.40) and installs it to `/usr/bin`. For quick tests:

```
limactl shell wallview -- bash -c 'cd ~/WallView/agent && CARGO_TARGET_DIR=$HOME/build/agent-target cargo build --release'
```

Tested 2026-11-26 against a temporary broker: discovery of all six entities, availability, brightness, on/off and sensor publishing.

## Kiosk login

The kiosk opens the URL in `/data/wallview/url`. Logging in needs either the Home Assistant `trusted_networks` auth provider for the panel's address (recommended for wall panels, no keyboard needed) or a one-time login. To be decided together with the dashboard setup.
