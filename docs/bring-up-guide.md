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

Never put BCB `boot-recovery` into `misc` for a mainline image: then U-Boot itself runs on our mainline DTB, and that hangs (see the [test log](test-log.md)).

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
3. Expected: Sonoff logo (vendor U-Boot), then stripes (our kernel has no display driver yet), then the backlight blinks 3× quickly (our `init` is running), and within a minute 2× slowly (Wi-Fi has an IP address).

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

`kexec-panel.sh` uploads `out/Image`, `out/initramfs.cpio.gz` and the DTB into the panel's RAM and runs `kexec`. The new kernel is back on Wi-Fi after about 15 seconds.

If a new kernel does not come back, unplug and repeat step 4: the image in `recovery` still has working Wi-Fi.

## Logs without a network

`init` stores logs in unused space of `misc` (it never touches the BCB area except to clear it):

| `misc` sectors | Content |
|---|---|
| 0–63 | BCB, cleared by `init` at every boot |
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
