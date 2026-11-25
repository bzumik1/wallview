# WallView OS

How the system is put together on the development panel (stock bootloader, Android still present but not booted). This describes phase 2 work in progress.

## eMMC layout

| Partition | Size | WallView use |
|---|---|---|
| `recovery` | 64 MB | loader (boot image: kernel + initramfs + vendor DTB) |
| `cache` | 384 MB | `primary` slot: 0–64 MiB boot image, 64–320 MiB root file system (EROFS, read-only), 320–384 MiB data (ext4) |
| `backup` | 112 MB | `golden` slot: boot image only, used as rescue system |
| `misc` | 4 MB | BCB (`boot-recovery`), boot counter, logs |

All other partitions (Android `boot`, `system`, `vendor`, `userdata`, ITEAD `oem`, `stback`, …) are untouched. Once Android is removed for good, the eMMC can be repartitioned properly.

## Boot flow

1. The vendor U-Boot reads BCB `boot-recovery` and boots `recovery` with the vendor DTB.
2. Loader stage (`initramfs/init`, vendor DTB detected): increments the boot counter, picks `primary` or `golden`, extracts kernel, ramdisk and our DTB from the slot and `kexec`s into it with `wallview.slot=<slot>`.
3. WallView stage of the initramfs (our DTB): starts the watchdog daemon. In the `primary` slot it looks for an EROFS super block at 64 MiB of `cache`; if found it mounts it read-only, moves `/dev`, `/proc`, `/sys` and `switch_root`s into the Buildroot system. Otherwise (and always in `golden`, or with `wallview.rescue=1`) it stays in the **rescue system**: Wi-Fi, SSH, logs.
4. Buildroot system (busybox init, eudev): `S05wallview-data` mounts `/data`, `S20wallview-devices` sets audio and the light sensor, `S40wallview-network` starts Wi-Fi, `S50dropbear` SSH, `S95wallview-health` resets the boot counter one minute after the network is up, `S99wallview-kiosk` runs Cog.

The watchdog daemon started by the initramfs keeps running after `switch_root`.

## Root file system

Built with Buildroot 2026.02 (LTS) from the `BR2_EXTERNAL` tree in `buildroot/`:

```
make -C ~/src/buildroot O=$HOME/build/br BR2_EXTERNAL=/Users/jakub/WallView/buildroot wallview_defconfig
make -C ~/src/buildroot O=$HOME/build/br
```

- Toolchain: Arm GNU Toolchain 14.2 (Bootlin toolchains only exist for x86-64 hosts; the build VM is arm64).
- Graphics: Mesa with Panfrost (EGL, GLES, GBM; Panfrost needs LLVM at build time), WPE WebKit 2.50, Cog 0.18 with the DRM platform (no compositor).
- Wi-Fi credentials and the SSH key come from `local/` through `board/wallview/post-build.sh`, the PSK only as a hash.
- Output `images/rootfs.erofs` (LZ4HC).

The build VM has 8 GB RAM; an 8 GB swap file and `BR2_JLEVEL=4` keep the WebKit build from running out of memory.

## Data partition

`/data` (ext4 on a loop device at 320 MiB of `cache`), created on first boot:

| Path | Content |
|---|---|
| `/data/wallview/url` | dashboard URL (default `http://homeassistant.local:8123`) |
| `/data/wallview/cog-args` | extra Cog arguments, e.g. `--ignore-tls-errors` when the URL is an IP address and the certificate is for a domain |
| `/data/xdg/` | WebKit data (local storage with the Home Assistant login, cookies, cache) |

A slot update never writes beyond 320 MiB, so `/data` survives updates.

## Building and installing a full slot

```
limactl shell wallview -- bash ~/WallView/tools/build-recovery.sh
limactl shell wallview -- bash ~/WallView/tools/build-slot.sh
IMAGE=~/WallView/out/wallview-primary.img ~/WallView/tools/install-panel.sh <panel-ip> primary
```

then `reboot -f` on the panel, check, and `wallview-boot promote` (copies only the boot image to `golden`, which therefore stays a rescue system).
