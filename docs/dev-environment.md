# Development environment

## Test station (macOS on Apple Silicon)

- Flashing and backups run directly on macOS:
  - `adb`: `brew install --cask android-platform-tools`
  - `rkdeveloptool`: build from [rockchip-linux/rkdeveloptool](https://github.com/rockchip-linux/rkdeveloptool) (`brew install libusb automake autoconf pkg-config`, then `autoreconf -i && ./configure && make CXXFLAGS=-Wno-error`), copy the binary to `~/.local/bin`
- Builds run in an arm64 Linux VM, so kernel and U-Boot are compiled natively for the panel's architecture.

## Build VM (Lima)

```
limactl create --name=wallview --vm-type=vz --cpus=6 --memory=8 --disk=60 \
  --mount-only ~/WallView:w --mount-type=virtiofs \
  --set '.images=[{"location":"https://cloud-images.ubuntu.com/releases/noble/release/ubuntu-24.04-server-cloudimg-arm64.img","arch":"aarch64"}]' \
  template:ubuntu-24.04
limactl start wallview
```

The `.images` override is needed because the template's dated Ubuntu image redirects to the slow archive server.

Packages inside the VM:

```
sudo apt-get install build-essential git bc bison flex libssl-dev libelf-dev \
  libgnutls28-dev uuid-dev swig python3-dev python3-pyelftools python3-setuptools \
  device-tree-compiler cpio kmod rsync ccache u-boot-tools xz-utils zstd lz4 \
  busybox-static qemu-user-static
```

Sources (in `~/src` inside the VM):

| Source | Version |
|---|---|
| Linux stable | v6.18.63 |
| U-Boot | v2026.10 |
| TF-A | v2.15.0 |
| rkbin (sparse) | `bin/rk33/px30_*`, `RKBOOT/PX30MINIALL.ini`, `tools/boot_merger` |

## Test image through the vendor U-Boot

`tools/build-recovery.sh` (run inside the VM) builds:

1. the kernel `Image` and `kernel/dts/px30-itead-nspanel-pro.dts`,
2. a busybox initramfs from `initramfs/init` (USB gadget with ACM serial + NCM network, backlight blink, log saved to the unused part of `misc`),
3. an Android boot image v0 with the DTB in an `RSCE` resource, using `tools/mkbootimg.py`.

Kernel configuration: `defconfig` plus built-in `USB_DWC2`, `USB_CONFIGFS_ACM`, `USB_CONFIGFS_NCM`, `PHY_ROCKCHIP_INNO_USB2`, `REGULATOR_RK808`, `MMC_DW_ROCKCHIP`, `BACKLIGHT_PWM`, `PSTORE_RAM`.

**Warning:** the DTB must contain `saradc` + `adc-keys` before it is booted through the vendor U-Boot again. See the [incident report](incident-2026-11-11-recovery-boot.md).

## MaskROM loader

Built from rkbin with the official merger:

```
qemu-x86_64-static tools/boot_merger RKBOOT/PX30MINIALL.ini
```

Output `px30_loader_v2.12.135.bin` (DDR init v2.12, usbplug v1.42, miniloader v1.35). Binary blobs are not committed to this repository.
