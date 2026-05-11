qemu-system-x86_64 \
-m 2048 \
-smp 2 \
-machine pc \
-drive file=/bharat-linux/images/bharat-disk.img,format=raw \
-kernel /bharat-linux/sources/linux-6.12.28/arch/x86/boot/bzImage \
-append "root=/dev/sda1 rw console=ttyS0 noapic nolapic" \
-nographic
