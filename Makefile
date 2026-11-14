# Bharat-linux build
#
#   make            compile every program in src/ into rootfs/
#   make initramfs  pack rootfs/ into images/initramfs.cpio.gz (no sudo needed)
#   make image      build the ext4 disk image images/bharat.img (needs sudo)
#   make modules    copy this PC's kernel modules into rootfs/ (for networking)
#   make clean      delete everything that was built

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -static
# libcrypt gives us crypt(), used to hash passwords (login, sudo, passwd)
LDLIBS  ?= -lcrypt

# every src/NAME.c becomes rootfs/bin/NAME, except init which is rootfs/init
SOURCES  := $(wildcard src/*.c)
PROGRAMS := $(filter-out init,$(notdir $(SOURCES:.c=)))
BINARIES := $(addprefix rootfs/bin/,$(PROGRAMS))
HEADERS  := $(wildcard src/*.h src/libbharat/*.h)

# folders the system needs at boot (git cannot store empty folders)
DIRS := rootfs/bin rootfs/dev rootfs/proc rootfs/sys rootfs/run rootfs/tmp \
        rootfs/etc/tasks rootfs/var/log rootfs/var/lib/bpm rootfs/packages \
        images

KERNEL_RELEASE := $(shell uname -r)

.PHONY: all initramfs image modules clean

all: $(DIRS) rootfs/init $(BINARIES)

$(DIRS):
	mkdir -p $@

rootfs/init: src/init.c $(HEADERS) | $(DIRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

rootfs/bin/%: src/%.c $(HEADERS) | $(DIRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# Kernel modules are skipped here: the network drivers are loaded from the
# disk image instead, and leaving them out keeps this file small.
# fakeroot lets the image record "owned by root" without needing sudo.
initramfs: all
	@fakeroot sh -c './tools/set-owners.sh rootfs && cd rootfs && \
		find . -path ./lib/modules -prune -o -print \
		| cpio -o -H newc 2>/dev/null \
		| gzip -9 > ../images/initramfs.cpio.gz'
	@echo "Built images/initramfs.cpio.gz"

image: all
	./rebuild.sh

# Drivers must match the kernel that boots, and run.sh boots this PC's kernel.
modules:
	mkdir -p rootfs/lib/modules
	cp -a /lib/modules/$(KERNEL_RELEASE) rootfs/lib/modules/
	@echo "Copied modules for kernel $(KERNEL_RELEASE)"

clean:
	rm -f rootfs/init $(BINARIES) images/initramfs.cpio.gz images/bharat.img
