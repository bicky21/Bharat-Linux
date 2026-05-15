rm -f images/bharat.img
dd if=/dev/zero of=images/bharat.img bs=1M count=1024
mkfs.ext4 images/bharat.img
sudo mount -o loop images/bharat.img mnt
sudo cp -a rootfs/. mnt/
sync
sudo umount mnt
sync

