#!/bin/sh

set -e

echo "================================="
echo " Building Bharat-linux "
echo "================================="

echo ""
echo "[1/5] Building BusyBox..."

cd busybox
make -j$(nproc)
make install
cd ..

echo ""
echo "[2/5] Updating root filesystem..."

cp -r busybox/_install/* rootfs/

echo ""
echo "[3/5] Building initramfs..."

./scripts/build-initramfs.sh

echo ""
echo "[4/5] Building ISO..."

./scripts/build-iso.sh

echo ""
echo "[5/5] Build complete."

echo ""
echo "Generated:"
echo "  Bharat-linux.iso"
