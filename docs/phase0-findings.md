# Phase 0 findings – NSPanel Pro 86 (Gen1)

Collected on 2026-11-11 from the development unit. Most of the survey was done offline from a full eMMC backup instead of the running Android, because ADB could not be enabled on this firmware.

## Unit identification

| Item | Value |
|---|---|
| Board | `NSPanel Pro 86 Main V1.0`, dated 2022-09-30 |
| Internal USB | micro-USB OTG port on the board (answers the plan's open question about the connector type) |
| USB IDs | Android: `2207:0001` "px30_evb"; Loader (rockusb): `2207:330d` "USB download gadget" |
| SoC | Rockchip PX30 (chip info `PX30`, `ro.board.platform=rk3326`) |
| eMMC | Samsung KLM8G1GETF, 7456 MB, 15 269 888 sectors |
| Zigbee/Thread module | `SM-044 V1.1` (EFR32MG21) |
| Touch controller | Hynitron `CST226SE` (on the back side of the main board) |
| PMIC | Rockchip RK809 (back side) |

## Original firmware

| Property | Value |
|---|---|
| Android | 8.1.0, SDK 27 |
| Build type | **userdebug, test-keys** (`adb root` works once ADB is enabled) |
| `ro.build.display.id` | 4.9.5 (same number the eWeLink app shows as "V4.9") |
| `ro.product.version` | `NSPanel86P_1.6.5` |
| Build date | 2026-10-10 |
| Fingerprint | `rockchip/px30_evb/px30_evb:8.1.0/OPM8.190605.003/140237:userdebug/test-keys` |
| U-Boot | Rockchip next-dev 2017.09, built 2026-10-10 |

ADB is disabled. The eWeLink "tap Device ID 8×" developer mode trick does not work on this firmware. The power-cycle recovery trick was not caught in time by hand.

## Partition table (GPT)

| Partition | Start LBA | Sectors | Size |
|---|---|---|---|
| uboot | 16384 | 8192 | 4 MB |
| trust | 24576 | 8192 | 4 MB |
| misc | 32768 | 8192 | 4 MB |
| resource | 40960 | 32768 | 16 MB |
| kernel | 73728 | 65536 | 32 MB |
| boot | 139264 | 65536 | 32 MB |
| recovery | 204800 | 131072 | 64 MB |
| backup | 335872 | 229376 | 112 MB |
| security | 565248 | 8192 | 4 MB |
| cache | 573440 | 786432 | 384 MB |
| system | 1359872 | 3145728 | 1536 MB |
| metadata | 4505600 | 32768 | 16 MB |
| vendor | 4538368 | 786432 | 384 MB |
| oem | 5324800 | 524288 | 256 MB |
| frp | 5849088 | 1024 | 0.5 MB |
| smatek | 5850112 | 32768 | 16 MB |
| keypart | 5882880 | 512 | 0.25 MB |
| STSN | 5883392 | 512 | 0.25 MB |
| stback | 5883904 | 2097152 | 1024 MB |
| userdata | 7981056 | 7288799 | 3559 MB |

On this unit `smatek`, `STSN` and `misc` were all zeros at the time of the backup.

## eMMC backup

- Taken in Loader mode (hold the BOOT button while plugging in USB) with `rkdeveloptool rl 0 15269888`.
- 7 818 182 656 bytes, valid GPT header.
- SHA-256 `a936aafac642aab3e205769ac7d6762e386f8f516fb8d9d4581393470a418405`.
- Two verified copies (local disk and NAS). The image itself is never committed to this repository.

## Vendor device tree

Extracted from the `resource` partition (also embedded in `boot`) and decompiled: [reference/vendor/px30-evb-ddr3-v10-nspanel-pro-86-vendor.dts](../reference/vendor/px30-evb-ddr3-v10-nspanel-pro-86-vendor.dts).

- Model `Rockchip PX30 evb ddr3 board`, compatible `rockchip,px30-evb-ddr3-v10`.
- Touch: both `cst226se@5a` (`HYN,CST226SE`) and `cst226@1a` (`HYN,CST226`) are declared, 480×480, with IRQ and wake GPIO.
- Light/proximity: `ls_stk3x3x` / `ps_stk3x3x` (plus `stk3a5x` variants).
- `smatek@3C` (`smatek,tp`): only `compatible`, `reg` and `status`, no IRQ and no GPIO → passive I2C device, not critical for a basic kiosk.
- Wi-Fi: `wifi_chip_type = "RTL8723DS"`.
- Debug console: `fiq-debugger` on UART2 (`uart2m0` pins GPIO1_D2/D3), 1 500 000 baud.
- BOOT button: `adc-keys` on SARADC channel 2. The "volume up" key (press threshold 17 000 µV, key-up 1 800 000 µV) is what U-Boot treats as the download key.

### Display (ST7701S over DSI)

- `simple-panel-dsi`, 2 lanes, `dsi,format = 0` (RGB888), `dsi,flags = 0x803`.
- Pixel clock 16 MHz, 480×480, h: front porch 12 / sync 10 / back porch 15, v: front porch 12 / sync 8 / back porch 15.
- Physical size 72 × 70 mm.
- The full `panel-init-sequence` (ST7701 command 2 `ff 77 01 …`) is in the vendor DTS. It is the source for the `panel-sitronix-st7701` entry.

### RK809 regulators vs mainline `px30-evb.dts`

| Regulator | Vendor (this panel) | Mainline px30-evb |
|---|---|---|
| DCDC1 vdd_logic | 1.10–1.35 V | 0.95–1.35 V |
| DCDC2 vdd_arm | 0.95–1.35 V | same |
| DCDC4 vcc_3v0 | 3.0 V | same |
| DCDC5 vcc3v3_sys | 3.3 V | same |
| LDO1 vcc_1v0 | 1.0 V | same |
| LDO2 vcc1v8_soc | 1.8 V | same |
| LDO3 vcc1v0_soc | 1.0 V | same |
| LDO4 vcc3v0_pmu | 3.0 V | same |
| LDO5 vccio_sd | 1.8–3.3 V | same |
| LDO6 vcc_sd | 3.3 V | same |
| **LDO7 vcc2v8_dvp** | **3.3 V** | 2.8 V |
| LDO8 vcc1v8_dvp | 1.8 V | same |
| **LDO9 vdd1v5_dvp** | **3.0 V** | 1.5 V |

Using `px30-evb.dts` unchanged would reprogram LDO7 and LDO9. Our board DTS uses the vendor values.

IO domains (vendor): vccio1 = vcc1v8_soc, vccio2 = vccio_sd, vccio3 = vcc_3v0, vccio4 = vcc3v0_pmu, vccio5 = vcc_3v0, pmuio1/2 = vcc3v0_pmu. vccio6 is not set.

## Vendor U-Boot behaviour

Important for anything we boot through the vendor bootloader:

- **"Using kernel dtb":** U-Boot takes the device tree from the image it is about to boot (the second-stage `RSCE` resource of `boot` or `recovery`) and probes its own drivers from it. U-Boot's built-in DTB has no keys.
- The BOOT button is only seen if that DTB contains `saradc` and `adc-keys`. **Any DTB we boot through the vendor U-Boot must include them**, otherwise the BOOT button stops working (see the [incident report](incident-2026-11-11-recovery-boot.md)).
- Boot mode from the `misc` BCB (both offset 0 and 16 KiB work): `boot-recovery` boots the `recovery` partition, `bootonce-bootloader` enters rockusb.
- No Android Verified Boot (no `avb`/`vbmeta`); boot images are not signature-checked. The boot image `id` hash is not verified either.
- `bootdelay=0`, but the "Hit any key to stop autoboot" path exists → a key on the UART console should stop it.
- Boot image layout: header v0, page 2048, raw arm64 `Image`, gzip ramdisk, second stage = `RSCE` resource image with `rk-kernel.dtb` and BMP logos. Resource entry format: tag `ENTR`, name (256 bytes), then block offset and size (both u32) at byte 260.

## Tools

- `adb` from Android platform-tools (Homebrew cask).
- `rkdeveloptool` 1.32 built from source (not packaged in Homebrew).
- MaskROM loader `px30_loader_v2.12.135.bin` built from `rkbin` (`PX30MINIALL.ini`) with the official `boot_merger` (x86-64, run under `qemu-x86_64-static` in the arm64 VM). `rkdeveloptool pack` is not usable for PX30: it does not support `RC4_OFF` and numbers loaders from 0.
