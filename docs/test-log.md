# Test log

Chronological record of hardware tests. "Panel 1" is the first development unit (stuck since 2026-11-11, see the [incident report](incident-2026-11-11-recovery-boot.md)), "panel 2" the second unit bought afterwards.

## 2026-11-11 – panel 1: first mainline boot via BCB

- `recovery` = test image, `misc` = BCB `boot-recovery`.
- No sign of life on USB; the BOOT button stopped working. Panel stuck.

## 2026-11-25 – panel 2: backup and comparison

- Full backup, re-read of the first 100 MiB verified.
- Same eMMC, identical GPT. Firmware `NSPanel86P_1.4.1` (vendor build 2024-12-09), U-Boot 2017.09 built 2024-12-09.
- `resource` (vendor DTB) and `trust` are byte-identical to panel 1; `uboot` is a different build with the same features.

## 2026-11-25 – review of the BCB plan (no hardware change)

An independent review against the vendor U-Boot disassembly found that the BCB approach would have stuck panel 2 as well, even with `adc-keys` in the DTB:

- The vendor U-Boot `rockchip_usb2phy` driver only matches `rockchip,rk3328-usb2phy`; mainline describes PX30 as `rockchip,px30-usb2phy` only. Without the PHY, VBUS detection returns 0 and BOOT + PC means "recovery key" instead of Loader mode.
- The vendor `board_usb_init()` only accepts a `snps,dwc2` node with `dr_mode = "otg"`.

Both were fixed in our DTS (`rockchip,rk3328-usb2phy` added as fallback compatible, `dr_mode = "otg"`).

## 2026-11-25 – panel 2: one-shot boot via charger + BOOT

- Stock recovery test first: holding BOOT on a phone charger shows the stock Android Recovery. No write needed to prove the path.
- `recovery` = test image (DTB at `RSCE` block 13), `misc` untouched.
- Result: **kernel boots**, 2 GB RAM, eMMC HS200, `init` runs, log saved to `misc`. Backlight blinks.
- USB gadget: `dwc2_core_reset: HANG! Soft Reset timeout` because the PHY's charger detection saw a DCP and powered the PHY down. Expected on a charger.

## 2026-11-25 – panel 2: vendor U-Boot on our DTB (one-shot reboot test)

Goal: find out whether the BCB path (U-Boot running on our DTB) is safe, without writing anything persistent.

- Our `init` wrote `0x5242C303` (recovery) into the PMU GRF reboot-mode register `0xff010200` and restarted with `sysrq b`. The register survives a warm reset but not a power cycle, and U-Boot clears it after reading it.
- First attempt: no log at all. The init collected diagnostics (dwc2 debugfs register dumps) before the reboot; it either hung there or the panel was unplugged too early. The init was reordered to save a basic log and reboot first.
- Second attempt: the first boot saved its log and rebooted. **The second boot (vendor U-Boot on our DTB) never reached `init`**, no backlight, no log. Power cycle → Android, BOOT + PC → Loader mode as usual.
- Conclusion: the vendor U-Boot (or our kernel right after it) hangs when U-Boot runs on the mainline DTB. This is very likely what happened to panel 1. **The BCB path is not used.**

## 2026-11-25 – panel 2: Wi-Fi and SSH

- First try: `sdio-pwrseq: deferred probe pending: pwrseq_simple: reset control not ready`. Since Linux 6.x `mmc-pwrseq-simple` with a single `reset-gpios` uses the `reset-gpio` reset controller, which was a module. Fixed with `CONFIG_RESET_GPIO=y`.
- Second try: RTL8723DS (`rtw88_8723ds`) up, WPA2 connected on 2.4 GHz, DHCP address, SSH login with key works.

## 2026-11-25 – panel 2: kexec loop

- `kexec` from our running kernel into a freshly built kernel and initramfs works; the panel is back on Wi-Fi after about 15 seconds.
- Development no longer needs the Loader mode or any eMMC write per iteration.

## 2026-11-25 – panel 2: kexec with a new DTB

- `kexec-tools` 2.0.28 on arm64 tries `kexec_file_load` first. That syscall ignores `--dtb` and reuses the running device tree, so DTB changes had no effect.
- Forcing `kexec_load` (`-c`) with our DTB hung the panel three times (three power cycles needed): our DTB has no `/memory` node, which the vendor U-Boot normally adds. `/proc/kcore` (`CONFIG_PROC_KCORE`) is also needed by `kexec_load` on arm64.
- Fix: `tools/kexec-dtb.sh` copies `/memory` and `/memreserve/` from the running `/sys/firmware/fdt` into the new DTB; `tools/kexec-panel.sh` uses `kexec_load` when the running kernel has `/proc/kcore`, otherwise `kexec_file_load` (keeps the old DTB; run it twice).

## 2026-11-25 – panel 2: display

- New panel driver `panel-itead-nspanel-pro` (kernel patch) sends the vendor ST7701S init sequence unchanged; mainline `panel-sitronix-st7701` computes its registers from parameters that do not reproduce the vendor values.
- First try: DRM up, `card0-DSI-1` connected, panel answers DCS reads (power mode `0x9c`: sleep out, display on), DSI PHY locked and streaming, but the screen stayed black. "All pixels on" (`0x23`) showed white, so the panel itself worked.
- Fix: `MIPI_DSI_CLOCK_NON_CONTINUOUS`, as all mainline ST7701 panels use. With a continuous HS clock the panel ignores the video stream.
- Result: Linux text console on the panel, RGB test pattern with correct colour order.
