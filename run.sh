#!/bin/sh
# Boot Bharat-linux in QEMU.
#
#   ./run.sh              boot the disk image (build it first with ./rebuild.sh)
#   ./run.sh initramfs    boot images/initramfs.cpio.gz (build with: make initramfs)
#   ./run.sh ... text     same, but inside this terminal instead of a window
#                         (leave with: Ctrl-A then X)
#
# The kernel is the one running on this PC, because the drivers copied into
# Bharat-linux (tools/copy-modules.sh) belong to it.
# Use another with: KERNEL=/path ./run.sh
set -e

cd "$(dirname "$0")"

KERNEL="${KERNEL:-/boot/vmlinuz-$(uname -r)}"
MODE=disk
TEXT=no

for arg in "$@"; do
    case "$arg" in
        initramfs) MODE=initramfs ;;
        text)      TEXT=yes ;;
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

if [ "$MODE" = initramfs ]; then
    [ -f images/initramfs.cpio.gz ] || { echo "Run 'make initramfs' first"; exit 1; }
    exec $SUDO qemu-system-x86_64 -m 256M $KVM $DISPLAY_ARGS $POINTER \
        -kernel "$KERNEL" \
        -initrd images/initramfs.cpio.gz \
        -append "rdinit=/init quiet $CONSOLE"
fi

[ -f images/bharat.img ] || { echo "Run './rebuild.sh' first"; exit 1; }
exec $SUDO qemu-system-x86_64 -m 256M $KVM $DISPLAY_ARGS $POINTER \
    -kernel "$KERNEL" \
    -drive file=images/bharat.img,format=raw \
    -append "root=/dev/sda rw init=/init quiet $CONSOLE"
