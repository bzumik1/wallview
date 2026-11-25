#!/bin/bash
set -euo pipefail

WALLVIEW=/Users/jakub/WallView
LINUX=$HOME/src/linux
OUT=$WALLVIEW/out
DTS_NAME=px30-itead-nspanel-pro
ROCKCHIP_DTS=$LINUX/arch/arm64/boot/dts/rockchip
CMDLINE="console=ttyS2,1500000n8 console=tty0 earlycon=uart8250,mmio32,0xff160000 rdinit=/init loglevel=7 ignore_loglevel panic=5 ${WALLVIEW_EXTRA_CMDLINE:-}"

mkdir -p "$OUT"

for patch in "$WALLVIEW"/kernel/patches/*.patch; do
	git -C "$LINUX" apply --reverse --check "$patch" 2>/dev/null && continue
	git -C "$LINUX" apply "$patch"
done

cp "$WALLVIEW/kernel/dts/$DTS_NAME.dts" "$ROCKCHIP_DTS/$DTS_NAME.dts"
grep -q "$DTS_NAME.dtb" "$ROCKCHIP_DTS/Makefile" ||
	echo "dtb-\$(CONFIG_ARCH_ROCKCHIP) += $DTS_NAME.dtb" >> "$ROCKCHIP_DTS/Makefile"

[ -f "$LINUX/.config" ] || make -C "$LINUX" -s defconfig
(cd "$LINUX" && scripts/kconfig/merge_config.sh -m -Q .config "$WALLVIEW/kernel/config/wallview.config" >/dev/null)
make -C "$LINUX" -s olddefconfig

make -C "$LINUX" -j"$(nproc)" -s Image "rockchip/$DTS_NAME.dtb"
cp "$LINUX/arch/arm64/boot/Image" "$OUT/Image"
cp "$ROCKCHIP_DTS/$DTS_NAME.dtb" "$OUT/$DTS_NAME.dtb"

rootfs=$(mktemp -d)
trap 'rm -rf "$rootfs"' EXIT
mkdir -p "$rootfs/bin"
cp /bin/busybox "$rootfs/bin/busybox"
install -m 0755 "$WALLVIEW/initramfs/init" "$rootfs/init"

copy_with_libs() {
	install -D -m 0755 "$1" "$rootfs$1"
	ldd "$1" | grep -oE '/[^ ]+' | while read -r library; do
		install -D -m 0755 "$(readlink -f "$library")" "$rootfs$library"
	done
}

WIFI_DIR=$WALLVIEW/local/wifi
if [ -d "$WIFI_DIR" ]; then
	install -D -m 0644 "$HOME/src/firmware/rtw88/rtw8723d_fw.bin" "$rootfs/lib/firmware/rtw88/rtw8723d_fw.bin"
	for binary in /usr/sbin/wpa_supplicant /usr/sbin/wpa_cli /usr/sbin/dropbear /usr/sbin/kexec; do
		copy_with_libs "$binary"
	done
	install -D -m 0755 "$WALLVIEW/initramfs/udhcpc.script" "$rootfs/etc/udhcpc.script"
	ssid=$(tr -d '\r\n' < "$WIFI_DIR/ssid")
	{
		echo "ctrl_interface=/run/wpa_supplicant"
		tr -d '\r\n' < "$WIFI_DIR/psk" | wpa_passphrase "$ssid" | grep -v '^[[:space:]]*#psk='
	} > "$rootfs/etc/wpa_supplicant.conf"
	chmod 0600 "$rootfs/etc/wpa_supplicant.conf"
	echo "root::0:0:root:/root:/bin/sh" > "$rootfs/etc/passwd"
	echo "root:x:0:" > "$rootfs/etc/group"
	install -d -m 0700 "$rootfs/root" "$rootfs/root/.ssh"
	install -m 0600 "$WALLVIEW/local/ssh/authorized_keys" "$rootfs/root/.ssh/authorized_keys"
fi
(cd "$rootfs" && find . | cpio -o -H newc --owner=0:0 --quiet | gzip -9) > "$OUT/initramfs.cpio.gz"

python3 -I "$WALLVIEW/tools/mkbootimg.py" \
	--kernel "$OUT/Image" \
	--ramdisk "$OUT/initramfs.cpio.gz" \
	--dtb "$OUT/$DTS_NAME.dtb" \
	--cmdline "$CMDLINE" \
	--output "$OUT/recovery-wallview.img"

ls -lh "$OUT"
