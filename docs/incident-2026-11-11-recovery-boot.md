# Incident 2026-11-11: panel stuck booting the first mainline test image

## Summary

The first mainline kernel test was booted through the vendor U-Boot from the `recovery` partition. It never appeared on USB, and because our minimal device tree did not describe the BOOT button, the vendor U-Boot stopped seeing the button. The panel can no longer be put into Loader mode by the usual method. Nothing else on the eMMC was touched: the bootloader, `boot`, `system` and `userdata` are intact, and a full verified backup exists.

## What was written to the eMMC

| Partition | Content |
|---|---|
| `misc` | BCB `boot-recovery` at offset 0 and at 16 KiB (32 KiB written, rest of the 4 MB still zeros) |
| `recovery` | `recovery-wallview.img`: Android boot image v0 with mainline 6.18.63 `Image`, busybox initramfs and our DTB in an `RSCE` resource |

Both writes were verified by reading back and comparing.

## What happened

1. With `misc` set to `boot-recovery`, the vendor U-Boot boots the `recovery` partition on every power-up.
2. The vendor U-Boot loads the device tree from the image it boots ("Using kernel dtb") and binds its drivers from it.
3. Our first DTB (`kernel/dts/px30-itead-nspanel-pro.dts`) intentionally contained only eMMC, PMIC, USB OTG, UART2 and TSADC. It had no `saradc` and no `adc-keys`.
4. The BOOT button is an ADC key (SARADC channel 2). Without `adc-keys` in the DTB, U-Boot's download-key check never fires.
5. Our kernel did not bring up the USB gadget (D+ measured at 0 V), so there is no way in over USB either.

The cause of the kernel/USB failure is still unknown, because there is no console.

## Recovery options considered

| Option | Status |
|---|---|
| BOOT button / UBOOT pad | does not work while our DTB is booted |
| MaskROM via an eMMC test point | no eMMC line exposed outside the shields (see [board notes](board-nspanel-pro-86.md)); pad "A" tried, no effect |
| Removing the shield | not possible for the owner |
| **UART console** | planned: U-Boot can be interrupted ("Hit any key to stop autoboot"), then `rockusb 0 mmc 0` gives Loader mode |

## Recovery procedure (once a USB-UART adapter is available)

1. Find the SoC TX pad among U6–U9 by listening at 1 500 000 baud while power-cycling the panel.
2. Connect the adapter TX to the SoC RX pad.
3. Power-cycle the panel while sending keys continuously, stop at the U-Boot prompt.
4. Run `rockusb 0 mmc 0`.
5. From the Mac: `rkdeveloptool wl 32768 misc-zero-restore.img` (32 KiB of zeros), restore `recovery` from the backup, `rkdeveloptool rd`.
6. The panel boots the original Android; the BOOT button works again.

## Lessons

- Every DTB booted through the vendor U-Boot must contain `saradc` and `adc-keys` with the download ("volume up") key, so the BOOT button keeps working.
- Do not do blind boot experiments without a serial console. The plan's test station requires a USB-UART adapter for exactly this reason.
- Prefer a recovery path that does not depend on the image under test, for example the BCB `bootonce-bootloader` or a watchdog that clears `misc`.
