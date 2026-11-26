#!/bin/bash
set -euo pipefail

WALLVIEW=/Users/jakub/WallView
OUT=${OUT:-$WALLVIEW/out}
ROOTFS=${ROOTFS:-$HOME/build/br/images/rootfs.erofs}
BOOT_AREA=$((64 * 1048576))
SLOT_LIMIT=$((320 * 1048576))
LOCAL=$WALLVIEW/local
EROFS_TOOLS=$HOME/build/br/host/bin

erofs_tool() {
	if [ -x "$EROFS_TOOLS/$1" ]; then
		echo "$EROFS_TOOLS/$1"
	else
		command -v "$1"
	fi
}

add_dev_files() {
	local work root
	work=$(mktemp -d)
	root=$work/root
	sudo "$(erofs_tool fsck.erofs)" --extract="$root" --xattrs "$ROOTFS" >/dev/null
	if [ -f "$LOCAL/wifi/ssid" ] && [ -f "$LOCAL/wifi/psk" ]; then
		{
			echo "ctrl_interface=/run/wpa_supplicant"
			tr -d '\r\n' < "$LOCAL/wifi/psk" | wpa_passphrase "$(tr -d '\r\n' < "$LOCAL/wifi/ssid")" | grep -v '^[[:space:]]*#psk='
		} | sudo install -m 0600 /dev/stdin "$root/etc/wpa_supplicant.conf"
	fi
	if [ -f "$LOCAL/ssh/authorized_keys" ]; then
		sudo rm -rf "$root/root/.ssh"
		sudo install -d -m 0700 "$root/root/.ssh"
		sudo install -m 0600 "$LOCAL/ssh/authorized_keys" "$root/root/.ssh/authorized_keys"
	fi
	sudo "$(erofs_tool mkfs.erofs)" -zlz4hc,9 "$work/rootfs.erofs" "$root" >/dev/null
	sudo rm -rf "$root"
	ROOTFS=$work/rootfs.erofs
	echo "development files added from $LOCAL"
}

[ "${DEV_FILES:-0}" = 1 ] && add_dev_files

boot_size=$(stat -c %s "$OUT/recovery-wallview.img")
rootfs_size=$(stat -c %s "$ROOTFS")
[ "$boot_size" -le "$BOOT_AREA" ] || { echo "boot image larger than 64 MiB" >&2; exit 1; }
[ $((BOOT_AREA + rootfs_size)) -le "$SLOT_LIMIT" ] || { echo "rootfs larger than 256 MiB" >&2; exit 1; }

cp "$OUT/recovery-wallview.img" "$OUT/wallview-primary.img"
truncate -s "$BOOT_AREA" "$OUT/wallview-primary.img"
cat "$ROOTFS" >> "$OUT/wallview-primary.img"
echo "$OUT/wallview-primary.img: boot $((boot_size / 1048576)) MiB + rootfs $((rootfs_size / 1048576)) MiB"
