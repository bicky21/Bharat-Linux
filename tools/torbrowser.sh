#!/bin/bash
# Adds Tor Browser to Bharat Linux (run by: make torbrowser).
#
# Tor Browser is the Tor Project's own program (Firefox with Tor). It is
# NOT written by us, and it needs things Bharat Linux does not use
# otherwise: the X11 display system, GTK and glibc. This script puts
# together the "Tor pack" in rootfs/:
#
#   1. Tor Browser itself, downloaded from torproject.org and checked
#      with the Tor Project's signature (OpenPGP key below), into
#      /opt/tor-browser. Its own updater is switched off: run this
#      script again to update (like Tails does).
#   2. The X server (Xorg) with the frame-buffer screen and evdev
#      keyboard/mouse drivers, xkbcomp, xauth, keyboard layouts.
#   3. Every library these need, copied from this PC (Ubuntu/Debian).
#   4. bharat-xwm, our tiny full-screen window manager.
#
# Use a file you already downloaded (its .asc signature must be next to it):
#   make torbrowser TB=~/Downloads/tor-browser-linux-x86_64-15.0.1.tar.xz
#
# Remove it all again:  make clean-torbrowser
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=rootfs
WORK=build/torbrowser

# The Tor Browser Developers signing key (from support.torproject.org,
# "How can I verify Tor Browser's signature?")
TB_KEY_FPR=EF6E286DDA85EA2A4BA7DE684E2C6E8793298290
TB_INDEX=https://aus1.torproject.org/torbrowser/update_3/release/downloads.json

say() { printf '\033[1m%s\033[0m\n' "$*"; }
die() { printf 'torbrowser: %s\n' "$*" >&2; exit 1; }

# ---------- 0. tools on this PC ----------

PKGS="xserver-xorg-core xserver-xorg-video-fbdev xserver-xorg-input-evdev xauth
x11-xkb-utils xkb-data libx11-dev libgtk-3-0 libdbus-glib-1-2 gnupg curl xz-utils"

missing=""
for f in /usr/lib/xorg/Xorg /usr/lib/xorg/modules/drivers/fbdev_drv.so \
         /usr/lib/xorg/modules/input/evdev_drv.so /usr/bin/xauth /usr/bin/xkbcomp \
         /usr/share/X11/xkb/rules/evdev /usr/include/X11/Xlib.h /usr/bin/gpg /usr/bin/curl /usr/bin/xz; do
    [ -e "$f" ] || missing="$missing $f"
done
ls /usr/lib/*/libgtk-3.so.0 >/dev/null 2>&1 || missing="$missing libgtk-3"
ls /usr/lib/*/libdbus-glib-1.so.2 >/dev/null 2>&1 || missing="$missing libdbus-glib"

if [ -n "$missing" ]; then
    echo "These are needed on this PC first:$missing"
    echo
    echo "Install them with:"
    echo "  sudo apt install $(echo $PKGS)"
    exit 1
fi

mkdir -p "$WORK"

# ---------- 1. Tor Browser: download and check the signature ----------

if [ -n "${TB:-}" ]; then
    TARBALL=$(readlink -f "$TB")
    SIG="$TARBALL.asc"
    VERSION=$(basename "$TARBALL" | sed -n 's/^tor-browser-linux-x86_64-\(.*\)\.tar\.xz$/\1/p')
    [ -f "$TARBALL" ] || die "no such file: $TB"
    [ -f "$SIG" ] || die "the signature $SIG is missing (download it from the same page)"
else
    say "Asking torproject.org for the newest Tor Browser ..."
    curl -fsSL --proto '=https' --tlsv1.2 -o "$WORK/downloads.json" "$TB_INDEX" ||
        die "cannot reach torproject.org (no internet, or it is blocked: then use TB=FILE)"

    read -r VERSION URL SIGURL < <(python3 -I - "$WORK/downloads.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
b = d["downloads"]["linux-x86_64"]["ALL"]
print(d["version"], b["binary"], b["sig"])
EOF
)
    case "$URL" in https://*.torproject.org/*) ;; *) die "unexpected download address: $URL" ;; esac

    TARBALL="$WORK/$(basename "$URL")"
    SIG="$TARBALL.asc"

    if [ ! -f "$TARBALL" ]; then
        say "Downloading Tor Browser $VERSION (about 110 MB) ..."
        curl -fL --proto '=https' --tlsv1.2 -o "$TARBALL.part" "$URL"
        mv "$TARBALL.part" "$TARBALL"
    fi
    curl -fsSL --proto '=https' --tlsv1.2 -o "$SIG" "$SIGURL"
fi

say "Checking the Tor Project's signature ..."
export GNUPGHOME="$PWD/$WORK/gnupg"
mkdir -p "$GNUPGHOME"
chmod 700 "$GNUPGHOME"

if ! gpg -q --list-keys "$TB_KEY_FPR" >/dev/null 2>&1; then
    # the Tor Project's own key server (WKD), then keys.openpgp.org
    gpg -q --auto-key-locate nodefault,wkd --locate-keys torbrowser@torproject.org >/dev/null 2>&1 ||
    { curl -fsSL "https://keys.openpgp.org/vks/v1/by-fingerprint/$TB_KEY_FPR" -o "$WORK/tor.key" &&
      gpg -q --import "$WORK/tor.key"; } ||
    die "cannot get the Tor Project's signing key"
fi

# only a good signature made by exactly this key (or one of its sub-keys) counts
STATUS=$(gpg --status-fd 1 --verify "$SIG" "$TARBALL" 2>/dev/null || true)

if ! echo "$STATUS" | grep -q "^\[GNUPG:\] VALIDSIG .* $TB_KEY_FPR\$"; then
    [ -n "${TB:-}" ] || rm -f "$TARBALL"
    die "BAD SIGNATURE: this file is not the Tor Project's Tor Browser. Not used."
fi

echo "Good signature from the Tor Browser Developers ($TB_KEY_FPR)"

say "Unpacking Tor Browser $VERSION into /opt/tor-browser ..."
rm -rf "$ROOT/opt/tor-browser" "$WORK/unpack"
mkdir -p "$WORK/unpack" "$ROOT/opt"
tar -xJf "$TARBALL" -C "$WORK/unpack"
mv "$WORK/unpack/tor-browser" "$ROOT/opt/tor-browser"
echo "$VERSION" > "$ROOT/opt/tor-browser/.version"

# updates come from this script (checked again), not from the browser itself
mkdir -p "$ROOT/opt/tor-browser/Browser/distribution"
cat > "$ROOT/opt/tor-browser/Browser/distribution/policies.json" <<'EOF'
{
  "policies": {
    "DisableAppUpdate": true
  }
}
EOF

# ---------- 2. the X server and helpers ----------

say "Copying the X11 display system from this PC ..."
copy() {   # copies a file (following links) to the same place in rootfs
    local f
    for f in "$@"; do
        [ -e "$f" ] || continue
        mkdir -p "$ROOT$(dirname "$f")"
        cp -L --preserve=mode "$f" "$ROOT$f"
    done
}

copy /usr/lib/xorg/Xorg /usr/bin/xkbcomp /usr/bin/xauth
for d in /usr/lib/xorg/modules /usr/share/X11/xkb /usr/share/X11/locale; do
    mkdir -p "$ROOT$d"
    cp -a "$d/." "$ROOT$d/"
done
mkdir -p "$ROOT/var/lib/xkb"

# GTK's pieces that are loaded by name, not linked
MA=$(gcc -print-multiarch)
copy /usr/share/glib-2.0/schemas/gschemas.compiled
# the file-type database (pictures are recognised with it): the compiled part only
for f in /usr/share/mime/*; do [ -f "$f" ] && copy "$f"; done
for d in "/usr/lib/$MA/gdk-pixbuf-2.0" /usr/share/icons/Adwaita /usr/share/icons/hicolor; do
    [ -d "$d" ] || continue
    mkdir -p "$ROOT$d"
    cp -a "$d/." "$ROOT$d/"
done

# ---------- 3. our tiny window manager ----------

mkdir -p "$ROOT/usr/lib/bharat"
gcc -O2 -Wall -Wextra -o "$ROOT/usr/lib/bharat/bharat-xwm" src/x11/bharat-xwm.c -lX11

# ---------- 4. every library they need ----------

say "Copying the libraries they need ..."
TBDIR="$ROOT/opt/tor-browser/Browser"
LIBPATH="$TBDIR:$TBDIR/TorBrowser/Tor"

# all programs and plug-ins in the pack
find "$ROOT/opt/tor-browser" "$ROOT/usr/lib/xorg" "$ROOT/usr/bin" "$ROOT/usr/lib/bharat" \
     "$ROOT/usr/lib/$MA/gdk-pixbuf-2.0" -type f \( -perm -u+x -o -name '*.so*' \) 2>/dev/null |
while read -r f; do
    head -c4 "$f" 2>/dev/null | grep -q "ELF" && echo "$f"
done > "$WORK/elf.list"

: > "$WORK/libs.list"
while read -r f; do
    # Xorg's modules find their symbols in Xorg itself: ignore "not found" lines
    LD_LIBRARY_PATH="$LIBPATH" ldd "$f" 2>/dev/null | awk '/=> \//{print $3} /^\t\/lib/{print $1}'
done < "$WORK/elf.list" | sort -u > "$WORK/libs.list"

# libraries loaded by name at run time (not visible to ldd)
for l in libnss_files.so.2 libnss_dns.so.2 libresolv.so.2 libgcc_s.so.1 \
         libX11-xcb.so.1 libXcursor.so.1 libXi.so.6 libXinerama.so.1 libXrandr.so.2 \
         libasound.so.2 libdbus-glib-1.so.2 libgtk-3.so.0 libgdk-3.so.0; do
    p=$(ls /lib/$MA/$l /usr/lib/$MA/$l 2>/dev/null | head -1) || true
    [ -n "$p" ] && echo "$p"
done >> "$WORK/libs.list"

# all go into one folder the program loader always searches, whatever
# folder they came from on this PC (some PCs also use /usr/local/lib)
LIBDIR="$ROOT/usr/lib/$MA"
mkdir -p "$LIBDIR"

add_lib() {
    local lib="$1"
    case "$lib" in "$PWD/$ROOT"/*|"$ROOT"/*) return ;; esac     # Tor Browser's own
    [ -e "$lib" ] && [ ! -e "$LIBDIR/$(basename "$lib")" ] || return 0
    cp -L "$lib" "$LIBDIR/"
}

while read -r lib; do add_lib "$lib"; done < <(sort -u "$WORK/libs.list")

# libraries of the libraries
for round in 1 2 3; do
    find "$LIBDIR" -maxdepth 1 -name '*.so*' -type f -print0 |
        xargs -0 -r ldd 2>/dev/null | awk '/=> \//{print $3}' | sort -u > "$WORK/more.list"
    while read -r lib; do add_lib "$lib"; done < "$WORK/more.list"
done

# the program loader, where every program looks for it
mkdir -p "$ROOT/lib64"
cp -L /lib64/ld-linux-x86-64.so.2 "$ROOT/lib64/"

# glibc looks up users and names in files only
printf 'passwd: files\ngroup: files\nshadow: files\nhosts: files dns\n' > "$ROOT/etc/nsswitch.conf"

# licences
mkdir -p "$ROOT/usr/share/licenses"
cp "$ROOT/opt/tor-browser/Browser/TorBrowser/Docs/Licenses/"* "$ROOT/usr/share/licenses/" 2>/dev/null || true

say "Done: Tor Browser $VERSION and its X11 pack are in rootfs/ ($(du -sh "$ROOT/opt/tor-browser" | cut -f1) + $(du -sch "$ROOT/lib/$MA" "$ROOT/usr/lib" "$ROOT/usr/share/X11" "$ROOT/usr/share/icons" 2>/dev/null | tail -1 | cut -f1) for X11)."
echo "Now:  ./rebuild.sh && ./run.sh ram=2048     or     make iso"
