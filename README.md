# WallView

Open mainline Linux for the Sonoff NSPanel Pro wall panel, turning it into a fast Home Assistant kiosk.

WallView is not affiliated with ITEAD/Sonoff or the Open Home Foundation. "for NSPanel Pro" and "for Home Assistant" describe compatibility only.

## Status

Mainline Linux 6.18 runs on an NSPanel Pro 86 (Gen1) with the original bootloader and without a serial console. The panel boots into WallView on its own (two-stage boot with a rescue slot and a hardware watchdog); everything after that is done over Wi-Fi. Android is still on the eMMC, untouched. The first development unit is still stuck from an earlier experiment (see the incident report).

| Hardware | Mainline driver | State |
|---|---|---|
| CPU, cpufreq (600–1296 MHz), thermal | `cpufreq-dt`, `rockchip_thermal` | works |
| eMMC, RK809 PMIC | `dw_mmc`, `rk808` | works |
| Display 480×480 (ST7701S, MIPI DSI) | own `panel-itead-nspanel-pro` | works |
| Backlight | `pwm-backlight` | works |
| GPU Mali-G31 | Panfrost | driver loads; Mesa in progress |
| Touch CST226SE | `hynitron_cstxxx` + patch | works (multi-touch, correct orientation) |
| Wi-Fi RTL8723DS | `rtw88_8723ds` | works |
| Bluetooth RTL8723DS | `hci_uart` H5 + `btrtl` | works (vendor firmware) |
| Light/proximity sensor | `stk3310` | works |
| Speaker, microphone (RK809 codec) | `rk817` codec | works |
| Zigbee/Thread module EFR32MG21 | UART5 | Zigbee coordinator over the network (EZSP bridge in the agent); Thread planned |
| Hardware watchdog | `dw_wdt` | works |

| Phase | State |
|---|---|
| 0 · Preparation | done |
| 1 · Bootloader and kernel | nearly done; vendor U-Boot kept, own bootloader later |
| 2 · System and kiosk | in progress: Buildroot root file system with WPE WebKit/Cog building, boot flow, start screen |
| 3 · Home Assistant integration | in progress: `wallview-agent` speaks the ESPHome native API (mDNS, Noise, key provisioning, 15 entities), tested with aioesphomeapi |
| 4 · User experience | started: boot logo, start/offline screen |
| 5–9 | not started |

## Documentation

- [Project plan](docs/plan/plan.md) (English transcription of the [original Czech PDF](docs/plan/WallView-projektovy-plan.pdf))
- [Bring-up guide](docs/bring-up-guide.md): step-by-step procedure to boot mainline Linux on a stock panel safely
- [Test log](docs/test-log.md): what was tried on the hardware and what happened
- [Phase 0 findings](docs/phase0-findings.md): unit, firmware, partitions, vendor DTS, vendor U-Boot behaviour
- [Board notes and test points](docs/board-nspanel-pro-86.md): photos and measured voltages of both board sides
- [Incident 2026-11-11](docs/incident-2026-11-11-recovery-boot.md): panel stuck booting the first test image, recovery plan
- [WallView OS](docs/wallview-os.md): eMMC layout, boot flow, root file system, data partition, kiosk
- [Home Assistant integration](docs/home-assistant.md): MQTT agent, entities, configuration
- [Development environment](docs/dev-environment.md): macOS test station, Lima build VM, tools
- [Design](docs/design/README.md): WallView OS design kit, logo, colour tokens, typography

## Repository layout

| Path | Content |
|---|---|
| `docs/` | plan, findings, board notes, photos |
| `kernel/dts/` | mainline board device tree (work in progress) |
| `kernel/config/` | kernel configuration fragment merged on top of `defconfig` |
| `kernel/patches/` | kernel patches (display panel driver, touch, Bluetooth quirk) |
| `initramfs/` | initramfs `init` (loader, rescue system, switch to the root file system), `wallview-boot`, DHCP script |
| `buildroot/` | Buildroot `BR2_EXTERNAL` tree for the WallView OS root file system |
| `agent/` | `wallview-agent`: Home Assistant integration over the ESPHome native API, optional MQTT (Rust) |
| `tools/` | Android boot image builder, test image build script, kexec deploy scripts |
| `reference/vendor/` | decompiled vendor device tree used as hardware reference |

Backups, firmware dumps and build outputs are intentionally excluded (`.gitignore`). The original ITEAD firmware must not be distributed; everyone backs up their own unit.

## Licence

MIT (`LICENSE`), with these exceptions:

- `kernel/patches/`: GPL-2.0, like the Linux kernel they apply to.
- `kernel/dts/`: GPL-2.0-or-later OR MIT, as stated in the file header (the usual licence of Linux device trees).
- `agent/proto/`: from aioesphomeapi, MIT, unchanged.
- `reference/vendor/`: decompiled from the vendor firmware, kept only as a hardware reference.
