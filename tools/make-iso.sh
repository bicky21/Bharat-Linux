#!/bin/sh
# Builds images/bharat-live.iso: a live system that starts from a DVD or
# USB stick (BIOS and UEFI), lets you try Bharat Linux without changing
# the computer, and installs it onto a disk.
#
#   make iso                      (runs this)
#   KERNEL=/path/vmlinuz KERNEL_RELEASE=6.8.0-1-generic make iso
#
# Needs (Ubuntu): sudo apt install grub-pc-bin grub-efi-amd64-bin
#   grub-common xorriso mtools systemd-boot-efi systemd-ukify openssl
#
# What goes inside:
#   boot/vmlinuz      the Linux kernel (the one of this PC, like run.sh)
#   boot/initrd.gz    the whole live system, unpacked into memory at start
#                     (nothing is written to any disk while trying it)
#   boot/grub/...     the start menu, for BIOS and UEFI computers
# and, inside the live system, the files the installer puts on the disk:
#   usr/share/bharat/install/bharat.efi   kernel + bootstrap + command line
#                     in one EFI file (a "unified kernel image")
#   usr/share/bharat/install/etc/         clean settings (no live user)
set -e

cd "$(dirname "$0")/.."

REL="${KERNEL_RELEASE:-$(uname -r)}"
KERNEL="${KERNEL:-/boot/vmlinuz-$REL}"
STUB="${EFI_STUB:-/usr/lib/systemd/boot/efi/linuxx64.efi.stub}"
OUT=images/bharat-live.iso
B=build/iso
PLACEHOLDER="root=PARTUUID=00000000-0000-0000-0000-000000000000"

missing=""
for tool in grub-mkrescue xorriso mformat ukify openssl cpio fakeroot; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
[ -f "$STUB" ] || missing="$missing systemd-boot-efi(stub)"
[ -d /usr/lib/grub/x86_64-efi ] || missing="$missing grub-efi-amd64-bin"
[ -d /usr/lib/grub/i386-pc ] || missing="$missing grub-pc-bin"

if [ -n "$missing" ]; then
    echo "Missing:$missing"
    echo "Install them with:"
    echo "  sudo apt install grub-pc-bin grub-efi-amd64-bin grub-common xorriso mtools systemd-boot-efi systemd-ukify openssl"
    exit 1
fi

if [ ! -e "$KERNEL" ]; then
    echo "Kernel not found: $KERNEL"
    exit 1
fi

if [ ! -d "rootfs/lib/modules/$REL" ]; then
    echo "Copying the drivers for kernel $REL"
    KERNEL_RELEASE="$REL" ./tools/copy-modules.sh
fi

make -s all

if [ ! -x rootfs/bin/mke2fs ] || [ ! -x rootfs/bin/e2fsck ] || [ ! -x rootfs/bin/mkfs.fat ]; then
    echo "The disk formatting tools are missing (the installer needs them)."
    echo "Build them with: make third-party   (see the message above)"
    exit 1
fi

rm -rf "$B"
mkdir -p "$B/boot-initrd" "$B/live" "$B/iso/boot/grub"

# Ubuntu lets only root read the kernel file
if [ -r "$KERNEL" ]; then
    cp "$KERNEL" "$B/vmlinuz"
else
    echo "Reading the kernel needs sudo:"
    sudo cat "$KERNEL" > "$B/vmlinuz"
fi

# ---------- 1. the bootstrap initrd of the installed system ----------
echo "1/4 bootstrap for installed systems"
BI="$B/boot-initrd"
mkdir -p "$BI/bin" "$BI/etc" "$BI/proc" "$BI/sys" "$BI/dev" "$BI/newroot" "$BI/lib/modules/$REL"
cp rootfs/bin/bootstrap "$BI/init"
cp rootfs/bin/modprobe rootfs/bin/e2fsck rootfs/bin/sh rootfs/bin/coreutils rootfs/bin/reboot "$BI/bin/"
for n in ls cat mount umount mkdir dmesg lsblk blkid; do ln -s coreutils "$BI/bin/$n"; done

# drivers for the disk the system lives on (others come from the disk)
BOOT_MODULES="ahci nvme usb_storage uas xhci_pci mmc_block sdhci_pci virtio_scsi"
printf '%s\n' $BOOT_MODULES > "$BI/etc/boot-modules"

MODS="rootfs/lib/modules/$REL"
for f in modules.dep modules.alias modules.builtin modules.order; do
    [ -f "$MODS/$f" ] && cp "$MODS/$f" "$BI/lib/modules/$REL/"
done
for name in $BOOT_MODULES; do
    alt=$(echo "$name" | tr '_' '-')
    line=$(grep -E "/($name|$alt)\.ko[^:]*:" "$MODS/modules.dep" 2>/dev/null | head -n 1) || true
    for path in $(echo "$line" | tr ':' ' '); do
        [ -f "$MODS/$path" ] || continue
        mkdir -p "$BI/lib/modules/$REL/$(dirname "$path")"
        cp "$MODS/$path" "$BI/lib/modules/$REL/$path"
    done
done

(cd "$BI" && find . -print | fakeroot cpio -o -H newc 2>/dev/null | gzip -9 > ../boot-initrd.gz)

# ---------- 2. the EFI file of the installed system ----------
echo "2/4 EFI file (kernel + bootstrap + command line)"
echo "$PLACEHOLDER rw quiet loglevel=3 fbcon=nodefer vt.global_cursor_default=0" > "$B/cmdline"
ukify build --linux="$B/vmlinuz" --initrd="$B/boot-initrd.gz" --cmdline="@$B/cmdline" \
    --os-release="@rootfs/etc/os-release" --stub="$STUB" --output="$B/bharat.efi" >/dev/null

grep -q "$PLACEHOLDER" "$B/bharat.efi" || { echo "The EFI file has no command line to fill in"; exit 1; }

# ---------- 3. the live system ----------
echo "3/4 live system"
L="$B/live"
cp -a rootfs/. "$L/"

touch "$L/etc/live"
echo live > "$L/etc/sudo-nopasswd"
echo bharat-live > "$L/etc/hostname"

# what the installer puts on the disk: clean settings, the EFI file
mkdir -p "$L/usr/share/bharat/install/etc"
for f in passwd shadow group hostname fstab; do
    cp "rootfs/etc/$f" "$L/usr/share/bharat/install/etc/"
done
cp "$B/bharat.efi" "$L/usr/share/bharat/install/bharat.efi"

# the live user: password "live", administrator without a password for sudo
echo "live:x:1000:1000:/home/live:/bin/sh" >> "$L/etc/passwd"
echo "live:x:1000:" >> "$L/etc/group"
echo "live:$(openssl passwd -6 live)" >> "$L/etc/shadow"
sed -i -E 's/^(wheel|users|video|input)(:x:[0-9]+:)$/\1\2live/; s/^(wheel|users|video|input)(:x:[0-9]+:.+)$/\1\2,live/' "$L/etc/group"
mkdir -p "$L/home/live"
cp -a rootfs/etc/skel/. "$L/home/live/"

fakeroot sh -c "./tools/set-owners.sh '$L' && cd '$L' && find . -print | cpio -o -H newc 2>/dev/null | gzip -9 > ../iso/boot/initrd.gz"

# ---------- 4. the ISO ----------
echo "4/4 ISO image"
cp "$B/vmlinuz" "$B/iso/boot/vmlinuz"

cat > "$B/iso/boot/grub/grub.cfg" <<'EOF'
set timeout=5
set default=0
insmod all_video

# a graphical menu only on BIOS: on UEFI, GRUB changing the screen mode
# can leave the kernel with a wrong picture size (a garbled screen)
if [ "$grub_platform" != "efi" ]; then
    insmod gfxterm
    if loadfont unicode; then
        terminal_output gfxterm
    fi
fi
set menu_color_normal=white/black
set menu_color_highlight=black/yellow

# BIOS: ask for 1024x768. UEFI: keep the screen mode the firmware chose
# (usually the screen's own resolution; changing it can garble the picture)
if [ "$grub_platform" = "efi" ]; then
    set screen_mode=keep
else
    set screen_mode=1024x768,auto
fi

menuentry "Bharat Linux - try or install" {
    set gfxpayload=$screen_mode
    linux /boot/vmlinuz rdinit=/init quiet loglevel=3 fbcon=nodefer vt.global_cursor_default=0
    initrd /boot/initrd.gz
}

menuentry "Bharat Linux - text mode" {
    set gfxpayload=text
    linux /boot/vmlinuz rdinit=/init quiet loglevel=3 bharat.text
    initrd /boot/initrd.gz
}

menuentry "Bharat Linux - safe graphics (if the screen stays black)" {
    linux /boot/vmlinuz rdinit=/init quiet loglevel=3 nomodeset vga=0x318 fbcon=nodefer
    initrd /boot/initrd.gz
}
EOF

grub-mkrescue -o "$OUT" "$B/iso" -- -volid BHARAT_LIVE >/dev/null 2>&1

echo "Built $OUT ($(du -h "$OUT" | cut -f1))"
echo "Try it:  ./run.sh iso         (a virtual PC with an empty disk to install on)"
echo "USB:     sudo dd if=$OUT of=/dev/sdX bs=4M status=progress conv=fsync"
