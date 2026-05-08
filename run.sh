qemu-system-x86_64 \
-kernel kernel/linux-6.9/arch/x86/boot/bzImage \
-initrd initramfs.cpio.gz \
-drive file=disk/bharat.img,format=raw \
-nographic \
-append "console=ttyS0 root=/dev/sda rw noapic nolapic quiet loglevel=0"
