#!/bin/sh
set -e

TARGET=$1
WALLVIEW=$(cd "$BR2_EXTERNAL_WALLVIEW_PATH/.." && pwd)
LOCAL=$WALLVIEW/local

mkdir -p "$TARGET/data" "$TARGET/lib/firmware"

rm -rf "$TARGET/usr/share/clc"
rm -f "$TARGET"/usr/lib/libclang*.so* "$TARGET"/usr/lib/libLLVMSPIRVLib.so* \
	"$TARGET"/usr/lib/libLTO.so* "$TARGET"/usr/lib/libRemarks.so* \
	"$TARGET"/usr/bin/amdgpu-arch "$TARGET"/usr/bin/nvptx-arch "$TARGET"/usr/bin/offload-arch \
	"$TARGET"/usr/bin/diagtool "$TARGET"/usr/bin/llvm-spirv "$TARGET"/usr/bin/spirv-*
install -D -m 0755 "$WALLVIEW/initramfs/wallview-boot" "$TARGET/usr/sbin/wallview-boot"

if [ -f "$LOCAL/wifi/ssid" ] && [ -f "$LOCAL/wifi/psk" ]; then
	ssid=$(tr -d '\r\n' < "$LOCAL/wifi/ssid")
	{
		echo "ctrl_interface=/run/wpa_supplicant"
		tr -d '\r\n' < "$LOCAL/wifi/psk" | wpa_passphrase "$ssid" | grep -v '^[[:space:]]*#psk='
	} > "$TARGET/etc/wpa_supplicant.conf"
	chmod 0600 "$TARGET/etc/wpa_supplicant.conf"
fi

rm -rf "$TARGET/root/.ssh"
if [ -f "$LOCAL/ssh/authorized_keys" ]; then
	install -d -m 0700 "$TARGET/root/.ssh"
	install -m 0600 "$LOCAL/ssh/authorized_keys" "$TARGET/root/.ssh/authorized_keys"
else
	ln -s /data/wallview/ssh "$TARGET/root/.ssh"
fi

CARGO=$HOME/.cargo/bin/cargo
env -i HOME="$HOME" PATH=/usr/bin:/bin CARGO_TARGET_DIR="$HOME/build/agent-target" \
	"$CARGO" build --release --locked --manifest-path "$WALLVIEW/agent/Cargo.toml"
install -D -m 0755 "$HOME/build/agent-target/release/wallview-agent" "$TARGET/usr/bin/wallview-agent"
