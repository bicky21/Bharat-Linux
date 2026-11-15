# Third-party software in Bharat Linux

Bharat Linux writes its own programs, with a few deliberate exceptions:
tools where a mistake can destroy data, and where decades of testing
matter more than writing it ourselves. They are downloaded from the
official projects by `tools/third-party.sh`, checked against a fixed
fingerprint (if it does not match, nothing is built), built as small
static programs, and copied into `/bin`. Their licences are copied into
`/usr/share/licenses/`.

| Program | Version | Used for | Source | Fingerprint | Licence |
|---------|---------|----------|--------|-------------|---------|
| e2fsprogs: `mke2fs` (`mkfs.ext4`), `e2fsck` (`fsck.ext4`) | 1.47.1 | formatting and repairing ext4 | https://git.kernel.org/pub/scm/fs/ext2/e2fsprogs.git, tag v1.47.1 | git commit `950a0d69c82b585aba30118f01bf80151deffe8c` | GPL-2.0 (programs), see NOTICE |
| dosfstools: `mkfs.fat` (`mkfs.vfat`), `fsck.fat` (`fsck.vfat`) | 4.2 | formatting and repairing FAT (USB sticks, EFI) | https://github.com/dosfstools/dosfstools/releases/tag/v4.2 | SHA-256 `64926eebf90092dca21b14259a5301b7b98e7b1943e8a201c7d726084809b527` | GPL-3.0-or-later |

Also borrowed, outside `/bin`:

- The **Linux kernel** and its drivers (from the computer that builds
  Bharat Linux, see "Drivers" in the README). GPL-2.0.
- **DejaVu fonts** (`fonts/LICENSE-DejaVu.txt`).

Everything else (init, the shell, the 86 everyday commands, login, sudo,
the desktop, the disk tools `bdisk`, `bharat-disk`, `blkid`, `lsblk`,
`mkswap`, USB handling in `devd`, the network service `netd` with its
DHCP client, `ip`, `ping`, `wget`, `ss`) is written from scratch in this
repository.
