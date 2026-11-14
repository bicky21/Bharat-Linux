# Bharat-linux

A small operating system project from India. The goal is an indigenous OS:
every part that the user touches is written here, from scratch, in C.

## What it is today

| Part | Where it comes from |
|------|---------------------|
| Kernel | The Linux kernel of your PC (`/boot/vmlinuz-<version>`) |
| `init` (first program, PID 1) | Written here: `src/init.c` |
| Login | Written here: `src/login.c` |
| Shell (`bharat-shell`) | Written here: `src/sh.c` |
| More than 50 commands (`ls`, `cat`, `ps`, `kill`, `bpm`, ...) | Written here: `src/*.c` |
| Graphical desktop (`desktop`) | Written here: `src/desktop/` |
| `modprobe`, device helper `devd` | Written here: `src/modprobe.c`, `src/devd.c` |
| Network helpers (`ip`, `udhcpc`) | BusyBox (ready-made, to be replaced) |

So right now this is **our own userland on top of the Linux kernel**.
It boots in QEMU to a login prompt, and `desktop` starts our own graphical
desktop.

![Bharat Desktop](docs/desktop.png)

## Build and run

You need: `gcc`, `make`, static libc (`libc6-dev`), `e2fsprogs`, `cpio`,
`fakeroot` and `qemu-system-x86`. On Ubuntu:

```sh
sudo apt install build-essential e2fsprogs cpio fakeroot qemu-system-x86
```

Then:

```sh
make            # 1. compile every program in src/ into rootfs/
./rebuild.sh    # 2. build the disk image images/bharat.img
./run.sh        # 3. boot it in QEMU
```

Log in with user `bicky`, password `1234` (or `root` / `x`).
Type `desktop` to start the graphical desktop.
Type `sudo poweroff` to shut down (it asks for the root password).

These two passwords are public defaults. Change them with `passwd` before
using the system for anything real.

Other ways to run:

```sh
./run.sh text              # boot inside the terminal (leave: Ctrl-A then X)
make initramfs             # quick test image, no disk needed
./run.sh initramfs
```

### Drivers

The mouse and network drivers must match the kernel that boots, so they
are copied from your PC (only the few in `tools/modules.list`, about 1 MB).
`./rebuild.sh` does this by itself the first time. After a kernel update
on your PC, copy them again:

```sh
make modules
./rebuild.sh
```

Networking: inside Bharat-linux, as root: `service modules`, then
`service network`.

## Bharat Desktop

Our own graphical desktop, written from scratch in C (`src/desktop/`).
It draws straight into the Linux framebuffer (`/dev/fb0`) and reads the
keyboard and mouse straight from the kernel (`/dev/input/event*`).
There is no X11, no Wayland and no toolkit underneath.

- Windows you can drag, resize (bottom-right corner), minimise and close
- Taskbar with a start menu, window buttons and a clock (India time)
- **Terminal**: a real shell inside a window (colours, Ctrl-C, resizing)
- **Files**: browse folders, open text files in the **Text Viewer**
- **System Monitor**: live processor graph, memory, uptime, processes
- Smooth (anti-aliased) text from the DejaVu fonts

| Keys | Action |
|------|--------|
| Ctrl+Alt+T | New terminal |
| Alt+Tab | Switch window |
| Alt+F4 | Close the window |
| Ctrl+Alt+Backspace | Leave the desktop (back to the text console) |

How it fits together:

1. `run.sh` boots with `vga=0x318`, so the kernel gets a 1024x768
   graphics screen and offers it as `/dev/fb0`.
2. `devd` (started by `init`) loads the mouse drivers listed in
   `/etc/modules` and gives the devices in `/etc/devices` to the `video`
   and `input` groups.
3. `login` puts `bicky` in those groups (`/etc/group`), so the desktop
   runs as a normal user, not as root.

The fonts are turned into C tables by `tools/make-fonts.py`
(licence: `fonts/LICENSE-DejaVu.txt`).

## Shell quick guide

```sh
echo "hello world" > file.txt      # write to a file (>> adds to the end)
cat file.txt | cat                 # pipes
sleep 30 &                         # background job (see: jobs, ps, kill)
NAME=Bharat                        # variable
echo Hello $NAME, last status $?
if exists file.txt then echo yes
while 3 do echo again
alias ll ls                        # saved in ~/.aliases
run script.sh                      # run a script file
```

Start-up commands go in `~/.bharatrc`.

## Folder layout

```
src/        all C source code (src/desktop/ = the graphical desktop)
rootfs/     the files that end up on the disk (/etc, /home, /bin, ...)
images/     built disk images (not stored in git)
Makefile    build rules
rebuild.sh  builds images/bharat.img
run.sh      starts QEMU
tools/      helper scripts (file owners, drivers, fonts)
docs/       pictures for this README
```

Compiled programs and kernel modules are **not** stored in git any more.
`make` creates them again on your PC.

## Users and security

- `login` checks the password, then switches to the user's real uid and
  gid. From then on the Linux kernel enforces every file permission.
- Passwords are stored only as salted SHA-512 hashes in `/etc/shadow`,
  which only root can read.
- `sudo <command>` runs one command as root. `passwd` changes a password.
- `ps` and `kill` work on the kernel's real process list.
- Root owns the system files; each user owns only their home folder.

## Known limits (honest list)

- No `adduser` yet: new users are added by editing `/etc/passwd` and
  running `passwd <name>` as root.
- `sudo` asks for the root password and has no per-user rules.
- `nano` can only write a new file line by line; it cannot edit.
- `ping` is a TCP connection test, not real ICMP.
- `rootfs/advanced.sh` and `rootfs/check.sh` use syntax (`function`, `loop`,
  `if EXIST ... endif`) that the shell does not understand yet.
- The desktop has no copy and paste, no scroll-back in the Terminal, and
  no Indian-language text yet.
- The desktop needs a graphics screen: it does not work with `./run.sh text`.

## Roadmap to an indigenous OS

1. **Real users** (done): `login` switches to the real uid/gid and
   passwords are hashed, so the kernel enforces permissions.
2. **Own tools instead of BusyBox**: `modprobe` is done; `ip` and a DHCP
   client are left.
3. **Better shell**: line editing, arrow-key history, `if/else`, functions.
4. **Own kernel configuration**: build the Linux kernel from source with only
   the drivers that are needed (`kernel/` folder), instead of borrowing the
   PC's kernel.
5. **Graphical desktop** (first version done): next are a text editor,
   copy and paste, settings, and apps launched as separate programs.
6. **Boot on real hardware**: bootloader, installer, ISO image.
7. **Indian-language support**: Devanagari and other scripts in the desktop.
8. **Long term**: an own kernel is a multi-year project; steps 1 to 7 give a
   system that is useful and understood line by line before that.
