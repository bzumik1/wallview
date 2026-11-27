# Zigbee radio firmware upgrade

The development panel's EFR32MG21 runs the factory EmberZNet 6.10.1 NCP (EZSP 8). Newer ITEAD Android images ship `ncp-uart-hw_nspanelpro86-120_v8.0.2.0.gbl` in `oem.img` (`/firmware/`): EmberZNet 8.0.2 (Simplicity SDK 4), hardware flow control, LZ4-compressed, unsigned, application at 0x4000. ITEAD's own host (`zgateway`, `libfreezgb.so`) installs it with EZSP `launchStandaloneBootloader` and an XMODEM upload. The image is vendor firmware: take it from your own panel's backup, never from this repository.

## What is proven

- 2026-11-27, development panel: EZSP `launchStandaloneBootloader` (mode 1) over the agent's TCP bridge starts **Gecko Bootloader v1.12.00** at 115200 baud, and `run` returns to the application (see `test-log.md`). The bootloader is reachable from software, without a reset GPIO, USB or opening the panel.
- The vendor host also drives sysfs gpio58 (GPIO1_D2). On the mainline device tree that pin is the debug console TX (`uart2m0`), so it is not used as a reset line until its function is confirmed read-only.

## Flow control

The 8.0.2 image expects RTS/CTS. The agent reads `/data/wallview/radio-flow-control` when the bridge opens `ttyS5`: `hardware` sets CRTSCTS, anything else (or no file) runs without flow control. In both cases the agent raises RTS, so a hardware flow control image can transmit before the file is written. The Gecko bootloader uses no flow control, so the file must be absent while flashing. `px30.dtsi` muxes UART5 TX, RX, CTS and RTS.

## Procedure

Run under the panel lock, on stable power, from a wired host, with the owner's explicit approval of the one-way risk, after an independent review.

Read-only checks first:
- `/sys/kernel/debug/pinctrl/*/pinmux-pins`: the UART5 TX, RX, CTS and RTS pins belong to the serial device.
- The image suits this board: panel 1 runs it on the same board revision, and `zgateway` has no other image or hardware check.
- The image decompressed offline: plausible stack pointer and reset vector, end below NVM3, fits the flash.
- bellows speaks the image's EZSP version.

1. Radio mode Zigbee coordinator, no host connected (ZHA and Zigbee2MQTT not using port 6638), `/data/wallview/radio-flow-control` absent.
2. Record the EZSP and stack version (`universal-silabs-flasher --device socket://<panel>:6638 probe`); the bootloader version comes from the entry test.
3. Check the image: `universal-silabs-flasher dump-gbl-metadata --firmware <gbl>` parses it, and its CRC is valid.
4. Flash: `universal-silabs-flasher --device socket://<panel>:6638 flash --firmware <gbl>`. It probes the application, enters the bootloader, uploads with XMODEM and runs the new application.
5. Verify EZSP version 8.0.2 at 115200. Then write `hardware` to `/data/wallview/radio-flow-control`, switch Radio mode Off and on again, and verify EZSP again.
6. Release the lock; log the result in `test-log.md`.

## Way back

- **Image rejected:** LZ4 decompression is a compile-time option of the Gecko bootloader and the version does not reveal it. A bootloader without it rejects the `PROG_LZ4` tag in the first XMODEM block, before anything is erased; `run` then returns to 6.10.1. The fallback is the same image repacked offline as an uncompressed `PROG` GBL (unsigned, so only the CRC changes), prepared before the flash.
- **Upload interrupted:** the application area may be partly written. The Gecko bootloader then stays in its menu after every reset or power-on, because it finds no valid application, so step 4 can be repeated over the bridge.
- **New application runs but does not answer:** while the file says `hardware`, writes block when CTS is not asserted, so first remove `/data/wallview/radio-flow-control` and retry, then retry with it set. If EZSP still does not answer, the bootloader cannot be entered from software and the radio needs a confirmed reset/bootloader GPIO or SWD (panel opened). This is the residual risk of the upgrade, and its worst case needs the panel opened.
- **No way back to 6.10.1:** the factory application cannot be read out without SWD, and EmberZNet does not support downgrades of its NVM3 data. The upgrade is one-way. Only ITEAD images built for this board are safe: a generic Silicon Labs image uses other UART pins and would lock the radio out until SWD.
- Network state lives in the radio's NVM3 tokens and migrates forward. Back up first in every case: `bellows backup`, manufacturing tokens and the `probe` output.
