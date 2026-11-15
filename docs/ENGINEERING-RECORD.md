# Bharat Linux: engineering record

This record covers the work from the first repair of the repository
(14 November 2026) to a live ISO that installs itself onto a disk
(15 November 2026): what the problem was at each stage, how we
approached it, how it was solved and tested, and what we learned.
Each stage is one pull request (PR) on GitHub.

- Repository: https://github.com/bicky21/Bharat-Linux
- Owner: Bicky Prasad Yadav
- Built with the help of Claude (Anthropic), working through pull requests
  that were tested in QEMU before being merged

---

## 1. The goal

Bharat Linux aims to be an indigenous operating system: "Made in India,
written from scratch". The Linux kernel is used as the base; everything
on top of it (start-up, shell, commands, users, desktop, disk tools,
installer) is our own code, with a few deliberate exceptions listed in
[THIRD-PARTY.md](../THIRD-PARTY.md).

The requirements, as set during the project:

1. A full, production-grade system, not a toy: real users, real
   security, a graphical desktop, a real start-up and login.
2. Small and fast: low memory and processor use, a small download.
3. Very secure (in the spirit of Tails) and reliable.
4. Usable like other Linux systems: familiar commands, disks, USB sticks.
5. Installable from a live USB stick or DVD, like Ubuntu.

---

## 2. How the system is built (architecture)

```
 UEFI / BIOS firmware
   └─ live ISO: GRUB → Linux kernel → initramfs (whole system in memory)
   └─ installed: BOOTX64.EFI = kernel + bootstrap + command line + logo
         └─ bootstrap: loads disk drivers, waits for root=PARTUUID,
            e2fsck -p, switches to the system disk
 /init (src/init.c)          mounts /proc /sys /dev /run /tmp, fstab disks,
                             zram, starts devd + serviced, keeps a login
 devd (src/devd.c)           drivers (coldplug), device permissions,
                             USB sticks in /media/USER, eject socket
 greeter (src/greeter/)      boot animation, first-account setup, login,
                             live "Try / Install", graphical installer
 desktop (src/desktop/)      window manager + Terminal, Files, Viewer,
                             Monitor, Disks, About; draws on /dev/fb0
 sh (src/sh.c)               the shell
 coreutils (src/coreutils/)  85 everyday commands in one program
 login, sudo, passwd, useradd, userdel, usermod   accounts
 bdisk, bharat-disk, bharat-install               disks and installing
 lib*.h                      shared code: users, accounts, shutdown,
                             disks/GPT, installation
```

Key design choices:

| Choice | Why |
|--------|-----|
| Own code on top of the Linux kernel | "Indigenous" without spending years on a kernel first; the kernel is a later, long-term step |
| C, static programs, musl libc | small (whole `/bin` is about 7 MB), no library version problems, starts fast |
| Desktop draws straight to the framebuffer | no X11/Wayland/toolkit: tens of MB less, fully our own |
| Standard file formats (`/etc/passwd`, `/etc/shadow`, `/etc/group`, `/etc/fstab`, GPT) | other Linux tools and people understand them |
| Proven tools only where mistakes destroy data (e2fsprogs, dosfstools) | formatting and repairing disks needs decades of testing |
| Everything tested in QEMU before merging | the owner pulls and runs it; it must work the first time |

---

## 3. Stage by stage

### Stage 1: repair the repository (PR #1)

**Problem.** The repository did not build or boot reliably. Running
`git pull` and `make` in the wrong folder failed; `rebuild.sh` needed
`sudo` to mount an image and failed; the kernel panicked at start.
In the code: `sh -c "cmd"` opened a nested shell instead of running the
command, so every tool that used `system()` misbehaved; the shell
looped forever at end of input; variable handling could overflow
buffers; `cat` said "Permission denied" for normal files; `touch`
emptied existing files; `serviced` started another copy of every
service every 5 minutes; 464 MB of compiled files and copied kernel
modules were stored in git.

**Approach.** Read every program, reproduce each failure in QEMU, fix
the cause rather than the symptom, and add a proper build.

**Solution.**
- Shell: `sh -c`, scripts, quotes, pipes, `<`, `$?`, `cd`, `exit`,
  comments, `~/.bharatrc`, aliases, history, Ctrl-C.
- init reaps all finished processes, mounts `/run` and `/tmp` in memory,
  sets the hostname, starts `serviced`, gives login a real terminal.
- A `Makefile` builds everything; `rebuild.sh` builds the disk image
  **without sudo** (`mkfs.ext4 -d` under `fakeroot`); `run.sh` boots it.
- Build output and kernel modules removed from git.

**What we also learned.** The owner's first test still panicked because
the fixes were on a branch that had not been merged yet. From then on,
each stage ended with simple pull/build/run steps, and PRs were merged
once tested.

### Stage 2: real users (PR #2)

**Problem.** Everything ran as root; a "user" was only a `USER`
variable. Permissions were faked with `/etc/owners` and `/etc/perms`.

**Approach.** Let the Linux kernel enforce security: real user and
group IDs, real file owners, password hashes.

**Solution.** `login` checks `/etc/shadow` (salted SHA-512, root-only)
and switches to the user's uid/gid; `sudo` and `passwd` are setuid
root; `cat`, `touch`, `chown`, `chmod`, `kill` use the real system
calls; `ps` reads `/proc`; the image build sets owners with `fakeroot`.

**Tested.** As a normal user: reading `/etc/shadow`, writing `/etc` or
`/bin`, `kill 1`, `poweroff` are all refused; own files work.

### Stage 3: our own graphical desktop (PR #3, #4, #5)

**Problem.** The owner wanted a full system with a GUI and chose
"Path A": write the desktop ourselves instead of using X11/Wayland.

**Approach.** Draw directly into the framebuffer `/dev/fb0`, read the
keyboard and mouse from the kernel (evdev), and build a small window
manager and apps on top.

**Solution.** Windows (drag, resize, minimise, close), taskbar, start
menu, clock (India time), icons, shortcuts; apps: Terminal (a real
shell on a pseudo-terminal), Files, Text Viewer, System Monitor, About.
Supporting work: our own `modprobe`, a new `devd` (drivers and device
permissions for the `video` and `input` groups), `/dev/pts`, a copied
driver set of about 1 MB, a USB tablet in QEMU so the pointer follows
the mouse.

**Problem found: too heavy.** The owner asked for lower resource use.
We measured first, then changed:

| Measured in QEMU | Before | After |
|------------------|--------|-------|
| Desktop idle, 10 s | 0.33 s CPU | 0 s |
| Moving the mouse, 6 s | 2.03 s CPU | 0.09 s |
| Terminal printing text | 0.99 s CPU | 0.07 s |
| Programs in `/bin` | 51 MB | 5.4 MB |
| QEMU memory | 512 MB | 256 MB (system about 40 MB) |

How: redraw only the parts of the screen that changed ("damage
rectangles"), sleep until something happens (`poll`, `inotify`) instead
of checking on a timer, build with musl and size optimisation.

**Problems found by the owner.**
- Typing `desktop` inside the desktop's Terminal started a second
  desktop that froze the first (both grabbed the keyboard). Fix: the
  desktop refuses to start twice (environment marker plus a lock file).
- The "Type desktop" hint showed inside the desktop's Terminal. Fix:
  only on the text login.

### Stage 4: start like a real Linux system (PR #6, #7)

**Problem.** The owner did not want to type a password and `desktop`
by hand, and supplied the Bharat Linux logo. Then: the system jumped
straight into the desktop, with no login screen; the start-up should be
animated.

**Solution.** A new program, `greeter`, run by init: a boot screen with
the logo (fade-in, then three pulsing dots in saffron, white and green,
at least 3 seconds), then the logo glides up into a graphical login
screen with Restart, Power off and Text mode. Automatic login exists
but is off by default. The desktop's start menu got Log out, Restart and
Power off (exit codes 0, 10, 11 back to the greeter). The logo was
converted into our own small image format (`tools/make-logo.py`), with
the dark background made transparent so no lighter box appears around
it.

### Stage 5: accounts and security like a distribution (PR #8)

**Problem.** "Where is root by default? Everything must be production
grade, not a toy." There were built-in passwords.

**Approach.** Follow what real distributions do: no default passwords,
root locked, the first account is an administrator, sudo with the
user's own password.

**Solution.**
- Root is locked; no default user. The first start shows "Welcome!
  Create your account" (graphical and text versions).
- `sudo` only for the `wheel` group, with the user's own password,
  remembered 5 minutes, every use logged to `/var/log/auth.log`.
- `useradd`, `userdel`, `usermod`, `passwd -l/-u/-S`; passwords need
  8+ characters; the last administrator cannot be deleted.
- Files are changed atomically (write a copy, `fsync`, rename), so a
  power cut cannot leave a half-written `/etc/shadow`.
- Clean shutdown: ask programs to stop, wait up to 5 s, force, save
  data, make the disk read-only.

**Bug found in testing.** A password typed very fast right after the
prompt appeared on screen. Cause: echo was turned off *after* the
prompt was printed. Fix: turn echo off first, then print the prompt.

### Stage 6: the black screen (PR #9)

**Problem.** On the owner's laptop (QEMU with KVM, fast) the screen was
black at start; in our slower tests it worked.

**Approach.** Find what differs on a fast machine. Ubuntu kernels start
the graphical text console late ("deferred takeover", after the first
text is printed). On a fast machine the login screen was drawn before
that moment and was then wiped.

**Solution.** `fbcon=nodefer` on the kernel command line; the greeter
waits until the console has started; the boot screen repaints fully
every half second; a crashing greeter falls back to the text login
instead of a black screen (errors in `/var/log/greeter.log`);
Ctrl+Alt+F2 opens the text console.

**Lesson.** Test on fast and slow machines: timing bugs hide on one of
them.

### Stage 7: Ubuntu-like commands (PR #10)

**Problem.** "Implement all commands from Ubuntu."

**Approach.** One multi-call program (like BusyBox, but our own code):
the command name decides what runs, one 200 KB binary instead of
dozens of programs.

**Solution.** 74 commands (now 85): files (`ls -la`, `cp -r`, `find`,
`du -sh`, `chmod u+x` ...), text (`grep -rni`, `sort -nu`, `cut`, `tr`,
`diff`, `less`, `xargs` ...), system (`uname -a`, `free -h`, `lsblk`,
`dmesg`, `mount`, `killall` ...). The shell gained wildcards (`*.c`),
`~`, `;` `&&` `||`, `2>`, `2>&1`, `&>`.

**Bugs found on the way.**

| Problem | Cause | Fix |
|---------|-------|-----|
| `sudo dmesg \| less` hid the password prompt | the prompt went into the pipe | read and write the password on `/dev/tty` |
| Makefile: "unterminated call to function shell" | parentheses inside a `sed` command in the Makefile | moved into `tools/core-names.sh` |
| a C macro broke on arguments with commas | non-variadic macro | `__VA_ARGS__` |
| `sort -n` gave different orders for equal numbers | no tie-breaker | compare the whole line on ties |

### Stage 8: planning security, size and installation (no code)

The owner asked for plans before code: an easier and safer command line
(like Tails), more compact and faster, production-grade security, a
unique installer, anonymous internet (Tor), and disks "like normal
Linux". The plans were written and the order was agreed: **disks →
live USB/ISO and installer → security**. Decisions still open are
listed in section 6.

### Stage 9: disks, like on other Linux systems (PR #11)

**Problem.** "If the user wants to add a disk, he can, like normal
Linux."

**Approach.** Write our own tools for everything that only reads disks
or writes partition tables (well-defined formats), and borrow the proven
formatting/repair tools.

**Solution.**
- `libdisk.h`: lists disks from `/sys`, recognises ext2/3/4, FAT,
  exFAT, NTFS, swap and ISO 9660 by reading their headers, reads and
  writes GPT tables (with CRC32), finds parts by UUID/LABEL/PARTUUID/
  PARTLABEL without udev, mounts `/etc/fstab`.
- `bdisk`: GPT partitioning (show/new/add/delete) with an **undo**
  (the old table is saved first).
- `bharat-disk add`: guided (choose disk, name, ext4 or FAT32, plain
  summary, type `yes`); the disk opens at `/data/NAME` at every start.
- `blkid`, `lsblk -f`, `mount -a`, `mkswap`, `swapon`, `swapoff`,
  `eject`; zram compressed swap at start-up.
- USB sticks open by themselves for the person at the desktop in
  `/media/USER/NAME` with `noexec,nosuid,nodev`; safe eject through a
  socket where devd checks the caller with `SO_PEERCRED`.
- Desktop: a new **Disks** app (Open, Eject, Add a disk...), and a
  Files window opens when a stick is plugged in.
- e2fsprogs 1.47.1 and dosfstools 4.2 downloaded from the official
  sources and checked against a pinned git commit / SHA-256.

**Serious bug caught in testing.** The first test run of `bdisk new sda`
**erased the system disk** in the test machine: the "is this disk in
use?" check looked for `/dev/sda` in `/proc/mounts`, but the disk the
system started from is listed there as `/dev/root`. Fix: compare kernel
device numbers through `/proc/self/mountinfo`, which always match. The
tests now also try to erase the system disk; it is refused.

**Other problems.**

| Problem | Cause | Fix |
|---------|-------|-----|
| a FAT stick did not open | FAT needs the `nls_iso8859-1` driver; its file name has a `-`, our list had `_` | correct name; also try `iocharset=utf8` |
| normal users saw no labels in `lsblk -f` / `blkid` | only root may read disks | devd publishes `/run/blkid.cache` (like `/dev/disk/by-uuid` elsewhere) |
| the Files window did not open for a new stick | devd made the folder before the desktop started watching it | look inside the folder when the watch is added |
| the kernel could not load drivers by itself | no `modprobe` path, no alias support | `/proc/sys/kernel/modprobe`, `modules.alias`, `modprobe -q` |

### Stage 10: live USB/ISO and the installer (PR #12)

**Problem.** "Make it a live USB, and install in a VM using the ISO."

**Approach.**
- The live system is the whole system in memory (initramfs): trying it
  changes nothing, and nothing is kept (a Tails-like property for free).
- Installed systems start with UEFI using one file: a **unified kernel
  image** (kernel + a tiny start-up program + command line), as
  `EFI/BOOT/BOOTX64.EFI`, which every UEFI PC starts by itself. No boot
  loader to configure or break.
- The command line inside that file contains a placeholder
  `root=PARTUUID=0000...`; the installer replaces it with the real ID
  of the new system part (same length, so the file stays valid).

**Solution.**
- `make iso` (`tools/make-iso.sh`): hybrid BIOS+UEFI ISO with GRUB;
  a live user (`live`/`live`, sudo without password only on the live
  system); clean copies of `/etc` for installing.
- Greeter: "Welcome to Bharat Linux" → **Try** or **Install**; install
  in three steps (disk, account, check), progress bar, "Restart now".
  Text version: `sudo bharat-install`.
- `libinstall.h`: GPT with `BHARAT-EFI` (256 MB FAT32) and
  `BHARAT-SYSTEM` (ext4); copy the system; fresh `/etc` (no live user,
  no live password); the account is created *inside* the new system
  (`chroot`); fstab; the EFI file; a "Bharat Linux" entry in the PC's
  UEFI boot menu (written through efivarfs, like `efibootmgr`).
- `bootstrap`: loads SATA/NVMe/USB/SD drivers, waits for the system
  disk, runs `e2fsck -p`, switches to it, starts `/init`; on failure it
  says why in plain words and offers a shell.
- `run.sh iso`, `run.sh installed`, `run.sh iso bios`: a virtual UEFI
  PC (OVMF) with an empty 20 GB SATA disk.

**Problems and fixes.**

| Problem | Cause | Fix |
|---------|-------|-----|
| garbled picture on UEFI | GRUB's graphical menu changed the screen mode; the kernel was told a different size | graphical menu only on BIOS; on UEFI keep the firmware's mode |
| "No disk found" in the installer | the SATA driver (`ahci`) is a module and nothing loaded it | devd loads drivers for all hardware it finds (PCI/USB/virtio "modalias"), like udev |
| installed system started the UEFI shell instead | the new disk's firmware entry came after the shell | the installer adds its own first boot entry |
| text login as `live` failed | the passwd line had an extra field | use the 6-field format of our `/etc/passwd` |
| the live system's name changed to the new PC's name | `sethostname` in the `chroot` | the account step runs in its own UTS namespace |

**Tested in QEMU (q35, OVMF and SeaBIOS):** welcome → install → restart
→ firmware starts the disk → login → desktop; Try → desktop; text
installer; live user sudo.

### Stage 11: problems on the owner's laptop (PR #13–#16)

Real use on a newer Ubuntu found problems that the test machine did not
have.

| PR | Problem the owner saw | Cause | Fix |
|----|----------------------|-------|-----|
| #13 | `make`: `implicit declaration of function my_llseek`; `make iso`: `cannot stat rootfs/bin/e2fsck` | the laptop's GCC 14+ turns an old e2fsprogs mistake (a function called but not defined with musl) into an error | call `lseek()` there (64-bit path), build the borrowed tools with `-std=gnu17`, start each build clean; `make iso` stops with a clear message if the tools are missing. Reproduced with gcc-14 before fixing |
| #14 | installer: "No disk of 2 GB or more was found" | the laptop kept an old driver copy (from before `ahci` was added); drivers were copied only when the folder was missing | copy again whenever `tools/modules.list` changes |
| #15 | after installing, "Restart now" showed the welcome screen again | the virtual DVD was first in the boot order (on a real PC you remove the stick) | `run.sh iso` puts the disk first; an empty disk falls through to the ISO |
| #16 | the TianoCore logo appeared at start | it is the virtual PC's firmware logo (like the HP logo on a laptop) | the EFI file now shows the Bharat Linux logo as soon as it starts; the firmware's own first second cannot be changed by any OS |

**Lesson.** The owner's machine is the real test: newer compiler, old
files left from earlier builds, a real boot order. Build steps must not
depend on what is already in the folder.

### Stage 12: network and right-click (PR #18)

**Problem.** "Add more Linux features with network capability, right
click, etc." The network depended on BusyBox (`ip`, `udhcpc`, 2 MB) and
small wrapper programs; `ping` only tried a web connection; the desktop
had no right-click menus, no clipboard, no maximise.

**Approach.** Replace the borrowed network tools with our own: a
network service that works by itself (like NetworkManager, in one small
program) and the everyday commands. On the desktop, add one shared
right-click menu system, a clipboard and dialogs, then use them in
every app.

**Solution.**
- `netd`: switches on `lo`, finds wired cards (also USB tethering),
  speaks DHCP over packet sockets (a card without an address cannot use
  normal sockets), watches cables and new cards through netlink, renews
  leases, writes `/etc/resolv.conf` and `/run/network/state`, takes
  requests on `/run/netd.sock` (root or the person at the desktop,
  checked with `SO_PEERCRED`). `/etc/network.conf` for fixed settings.
- Commands: `ip`, `ifconfig`, a real `ping` (ICMP "ping sockets", no
  root needed), `host`, `nslookup`, `wget` (HTTP, redirects, chunked),
  `ss`/`netstat`, `network`, `hostname -I`. BusyBox removed (2 MB less).
- Desktop: network icon in the taskbar and a Network app; right-click
  menus on the desktop, icons, title bars, taskbar buttons, Files,
  Terminal and Text Viewer; a shared clipboard; dialogs (name,
  confirm, information); Files can copy, cut, paste, rename, make
  folders and files, use a Trash, show properties, open a Terminal in
  the folder, show or hide hidden files; maximise; four backgrounds.

**Problems and fixes.**

| Problem | Cause | Fix |
|---------|-------|-----|
| no `/etc/resolv.conf` after connecting | it was written before the card was marked "connected" | mark it first |
| a packet socket woke `netd` for every packet | it stayed open while connected | open it only while talking DHCP |

**Tested in QEMU:** DHCP address, route and DNS; `ip a`, `ip r`,
`ifconfig`, `ping`, `wget` from a web server, `ss`; network off/on
(refused for other users); the taskbar icon and Network app; every
right-click menu; new folder and file, copy and paste into a folder,
rename, Terminal here, maximise, background change. Not testable here:
name lookups of internet names (the test machine has no outside DNS).

### Stage 13: our own web browser (PR #19)

**Problem.** "Build our own browser inside my Linux." A full modern
browser (with JavaScript) takes large teams years; the owner chose a
**reading browser** first, **BearSSL** for https, and Tor later.

**Approach.** Split the browser in two processes:

- `bharat-fetch` (the helper): network, TLS (BearSSL), cookies, picture
  decoding (stb_image), downloads. It talks to the window through a
  pipe with simple framed messages. If a page or picture crashes it,
  only the helper dies and is started again.
- the browser window (in the desktop): our own HTML reader (forgiving
  of broken pages), our own layout (words into lines, blocks, lists,
  tables, pictures, form fields), tabs, history, bookmarks, forms.

**Solution.**
- `src/liburl.h` (addresses), `src/libhttp.h` (HTTP/1.1, redirects,
  chunked answers, cookies, TLS with certificate checks against
  `/etc/ssl/certs/ca-certificates.crt`).
- `src/net/bharat-fetch.c`, `src/net/wget.c` (now with https).
- `src/desktop/html.c`, `layout.c`, `browser.c`; three more font sizes
  (italic, two heading sizes).
- `tools/third-party.sh`: BearSSL and stb_image pinned by SHA-256.

**Problems and fixes.**

| Problem | Cause | Fix |
|---------|-------|-----|
| bearssl.org could not be reached from the build machine | network rules | use Debian's copy of the release from the Ubuntu archive, pinned by SHA-256 |
| the window did not redraw after typing or loading | the window manager leaves redrawing to windows that read a file | the browser redraws itself after keys and answers |
| a gap in the underline between the words of a link | each word is drawn on its own | join the underline (and the click area) across spaces |
| `about:` links on the start page did nothing | the address reader refused unknown schemes | the browser's own pages are allowed |
| a lost "else" in the table code | a missing pair of braces | braces (the compiler warned) |

**Tested in QEMU** with a local test site (QEMU's network reaches the
host's web server): start page, a test encyclopedia page with headings,
lists, a bordered table, code, pictures (PNG with a given size, JPEG
with its own size), hidden parts, a form (text, check box, choice) sent
with GET, a 404 page, a ZIP download to `~/Downloads`, a text file,
tabs, back, find, history, right-click menus. https was tested on the
build machine with a local server and a test certificate authority
(accepted with it, refused without it). Real internet sites could not
be reached from the test machine.

### Stage 14: Tor Browser instead of our own browser (PR #20)

**Problem.** The owner asked to remove Bharat Browser and use only Tor.
Asked to choose, he picked the **real Tor Browser** over adding Tor to
our own browser. Tor Browser is Firefox: it needs the X11 display
system, GTK and glibc, none of which Bharat Linux uses (our desktop
draws straight onto the screen and our programs are static musl).

**Approach.** Keep Bharat Linux as it is, and give Tor Browser its own
world only while it runs, like switching to another screen:

1. The desktop's **Tor Browser** icon ends the desktop with a special
   code (`EXIT_TOR`).
2. The login screen (root) starts `x11-session` for that user and waits.
3. `x11-session` starts an X server with a fixed configuration (no udev:
   `/dev/fb0` and every `/dev/input/event*`), a random secret (X
   cookie), no network listening; then our tiny window manager
   `bharat-xwm`; then Tor Browser as the user.
4. When Tor Browser closes, X stops and the desktop starts again.

Tor Browser and everything it needs is the optional "Tor pack", made by
`tools/torbrowser.sh` (`make torbrowser`): Tor Browser checked with the
Tor Project's OpenPGP key, its updater switched off by a policy, Xorg
and its drivers, and all the libraries, copied from the building PC.

Each user gets `/var/lib/tor-browser/USER`, made by root: the program as
**hard links** to `/opt/tor-browser` (no extra space; the user cannot
change it), a writable copy of `TorBrowser/Data` for their settings and
Tor's state, and a private HOME.

**Problems and fixes.**

| Problem | Cause | Fix |
|---------|-------|-----|
| torproject.org cannot be reached from the test machine | network rules | tested with a fake "Tor Browser" (a GTK program) signed by a test key; the real one is checked on the owner's PC |
| a tampered file must never be used | security | the build only accepts a VALIDSIG by the Tor Browser Developers key; tested: a changed file is refused |
| the X server stopped at once | `libz.so.1` came from `/usr/local/lib` on the build PC, which the program loader does not search | all libraries are copied into `/usr/lib/x86_64-linux-gnu` |
| GTK crashed: "Unrecognized image file format" | GTK finds picture types with the file-type database | copy the compiled part of `/usr/share/mime` |
| GTK windows stayed empty | the window manager forced a size smaller than the window's minimum, and GTK waited for its size | never smaller than the minimum |
| how to close it without title bars | no title bars on purpose | Alt+F4 in `bharat-xwm`, plus Quit / Ctrl+Q in Tor Browser |
| hard links refused for normal users | Linux protects links to other people's files | root makes the user's folder |
| Firefox needs `/dev/shm` | shared memory between its processes | `init` mounts it |
| a user could aim root at other files | symbolic-link tricks in their own folders | root works only in `/var/lib/tor-browser`, refuses links (`O_NOFOLLOW`), and never writes in the user's home |

**Tested in QEMU** (2 GB memory): the signature check (good accepted,
changed refused), start from the desktop icon, the program full screen
with a working mouse and keyboard, Alt+F4 back to the desktop, a second
start (reusing the user's folder), and the "not installed" message
without the pack. Bharat Browser, `bharat-fetch` and stb_image were
removed; `wget` with https stays.

---

## 4. How we work and test

1. Understand the request; for big topics, plan first and agree.
2. Build on a branch; reproduce problems before fixing them.
3. Test in QEMU with scripts:
   - serial console scripts (log in, run commands, expect output),
   - screen tests (type, move and click the mouse, take screenshots),
   - measurements (processor time, memory, sizes).
4. Re-run older tests (accounts, security) after big changes.
5. Pull request with what changed, how it was tested, and steps to
   run it; merge; then simple `git pull` / `make` / `./run.sh` steps
   for the owner.
6. When the owner reports a problem, find the cause first, fix it,
   and add it to the tests.

---

## 5. Where things stand (15 November 2026)

| Area | State |
|------|-------|
| Start-up | BIOS and UEFI live ISO; installed systems start by UEFI (Secure Boot off) |
| Users | standard formats, locked root, first-account setup, sudo, logging |
| Desktop | own window manager and 6 apps, low resource use |
| Commands | 85 everyday commands in one program of about 250 KB, a capable shell |
| Disks | GPT tool, guided add, fstab, USB sticks, zram, swap |
| Install | graphical and text installer, Try mode |
| Size | ISO about 45–55 MB; system uses about 40 MB of memory |

Known limits (also in the README): no disk encryption or firewall yet;
no dual boot with Windows; Secure Boot must be off; the ISO uses the
kernel of the PC that builds it; the desktop has no copy/paste and no
Indian-language text yet; tested in QEMU, VirtualBox testing in
progress.

---

## 6. Next: security

Agreed as the next stage. Planned topics: kernel and system hardening,
a firewall, disk encryption, a read-only and verified system, A/B
updates, sandboxing of apps, an amnesic ("forget everything") mode, and
anonymous internet through Tor (Tor Browser is in, stage 14).

Decisions waiting for the owner:

1. Tor at every start, or only in a "Private" mode?
2. Default protection level (suggested: Everyday)?
3. "Forget everything at power off": default for installed systems, or
   a choice in the boot menu?
4. Browser: decided, the official Tor Browser as an optional pack.
5. `rm` moving files to a Trash, or a separate `del` command?
6. First Indian languages (suggested: Hindi and Bengali)?
7. OK to use proven open-source parts for Tor and encryption?

---

## 7. Pull requests

| PR | Date | Title |
|----|------|-------|
| #1 | 14 Nov | Fix shell, init and core tools; add Makefile; clean the repo |
| #2 | 14 Nov | Real users: kernel-enforced permissions and hashed passwords |
| #3 | 14 Nov | Bharat Desktop: a graphical desktop written from scratch |
| #4 | 14 Nov | Desktop: refuse to start a second desktop |
| #5 | 14 Nov | Show the desktop hint only on the text login |
| #6 | 14 Nov | Boot like a real Linux system: boot screen, automatic login, login screen |
| #7 | 14 Nov | Login screen at boot, animated boot screen |
| #8 | 14 Nov | Accounts and security like a real distribution |
| #9 | 14 Nov | Fix black screen on fast machines (KVM) |
| #10 | 14 Nov | 74 everyday commands like Ubuntu, and a more capable shell |
| #11 | 15 Nov | Disks: partitions, formatting, fstab, USB sticks, zram |
| #12 | 15 Nov | Live USB/ISO: try it, or install it onto a disk (UEFI) |
| #13 | 15 Nov | Fix building the disk tools with GCC 14+ |
| #14 | 15 Nov | Fix: installer finds no disk (stale driver copy) |
| #15 | 15 Nov | run.sh iso: boot the installed disk after Restart |
| #16 | 15 Nov | Boot logo: Bharat Linux instead of TianoCore |
| #17 | 15 Nov | Engineering record from the beginning |
| #18 | 15 Nov | Network service and commands; right-click menus, clipboard, maximise |
| #19 | 15 Nov | Bharat Browser: our own web browser (https via BearSSL) |
| #20 | 15 Nov | Tor Browser (official, signature checked) replaces Bharat Browser |
