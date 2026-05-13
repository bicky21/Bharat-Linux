#!/bin/bash

sync

sudo qemu-system-x86_64 \
-m 512M \
-kernel /boot/vmlinuz \
-drive file=images/bharat.img,format=raw,cache=none \
-append "root=/dev/sda rw init=/init console=ttyS0" \
-nographic
