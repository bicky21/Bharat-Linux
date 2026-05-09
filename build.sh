#!/bin/sh

set -e

echo "[+] Building initramfs..."

cd rootfs

find . | cpio -H newc -ov | gzip > ../initramfs.cpio.gz

cd ..

echo "[+] Done"
