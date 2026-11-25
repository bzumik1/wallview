#!/bin/bash
set -euo pipefail

PANEL=${1:?usage: install-panel.sh <panel-ip> <loader|primary|golden>}
SLOT=${2:?usage: install-panel.sh <panel-ip> <loader|primary|golden>}
WALLVIEW=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=$WALLVIEW/out/recovery-wallview.img
SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)

panel() {
	ssh "${SSH_OPTIONS[@]}" "root@$PANEL" "$@"
}

checksum=$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)
panel "cat > /tmp/install.img" < "$IMAGE"
panel "wallview-boot install $SLOT /tmp/install.img $checksum && rm /tmp/install.img"
