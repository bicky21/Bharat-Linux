#!/bin/sh

set -e

echo "================================="
echo " Building Bharat-linux "
echo "================================="

echo ""
echo "[1/7] Building BusyBox..."

cd busybox

make -j$(nproc)

make install

cd ..

echo ""
echo "[2/7] Updating root filesystem..."

cp -r busybox/_install/* rootfs/

echo ""
echo "[3/7] Installing device nodes..."

mkdir -p rootfs/dev

if [ ! -e rootfs/dev/console ]; then
    sudo mknod -m 600 rootfs/dev/console c 5 1
fi

if [ ! -e rootfs/dev/null ]; then
    sudo mknod -m 666 rootfs/dev/null c 1 3
fi

if [ ! -e rootfs/dev/ttyS0 ]; then
    sudo mknod -m 666 rootfs/dev/ttyS0 c 4 64
fi

echo ""
echo "[4/7] Installing custom PID1..."

rm -f rootfs/sbin/init

cat > rootfs/sbin/init << 'EOF'
#!/bin/sh

exec </dev/ttyS0 >/dev/ttyS0 2>&1

export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export HOME=/root
export TERM=vt100
export PS1='Bharat-linux:# '

echo ""
echo "========================================"
echo "Bharat-linux 0.1"
echo "By Bicky Prasad Yadav"
echo "Custom Linux distribution engineered from scratch."
echo "========================================"
echo ""

echo "[DEBUG] PID: $$"

echo "[DEBUG] tty:"
tty

echo "[DEBUG] launching interactive shell..."

exec setsid /bin/sh -i
EOF

chmod +x rootfs/sbin/init

echo ""
echo "[5/7] Verifying BusyBox applets..."

busybox/busybox --list | grep setsid
busybox/busybox --list | grep sh
busybox/busybox --list | grep switch_root

echo ""
echo "[6/7] Building initramfs..."

./scripts/build-initramfs.sh

echo ""
echo "[7/7] Updating ISO..."

cp initramfs.cpio.gz iso/boot/

./scripts/build-iso.sh

echo ""
echo "================================="
echo " Build completed successfully "
echo "================================="

echo ""
echo "Generated files:"
echo "  Bharat-linux.iso"
echo "  initramfs.cpio.gz"
