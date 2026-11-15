#!/bin/sh
# Compile everything and build the disk image images/bharat.img.
# No sudo needed: mkfs.ext4 copies rootfs/ straight into the image, and
# fakeroot lets it record the right owner for every file.
set -e

cd "$(dirname "$0")"

make

# 256 MB, or more when rootfs/ is big (for example with Tor Browser)
NEED_MB=$(( $(du -sm rootfs | cut -f1) * 3 / 2 + 64 ))
[ "$NEED_MB" -lt 256 ] && NEED_MB=256
SIZE_MB="${SIZE_MB:-$NEED_MB}"

# drivers (mouse, disks, USB) come from this PC; copy them again when
# tools/modules.list has changed since the last copy
MODS="rootfs/lib/modules/${KERNEL_RELEASE:-$(uname -r)}"
if ! cmp -s tools/modules.list "$MODS/.modules.list"; then
    ./tools/copy-modules.sh ||
        echo "Warning: no drivers copied, the mouse will not work in the desktop"
fi

mkdir -p images
rm -f images/bharat.img
truncate -s "${SIZE_MB}M" images/bharat.img

# set-owners.sh        root owns the system, each user owns their home
# -d rootfs            fill the new filesystem with the contents of rootfs/
# -E root_owner=0:0    make root the owner of "/"
if ! command -v fakeroot >/dev/null; then
    echo "fakeroot is missing. Install it with: sudo apt install fakeroot"
    exit 1
fi

fakeroot sh -c './tools/set-owners.sh rootfs &&
    mkfs.ext4 -q -F -L bharat -E root_owner=0:0 -d rootfs images/bharat.img'

echo "Built images/bharat.img (${SIZE_MB} MB). Start it with ./run.sh"
