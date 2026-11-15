#!/bin/sh
# Boot Bharat-linux in QEMU.
#
#   ./run.sh              boot the disk image (build it first with ./rebuild.sh)
#   ./run.sh initramfs    boot images/initramfs.cpio.gz (build with: make initramfs)
#   ./run.sh ... text     same, but inside this terminal instead of a window
#                         (leave with: Ctrl-A then X)
#   ./run.sh ... disks=2  attach 2 extra empty disks (images/extra1.img ...,
#                         8 GB each, made the first time) to try bharat-disk
#   ./run.sh ... usb      plug in a USB stick (images/usb.img, 1 GB)
#   ./run.sh ... ram=1024 memory in MB (default 256)
#
# The kernel is the one running on this PC, because the drivers copied into
# Bharat-linux (tools/copy-modules.sh) belong to it.
# Use another with: KERNEL=/path ./run.sh
set -e

cd "$(dirname "$0")"

KERNEL="${KERNEL:-/boot/vmlinuz-$(uname -r)}"
MODE=disk
TEXT=no
DISKS=0
USB=no
RAM=256

for arg in "$@"; do
    case "$arg" in
        initramfs) MODE=initramfs ;;
        text)      TEXT=yes ;;
        disks=*)   DISKS="${arg#disks=}" ;;
        usb)       USB=yes ;;
        ram=*)     RAM="${arg#ram=}" ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

if [ ! -e "$KERNEL" ]; then
    echo "Kernel not found: $KERNEL"
    exit 1
fi

# Ubuntu only lets root read /boot/vmlinuz, so use sudo only when needed.
SUDO=""
[ -r "$KERNEL" ] || SUDO="sudo"

# KVM makes the virtual machine much faster when the PC supports it.
KVM=""
if [ -e /dev/kvm ]; then
    if [ -w /dev/kvm ] || [ -n "$SUDO" ]; then
        KVM="-enable-kvm"
    fi
fi

if [ "$TEXT" = yes ]; then
    DISPLAY_ARGS="-nographic"
    CONSOLE="console=ttyS0"
else
    DISPLAY_ARGS=""
    # vga=0x318 asks the BIOS for a 1024x768 graphics screen, which the
    # kernel then offers as /dev/fb0 (the desktop draws there)
    # loglevel=3 and the hidden cursor keep the boot screen clean;
    # fbcon=nodefer starts the graphical text console at once (Ubuntu
    # kernels start it late, which can wipe the boot screen: black screen)
    CONSOLE="vga=0x318 fbcon=nodefer loglevel=3 vt.global_cursor_default=0"
fi

# A USB tablet makes the pointer inside Bharat-linux follow your mouse,
# without QEMU "grabbing" it.
POINTER="-usb -device usb-tablet"

# extra disks (they appear as /dev/vda, /dev/vdb ...)
EXTRA=""
i=1
while [ "$i" -le "$DISKS" ]; do
    [ -f "images/extra$i.img" ] || truncate -s 8G "images/extra$i.img"
    EXTRA="$EXTRA -drive file=images/extra$i.img,format=raw,if=virtio"
    i=$((i + 1))
done

# a USB stick on a USB 3 port (it appears as /dev/sdb)
if [ "$USB" = yes ]; then
    [ -f images/usb.img ] || truncate -s 1G images/usb.img
    EXTRA="$EXTRA -device qemu-xhci,id=xhci -drive if=none,id=stick,file=images/usb.img,format=raw"
    EXTRA="$EXTRA -device usb-storage,bus=xhci.0,drive=stick,removable=on"
fi

if [ "$MODE" = initramfs ]; then
    [ -f images/initramfs.cpio.gz ] || { echo "Run 'make initramfs' first"; exit 1; }
    exec $SUDO qemu-system-x86_64 -m "$RAM" $KVM $DISPLAY_ARGS $POINTER $EXTRA \
        -kernel "$KERNEL" \
        -initrd images/initramfs.cpio.gz \
        -append "rdinit=/init quiet $CONSOLE"
fi

[ -f images/bharat.img ] || { echo "Run './rebuild.sh' first"; exit 1; }
exec $SUDO qemu-system-x86_64 -m "$RAM" $KVM $DISPLAY_ARGS $POINTER $EXTRA \
    -kernel "$KERNEL" \
    -drive file=images/bharat.img,format=raw \
    -append "root=/dev/sda rw init=/init quiet $CONSOLE"
