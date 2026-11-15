#!/bin/sh
# Builds the few proven tools Bharat Linux borrows instead of writing
# its own (formatting and checking disks must never lose data):
#
#   e2fsprogs 1.47.1  mke2fs (mkfs.ext4) and e2fsck (fsck.ext4)
#   dosfstools 4.2    mkfs.fat (mkfs.vfat) and fsck.fat (fsck.vfat)
#   BearSSL 0.6       the encryption (TLS) of https, for wget
#   CA certificates   the list of trusted authorities for https, copied
#                     from this PC (the ca-certificates package, Mozilla's list)
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

# BearSSL from Ubuntu's archive (Debian's copy of the 0.6 release, with
# one non-free test file removed): bearssl.org itself is often unreachable
BEARSSL_URL=http://archive.ubuntu.com/ubuntu/pool/universe/b/bearssl/bearssl_0.6+dfsg.1.orig.tar.xz
BEARSSL_SHA256=102a625bd8f6155260867a50c508df8d8c3f796985dd744df1a68e5cd1a79004

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
# -std=gnu17 and the -Wno-error flags: GCC 14 and newer (Ubuntu 24.10+)
# turn old C habits in these projects into errors; they are harmless here.
CFLAGS="-Os -ffunction-sections -fdata-sections -std=gnu17 \
 -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types \
 -Wno-error=int-conversion -Wno-error=implicit-int"
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
        # start clean, so settings from an earlier failed try are not reused
        git clean -qfdx
        git checkout -q -- .
        # e2fsprogs 1.47.1 calls my_llseek() in lib/blkid on a path that is
        # never taken on 64-bit PCs, but does not define it with musl;
        # GCC 14+ stops on that. Call plain lseek() there (64-bit anyway).
        sed -i 's/result = my_llseek(fd, offset, whence);/result = lseek(fd, (off_t) offset, whence);/' lib/blkid/llseek.c
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

# ---------- BearSSL (TLS for https) ----------
if [ ! -f "$OUT/bearssl/libbearssl.a" ]; then

    tarball="$SRC/bearssl-0.6.tar.xz"

    if [ ! -f "$tarball" ]; then
        say "downloading BearSSL 0.6"
        curl -fsSL -o "$tarball.part" "$BEARSSL_URL"
        mv "$tarball.part" "$tarball"
    fi

    if ! echo "$BEARSSL_SHA256  $tarball" | sha256sum -c - >/dev/null 2>&1; then
        say "BearSSL download does not match its fingerprint. Stopping."
        rm -f "$tarball"
        exit 1
    fi

    say "building BearSSL"
    rm -rf "$SRC/bearssl-0.6"
    tar -xJf "$tarball" -C "$SRC"
    quiet make -C "$SRC/bearssl-0.6" -j"$JOBS" CC="$CC" \
        CFLAGS="-Os -fPIC -ffunction-sections -fdata-sections -std=gnu17" lib
    mkdir -p "$OUT/bearssl"
    cp -r "$SRC/bearssl-0.6/inc" "$OUT/bearssl/"
    cp "$SRC/bearssl-0.6/build/libbearssl.a" "$OUT/bearssl/"
    cp "$SRC/bearssl-0.6/LICENSE.txt" "$OUT/LICENSE-bearssl.txt"
fi

# ---------- trusted certificate authorities ----------
mkdir -p rootfs/etc/ssl/certs
if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
    cp /etc/ssl/certs/ca-certificates.crt rootfs/etc/ssl/certs/ca-certificates.crt
else
    say "no /etc/ssl/certs/ca-certificates.crt on this PC (sudo apt install ca-certificates): https will not work"
fi

# ---------- into the system ----------
mkdir -p "$BIN" rootfs/usr/share/licenses
for p in mke2fs e2fsck mkfs.fat fsck.fat; do
    cp "$OUT/$p" "$BIN/$p"
done
cp "$OUT"/LICENSE-*.txt rootfs/usr/share/licenses/ 2>/dev/null || true

cd "$BIN"
for n in mkfs.ext4 mkfs.ext3 mkfs.ext2; do ln -sfn mke2fs "$n"; done
for n in fsck.ext4 fsck.ext3 fsck.ext2; do ln -sfn e2fsck "$n"; done
ln -sfn mkfs.fat mkfs.vfat
ln -sfn fsck.fat fsck.vfat
