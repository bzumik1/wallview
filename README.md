# WallView

Open mainline Linux for the Sonoff NSPanel Pro wall panel, turning it into a fast Home Assistant kiosk.

WallView is not affiliated with ITEAD/Sonoff or the Open Home Foundation. "for NSPanel Pro" and "for Home Assistant" describe compatibility only.

## Status

Phase 0 (preparation) is mostly done on a development NSPanel Pro 86 (Gen1). The first mainline boot test is blocked until a USB-UART adapter arrives; see the incident report.

| Phase | State |
|---|---|
| 0 · Preparation | backup done, hardware mapped from the vendor DTS, board test points measured |
| 1 · Bootloader and kernel | build environment ready, first test image built, not booting yet |
| 2–9 | not started |

## Documentation

- [Project plan](docs/plan/plan.md) (English transcription of the [original Czech PDF](docs/plan/WallView-projektovy-plan.pdf))
- [Phase 0 findings](docs/phase0-findings.md): unit, firmware, partitions, vendor DTS, vendor U-Boot behaviour
- [Board notes and test points](docs/board-nspanel-pro-86.md): photos and measured voltages of both board sides
- [Incident 2026-11-11](docs/incident-2026-11-11-recovery-boot.md): panel stuck booting the first test image, recovery plan
- [Development environment](docs/dev-environment.md): macOS test station, Lima build VM, tools
- [Design](docs/design/README.md): WallView OS design kit, logo, colour tokens, typography

## Repository layout

| Path | Content |
|---|---|
| `docs/` | plan, findings, board notes, photos |
| `kernel/dts/` | mainline board device tree (work in progress) |
| `initramfs/` | test initramfs `init` (USB gadget console) |
| `tools/` | Android boot image builder and test image build script |
| `reference/vendor/` | decompiled vendor device tree used as hardware reference |

Backups, firmware dumps and build outputs are intentionally excluded (`.gitignore`). The original ITEAD firmware must not be distributed; everyone backs up their own unit.

## Licence

Not decided yet (see the plan: GPL-2.0 for kernel parts, MIT or Apache-2.0 for userspace).
