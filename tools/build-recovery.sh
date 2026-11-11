#!/bin/bash
set -euo pipefail

WALLVIEW=/Users/jakub/WallView
LINUX=$HOME/src/linux
OUT=$WALLVIEW/out
DTS_NAME=px30-itead-nspanel-pro
ROCKCHIP_DTS=$LINUX/arch/arm64/boot/dts/rockchip
CMDLINE="console=ttyS2,1500000n8 earlycon=uart8250,mmio32,0xff160000 rdinit=/init loglevel=7 ignore_loglevel"

mkdir -p "$OUT"

cp "$WALLVIEW/kernel/dts/$DTS_NAME.dts" "$ROCKCHIP_DTS/$DTS_NAME.dts"
grep -q "$DTS_NAME.dtb" "$ROCKCHIP_DTS/Makefile" ||
	echo "dtb-\$(CONFIG_ARCH_ROCKCHIP) += $DTS_NAME.dtb" >> "$ROCKCHIP_DTS/Makefile"

make -C "$LINUX" -j"$(nproc)" -s Image "rockchip/$DTS_NAME.dtb"
cp "$LINUX/arch/arm64/boot/Image" "$OUT/Image"
cp "$ROCKCHIP_DTS/$DTS_NAME.dtb" "$OUT/$DTS_NAME.dtb"

rootfs=$(mktemp -d)
trap 'rm -rf "$rootfs"' EXIT
mkdir -p "$rootfs/bin"
cp /bin/busybox "$rootfs/bin/busybox"
install -m 0755 "$WALLVIEW/initramfs/init" "$rootfs/init"
(cd "$rootfs" && find . | cpio -o -H newc --owner=0:0 --quiet | gzip -9) > "$OUT/initramfs.cpio.gz"

python3 -I "$WALLVIEW/tools/mkbootimg.py" \
	--kernel "$OUT/Image" \
	--ramdisk "$OUT/initramfs.cpio.gz" \
	--dtb "$OUT/$DTS_NAME.dtb" \
	--cmdline "$CMDLINE" \
	--output "$OUT/recovery-wallview.img"

ls -lh "$OUT"
