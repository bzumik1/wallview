# WallView – Project Plan

English transcription of [WallView-projektovy-plan.pdf](WallView-projektovy-plan.pdf) (original in Czech, Nov 9, 2026, Andrea Znamenáčková).

Open mainline Linux for the Sonoff NSPanel Pro wall panel as a Home Assistant kiosk.

## Project goal

Replace the original Android 8.1 on the Sonoff NSPanel Pro with a clean mainline Linux that turns the panel into a fast Home Assistant kiosk, lasts in the wall for decades, and can be installed by anyone without a terminal.

### Deliverables

1. **Upstream:** board support in mainline Linux, U-Boot and Buildroot, so the support does not go stale and other distributions can use it.
2. **WallView system image:** Buildroot, read-only system, kiosk with the HA dashboard, signed A/B OTA updates.
3. **Installer:** HA add-on and web installer, mandatory backup of the original, one-click restore.

### Principles

- Mainline-first: no vendor 4.4 kernels, no binary GPU blobs. The only binary piece is the Rockchip DDR init.
- The panel is a thin client: logic lives in HA, the panel displays and reports sensors.
- Secure defaults, signed updates, unbreakable installation (MaskROM rescue always possible).
- Speed and longevity before features: lightweight system, minimal eMMC writes.
- Upstream is a goal, not a requirement: the project must work with its own patch set on top of a mainline LTS.

**Out of scope for phase 1:** Android (see Future extensions), relays (Gen2 only), Matter entities exposed directly from the panel.

## NSPanel Pro hardware (Gen1)

All components needed for the kiosk have a mainline driver except touch, which we will write. Data comes from the vendor device tree `px30-evb-ddr3-v10.dts` in the [itead/NSPro_Kernel](https://github.com/itead/NSPro_Kernel) repository (kernel 4.4.138, modified by the manufacturer Smatek). The panel is a rebranded Smatek T6E.

| Component | Chip / wiring | Mainline status |
|---|---|---|
| SoC | Rockchip PX30, 4× Cortex-A35 1.3–1.5 GHz | ✅ |
| GPU | Mali-G31 MP2 | ✅ Panfrost + Mesa |
| RAM / storage | 2 GB DDR3 / 8 GB Samsung eMMC | ✅ |
| PMIC + audio codec | RK809 on I2C0 (0x20), PDM microphone, 1 W speaker | ✅ rk808 + rk817 codec |
| Display | Sitronix ST7701S, 480×480, MIPI DSI 2 lanes, RGB888, 16 MHz → 60.1 Hz | ⚠️ init sequence into `panel-sitronix-st7701` |
| Backlight | PWM1, period 25 µs (40 kHz) | ✅ pwm-backlight |
| Touch | Hynitron CST226SE (0x5A) or CST226 (0x1A) on I2C1, IRQ GPIO0_B5, reset GPIO0_B4 | ❌ write (extend `hynitron_cstxxx`) |
| Light + proximity | Sensortek STK3x3x on I2C2 (0x57), IRQ GPIO3_C0; alternative APDS9933 | ⚠️ verify ID in `stk3310`, otherwise userspace |
| Unknown device | `smatek,tp` on I2C2 (0x3C), driver missing | ❓ find out its function |
| Wi-Fi | Realtek RTL8723DS, SDIO, 2.4 GHz only; alternative AP6212 in some revisions | ✅ `rtw88_8723ds` (since 6.5) / `brcmfmac` |
| Bluetooth | RTL8723DS on UART1 | ✅ hci_uart + btrtl |
| Zigbee/Thread radio | Silabs EFR32MG21 on UART4 or UART5 (RTS/CTS) | ✅ UART only |
| Video decoder | Hantro VPU, H.264 up to 1080p | ✅ `hantro` V4L2 |
| USB | Internal OTG port (inside after disassembly), host port active in DTS | ✅ |
| Recovery | MaskROM in SoC, Loader via `adb reboot loader` or pin UBOOT→GND | ✅ |

**Gen2 (since April 2026)** has RK3326-S, 32 GB eMMC, dual-band Wi-Fi with a different chip, EFR32MG24 and two relays. Most of the work carries over, but Wi-Fi and the relay wiring must be verified separately.

**Open:** USB connector type, which UART the MG21 is on, MG21 reset/boot pins, board revision of our unit.

## System architecture

The panel talks to HA in two ways: the browser loads the dashboard, WallView services report entities, Thread and voice. Project-specific code exists in only two layers; the rest are upstream components.

Panel (WallView OS), top to bottom:

| Layer | Contents |
|---|---|
| Kiosk and UI | Cog (WPE WebKit): dashboard, offline screen, screensaver |
| **WallView services** | `wallview-agent` (Rust): ESPHome API, Improv, OTA, otbr, voice |
| Graphics and media | Mesa Panfrost, Cog directly on DRM/KMS, GStreamer |
| System (Buildroot LTS) | read-only rootfs + data partition, iwd, Avahi, NTP |
| **Mainline kernel (LTS 6.18)** | board DTS, ST7701, CST226, rtw88, hantro, Panfrost |
| Bootloader | mainline U-Boot, TF-A, Rockchip DDR blob, boot logo |
| Hardware | PX30, Mali-G31, RTL8723DS, MG21, STK3x3x, RK809 |

Home Assistant server: Dashboard (HTTP/WebSocket, Lovelace theme, camera via go2rtc), ESPHome integration (API :6053, entities, OTA), OpenThread BR (REST, Thread, Matter), Assist (audio, STT, TTS, intents).

## Phase 0 – Preparation

Goal: have a second panel on the desk, a complete backup and an exact list of the chips in our unit before anything is overwritten.

### Equipment

- [ ] Second NSPanel Pro for development (second-hand), the daily panel stays in the wall
- [ ] USB cable to the internal OTG port (verify connector type after disassembly)
- [ ] USB-UART adapter 3.3 V (with RTS/CTS exposed to control reset and MaskROM)
- [ ] Linux computer or Raspberry Pi as test station
- [ ] Optional: USB hub with port power switching (uhubctl), webcam pointed at the display

### Survey of the running Android (ADB + root via seaky root tool)

- [ ] `cp /sys/firmware/fdt` → live device tree, compare with vendor DTS from GitHub
- [ ] `cat /sys/class/net/wlan0/device/uevent` → confirm RTL8723DS (`SDIO_ID=024C:D723`) or AP6212
- [ ] List of I2C devices (`/sys/bus/i2c/devices/*/name`), `lsmod`, `dmesg`
- [ ] Find out what `smatek,tp` at 0x3C is (helper MCU for touch or power, or a leftover)
- [ ] Verify the touch protocol: read 28 bytes with `i2ctransfer` on touch and compare with the ESPHome decoding
- [ ] Find the UART of the Zigbee MG21 and the GPIOs for its reset and bootloader
- [ ] Back up Wi-Fi and BT firmware files (`/vendor/etc/firmware`, `/system/etc/firmware`)

### eMMC backup (mandatory, before the first write)

```
adb reboot loader
rkdeveloptool ld
rkdeveloptool rfi            # eMMC size
rkdeveloptool rl 0 <sectors> nspanel-emmc-full.img
```

Store the backup twice in different places. The original firmware must not be distributed; everyone backs up their own unit.

### Organisational

- [ ] Politely ask ITEAD for the missing GPL sources (CST226 touch driver, STK3x3x sensor, `smatek,tp`)
- [ ] Check availability of the name WallView (GitHub, domains, EUIPO, USPTO)

### State of the first unit (Nov 11, 2026)

- [ ] The panel arrived with app V4.9-495-495-1180 and a downloaded update 4.10.0. **Do not install** until we verify it does not close the path to ADB and root (public release notes end at 4.6.0).
- [ ] Enable "Block Auto Updates" in About device
- [ ] Determine the generation from the rear label: model with "-Relay" and terminals L Out1/L Out2 = Gen2, only L and N = Gen1
- [ ] Power from USB during development, not from 230 V via loose wires

### Baseline measurements on the original (the same dashboard is measured again on WallView)

| Measurement | Original | WallView |
|---|---|---|
| Boot to dashboard (s) | | |
| Display wake to usable state (s) | | |
| Dashboard load (s) | | |
| Open entity dialog (s) | | |
| Free RAM (MB) | | |
| Scroll smoothness (FPS) | | |
| System WebView version | | WPE |

Estimated improvement (to be verified): boot 2–4×, instant wake, much more free RAM, smoother scrolling, heavy dashboard actions tens of percent faster. If the WebView is outdated, HA serves it the slower ES5 frontend build and the difference will be larger.

## Test station and development loop

Development happens on the desk with the panel powered from USB, without 230 V. The station allows Claude to build, flash and read logs in a loop on its own.

### Wiring

- Panel disassembled, powered via the internal USB OTG (safe, no mains voltage)
- USB OTG to the test computer: Loader/MaskROM flashing, later USB gadget (network + serial console over one cable)
- USB-UART on the U-Boot and kernel console; its RTS/CTS drive the RST and UBOOT pins (reset and MaskROM entry hands-free, procedure per The Roboverse)
- Webcam on the display: Claude sees what the panel shows
- Test computer connected to Claude via the desktop app (shell on the computer)

### Loop rules

- The bootloader is not changed inside the loop. Flash a verified U-Boot once, then test only kernels, ideally loaded into RAM without writing to eMMC.
- eMMC backup done beforehand.
- Writing the bootloader and erasing partitions only after approval.
- Subjective evaluation (touch responsiveness, smoothness) is done by a human.

### Build infrastructure

- Kernel and device tree: locally or in CI (clang/LLVM cross compilation)
- Buildroot image with WPE WebKit: GitHub Actions (takes hours on a small machine)
- QEMU (aarch64 virt + virtio-gpu): test kiosk, network and scripts without hardware; PX30 drivers cannot be tested this way
- Static checks: `checkpatch`, `dtbs_check`, kernel coding style

## Phase 1 – Bootloader and mainline kernel

Goal: current LTS kernel (6.18) with full panel support and a shell over UART and USB. Write code directly in upstreamable form.

### Bootloader

- [ ] Mainline U-Boot + TF-A (both support PX30) + DDR init blob from rkbin
- [ ] Boot via EFI or extlinux (standard path for distributions)
- [ ] Rockusb or fastboot over OTG for reflashing without pins
- [ ] WallView boot logo from U-Boot immediately after power-on (mandatory)
- [ ] Keep GPIO state from the first moments (important for Gen2 with relays)

### Device tree `px30-<vendor>-nspanel-pro.dts` (rewrite of the vendor DTS)

- [ ] RK809 regulators (vdd_arm, vdd_logic, vcc3v3_lcd…), cpufreq/OPP, io-domains
- [ ] eMMC, SDIO with mmc-pwrseq (WIFI_REG_ON GPIO0_A2, host wake GPIO0_A5)
- [ ] DSI + VOP, panel with reset GPIO3_A0, pwm-backlight on PWM1
- [ ] I2C1 touch, I2C2 STK3x3x sensor, I2C0 PMIC
- [ ] UART1 Bluetooth (`realtek,rtl8723ds-bt`), UART for MG21 with RTS/CTS
- [ ] Audio: RK809 codec + PDM microphone, simple-audio-card
- [ ] Hantro VPU, GPU, TSADC (temperature), watchdog, USB OTG/host

### Drivers and patches

1. **Panel:** new entry in `panel-sitronix-st7701` with init sequence and timing from the vendor DTS (dozens of lines). Compatible per board, panel manufacturer unknown.
2. **CST226 touch:** extend `hynitron_cstxxx` with a CST226 variant + YAML binding. Protocol: init write 0xD1, ID from 0xD204, resolution from 0xD1F8, read 28 bytes after IRQ, up to 5 touches, 12-bit X/Y. References: ESPHome `cst226`, SensorLib, Linux `hyn_cst2xx` from the Google Nest kernel. Estimate: days, upstream form within a week.
3. **Sensor:** add the chip ID to `stk3310` (IIO) if it does not match.
4. **Wi-Fi/BT:** configuration only, firmware from `linux-firmware`.

### Milestones

- M1: U-Boot + kernel → shell on UART
- M2: Wi-Fi and SSH (from now on work without cables)
- M3: picture – display, Panfrost, test image
- M4: touch works

Plan B: if anything does not get upstream, we maintain mainline LTS + our own patch set (like Armbian or LibreELEC).

## Phase 2 – System and kiosk

Goal: the panel boots into the HA dashboard in roughly 10–15 s (estimate, to be measured) and runs smoothly. After this phase the kiosk is functional (M5).

### System base

- [ ] Buildroot LTS, board support in `BR2_EXTERNAL` (own repository)
- [ ] BusyBox init instead of systemd, fast boot
- [ ] Read-only root partition + small writable data partition (Wi-Fi, HA login, settings)
- [ ] Logs in RAM, zram instead of swap, no swap on eMMC
- [ ] CPU governor `schedutil`, PX30 hardware watchdog

### Graphics and browser

- [ ] Mesa with Panfrost (no libmali)
- [ ] Cog directly on DRM/KMS without a Wayland compositor (Cage only as fallback)
- [ ] Cog (WPE WebKit), data directory on the writable partition (localStorage with login)
- [ ] Cog never closes: when idle only the backlight turns off, after wake the dashboard is there instantly
- [ ] With the display off the page receives a visibility-hidden signal and stops rendering (CPU, heat)
- [ ] Measure FPS via `WEBKIT_SHOW_FPS=1`

### Network

- [ ] `rtw88_8723ds` + iwd (built-in DHCP, WPA2/WPA3, automatic reconnect)
- [ ] Wi-Fi power saving off for responsiveness
- [ ] NTP: the panel has no battery-backed RTC
- [ ] Avahi/mDNS: discover HA and announce `wallview-xxxx.local`
- [ ] IPv6 properly configured (needed for Thread and Matter)
- [ ] Local HA address, not cloud

### Performance – realistic expectations

The panel's CPU is roughly at Raspberry Pi 3 level, but with 2 GB RAM and a GLES 3.2 GPU. Boot, smoothness, responsiveness and wake will be much better than the original Android. Computationally heavy dashboard actions (opening a dialog, switching views) stay limited by the slow A35 core. 60 fps is achievable for scrolling and GPU CSS animations.

### Stack for maximum speed (decided)

| Layer | Choice | Why |
|---|---|---|
| Bootloader | mainline U-Boot without delay | logo right after power-on |
| Kernel | mainline LTS, boot drivers built in, LZ4, no debug options | fast boot without initramfs |
| Root filesystem | EROFS with LZ4, read-only | small, fast reads, no writes |
| Data | ext4 or f2fs | Wi-Fi, login, settings |
| Init | BusyBox init, parallel service start | no systemd overhead |
| Display | Cog directly on DRM/KMS | one layer and process less |
| Browser | current WPE WebKit, GPU compositing | lightest full-featured browser |
| Video | GStreamer, only needed plugins + v4l2codecs | HW decoding via Hantro |
| Network | iwd with built-in DHCP | fast connection |
| Services | single program `wallview-agent` in Rust | single-digit MB RAM, start in ms |
| Memory | zram, no swap | fast, spares eMMC |

Python only for the voice assistant, and only if the user enables it. There will be no systemd, X11, desktop, Chromium, or Zigbee/ser2net. Target: logo instantly, dashboard within 10 s (to be measured).

### Why WPE WebKit and not Chromium

WPE is the official WebKit (Safari engine) port for embedded devices, developed by Igalia; Cog is a thin shell that opens one page full screen. The HA frontend targets the latest versions of modern browsers including Safari, and the HA iPhone app renders the dashboard through WebKit. In practice: what works in Safari will run on the panel.

| | WPE WebKit (Cog) | Chromium |
|---|---|---|
| HA compatibility | very good | best |
| WebRTC | in development | works |
| Memory and size | much smaller | hundreds of MB more |
| Startup | fast | slower |
| Display | directly on DRM | needs a compositor |
| Buildroot | ready package | package missing |

Considered and rejected: WebKitGTK (heavier sibling of WPE), Qt WebEngine (Chromium inside), Firefox (not built for kiosks), Servo and Ladybird (still in development).

- [ ] Bundle fonts (Inter, icons, emoji) and the required GStreamer plugins
- [ ] Automated test: with every HA release load the dashboard in WPE and verify rendering
- [ ] Comparative WPE vs Chromium measurement on the panel with the same dashboard
- [ ] Browser replaceable without touching the rest of the system

## Phase 3 – Home Assistant integration

Goal: HA discovers and adds the panel as an ESPHome device by itself; no MQTT broker, no YAML. The panel implements the ESPHome native API (protobuf over TCP 6053, Noise encryption, mDNS announcement). [linux-voice-assistant](https://github.com/OHF-Voice/linux-voice-assistant) from the Open Home Foundation proves this works on Linux.

### Panel entities in HA

| Entity | Type | Note |
|---|---|---|
| Ambient light | sensor (lx) | STK3x3x |
| Proximity | binary sensor | IR, range from a few to tens of cm, detects a hand, not a person in the room |
| Backlight | light with brightness | |
| Display | switch | |
| Screensaver | switch + mode select (photos, clock, black) + delay | see phase 4 |
| Displayed dashboard | select / text | central management of multiple panels |
| Reload dashboard | button | page reload, instant |
| Restart kiosk | button | restart Cog, frees memory, a few seconds |
| Restart panel | button | reboot, within 15 s |
| Browser memory, free RAM, uptime | diagnostic sensors | for restart automations |
| CPU temperature, Wi-Fi signal, IP, version | diagnostic sensors | |
| eMMC lifetime | diagnostic sensor | from eMMC extCSD |
| System update | update entity | OTA, see phase 6 |
| Speaker | media player | doorbell, announcements |

**Restart rule:** normal logic lives in HA (automations like "restart the kiosk at 4:00 when nobody is at the panel" or "when memory exceeds 600 MB"). Only a safety net stays on the panel: local watchdog for a frozen browser or exhausted memory, because the problem may happen exactly when HA cannot talk to the panel.

**Central management (required):** every panel is a device in HA; dashboard, brightness and night mode configurable centrally.

**Alternative:** MQTT discovery as an optional add-on for those who have a broker. The main path is the ESPHome API.

**wallview-agent (Rust)** – one small program instead of several scripts:

- ESPHome API for HA: entities, buttons, update entity
- light and proximity sensor, backlight, display wake
- HA watchdog and switching to the offline screen
- Improv over USB and BLE
- safety net for the browser, watchdog
- CPU performance by display state: on = higher performance for responsiveness, off = power saving
- native two-way camera audio (see phase 5)

## Phase 4 – User experience

Goal: setup and daily use without a terminal and without editing HA configuration.

### Wi-Fi setup – three paths

1. **Setup screen on the panel** (main): local page in Cog, list of networks, password via touch keyboard, selection of HA discovered via mDNS, one-time HA login on screen.
2. **Improv over USB** during installation: the installer sends Wi-Fi with the same protocol, the panel connects right after going back into the wall.
3. **Improv over Bluetooth LE:** HA (Improv via BLE integration) finds the panel and Wi-Fi is entered from the app. Authorisation by tapping "Allow setup?" on the display. BlueZ runs only in setup mode.

The panel supports 2.4 GHz only; the setup screen and the installer must say so clearly. Changing the network later: hidden gesture (long press in a corner) or factory reset by power-cycling several times.

### Offline screen with clock

- Shown when HA is unreachable (delay of tens of seconds against flicker), without Wi-Fi and during boot
- Large clock, date, unobtrusive connection status; tap for details (IP, signal, retry, Wi-Fi setup)
- Last known values from HA, dimmed and with update time
- Instant return to the dashboard as soon as HA is back
- Never show an uncertain time: after a power outage without network only the connection status, the clock only after NTP sync
- Brightness locally from the light sensor, a hand approaching wakes the display

### Screensaver and night mode (controlled from HA)

- Photos and information via a screensaver in the dashboard (e.g. HACS WallPanel: HA media, Immich)
- Send photos downscaled to 480–720 px (decoding 12 Mpx photos is slow on A35)
- Modes: photos, clock, black; dim at night based on the light sensor
- On doorbell ring cancel the screensaver and show the camera; a hand approaching returns the dashboard

### Design

- Dark theme with tiles per the design, adapted to a 480×480 square
- Controls at least 60–65 px (≈ 9–10 mm, the display is 72 mm wide at 6.7 px/mm); fewer controls per screen, blinds with large buttons
- Status numbers larger and brighter for readability from a metre away
- Glass effect without `backdrop-filter`: static pre-blurred background + translucent cards
- Own WallView logo on the offline screen, not the Home Assistant logo
- Main dashboard in HA with the WallView theme and template (Mushroom, Bubble Card in moderation), offline screen and screensaver are own code
- Navigation between views by swiping
- [ ] Redraw the design for 480×480 (home, room, door intercom, offline screen)

### UI in Home Assistant (decided)

The main UI lives in HA: the user edits it where everything else is, and it updates with HA. Only what must run without HA stays on the panel.

| In Home Assistant | On the panel (WallView OS) |
|---|---|
| Dashboard: home, lights, blinds, door intercom, media | Boot logo and animation |
| Screensaver with photos | Offline screen with clock |
| What each panel shows and where | Wi-Fi setup and first boot |
| | Update screen |

- [ ] WallView theme: colours from the design kit (Primary #0A84FF, Surface #0F172A, Elevated #1E293B…) mapped to HA theme variables; Inter font (OFL) bundled locally
- [ ] Own lightweight WallView cards in Lit: status tile, circular dimmer, blinds with large buttons, door intercom, player, swipe paging
- [ ] Dashboard strategy: screens generated automatically from HA areas, editable in the editor
- [ ] Distribution via HACS (theme + cards + strategy), the installer installs it together with the panel
- [ ] Same design tokens for the offline screen on the panel so the transition is invisible
- [ ] Effects: glass as a static background, glow sparingly, no live blur
- **The Home Assistant logo is not used in the UI or the firmware.** Instead the WallView logo or a home icon.

### Boot logo and animation

The WallView OS logo (blue "W" made of overlapping bands) is approved.

| Time | Stage | Screen |
|---|---|---|
| 0 to ~2 s | U-Boot and kernel | static logo, 24-bit BMP 480×480 |
| ~2 s to loaded | Cog with local page | animated logo (SVG + CSS on GPU) |
| then | dashboard | smooth crossfade |

- [ ] Animation starts exactly from the static frame (same position and size)
- [ ] Animate only movement and opacity: light sweep, band assembly, glow breathing
- [ ] Turn on the backlight only after panel initialisation; Cog with a black background (otherwise white flashes)
- [ ] Same style for update (progress, "Updating…"), restart and restore
- [ ] Logo as SVG, simplified variant for small sizes, monochrome versions, banner for README and installer
- [ ] If it cannot be shown from U-Boot (missing ST7701 panel driver there), fallback: logo from the kernel or the first program

## Phase 5 – Camera and door intercom

Goal: camera preview with less than half a second of latency, automatically on doorbell ring.

### Rules

- Only the substream to the panel: H.264, 640×480 or 640×360, 10–15 fps. Never the main 4K/H.265 stream (Hantro cannot do H.265, A35 cannot do it in software).
- Camera in a separate view or popup, not permanently on the main screen (CPU and heat in the box).
- Software decoding of the substream is enough; HW decoding via `hantro` + GStreamer `v4l2codecs` is a bonus.

### Transport via go2rtc in HA

| Method | Latency | State on WPE |
|---|---|---|
| MJPEG | < 0.5 s | always works, start with this |
| HLS (MSE) | 1–3 s | works, automatic fallback of the HA card |
| WebRTC | < 0.5 s | uncertain, verify in the current WPE version |

### Components

- [ ] GStreamer with `gst-libav` (SW H.264) and `v4l2codecs` (HW), kernel with `hantro`
- [ ] Substream in go2rtc, Advanced Camera Card (manual source selection)
- [ ] browser_mod or panel entities: on doorbell ring turn on the display and open the camera popup, after tens of seconds go back and turn off
- [ ] Gate unlock button in the popup
- [ ] Two-way audio – see below

### Two-way audio (Reolink and others)

HA 2026.11 added two-way camera audio as a Labs preview. It works only via WebRTC (not HLS) and only with HA opened over HTTPS; Reolink offers it when the camera supports two-way audio and HA streams from it via RTSP.

1. **Path A – via the dashboard:** enable WebRTC in WPE at build time (GStreamer backend, still in development), HTTPS to HA, echo cancellation in PipeWire. The microphone button in the HA camera card then works like on mobile.
2. **Path B – natively via wallview-agent (fallback):** a dashboard button sends a command, the agent opens a call with the camera via go2rtc using GStreamer `webrtcbin`. Needs neither WebRTC in WPE nor HTTPS, echo fully under our control. The picture stays in the dashboard (MJPEG).

- [ ] At the start of phase 5 try path A, deploy B if unreliable
- [ ] Echo cancellation mandatory (microphone and speaker are close; HA already has an echo report for the Reolink doorbell)
- [ ] Test with the user's specific camera or doorbell model (find out the model)

## Phase 6 – OTA, security and long-term operation

Goal: the panel in the wall updates itself for years, safely and without removal. The software must outlive the hardware.

### Updates

- [ ] A/B slots with automatic fallback (RAUC or SWUpdate in Buildroot)
- [ ] Images signed with the project key, the panel accepts no other
- [ ] Update entity in HA: "A new version is available, update?" like ESPHome
- [ ] Images downloadable from GitHub Releases; local update works without the project server
- [ ] MG21 firmware updates the same way
- [ ] Buildroot LTS (yearly release with three years of support), migration = new build + OTA
- [ ] Kernel: LTS security fixes continuously; move to the next LTS every few years; for a long horizon CIP kernels (~10 years) are an option
- [ ] Change U-Boot only exceptionally

### Security

- [ ] No default passwords, SSH disabled or key-only
- [ ] ESPHome API encryption key generated per panel
- [ ] Management of project signing keys (who holds them, backup, rotation)

### Operation

- [ ] Spare the eMMC: read-only system, logs in RAM, minimal writes; eMMC lifetime as a sensor in HA
- [ ] Browser memory leak: restarts controlled from HA + local safety net
- [ ] Heat in the box: stop rendering with the display off, CPU temperature to HA
- [ ] Hardware watchdog

### Real limits over 10+ years

- Growing demands of the HA frontend on A35 → safeguard: native lightweight UI (LVGL) via the HA WebSocket API
- Mains power supply with electrolytic capacitors (typically 10–15 years)
- The panel supports 2.4 GHz Wi-Fi only

## Phase 7 – Thread border router

Decision: MG21 runs only as a Thread RCP, the panel becomes a Thread border router. Zigbee stays a possible community extension (same flashing tools). ser2net is dropped.

**Why:** the panel is mains powered and always on; a panel in every room = a border router in every room. More border routers increase reliability, HA shares the Thread network credentials between them.

### Tasks

- [ ] Find the GPIOs for MG21 reset and bootloader (the original app updates its firmware, so a path exists)
- [ ] Build OpenThread RCP firmware for MG21 with the board's UART pins (same chip as HA Connect ZBT-1, the community builds for various MG21 devices)
- [ ] Flash from the panel via `universal-silabs-flasher`
- [ ] `otbr-agent` on the panel, UART with hardware flow control
- [ ] Connect to the HA OpenThread Border Router integration, ideally offered automatically
- [ ] IPv6 in the system (link-local addresses are enough)

### Caution

- Do not use multiprotocol (Zigbee + Thread on one chip), Nabu Casa abandoned it in 2024
- Interference with the panel's Wi-Fi (both 2.4 GHz on one board): Thread channel as far from the Wi-Fi channel as possible (e.g. Wi-Fi 1, Thread 25); warn in the settings

Estimate: a few weeks, mainly verifying pins and building firmware.

## Phase 8 – Voice assistant

Goal: the panel as an HA voice satellite in every room where it hangs. Assembling ready-made parts; audio tuning takes most of the time.

**How it works:** the wake word is detected locally on the panel (microWakeWord or openWakeWord), then audio is streamed to HA. Speech recognition, intent handling and the voice response are handled by HA locally (Whisper / Speech-to-Phrase, Piper) or by Nabu Casa cloud.

### Tasks

- [ ] Audio from the kernel: RK809 codec + PDM microphone (mainline)
- [ ] PipeWire or PulseAudio with echo and noise cancellation
- [ ] [linux-voice-assistant](https://github.com/OHF-Voice/linux-voice-assistant) (ESPHome protocol, Python 3.11+, aarch64, 512 MB RAM)
- [ ] Visual feedback: the display turns on and shows it is listening; answers also visually, timers on the display

**Expectations:** one microphone in the wall without hardware audio processing. Reliable in a quiet room up to a few metres, much worse across a room with the TV on. The 1 W speaker is for answers and the doorbell, not for music.

## Phase 9 – Installer for everyone

Goal: someone who has never opened a terminal can do it. The installer makes sense only after a stable system and a few weeks of operation in the wall.

### User flow

1. Turn off the breaker, take the panel off, open the rear cover
2. Connect USB to the HA server (or a computer) and click Install
3. Close, put back into the wall, turn on the breaker
4. Enter Wi-Fi on the panel (or the installer already sent it), the panel appears in HA
5. Updates arrive on their own via the update entity

### Installer forms

- **HA add-on** (main): panel connected via USB directly to the HA server, no drivers, no browser
- **Web installer** via WebUSB (Chrome/Edge), like the ESPHome web installer; explain the one-time Rockchip USB driver on Windows well
- Alternatively a small desktop app on top of `rkdeveloptool`

### What the installer does on its own

- [ ] Detect the board revision (Wi-Fi chip, type 86/120), refuse an unknown variant
- [ ] Mandatory backup of the original to the user's disk (8 GB, a few minutes)
- [ ] Flash the system via Loader mode
- [ ] Hand over Wi-Fi via Improv over USB
- [ ] One-click restore of the original from the backup

### Documentation and warnings

- [ ] Disassembly guide with photos or video
- [ ] Safety: disassemble only disconnected from mains, install on USB power
- [ ] Loss of warranty; the original firmware is not distributed
- [ ] Emphasise that the panel can always be rescued thanks to MaskROM

**Installation without disassembly** (bonus for advanced users): write the system from the running Android via ADB over the network with root. Fragile, depends on the original firmware version; explore, but do not rely on it.

## Upstream

Goal: everything for the board in mainline, so the community carries the support and anyone can take over the project. Upstream is a goal, not a requirement.

| Part | Project | Route | Note |
|---|---|---|---|
| Device tree + vendor prefix + board binding | Linux | linux-rockchip (Heiko Stübner) | easiest, start |
| ST7701 panel | Linux DRM | drm-misc | compatible per board |
| CST226 touch | Linux input | linux-input | extend `hynitron_cstxxx` + YAML |
| STK sensor | Linux IIO | linux-iio | a few lines if only the ID |
| Board defconfig | U-Boot | Rockchip maintainers | takes the DTS from Linux |
| Board defconfig | Buildroot | mailing list | once everything is in mainline |
| Kiosk, installer, theme | own GitHub project | | for end users |

**Order:** get it running locally → a few weeks of operation in the wall → publish the GitHub project and an image for testers → device tree and panel into Linux → touch and sensor → Buildroot defconfig.

### Process

- Patches by e-mail (`git send-email`), `checkpatch` and `dtbs_check`, several review rounds, typically months until release
- The submitter signs off (`Signed-off-by`): it must be a person from the project who understands the code and answers reviewers
- Before submitting read each project's current rules on AI use and disclose assistance accordingly
- After acceptance the submitter becomes the contact for the board

**Plan B:** mainline LTS + own patch set in `BR2_EXTERNAL`. OTA ships the whole image including the kernel, so updates work the same. Whatever gets upstream disappears from the patch set.

## Name, trademarks, licence and community

**Name: WallView** (working title). Short, descriptive, free of third-party marks, suitable for other panels too. It is generic, hence the established form **WallView OS** / `wallview-os`. The remote similarity to Apple HomeView (wall controller, 2026) is cosmetic.

- [ ] Check GitHub organisation, domains (wallview.dev, .io), EUIPO and USPTO

### Trademarks

- "Sonoff" and "NSPanel" (ITEAD) only in descriptions ("for NSPanel Pro"), not in the name
- "Home Assistant" and the HA logo (Open Home Foundation): not in the name or firmware, the description "for Home Assistant" is fine; optionally ask the foundation or consider the Works with Home Assistant programme
- Do not distribute the original ITEAD firmware

### Licence

- Kernel patches and drivers: GPL-2.0 (device tree GPL-2.0+ OR MIT like the other Rockchip DTS)
- Own userspace (daemons, setup page, installer): permissive licence (MIT or Apache-2.0), to be decided

### Community

- Reach out to seaky (Pro Tools), blakadder, communities of Smatek T6E clones (Zemismart, Avatto, Tuya) for testers and board revisions
- Look at the Ha-paneld project (Android app for wall panels) for the dashboard
- Possibly Armbian and postmarketOS once the board is in mainline
- Thread on the HA community forum when the first images are published

## Risks and open questions

No risk is blocking; the biggest unknowns are the `smatek,tp` device and board revisions.

| Risk | Impact | Mitigation |
|---|---|---|
| `smatek,tp` (0x3C) is required for touch or power | touch delayed | investigate from Android in phase 0, ask ITEAD for the source |
| Revision with AP6212 instead of RTL8723DS | different Wi-Fi driver | `brcmfmac` in mainline; the installer detects the revision |
| Sensor not STK3310-compatible | small extra work | add the ID or read in userspace |
| WebRTC in WPE uncertain | two-way intercom audio, low latency | MJPEG for preview, two-way audio later |
| Performance of heavy dashboards on A35 | slow dialogs | light dashboard, no `backdrop-filter` and heavy cards; LVGL as fallback |
| Thread and Wi-Fi interference | unreliable Thread | separated channels, warning in settings |
| Microphone quality in the wall | weaker recognition | software echo cancellation, realistic expectations |
| Bootloader blob and DDR tuning | stuck first boot | UART console, MaskROM rescue |
| Upstream review takes months | slower inclusion | plan B with own patch set |
| Browser memory leak | frozen kiosk | restarts from HA + local safety net |
| User with a 5 GHz-only network | panel will not connect | clear warning |

### Open questions

- [ ] USB connector type on the board
- [ ] Which UART the MG21 is on and its reset/boot GPIO
- [ ] Board revision of our unit (Wi-Fi chip, touch 0x5A or 0x1A)
- [ ] Do the 86 and 120 boards differ?
- [ ] Licence of own userspace (MIT or Apache-2.0)
- [ ] Who will sign upstream patches

## Future extensions

The mainline kernel from phase 1 is the basis for all of these; none of it is in the first version.

### Android (phase 2 of the project)

- Path: AOSP on a mainline kernel following GloDroid (Mesa/Panfrost, drm_hwcomposer, minigbm, generic HALs)
- Work: graphics HAL (hardest), Wi-Fi, BT, audio, sensors, backlight, health/power/thermal; SELinux policy; AOSP build (hundreds of GB, 32+ GB RAM)
- Low-memory configuration (Android Go / `ro.config.low_ram`), performance lower than the Linux kiosk
- Strategy: vendor once per Treble for the version GloDroid supports (publicly around Android 13), then try GSIs of newer versions; the current Android 17 (June 2026) requires moving components forward ourselves
- Google publishes AOSP only twice a year since 2026 (Q2, Q4)
- Estimate: first version weeks to a few months, further versions a few weeks of maintenance (AIDL HALs are versioned)

### NSPanel Pro Gen2 with relays

- RK3326-S (same family), verify the Wi-Fi chip and relay wiring (GPIO or MCU)
- Relays as switches in HA; keep the state from the bootloader, no flicker on restart and OTA, restore state after a power outage

### Other

- Bluetooth proxy for HA (requires the ESPHome API for BLE on Linux)
- Zigbee router firmware for MG21 as an alternative to Thread (community)
- Native LVGL UI as a fallback if the HA frontend outgrows the A35
- Matter: do not expose panel entities from the panel (CSA certification, SDK size); HA exposes them to Apple/Google (HomeKit Bridge, Google Assistant, Home Assistant Matter Hub)
- Support for Smatek T6E clones (Zemismart, Avatto, Tuya)
- Distributions: Armbian, postmarketOS, Debian, once the board is in mainline

### Second target board: Shelly Wall Display X2i

WallView is built from the start as a system for multiple panels (board configuration separate, UI for 480×480 and 1440×720). NSPanel Pro Gen1 is the safest start; X2i will be added once the base is done.

| | NSPanel Pro | Shelly X2i |
|---|---|---|
| Processor | PX30 / RK3326-S, 4× A35 | RK3326-S, 4× A35 1.5 GHz |
| RAM / storage | 2 GB / 8 or 32 GB | 2 GB / 8 GB |
| Display | 3.95", 480×480 | 6.9", 1440×720 |
| Wi-Fi | RTL8723DS (Gen1), Gen2 unknown | Wi-Fi 6, 2.4 + 5 GHz, chip unknown |
| Relays | Gen2 only | in the power base, 5 A |
| Thread/Zigbee | yes | not mentioned |
| Original system | Android 8.1 | Android 11 |

- Carries over: kernel for RK3326, GPU, VPU, bootloader, Buildroot, kiosk, agent, OTA, installer
- Verify: Wi-Fi 6 chip (risk of a missing mainline driver), communication with the relay base, browser performance on 4.5× more pixels, availability of kernel sources
- Possible advantage: external USB-C, installation might work without disassembly (verify Loader mode)
- Community: shelly-x2i-root (experimental root via fastboot), ShellyElevate, mainline Linux plan for X1i (plan only)
- The Shelly Wall Display XL has an RK3566, which has even better mainline support

## Milestone overview

The functional kiosk stands after phase 2, everything else builds on it. Solid line: project core; dashed: extensions, order can change.

| Phase | Content | Estimate |
|---|---|---|
| 0 · Preparation | second panel, eMMC backup, survey from Android, test station | |
| 1 · Bootloader and kernel | M1 shell · M2 Wi-Fi and SSH · M3 picture · M4 touch | ≈ 3–4 weeks |
| 2 · System and kiosk | Buildroot, read-only system, Cage + Cog, network → **M5 functional kiosk** | |
| 3 · HA integration | ESPHome API, entities, restarts, central management | |
| 4 · User experience | Wi-Fi setup, Improv, offline clock, screensaver, design | |
| 5 · Camera and intercom (extension) | substream, MJPEG, popup on ring | |
| 6 · OTA and operation | RAUC A/B, signed images, watchdog, security → **M6 a few weeks in the wall** | |
| 7 · Thread border router (extension) | MG21 as OpenThread RCP, otbr-agent | a few weeks |
| 8 · Voice assistant (extension) | linux-voice-assistant, PipeWire, display feedback | |
| 9 · Installer | HA add-on, WebUSB, mandatory backup, restore → **M7 public release** | weeks |

In parallel: upstream (after M4 device tree + panel → Linux, then CST226 touch and STK sensor → Linux, U-Boot board defconfig; after release Buildroot defconfig; plan B own patches on top of LTS). Review takes months and runs in parallel with phases 2–9.

## Sources

- [itead/NSPro_Kernel](https://github.com/itead/NSPro_Kernel) – vendor kernel and device tree
- rtw88: RTL8723DS SDIO support
- ESPHome `cst226` touchscreen
- `hyn_cst2xx` in the Google Nest kernel
- The Roboverse – Loader and MaskROM
- blakadder – sideload and OTG port
- [OHF-Voice/linux-voice-assistant](https://github.com/OHF-Voice/linux-voice-assistant)
- CNX – NSPanel Pro Gen2
- Android Authority – AOSP schedule
- GloDroid manifest wiki
