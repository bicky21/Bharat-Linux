#!/bin/sh
# Boot Bharat-linux in QEMU.
#
#   ./run.sh              boot the disk image (build it first with ./rebuild.sh)
#   ./run.sh initramfs    boot images/initramfs.cpio.gz (build with: make initramfs)
#   ./run.sh ... text     same, but inside this terminal instead of a window
#                         (leave with: Ctrl-A then X)
#
# The kernel is the one from this PC. Use another with: KERNEL=/path ./run.sh
set -e

cd "$(dirname "$0")"

KERNEL="${KERNEL:-/boot/vmlinuz}"
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
    CONSOLE=""
fi

if [ "$MODE" = initramfs ]; then
    [ -f images/initramfs.cpio.gz ] || { echo "Run 'make initramfs' first"; exit 1; }
    exec $SUDO qemu-system-x86_64 -m 512M $KVM $DISPLAY_ARGS \
        -kernel "$KERNEL" \
        -initrd images/initramfs.cpio.gz \
        -append "rdinit=/init quiet $CONSOLE"
fi

[ -f images/bharat.img ] || { echo "Run './rebuild.sh' first"; exit 1; }
exec $SUDO qemu-system-x86_64 -m 512M $KVM $DISPLAY_ARGS \
    -kernel "$KERNEL" \
    -drive file=images/bharat.img,format=raw \
    -append "root=/dev/sda rw init=/init quiet $CONSOLE"
