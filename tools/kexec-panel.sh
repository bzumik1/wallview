#!/bin/bash
set -euo pipefail

PANEL=${1:?usage: kexec-panel.sh <panel-ip>}
WALLVIEW=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$WALLVIEW/out}
DTB=${DTB:-$OUT/px30-itead-nspanel-pro.dtb}
CMDLINE=$(dd if="$OUT/recovery-wallview.img" bs=1 skip=64 count=512 2>/dev/null | tr -d '\0')
SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)

panel() {
	ssh "${SSH_OPTIONS[@]}" "root@$PANEL" "$@"
}

if ! panel 'command -v kexec >/dev/null'; then
	limactl shell wallview -- bash -c '
		files="/usr/sbin/kexec $(ldd /usr/sbin/kexec | grep -oE "/[^ ]+")"
		tar -chf - $files 2>/dev/null' | panel 'tar -xf - -C /'
fi

for file in "$OUT/Image" "$OUT/initramfs.cpio.gz"; do
	panel "cat > /tmp/kexec-$(basename "$file")" < "$file"
done

if panel 'test -e /proc/kcore'; then
	panel 'cat /sys/firmware/fdt' > "$OUT/running.dtb"
	limactl shell wallview -- bash "$WALLVIEW/tools/kexec-dtb.sh" "$OUT/running.dtb" "$DTB" "$OUT/kexec.dtb"
	panel "cat > /tmp/kexec.dtb" < "$OUT/kexec.dtb"
	panel "kexec -c -l /tmp/kexec-Image --initrd=/tmp/kexec-initramfs.cpio.gz --dtb=/tmp/kexec.dtb --command-line='$CMDLINE'"
	echo "kernel loaded on $PANEL with $(basename "$DTB")"
else
	panel "kexec -s -l /tmp/kexec-Image --initrd=/tmp/kexec-initramfs.cpio.gz --command-line='$CMDLINE'"
	echo "kernel loaded on $PANEL; running kernel has no /proc/kcore, so the current DTB is kept. Run again to load $(basename "$DTB")."
fi
panel "sync; (sleep 1; kexec -e) </dev/null >/dev/null 2>&1 &"
echo "kexec started on $PANEL"
