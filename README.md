# WallView

Open mainline Linux for the Sonoff NSPanel Pro wall panel, turning it into a fast Home Assistant kiosk.

WallView is not affiliated with ITEAD/Sonoff or the Open Home Foundation. "for NSPanel Pro" and "for Home Assistant" describe compatibility only.

## Status

Mainline Linux 6.18 boots on an NSPanel Pro 86 (Gen1) with the original bootloader and without a serial console; only the `recovery`, `backup` and `misc` partitions are used. The display, Wi-Fi and SSH work, the panel can boot into WallView on its own with a fallback image and a watchdog, and new kernels are started over the network with `kexec`. The first development unit is still stuck from an earlier experiment (see the incident report).

| Phase | State |
|---|---|
| 0 · Preparation | done: backups of two units, hardware mapped from the vendor DTS, board test points measured |
| 1 · Bootloader and kernel | in progress: kernel, eMMC, PMIC, backlight, display, Wi-Fi, SSH, kexec, two-stage autoboot (enabled on the dev unit) with loader, primary and golden slots and watchdog, light sensor, touch controller (probe), sound (speaker and microphone) work; Zigbee module found on UART5; touch events next; vendor U-Boot kept |
| 2–9 | not started |

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
| `kernel/patches/` | kernel patches (display panel driver) |
| `initramfs/` | initramfs `init` (loader, rescue system, switch to the root file system), `wallview-boot`, DHCP script |
| `buildroot/` | Buildroot `BR2_EXTERNAL` tree for the WallView OS root file system |
| `agent/` | `wallview-agent`: Home Assistant integration over MQTT (Rust) |
| `tools/` | Android boot image builder, test image build script, kexec deploy scripts |
| `reference/vendor/` | decompiled vendor device tree used as hardware reference |

Backups, firmware dumps and build outputs are intentionally excluded (`.gitignore`). The original ITEAD firmware must not be distributed; everyone backs up their own unit.

## Licence

Not decided yet (see the plan: GPL-2.0 for kernel parts, MIT or Apache-2.0 for userspace).
