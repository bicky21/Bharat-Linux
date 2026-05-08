#!/bin/sh

set -e

cp initramfs.cpio.gz iso/boot/initramfs.cpio.gz

grub-mkrescue -o Bharat-linux.iso iso
