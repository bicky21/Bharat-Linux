#!/bin/sh
# Builds the few proven tools Bharat Linux borrows instead of writing
# its own (formatting and checking disks must never lose data):
#
#   e2fsprogs 1.47.1  mke2fs (mkfs.ext4) and e2fsck (fsck.ext4)
#   dosfstools 4.2    mkfs.fat (mkfs.vfat) and fsck.fat (fsck.vfat)
#
# The source is downloaded once from the official projects and checked
# against a fixed fingerprint (a git commit or a SHA-256 sum). If it does
# not match, nothing is built. The finished programs are kept in
# build/third-party/ so this runs only once.
#
#   tools/third-party.sh          build (if needed) and copy into rootfs/bin
#   tools/third-party.sh --clean  forget the built copies

set -eu

cd "$(dirname "$0")/.."

OUT=build/third-party
SRC=build/third-party-src
BIN=rootfs/bin

E2FS_URL=https://git.kernel.org/pub/scm/fs/ext2/e2fsprogs.git
E2FS_TAG=v1.47.1
E2FS_COMMIT=950a0d69c82b585aba30118f01bf80151deffe8c

DOSFS_URL=https://github.com/dosfstools/dosfstools/releases/download/v4.2/dosfstools-4.2.tar.gz
DOSFS_SHA256=64926eebf90092dca21b14259a5301b7b98e7b1943e8a201c7d726084809b527

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$OUT" "$SRC"
    exit 0
fi

if command -v musl-gcc >/dev/null 2>&1; then
    CC="musl-gcc -isystem $(pwd)/build/musl-include"
else
    CC=gcc
fi

JOBS=$(nproc 2>/dev/null || echo 2)
CFLAGS="-Os -ffunction-sections -fdata-sections"
LDFLAGS="-static -Wl,--gc-sections -s"

mkdir -p "$OUT" "$SRC"

say() { echo "third-party: $*"; }

# compiler messages from other projects go to a log, not the screen
LOG="$(pwd)/$SRC/build.log"
quiet() { "$@" >>"$LOG" 2>&1 || { say "failed, see $LOG"; tail -n 20 "$LOG"; exit 1; }; }

# ---------- e2fsprogs ----------
if [ ! -x "$OUT/mke2fs" ] || [ ! -x "$OUT/e2fsck" ]; then

    if [ ! -d "$SRC/e2fsprogs/.git" ]; then
        say "downloading e2fsprogs $E2FS_TAG"
        rm -rf "$SRC/e2fsprogs"
        # the official server first, then its mirror on GitHub
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$E2FS_TAG" "$E2FS_URL" "$SRC/e2fsprogs" 2>/dev/null ||
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$E2FS_TAG" https://github.com/tytso/e2fsprogs.git "$SRC/e2fsprogs"
    fi

    have=$(git -C "$SRC/e2fsprogs" rev-parse HEAD)
    if [ "$have" != "$E2FS_COMMIT" ]; then
        say "e2fsprogs is not the expected version ($have). Stopping."
        exit 1
    fi

    say "building e2fsprogs (a few minutes, only once)"
    (
        cd "$SRC/e2fsprogs"
        [ -f Makefile ] || quiet ./configure CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
            --disable-nls --disable-fuse2fs --disable-defrag --disable-imager \
            --disable-debugfs --disable-e2initrd-helper --disable-uuidd \
            --disable-tdb --without-libarchive
        quiet make -j"$JOBS" libs
        quiet make -j"$JOBS" -C misc mke2fs
        quiet make -j"$JOBS" -C e2fsck e2fsck
    )
    cp "$SRC/e2fsprogs/misc/mke2fs" "$SRC/e2fsprogs/e2fsck/e2fsck" "$OUT/"
    cp "$SRC/e2fsprogs/NOTICE" "$OUT/LICENSE-e2fsprogs.txt"
fi

# ---------- dosfstools ----------
if [ ! -x "$OUT/mkfs.fat" ] || [ ! -x "$OUT/fsck.fat" ]; then

    tarball="$SRC/dosfstools-4.2.tar.gz"

    if [ ! -f "$tarball" ]; then
        say "downloading dosfstools 4.2"
        curl -fsSL -o "$tarball.part" "$DOSFS_URL"
        mv "$tarball.part" "$tarball"
    fi

    if ! echo "$DOSFS_SHA256  $tarball" | sha256sum -c - >/dev/null 2>&1; then
        say "dosfstools download does not match its fingerprint. Stopping."
        rm -f "$tarball"
        exit 1
    fi

    say "building dosfstools"
    rm -rf "$SRC/dosfstools-4.2"
    tar -xzf "$tarball" -C "$SRC"
    (
        cd "$SRC/dosfstools-4.2"
        quiet ./configure CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
            --without-udev --disable-compat-symlinks
        quiet make -j"$JOBS"
    )
    cp "$SRC/dosfstools-4.2/src/mkfs.fat" "$SRC/dosfstools-4.2/src/fsck.fat" "$OUT/"
    cp "$SRC/dosfstools-4.2/COPYING" "$OUT/LICENSE-dosfstools.txt"
fi

# ---------- into the system ----------
mkdir -p "$BIN" rootfs/usr/share/licenses
for p in mke2fs e2fsck mkfs.fat fsck.fat; do
    cp "$OUT/$p" "$BIN/$p"
done
cp "$OUT"/LICENSE-*.txt rootfs/usr/share/licenses/

cd "$BIN"
for n in mkfs.ext4 mkfs.ext3 mkfs.ext2; do ln -sfn mke2fs "$n"; done
for n in fsck.ext4 fsck.ext3 fsck.ext2; do ln -sfn e2fsck "$n"; done
ln -sfn mkfs.fat mkfs.vfat
ln -sfn fsck.fat fsck.vfat
