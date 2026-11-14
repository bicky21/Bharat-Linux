#!/bin/sh
# Sets the owner and mode of every file in rootfs/ the way the running
# system needs them. It is run inside "fakeroot" by the Makefile and
# rebuild.sh, so it needs no sudo and does not change who owns the files
# on your PC: fakeroot only remembers the owners for the image builder.
set -e

ROOT="${1:-rootfs}"

# 1. everything belongs to root
chown -R 0:0 "$ROOT"

# 2. each user owns their own home folder (read from /etc/passwd)
while IFS=: read -r name _ uid gid home _; do
    [ -n "$name" ] && [ -d "$ROOT$home" ] || continue
    chown -R "$uid:$gid" "$ROOT$home"
    chmod 700 "$ROOT$home"
done < "$ROOT/etc/passwd"

# 3. the password hashes are for root's eyes only
chmod 600 "$ROOT/etc/shadow"
chmod 644 "$ROOT/etc/passwd"

# 4. these two must run with root rights no matter who starts them
chmod 4755 "$ROOT/bin/sudo" "$ROOT/bin/passwd"

# 5. /tmp: everyone may write, but only delete their own files
chmod 1777 "$ROOT/tmp"
