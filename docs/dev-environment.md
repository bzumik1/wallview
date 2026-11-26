# Development environment

## Test station (macOS on Apple Silicon)

- Flashing and backups run directly on macOS:
  - `adb`: `brew install --cask android-platform-tools`
  - `rkdeveloptool`: build from [rockchip-linux/rkdeveloptool](https://github.com/rockchip-linux/rkdeveloptool) (`brew install libusb automake autoconf pkg-config`, then `autoreconf -i && ./configure && make CXXFLAGS=-Wno-error`), copy the binary to `~/.local/bin`
- Builds run in an arm64 Linux VM, so kernel and U-Boot are compiled natively for the panel's architecture.

## Build VM (Lima)

```
limactl create --name=wallview --vm-type=vz --cpus=6 --memory=12 --disk=60 \
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
  busybox-static qemu-user-static wpasupplicant dropbear-bin kexec-tools alsa-utils
```

`kexec-tools` asks whether to load kexec at shutdown; answer no (`echo "kexec-tools kexec-tools/load_kexec boolean false" | sudo debconf-set-selections` before installing).

Wi-Fi firmware for the RTL8723DS, from linux-firmware:

```
mkdir -p ~/src/firmware/rtw88
curl -fsSL -o ~/src/firmware/rtw88/rtw8723d_fw.bin \
  https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtw88/rtw8723d_fw.bin
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
2. a busybox initramfs from `initramfs/init` (backlight blink, USB gadget with ACM serial + NCM network, logs saved to the unused part of `misc`, and with local Wi-Fi credentials also Wi-Fi, SSH and `kexec`),
3. an Android boot image v0 with the DTB in an `RSCE` resource at block 13, using `tools/mkbootimg.py`.

Kernel patches in `kernel/patches/` are applied to the kernel tree by the build script (skipped if already applied). Currently: the panel driver `panel-itead-nspanel-pro`.

Kernel configuration: `defconfig` plus the fragment `kernel/config/wallview.config`, merged by the build script. Notable: `RESET_GPIO=y` (needed by `mmc-pwrseq-simple` for the Wi-Fi reset), `RTW88_8723DS=y`, `CFG80211`/`MAC80211` built in, so no modules are needed in the initramfs.

How to boot and iterate on the panel is described in the [bring-up guide](bring-up-guide.md). In short: write `recovery` once, boot it with BOOT + phone charger, then use `tools/kexec-panel.sh <panel-ip>` for every new build.

**Never** boot a mainline DTB through the vendor U-Boot's own DTB selection (BCB `boot-recovery` in `misc`); the vendor U-Boot hangs on it. See the [incident report](incident-2026-11-11-recovery-boot.md).

## MaskROM loader

Built from rkbin with the official merger:

```
qemu-x86_64-static tools/boot_merger RKBOOT/PX30MINIALL.ini
```

Output `px30_loader_v2.12.135.bin` (DDR init v2.12, usbplug v1.42, miniloader v1.35). Binary blobs are not committed to this repository.
