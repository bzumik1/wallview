#!/bin/bash
set -euo pipefail

PANEL=${1:?usage: kexec-panel.sh <panel-ip>}
WALLVIEW=$(cd "$(dirname "$0")/.." && pwd)
OUT=$WALLVIEW/out
DTB=$OUT/px30-itead-nspanel-pro.dtb
CMDLINE="console=ttyS2,1500000n8 earlycon=uart8250,mmio32,0xff160000 rdinit=/init loglevel=7 ignore_loglevel panic=5"
SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)

panel() {
	ssh "${SSH_OPTIONS[@]}" "root@$PANEL" "$@"
}

if ! panel 'command -v kexec >/dev/null'; then
	limactl shell wallview -- bash -c '
		files="/usr/sbin/kexec $(ldd /usr/sbin/kexec | grep -oE "/[^ ]+")"
		tar -chf - $files 2>/dev/null' | panel 'tar -xf - -C /'
fi

for file in "$OUT/Image" "$OUT/initramfs.cpio.gz" "$DTB"; do
	panel "cat > /tmp/kexec-$(basename "$file")" < "$file"
done

panel "kexec -l /tmp/kexec-Image --initrd=/tmp/kexec-initramfs.cpio.gz --dtb=/tmp/kexec-$(basename "$DTB") --command-line='$CMDLINE'"
echo "kernel loaded on $PANEL"
panel "sync; (sleep 1; kexec -e) </dev/null >/dev/null 2>&1 &"
echo "kexec started on $PANEL"
