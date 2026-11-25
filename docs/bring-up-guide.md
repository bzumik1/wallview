# Bring-up guide: mainline Linux on an NSPanel Pro 86 without a serial console

This is the procedure that works on an unmodified NSPanel Pro 86 (Gen1) with the original ITEAD firmware. It never changes the bootloader, `boot`, `system` or `misc`, and it only ever writes the `recovery` partition. Every step can be undone from the backup.

Read the [vendor U-Boot notes](phase0-findings.md#vendor-u-boot-behaviour) and the [incident report](incident-2026-11-11-recovery-boot.md) first. They explain why the obvious approach (BCB `boot-recovery` in `misc`) is dangerous on this panel.

## What you need

- The panel opened (front glass unit with the main board), the internal micro-USB port reachable and the BOOT button accessible. Never connect 230 V while the panel is open.
- A USB data cable (check it: a charge-only cable shows no device at all).
- A plain phone charger with a USB-A port (a "dedicated charging port"; an Android charger works, an Apple charger may be detected differently).
- A Mac or Linux PC with `rkdeveloptool` and an arm64 Linux build environment (see [development environment](dev-environment.md)).
- A 2.4 GHz Wi-Fi network (the RTL8723DS chip has no 5 GHz).

## How the vendor U-Boot is used

| Action | Result |
|---|---|
| Power up normally | Android |
| Hold BOOT, plug into a PC | Loader mode (rockusb `2207:330d`), U-Boot runs on the vendor DTB |
| Hold BOOT, plug into a phone charger | the `recovery` partition boots **once**; U-Boot itself still runs on the vendor DTB from `boot` and only hands our DTB to our kernel |

The third row is the whole trick. U-Boot decides "download" vs "recovery" from the BOOT key and the USB charger type: with a PC (SDP/CDP) it enters rockusb, with a charger (DCP) it sets `reboot_mode=recovery` for this boot only. Nothing persistent is written, so the next normal power-up is Android again and BOOT + PC is always Loader mode.

U-Boot must never run on our mainline DTB: it hangs (see the [test log](test-log.md)). That is why the image in `recovery` carries the **vendor** DTB in its `RSCE` resource and our DTB inside the initramfs (see [two-stage boot](#two-stage-boot) below). With this layout U-Boot behaves exactly as when it boots Android, also when it selects `recovery` by itself.

## Step 1: full backup

1. Do not connect the panel to Wi-Fi or eWeLink before the backup (firmware update 4.10.0 may close the ADB/root path).
2. Hold BOOT, plug the panel into the PC, release BOOT. `rkdeveloptool ld` shows `Loader`.
3. Record the unit:
   ```
   rkdeveloptool rfi > flash-info.txt
   rkdeveloptool ppt > partitions-gpt.txt
   ```
4. Read the whole eMMC (about 10 minutes, 7.3 GB):
   ```
   rkdeveloptool rl 0 15269888 nspanel-emmc-full.img
   shasum -a 256 nspanel-emmc-full.img > nspanel-emmc-full.img.sha256
   ```
   Use the sector count from `flash-info.txt` if it differs.
5. Verify the read: read the first 100 MiB again and compare hashes.
   ```
   rkdeveloptool rl 0 204800 head.img
   head -c 104857600 nspanel-emmc-full.img | shasum -a 256
   shasum -a 256 head.img
   ```
6. Copy the image to a second disk and verify the hash there. Never publish it.
7. Optional but useful: compare the partition table and `uboot`/`resource` partitions with [phase 0 findings](phase0-findings.md). Our DTS was written for the vendor DTB of these units.

## Step 2: build the test image

Inside the build VM:

```
bash ~/WallView/tools/build-recovery.sh
```

The script:

1. copies `kernel/dts/px30-itead-nspanel-pro.dts` into the kernel tree,
2. merges `kernel/config/wallview.config` into the kernel configuration (starting from `defconfig` on a fresh tree),
3. builds `Image` and the DTB,
4. builds a busybox initramfs from `initramfs/init`; if `local/wifi/ssid` and `local/wifi/psk` exist, it adds Wi-Fi (firmware, `wpa_supplicant`, DHCP), SSH (`dropbear`, key-only login with `local/ssh/authorized_keys`) and `kexec`,
5. packs an Android boot image v0 with `tools/mkbootimg.py`. The DTB goes into the `RSCE` resource at block 13, the same offset as in the vendor images, because U-Boot looks it up at the offset it remembered from `boot`.

Local files (never committed, `local/` is in `.gitignore`):

| File | Content |
|---|---|
| `local/wifi/ssid` | Wi-Fi network name |
| `local/wifi/psk` | Wi-Fi password; the build stores only the derived PSK hash in the image |
| `local/ssh/authorized_keys` | public SSH key(s) allowed to log in as root |

| `local/firmware/` | extra firmware copied to `/lib/firmware` in the image (Bluetooth, see below) |

Bluetooth firmware is not in linux-firmware for the RTL8723DS. Take it from your own backup (vendor partition, LBA 4538368, 786432 sectors):

```
dd if=nspanel-emmc-full.img of=vendor.img bs=512 skip=4538368 count=786432
mkdir -p local/firmware/rtl_bt
debugfs -R "dump /etc/firmware/rtl8723ds_fw local/firmware/rtl_bt/rtl8723ds_fw.bin" vendor.img
debugfs -R "dump /etc/firmware/rtl8723ds_config local/firmware/rtl_bt/rtl8723ds_config.bin" vendor.img
```

Additional kernel command line options can be passed with `WALLVIEW_EXTRA_CMDLINE`.

## Step 3: write `recovery`

The panel is in Loader mode (BOOT + PC).

1. Confirm it is the expected unit, for example by comparing the first MiB of `boot` with the backup:
   ```
   rkdeveloptool rl 139264 2048 boot-check.img
   ```
2. Write and verify:
   ```
   rkdeveloptool wl 204800 out/recovery-wallview.img
   rkdeveloptool rl 204800 <image size / 512> readback.img
   ```
   The SHA-256 of `readback.img` must equal the image.
3. Check that `misc` has no BCB command (the first 32 KiB of LBA 32768 must be zeros).
4. **Do not** run `rkdeveloptool rd` and do not boot Android afterwards. Android runs `install-recovery.sh` on boot and would restore the stock recovery.

## Step 4: boot it once

1. Unplug the panel from the PC.
2. Hold BOOT and plug the panel into the phone charger; keep BOOT pressed for about 5 seconds.
3. Expected: Sonoff logo (vendor U-Boot), then the Linux text console on the display (an image built before the display driver shows stripes instead), the backlight blinks 3× quickly (our `init` is running), and within a minute 2× slowly (Wi-Fi has an IP address).

## Step 5: log in

The panel requests DHCP with host name `wallview`. If the router does not resolve it, find the panel by its SSH banner:

```
for i in $(seq 2 254); do ping -c1 -W300 -t1 192.168.1.$i >/dev/null & done; wait
for ip in $(arp -an | grep -oE '192\.168\.1\.[0-9]+' | sort -u); do
	nc -G1 -w2 "$ip" 22 </dev/null 2>/dev/null | grep -q dropbear_2022 && echo "$ip"
done
```

Then:

```
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@<panel-ip>
```

The host key is generated at every boot, hence the options.

## Step 6: iterate with kexec

From now on the panel stays on the charger. A new kernel, DTB or initramfs is started without the bootloader and without writing anything:

```
limactl shell wallview -- bash ~/WallView/tools/build-recovery.sh
~/WallView/tools/kexec-panel.sh <panel-ip>
```

`kexec-panel.sh` uploads `out/Image`, `out/initramfs.cpio.gz` and the DTB into the panel's RAM and runs `kexec`. The new kernel is back on Wi-Fi after about 15 seconds. Wait for it by watching `/proc/sys/kernel/random/boot_id` change.

How the DTB is passed:

- If the running kernel has `/proc/kcore` (`CONFIG_PROC_KCORE=y`), the script uses `kexec_load` (`kexec -c`) with our DTB. Before that, `tools/kexec-dtb.sh` copies the `/memory` node and `/memreserve/` entries from the running device tree (`/sys/firmware/fdt`, which contains the vendor U-Boot's fixups) into it. Without the memory node the new kernel hangs.
- Otherwise it uses `kexec_file_load`, which works but keeps the running DTB. Run the script a second time from the new kernel to switch the DTB.

If a new kernel does not come back, unplug and repeat step 4: the image in `recovery` still has working Wi-Fi.

## Step 7: autoboot and assembly

Do this while the panel is still open.

1. Make sure all slots hold an image that boots with Wi-Fi: `install-panel.sh <ip> loader`, `install-panel.sh <ip> primary`, reboot, check, `wallview-boot promote`.
2. On the panel: `wallview-boot linux`.
3. Unplug and plug the charger **without** BOOT. The panel must come up with display and Wi-Fi on its own.
4. Hold BOOT and plug into the PC: `rkdeveloptool ld` must show `Loader`. With the vendor DTB in `recovery` U-Boot still sees the button and the USB PHY.
5. Unplug, assemble, connect mains. From now on everything is done over Wi-Fi; to go back to Android run `wallview-boot android` and reboot.

## Two-stage boot

The `recovery` partition holds the **loader**. U-Boot boots it with the vendor DTB (from `RSCE` block 13). Our kernel boots with that DTB far enough to run `init`, which sees model `Rockchip PX30 evb ddr3 board` instead of `Sonoff NSPanel Pro` and acts as a loader: it reads kernel, ramdisk and command line of an image slot from the eMMC (Android boot image header), adds `wallview.slot=<slot>` and starts it with `kexec -c` and our DTB. The DTB is taken from the slot's own ramdisk (`boot/wallview.dtb`), so a slot update also updates the device tree; the loader's own copy is only a fallback. Our DTS contains the `/memory` node with the same ranges the vendor U-Boot reports, which `kexec_load` needs.

The loader and the slots currently contain the same image; the loader is a separate copy so that a broken or interrupted slot update can never take the loader with it.

## Autoboot, slots and fallbacks

For an assembled panel (no BOOT button, no USB, mains power) the BCB `boot-recovery` is written to `misc`; U-Boot then selects `recovery` on every power-up, still running on the vendor DTB. BOOT + PC still gives Loader mode with the BCB set (tested).

| Slot | Partition | Purpose |
|---|---|---|
| `loader` | `recovery` (64 MB) | the loader; not updated over the network |
| `primary` | `cache` (384 MB, Android's cache, unused while Android does not run) | current image |
| `golden` | `backup` (112 MB, unused by the stock firmware, all zeros on both units) | copy of the last `primary` that booted with Wi-Fi |

The loader keeps a boot counter in `misc` sector 2046 (`WALLVIEW-TRIES=<n>`; anything that is not a plain number reads as 0):

| Counter after increment | Loader tries |
|---|---|
| 1–2 | `primary`, then `golden` if `primary` cannot be loaded |
| 3 and more | `golden`, then `primary` |
| no slot loadable | clears the BCB and reboots into Android |

`golden` is sticky: only a healthy `primary` boot resets the counter (60 s after Wi-Fi has an address), so after two failed `primary` boots the panel stays on `golden` until `wallview-boot reset`. Repeated failures never lead to Android.

Health and recovery in the WallView stage:

- The hardware watchdog (`dw_wdt`, about 21 s, cannot be stopped once running) is started by the busybox `watchdog` daemon at the very beginning of `init`. A hang of the kernel or of `init` ends in a reset (tested: 67 s from stopping the daemon to WallView again). The loader itself runs with the vendor DTB, which has no watchdog node, so the short loader phase is not covered.
- `panic=5` restarts after a kernel panic (tested: 61 s).
- If `wlan0` does not appear, the SDIO controller is unbound and bound again, which power-cycles the RTL8723DS through `mmc-pwrseq` (up to three times). After `kexec` the chip sometimes fails with `failed to dump efuse logical map`.
- Without an address for 10 minutes the panel reboots, but only when autoboot is enabled (otherwise a reboot would start Android).

Commands on the panel (`wallview-boot`):

| Command | Effect |
|---|---|
| `wallview-boot status` | checksums of the three slots, BCB and counter |
| `wallview-boot linux` | write BCB `boot-recovery` (autoboot WallView) |
| `wallview-boot android` | clear BCB (next boot is Android, which restores the stock `recovery`; coming back needs Loader mode) |
| `wallview-boot reset` | counter to 0 |
| `wallview-boot install <loader\|primary\|golden> <image> <sha256>` | write an image to a slot and verify it by reading back |
| `wallview-boot promote` | copy `primary` to `golden` and verify |

From the Mac, `tools/install-panel.sh <panel-ip> <loader|primary|golden>` uploads `out/recovery-wallview.img` and runs `wallview-boot install`.

Update procedure:

1. Build and test with `kexec-panel.sh` (only in RAM).
2. `install-panel.sh <ip> primary`, then `reboot -f` on the panel: the loader starts the new `primary`.
3. Check display, Wi-Fi and `wallview-boot status` (counter back to 0 after about a minute).
4. `wallview-boot promote`.

Update the `loader` only when really needed, and only while the panel is still reachable through Loader mode if possible.

Limitations of `kexec` with a running watchdog: never `kexec` into a kernel with the vendor DTB (it does not feed the watchdog, the loader takes longer than 21 s, the watchdog resets the panel). Our own kernels pick up the running watchdog in their first second, so `kexec-panel.sh` is fine.

## Logs without a network

`init` stores logs in unused space of `misc` (it never touches the BCB area; only `wallview-boot` and the loader fallback do):

| `misc` sectors | Content |
|---|---|
| 0–63 | BCB (`boot-recovery` at offsets 0 and 16 KiB) |
| 2046 | boot counter (`WALLVIEW-TRIES=<n>`) |
| 2047 | marker for the one-shot reboot test (`WALLVIEW-RBTEST`) |
| 2048–4095 | log slot 1: basic log at start |
| 4096–6143 | log slot 2: full log after 15 s |
| 6144–8191 | log slot 3: full log after 60 s |

Read them in Loader mode:

```
rkdeveloptool rl 32768 8192 misc.img
dd if=misc.img bs=512 skip=2048 count=2048 | tr -d '\0'
```

## Going back to stock

In Loader mode, write the original `recovery` (and if needed `misc`) back from the backup:

```
dd if=nspanel-emmc-full.img of=recovery-stock.img bs=512 skip=204800 count=131072
rkdeveloptool wl 204800 recovery-stock.img
```
