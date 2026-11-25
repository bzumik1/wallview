#!/bin/bash
set -euo pipefail

WALLVIEW=/Users/jakub/WallView
OUT=${OUT:-$WALLVIEW/out}
ROOTFS=${ROOTFS:-$HOME/build/br/images/rootfs.erofs}
BOOT_AREA=$((64 * 1048576))
SLOT_LIMIT=$((320 * 1048576))

boot_size=$(stat -c %s "$OUT/recovery-wallview.img")
rootfs_size=$(stat -c %s "$ROOTFS")
[ "$boot_size" -le "$BOOT_AREA" ] || { echo "boot image larger than 64 MiB" >&2; exit 1; }
[ $((BOOT_AREA + rootfs_size)) -le "$SLOT_LIMIT" ] || { echo "rootfs larger than 256 MiB" >&2; exit 1; }

cp "$OUT/recovery-wallview.img" "$OUT/wallview-primary.img"
truncate -s "$BOOT_AREA" "$OUT/wallview-primary.img"
cat "$ROOTFS" >> "$OUT/wallview-primary.img"
echo "$OUT/wallview-primary.img: boot $((boot_size / 1048576)) MiB + rootfs $((rootfs_size / 1048576)) MiB"
