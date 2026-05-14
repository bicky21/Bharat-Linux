#!/bin/bash

sync

sudo qemu-system-x86_64 \
-m 512M \
-kernel /boot/vmlinuz \
-drive file=images/bharat.img,format=raw,cache=none \
-append "root=/dev/sda rw rootfstype=ext4 init=/init console=ttyS0 fsck.repair=yes quiet" \
-nographic
