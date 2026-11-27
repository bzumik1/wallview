#!/bin/bash
set -euo pipefail

WALLVIEW=$(cd "$(dirname "$0")/.." && pwd)
ROOT=${ROOT:-$HOME/build/chromium-root}
OUTPUT=${OUTPUT:-$WALLVIEW/out/chromium-root.erofs}
SUITE=${SUITE:-trixie}
MIRROR=${MIRROR:-http://deb.debian.org/debian}
MKFS_EROFS=${MKFS_EROFS:-$HOME/build/br/host/bin/mkfs.erofs}
PACKAGES="chromium cage seatd libgl1-mesa-dri libegl1 libgles2 libgbm1 fonts-dejavu-core ca-certificates udev"

cleanup() {
	for mount in proc dev sys; do
		mountpoint -q "$ROOT/$mount" && sudo umount -l "$ROOT/$mount"
	done
	return 0
}
trap cleanup EXIT

sudo rm -rf "$ROOT"
sudo debootstrap --variant=minbase --include=ca-certificates "$SUITE" "$ROOT" "$MIRROR"
sudo cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
for mount in proc dev sys; do
	sudo mount --bind "/$mount" "$ROOT/$mount"
done
sudo chroot "$ROOT" env DEBIAN_FRONTEND=noninteractive apt-get update -qq
sudo chroot "$ROOT" env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $PACKAGES
sudo chroot "$ROOT" chromium --version
cleanup

sudo rm -rf "$ROOT"/usr/share/doc/* "$ROOT"/usr/share/man/* "$ROOT"/usr/share/info/* \
	"$ROOT"/var/cache/apt/archives/*.deb "$ROOT"/var/lib/apt/lists/* "$ROOT/debootstrap"
sudo find "$ROOT/usr/share/locale" -mindepth 1 -maxdepth 1 ! -name "en*" ! -name "cs*" -exec rm -rf {} +
sudo find "$ROOT/usr/lib/chromium/locales" -type f ! -name "en-US*" ! -name "cs*" -delete
sudo rm -f "$ROOT/etc/resolv.conf"
sudo ln -s /run/resolv.conf "$ROOT/etc/resolv.conf"
sudo mkdir -p "$ROOT/run" "$ROOT/data" "$ROOT/usr/share/wallview"

sudo rm -f "$OUTPUT"
sudo "$MKFS_EROFS" -zlz4hc,9 "$OUTPUT" "$ROOT" >/dev/null
sudo chown "$(id -u):$(id -g)" "$OUTPUT"
sha256sum "$OUTPUT"
echo "$OUTPUT: $(($(stat -c %s "$OUTPUT") / 1048576)) MiB"
