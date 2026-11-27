#!/bin/bash
set -euo pipefail

PANEL=${1:?usage: install-chromium.sh <panel-ip> [image]}
IMAGE=${2:-$(cd "$(dirname "$0")/.." && pwd)/out/chromium-root.erofs}
SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)

panel() {
	ssh "${SSH_OPTIONS[@]}" "root@$PANEL" "$@"
}

size=$(stat -f %z "$IMAGE" 2>/dev/null || stat -c %s "$IMAGE")
checksum=$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)
partition=$(panel 'for uevent in /sys/class/block/*/uevent; do grep -qx PARTNAME=system "$uevent" && sed -n "s/^DEVNAME=//p" "$uevent"; done; true' | head -n 1)
[ -n "$partition" ] || { echo "no partition named system on the panel" >&2; exit 1; }
capacity=$(($(panel "cat /sys/class/block/$partition/size") * 512))
[ "$size" -le "$capacity" ] || { echo "image ($size bytes) larger than $partition ($capacity bytes)" >&2; exit 1; }
panel "grep -q '^/dev/$partition ' /proc/mounts" && { echo "$partition is mounted, stop the kiosk first" >&2; exit 1; }

echo "writing $IMAGE ($((size / 1048576)) MiB) to /dev/$partition (system, $((capacity / 1048576)) MiB)"
panel "cat > /dev/$partition && sync" < "$IMAGE"
written=$(panel "head -c $size /dev/$partition | sha256sum | cut -d' ' -f1")
[ "$written" = "$checksum" ] || { echo "verification failed: $written != $checksum" >&2; exit 1; }
echo "verified sha256 $checksum"
