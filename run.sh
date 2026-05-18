sudo qemu-system-x86_64 -m 512M -kernel /boot/vmlinuz -drive file=images/bharat.img,format=raw -append "root=/dev/sda rw init=/init quiet"

