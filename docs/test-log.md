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

## 2026-11-25 – panel 2: two-stage boot with the vendor DTB

- Image layout: vendor DTB in the `RSCE` resource (compiled from `reference/vendor/…-vendor.dts`; identical to the original after decompilation, same size), our DTB in the initramfs.
- Test 1, no eMMC write: `kexec` into our kernel with the vendor DTB. The kernel booted, `init` detected the vendor model, loaded the image from `recovery` and started it with `kexec -c` and our DTB. Back on Wi-Fi after 11 s. Using our full kernel with the vendor DTB works, no separate loader kernel needed.
- Test 2, the path an assembled panel uses: new image written to `recovery` over SSH (`wallview-boot install`, readback verified). Reboot-mode register set to recovery (`0xff010200 = 0x5242C303`) and `sysrq b` from a kernel without `system-power-controller` (a PMIC restart would clear the register). **U-Boot selected `recovery` by itself, ran on the vendor DTB, the loader started the `primary` slot**; display and Wi-Fi up 41 s after the reboot. Boot counter 1, reset to 0 after 60 s.
- Test 3: same image written to `backup` (`golden`). Counter set to 2, same reboot: the loader counted 3 and **started the `golden` slot**. Counter reset afterwards.
- Not tested yet: the Android fallback after 5 tries, a watchdog-triggered reset, and a cold power-up with the BCB in `misc` (planned with the panel still open).

## 2026-11-25 – panel 2: I2C devices, Zigbee UART, light sensor, touch controller

- I2C scan with I2C1 and I2C2 enabled: `1-005a` (touch controller CST226SE, on the main board, answers even with the touch panel cable unplugged), `2-0047` (light/proximity sensor), `2-003c` and `2-003d` (probably the `smatek` device).
- **Zigbee**: the SM-044 module (EFR32MG21) is on **UART5** (`ttyS1`). An ASH reset frame (`1a c0 38 bc 7e`) at 115200 baud without RTS/CTS was answered with `11 1a c1 02 0b 0a 52 7e` (XON + `RSTACK`): EZSP NCP firmware, software flow control. No answer on UART4.
- **Light/proximity sensor**: chip id `0x5f`, unknown to mainline `stk3310` but the register map works (driver only logs "new unknown chip id"). The driver default (gain scale 6.4, integration time 0.74 ms) always reads 0 indoors; with scale 0.1 and 189 ms: 0–10 in the dark, 183 with the dining room light at 1 % (switched through Home Assistant), then back down. Proximity reads 3 with nothing in front.
- **Touch controller**: answers the CST3xx protocol of the mainline `hynitron_cstxxx` driver (check code `0xcaca`, firmware `0x03000001`, resolution 480×480 from register `0xd1f8`, touch frames end with `0xab`). It does not enter the CST3xx bootloader (`0xa002` reads 0; the reset line GPIO0_B4 itself works), so the patch adds a `hynitron,cst226se` entry that skips the bootloader presence check. Probe succeeds, `input0` "Hynitron cst3xx Touchscreen", IRQ on GPIO0_B5. Touch events still to be tested with the touch panel connected.

## 2026-11-25 – panel 2: audio

- DTS following the RK3326 Odroid Go layout: RK809 codec (`rk817` driver, `rockchip,mic-in-differential`) on I2S1 with `simple-audio-card` "NSPanel Pro", MCLK from `SCLK_I2S1_OUT`, routing Mic → MICL, SPKO → Speaker. No external amplifier GPIO in the vendor DTS.
- Result: card 0 "NSPanel Pro" (`ff070000.i2s-rk817-hifi`), mixer controls `Master`, `Mic Capture Gain`, `Playback Mux` (HP/SPK, default HP).
- Capture (16 kHz stereo): after about 0.5 s of ADC settling, channel 0 (microphone) has DC 2075 and 14 LSB RMS noise in a quiet room, channel 1 7.8 LSB.
- Speaker (`Playback Mux` = SPK): a 1 kHz tone at −26 dBFS with `Master` 200 (−20.5 dB) was heard, and recording at the same time showed the 1 kHz component exactly during the tone (0.01 → 155). The level was suspiciously constant, so part of it may be an internal codec loopback rather than the acoustic path. A chime (440/660/880 Hz) at −14 dBFS with `Master` 240 (−5.6 dB) sounded clean. `init` now selects SPK and sets `Master` to 220.
- `alsa-utils` (`aplay`, `arecord`, `amixer`) and the ALSA configuration (without UCM) are in the initramfs; the image is 59.3 MiB of the 64 MiB `recovery` partition.

## 2026-11-26 – panel 2: autoboot redesign after review, autoboot enabled

An independent review of the first autoboot version found:

- the loader could not start the watchdog (the vendor DTB has no enabled watchdog node), so the documented protection did not exist;
- the boot counter led to Android after a few slow-router power cuts or a corrupted counter value could make `init` (PID 1) fail;
- `golden` was not sticky;
- the `primary` slot was also the loader, so an interrupted update could break the loader.

Changes: separate `loader` (`recovery`), `primary` (`cache`) and `golden` (`backup`) slots; robust counter; `golden` sticky, Android only when no slot can be loaded; watchdog daemon from the start of the WallView stage; waiting for Wi-Fi without limit and resetting the counter only after an address; `wallview-boot promote`.

Tests:

- `kexec` into the loader (vendor DTB) from a WallView with the watchdog running: the panel was reset by the watchdog. Not a real-boot problem, but this test path is no longer used.
- A `kexec`ed kernel came up without Wi-Fi: `rtw88_8723ds: failed to dump efuse logical map`. Added an SDIO unbind/bind power cycle and a reboot after 10 minutes without an address (when autoboot is on). Four following `kexec` rounds were fine (about 65 s each).
- Cold boot through charger + BOOT with the new loader: `primary` loaded from `cache`.
- **BCB `boot-recovery` written in Loader mode (2026-11-26).** Cold power-up on the charger without BOOT: autoboot into `primary`. BOOT + PC with the BCB set: Loader mode.
- Recovery tests with autoboot on: watchdog daemon stopped (hardware reset, back after 67 s), `reboot -f` through the PMIC (41 s), kernel panic (61 s). All came back to `primary`.
- Slot update over Wi-Fi: new `primary` installed, `reboot -f`, booted, counter reset, `wallview-boot promote` to `golden`.

## 2026-11-26 – panel 2: smaller kernel, GPU, thermal, loader update

- Kernel limited to `ARCH_ROCKCHIP` (`kernel/config/platforms.config`): `Image` 30 MB instead of 52.6 MB, boot image 38 MiB instead of 59 MiB. All hardware still works (display, touch controller, audio, light sensor, Wi-Fi, watchdog, UARTs); `kexec` round trip 44 s instead of 65 s.
- cpufreq already worked: `cpufreq-dt`, `schedutil`, 600–1296 MHz.
- Thermal: `ROCKCHIP_THERMAL` built in, zones `soc-thermal` and `gpu-thermal` (about 40 °C idle), hardware shutdown at 120 °C via CRU reset (vendor value, mainline default).
- GPU: Panfrost finds Mali-G31 (`0x7093`), `/dev/dri/renderD128`. Only the 480 MHz OPP is usable because `vdd_log` has the vendor minimum of 1.1 V.
- After installing the new `primary`, the GPU was missing: the loader passed its own (older) DTB. The loader now extracts `boot/wallview.dtb` from the slot's ramdisk. Tested first without writing anything by `kexec` into the new kernel with a vendor DTB whose watchdog node was enabled (so that the loader stage feeds the running watchdog), then installed as `loader` and `primary`: reboot through the new loader in 36 s, GPU present. The previous loader is saved locally in `out-saved/`.

## 2026-11-26 – panel 2: Bluetooth, stable serial port numbers

- `realtek,rtl8723ds-bt` on UART1 with RTS/CTS; RK809 now exposes two clock outputs and `clkout2` (32 kHz) is enabled through `sdio-pwrseq` (`ext_clock`), as in the vendor DTS. Wi-Fi unaffected.
- `BT_HCIUART` stayed a module until `POWER_SEQUENCING` was built in (`depends on POWER_SEQUENCING || !POWER_SEQUENCING`).
- linux-firmware has no `rtl8723ds_fw`/`rtl8723ds_config`; the files from the vendor partition (`/etc/firmware`) work: `hci0`, RTL8723D ROM v2, firmware `0xaa897e1b`. `Opcode 0x0c77 failed: -56` is a harmless unsupported optional command.
- Serial aliases fix the numbering: `ttyS1` Bluetooth, `ttyS2` console, `ttyS4` UART4, `ttyS5` Zigbee (EZSP `RSTACK` confirmed again).

## 2026-11-26 – panel 2: first WPE/Cog test (Debian packages)

While the Buildroot WebKit build was still running (it needs about 3 GB per compiler process and crawls on a 16 GB Mac), a throwaway Debian trixie root file system (`debootstrap`, Cog 0.18.4, WPE WebKit 2.48.3, Mesa 25 with Panfrost, 596 MB after trimming) was copied into the panel's RAM and started with `chroot` from the rescue system. Nothing was written to the eMMC.

- `udev` in a chroot needs `SYSTEMD_IGNORE_CHROOT=1` for `udevadm trigger`; the touch controller is then tagged `ID_INPUT_TOUCHSCREEN=1`.
- Debian's WebKit sandbox (bubblewrap) fails in an initramfs root (`Failed to make / slave`); `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` for the test. The Buildroot build has no sandbox.
- The start page (`launcher.html`) loads in about 1.3 s and switches to Home Assistant once it answers.
- Cog's default DRM renderer `modeset` crashed (SIGSEGV) while loading Home Assistant. With `--platform-params=renderer=gles` Home Assistant loads and shows its login page; Panfrost handles the GPU jobs. Cog with two web processes uses about 340 MB of RAM.
- The kiosk script now uses the `gles` renderer.

## 2026-11-26 – panel 2 assembled: autoboot on mains, touch

- Panel assembled and powered from mains: autoboot into WallView (primary slot) without any button. Reboot to SSH takes 34 s: about 19 s vendor U-Boot plus loader, 15 s WallView kernel (Wi-Fi associated at 10.9 s). The loader stage is the main target for boot time optimisation.
- Touch works with the mainline `hynitron_cstxxx` driver and our CST226SE patch: 2135 interrupts and 56 touch-downs in 30 s, multi-touch events on `event0`. Corner taps: top left X≈8 Y≈36, bottom right X≈469 Y≈472, so no axis swap or inversion is needed. (Earlier "no touch" results were simply captures while nobody touched the screen.)
