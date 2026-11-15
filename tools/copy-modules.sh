#!/bin/sh
# Copies the drivers listed in tools/modules.list (and the drivers they
# need) from this PC into rootfs/lib/modules/<kernel version>/.
#
#   ./tools/copy-modules.sh                 drivers of the running kernel
#   KERNEL_RELEASE=6.8.0-1-generic ./tools/copy-modules.sh
#   MODULES_ROOT=/some/dir ./tools/copy-modules.sh   (default: /lib/modules)
set -e

cd "$(dirname "$0")/.."

REL="${KERNEL_RELEASE:-$(uname -r)}"
SRC="${MODULES_ROOT:-/lib/modules}/$REL"
DST="rootfs/lib/modules/$REL"

if [ ! -d "$SRC" ]; then
    echo "No drivers found in $SRC"
    exit 1
fi

rm -rf rootfs/lib/modules
mkdir -p "$DST"

for f in modules.dep modules.builtin modules.order; do
    [ -f "$SRC/$f" ] && cp "$SRC/$f" "$DST/"
done

copy() {
    mkdir -p "$DST/$(dirname "$1")"
    cp "$SRC/$1" "$DST/$1"
}

grep -v '^#' tools/modules.list | while read -r name; do
    [ -n "$name" ] || continue
    # module files may be written with - or _ ; try both
    alt=$(echo "$name" | tr '_-' '-_')

    if [ -f "$SRC/modules.dep" ]; then
        line=$(grep -E "/($name|$alt)\.ko[^:]*:" "$SRC/modules.dep" | head -n 1)
    else
        file=$(cd "$SRC" && find . -name "$name.ko*" -o -name "$alt.ko*" | head -n 1)
        line="${file#./}:"
    fi

    if [ -z "$line" ] || [ "$line" = ":" ]; then
        echo "  (no driver $name for this kernel, skipped)"
        continue
    fi

    # the module itself, then everything it needs
    for path in $(echo "$line" | tr ':' ' '); do
        copy "$path"
    done
    echo "  $name"
done

# other names of the copied drivers ("fs-iso9660" -> isofs), so the kernel
# can load them by itself; only the lines for drivers we have
if [ -f "$SRC/modules.alias" ]; then
    find "$DST" -name '*.ko*' | sed 's|.*/||; s|\.ko.*||; s|-|_|g' | sort -u > "$DST/.names"
    awk 'NR == FNR { have[$1] = 1; next }
         $1 == "alias" { m = $3; gsub("-", "_", m); if (m in have) print }' \
        "$DST/.names" "$SRC/modules.alias" > "$DST/modules.alias"
    rm -f "$DST/.names"
fi

echo "Copied drivers for kernel $REL"
