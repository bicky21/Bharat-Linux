# Bharat-linux

A small operating system project from India. The goal is an indigenous OS:
every part that the user touches is written here, from scratch, in C.

## What it is today

| Part | Where it comes from |
|------|---------------------|
| Kernel | The Linux kernel of your PC (`/boot/vmlinuz`) |
| `init` (first program, PID 1) | Written here: `src/init.c` |
| Login | Written here: `src/login.c` |
| Shell (`bharat-shell`) | Written here: `src/sh.c` |
| More than 50 commands (`ls`, `cat`, `ps`, `kill`, `bpm`, ...) | Written here: `src/*.c` |
| Network helpers (`ip`, `modprobe`, `udhcpc`) | BusyBox (ready-made, to be replaced) |

So right now this is **our own userland on top of the Linux kernel**.
It boots in QEMU to a login prompt.

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
Type `sudo poweroff` to shut down (it asks for the root password).

These two passwords are public defaults. Change them with `passwd` before
using the system for anything real.

Other ways to run:

```sh
./run.sh text              # boot inside the terminal (leave: Ctrl-A then X)
make initramfs             # quick test image, no disk needed
./run.sh initramfs
```

### Networking

The network drivers must match the kernel that boots. Copy them from your PC
once, then rebuild the image:

```sh
make modules
./rebuild.sh
```

Inside Bharat-linux, as root: `service modules`, then `service network`.

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
src/        all C source code
rootfs/     the files that end up on the disk (/etc, /home, /bin, ...)
images/     built disk images (not stored in git)
Makefile    build rules
rebuild.sh  builds images/bharat.img
run.sh      starts QEMU
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
- No graphical interface yet.

## Roadmap to an indigenous OS

1. **Real users** (done): `login` switches to the real uid/gid and
   passwords are hashed, so the kernel enforces permissions.
2. **Own tools instead of BusyBox**: write `ip`, `modprobe` and a DHCP client.
3. **Better shell**: line editing, arrow-key history, `if/else`, functions.
4. **Own kernel configuration**: build the Linux kernel from source with only
   the drivers that are needed (`kernel/` folder), instead of borrowing the
   PC's kernel.
5. **Boot on real hardware**: bootloader, installer, ISO image.
6. **Indian-language support**: Devanagari and other scripts on the console.
7. **Long term**: an own kernel is a multi-year project; steps 1 to 6 give a
   system that is useful and understood line by line before that.
