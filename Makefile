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

# the graphical desktop is one program made from several files
DESKTOP_SRC := $(wildcard src/desktop/*.c)
DESKTOP_HDR := $(wildcard src/desktop/*.h)

# folders the system needs at boot (git cannot store empty folders)
DIRS := rootfs/bin rootfs/dev rootfs/proc rootfs/sys rootfs/run rootfs/tmp \
        rootfs/etc/tasks rootfs/var/log rootfs/var/lib/bpm rootfs/packages \
        images

KERNEL_RELEASE := $(shell uname -r)

.PHONY: all initramfs image modules clean

all: $(DIRS) rootfs/init $(BINARIES) rootfs/bin/desktop

$(DIRS):
	mkdir -p $@

rootfs/init: src/init.c $(HEADERS) | $(DIRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

rootfs/bin/%: src/%.c $(HEADERS) | $(DIRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

rootfs/bin/desktop: $(DESKTOP_SRC) $(DESKTOP_HDR) | $(DIRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(DESKTOP_SRC) -lm

# Kernel modules are skipped here: the network drivers are loaded from the
# disk image instead, and leaving them out keeps this file small.
# fakeroot lets the image record "owned by root" without needing sudo.
initramfs: all
	@fakeroot sh -c './tools/set-owners.sh rootfs && cd rootfs && \
		find . -print | cpio -o -H newc 2>/dev/null \
		| gzip -9 > ../images/initramfs.cpio.gz'
	@echo "Built images/initramfs.cpio.gz"

image: all
	./rebuild.sh

# Copies the few drivers Bharat-linux needs (mouse, network cards) from
# this PC. They must match the kernel that boots, and run.sh boots this
# PC's kernel. rebuild.sh runs this by itself when they are missing.
modules:
	./tools/copy-modules.sh

clean:
	rm -f rootfs/init $(BINARIES) rootfs/bin/desktop images/initramfs.cpio.gz images/bharat.img
