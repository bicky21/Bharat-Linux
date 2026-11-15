# Bharat-linux build
#
#   make            compile every program in src/ into rootfs/
#   make initramfs  pack rootfs/ into images/initramfs.cpio.gz (no sudo needed)
#   make image      build the ext4 disk image images/bharat.img (no sudo needed)
#   make modules    copy the drivers Bharat-linux needs from this PC
#   make third-party  build the disk formatting tools (done by make)
#   make clean      delete everything that was built
#   make LIBC=glibc build with glibc even when musl is installed

# ---------- which C library ----------
#
# musl is a small C library: our programs become 20 to 30 times smaller
# (about 2 MB for all of /bin instead of 50 MB). Install it with
#   sudo apt install musl-tools
# Without it, the normal C library (glibc) is used.

MUSL_GCC := $(shell command -v musl-gcc 2>/dev/null)
LIBC     ?= $(if $(MUSL_GCC),musl,glibc)

# -Os: optimise for size.  -s: drop debug names.  gc-sections: drop
# functions that are never called.
SMALL := -Os -s -ffunction-sections -fdata-sections -Wl,--gc-sections

ifeq ($(LIBC),musl)
CC      := musl-gcc
# musl does not ship the Linux kernel headers (linux/input.h, ...);
# build/musl-include points to the ones already on this PC
CFLAGS  := $(SMALL) -Wall -Wextra -isystem build/musl-include
LDLIBS  :=
EXTRA   := build/musl-include
else
CC      := gcc
CFLAGS  := $(SMALL) -Wall -Wextra
# libcrypt gives us crypt(), used to hash passwords (login, sudo, passwd)
LDLIBS  := -lcrypt
EXTRA   :=
endif

LDFLAGS := -static
MULTIARCH := $(shell gcc -print-multiarch 2>/dev/null)

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
        rootfs/etc/tasks rootfs/var/log rootfs/var/lib/bpm rootfs/var/lib/bharat \
        rootfs/packages rootfs/media rootfs/mnt \
        images

KERNEL_RELEASE := $(shell uname -r)

.PHONY: all initramfs image modules clean third-party

all: $(DIRS) $(EXTRA) rootfs/init $(BINARIES) rootfs/bin/coreutils rootfs/bin/desktop rootfs/bin/greeter third-party
	@echo "Built with $(LIBC). Size of /bin: $$(du -sh rootfs/bin | cut -f1)"

$(DIRS):
	mkdir -p $@

build/musl-include:
	mkdir -p $@
	ln -sfn /usr/include/linux $@/linux
	ln -sfn /usr/include/asm-generic $@/asm-generic
	if [ -d /usr/include/$(MULTIARCH)/asm ]; then \
		ln -sfn /usr/include/$(MULTIARCH)/asm $@/asm; \
	else \
		ln -sfn /usr/include/asm $@/asm; \
	fi

rootfs/init: src/init.c $(HEADERS) | $(DIRS) $(EXTRA)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

rootfs/bin/%: src/%.c $(HEADERS) | $(DIRS) $(EXTRA)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# the everyday commands (ls, cp, grep, ...): one program, many names
CORE_SRC   := $(wildcard src/coreutils/*.c)
CORE_HDR   := $(wildcard src/coreutils/*.h)
CORE_NAMES := $(shell ./tools/core-names.sh)

rootfs/bin/coreutils: $(CORE_SRC) $(CORE_HDR) $(HEADERS) | $(DIRS) $(EXTRA)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CORE_SRC) $(LDLIBS)
	@cd rootfs/bin && for n in $(CORE_NAMES) "["; do ln -sfn coreutils "$$n"; done

# the boot and login screen shares the drawing and input code
GREETER_SRC := src/greeter/greeter.c src/desktop/gfx.c src/desktop/input.c

rootfs/bin/greeter: $(GREETER_SRC) $(DESKTOP_HDR) $(HEADERS) | $(DIRS) $(EXTRA)
	$(CC) $(CFLAGS) -O2 $(LDFLAGS) -o $@ $(GREETER_SRC) -lm $(LDLIBS)

# the desktop draws many pixels: optimise it for speed (-O2), not size
rootfs/bin/desktop: $(DESKTOP_SRC) $(DESKTOP_HDR) | $(DIRS) $(EXTRA)
	$(CC) $(CFLAGS) -O2 $(LDFLAGS) -o $@ $(DESKTOP_SRC) -lm

# Formatting and checking disks use proven tools (e2fsprogs, dosfstools),
# downloaded once from their official sources and checked against a fixed
# fingerprint. See THIRD-PARTY.md. Without internet the rest still builds.
third-party: | $(DIRS) $(EXTRA)
	@./tools/third-party.sh || echo "WARNING: disk formatting tools not built (no internet?). Run: make third-party"

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
	rm -f rootfs/init $(BINARIES) rootfs/bin/desktop rootfs/bin/greeter images/initramfs.cpio.gz images/bharat.img
	rm -rf build/musl-include
	cd rootfs/bin && rm -f coreutils $(CORE_NAMES) "[" mke2fs e2fsck mkfs.* fsck.*
