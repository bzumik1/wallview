#!/bin/bash
set -euo pipefail

PANEL=${1:?usage: install-panel.sh <panel-ip> <loader|primary|golden>}
SLOT=${2:?usage: install-panel.sh <panel-ip> <loader|primary|golden>}
WALLVIEW=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${IMAGE:-$WALLVIEW/out/recovery-wallview.img}
SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)

panel() {
	ssh "${SSH_OPTIONS[@]}" "root@$PANEL" "$@"
}

wait_for_panel() {
	for attempt in $(seq 1 120); do
		panel true 2>/dev/null && return 0
		sleep 5
	done
	echo "panel did not come back" >&2
	exit 1
}

reboot_panel() {
	panel 'sync; (sleep 1; reboot -f) </dev/null >/dev/null 2>&1 &'
	sleep 20
	wait_for_panel
}

upload_tool() {
	panel "cat > /tmp/wallview-boot" < "$WALLVIEW/initramfs/wallview-boot"
}

upload_tool
if [ "$SLOT" = primary ] && panel 'grep -q " / erofs " /proc/mounts'; then
	echo "panel runs from the primary slot, restarting into the rescue system"
	panel "sh /tmp/wallview-boot rescue"
	reboot_panel
	upload_tool
	rescued=1
fi

checksum=$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)
panel "cat > /tmp/install.img" < "$IMAGE"
panel "sh /tmp/wallview-boot install $SLOT /tmp/install.img $checksum && rm /tmp/install.img"

if [ -n "${rescued:-}" ]; then
	panel "sh /tmp/wallview-boot reset"
	echo "restarting into the new primary slot"
	reboot_panel
fi
