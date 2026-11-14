#!/bin/sh
# Compile everything and build the disk image images/bharat.img.
# No sudo needed: mkfs.ext4 copies rootfs/ straight into the image.
set -e

cd "$(dirname "$0")"

SIZE_MB="${SIZE_MB:-1024}"

make

mkdir -p images
rm -f images/bharat.img
truncate -s "${SIZE_MB}M" images/bharat.img

# -d rootfs            fill the new filesystem with the contents of rootfs/
# -E root_owner=0:0    make root the owner of "/"
mkfs.ext4 -q -F -L bharat -E root_owner=0:0 -d rootfs images/bharat.img

echo "Built images/bharat.img (${SIZE_MB} MB). Start it with ./run.sh"
