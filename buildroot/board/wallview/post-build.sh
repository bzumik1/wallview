#!/bin/sh
set -e

TARGET=$1
WALLVIEW=$(cd "$BR2_EXTERNAL_WALLVIEW_PATH/.." && pwd)
LOCAL=$WALLVIEW/local

mkdir -p "$TARGET/data"
install -D -m 0755 "$WALLVIEW/initramfs/wallview-boot" "$TARGET/usr/sbin/wallview-boot"

if [ -f "$LOCAL/wifi/ssid" ] && [ -f "$LOCAL/wifi/psk" ]; then
	ssid=$(tr -d '\r\n' < "$LOCAL/wifi/ssid")
	{
		echo "ctrl_interface=/run/wpa_supplicant"
		tr -d '\r\n' < "$LOCAL/wifi/psk" | wpa_passphrase "$ssid" | grep -v '^[[:space:]]*#psk='
	} > "$TARGET/etc/wpa_supplicant.conf"
	chmod 0600 "$TARGET/etc/wpa_supplicant.conf"
fi

if [ -f "$LOCAL/ssh/authorized_keys" ]; then
	install -d -m 0700 "$TARGET/root/.ssh"
	install -m 0600 "$LOCAL/ssh/authorized_keys" "$TARGET/root/.ssh/authorized_keys"
fi
