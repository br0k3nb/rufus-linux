# Rufus for Linux

A Linux port of [Rufus](https://github.com/pbatard/rufus), the Reliable USB
Formatting Utility by Pete Batard: create bootable USB drives from ISO and
disk images (Windows, Linux, FreeDOS, ...), format drives, check them for bad
blocks, compute checksums and save drives to images — with a GTK3 GUI that
follows the original's layout, and a command line interface.

This is an **unofficial** port, not affiliated with or endorsed by the Rufus
project. It is distributed under the same license as Rufus, the GNU General
Public License v3 or later (see [LICENSE.txt](LICENSE.txt)).

## Layout

```
├── src/           port sources: adapted upstream code + Linux platform layer
├── include/       port headers (core definitions, Win32 compatibility shim)
├── gui/           GTK3 front-end (rufus-gui)
├── upstream/      the parts of the upstream Rufus tree that are used (see UPSTREAM.md)
│   ├── src/       libcdio, wimlib, bled, syslinux, ms-sys, ezxml, cregex, headers
│   └── res/       boot files, MBRs, FreeDOS, UEFI:NTFS image, translations...
├── wimlib-unix/   wimlib's own UNIX backends (upstream Rufus only ships Windows ones)
├── wimlib-config/, libcdio-config/   build configuration of the vendored libraries
├── tools/         build helpers (resource tables, vendor preparation)
├── tests/         root-free regression tests, QEMU boot test helper
├── data/          desktop entry
├── FEATURES.md    feature parity matrix (what is verified, what can't exist on Linux)
└── PORTING.md     engineering notes
```

## How it works

Most of Rufus is reused as is: the ISO, WIM, compressed image, Syslinux and
boot record code compile from `upstream/src`, and the Windows-specific parts
of `format.c`, `iso.c`, `hash.c`, `wue.c`, `dos.c`, `badblocks.c`, ... are
adapted copies in `src/` (marked `PORT:`). Windows facilities are replaced
by their Linux equivalents:

| Windows                                    | Linux                                              |
|--------------------------------------------|----------------------------------------------------|
| `\\.\PhysicalDriveN` + `DeviceIoControl`   | `/dev/sdX` + `BLK*` ioctls                         |
| SetupAPI device enumeration                | `/sys/block`, `/sys/bus/usb`                       |
| VDS partitioning                           | native MBR/GPT writer + `BLKRRPART`                |
| `fmifs!FormatEx` (NTFS/exFAT/UDF/FAT16)    | `mkntfs`/`mkfs.exfat`/`mkudffs`/`mkfs.fat`         |
| drive letters                              | mounting through udisks2 (or `mount(2)` as root)   |
| WinINet / CryptoAPI                        | libcurl (loaded at runtime) / OpenSSL              |
| Virtual Disk API (VHD/VHDX)                | native VHD/VHDX reader and writer                  |
| Win32 dialogs                              | GTK3 front-end + CLI                               |
| Administrator rights                       | the GUI runs `rufus --worker` through `pkexec`     |

## Building

```sh

make            # builds ./rufus (CLI)
make gui        # builds ./rufus-gui (GTK3)
make test       # root-free regression tests (image files as drives)
sudo make install PREFIX=/usr/local
```

Build dependencies: gcc/clang, make, OpenSSL (libcrypto) headers, GTK3
development files for the GUI. Everything else (libcdio, wimlib, bled,
syslinux, ms-sys, ezxml, ...) is vendored and built from `upstream/`.

Runtime: libcurl (loaded on demand, for downloads), udisks2 (mounting as a
regular user), `mkfs.fat`/`mkntfs`/`mkfs.exfat`/`mke2fs`/`mkudffs` for the
corresponding file systems, `xorriso` to save a drive as ISO, `pkexec` for
the GUI to write to devices.

## Usage

GUI: run `rufus-gui` (optionally with an image file as argument). It runs
as your user; when writing to a device, the operation runs through
`rufus --worker` with `pkexec`, so you are asked to authenticate.

CLI:

```
rufus --list [-x]                         # list drives (-x: also fixed/HDD)
rufus -i image.iso                        # analyze an image
rufus --device /dev/sdX -i image.iso      # write an image (ISO or DD mode)
rufus --device /dev/sdX --boot freedos    # FreeDOS, msdos, syslinux4/6, grub4dos, reactos, uefi-ntfs
rufus --device /dev/sdX --format -f exFAT --label DATA
rufus --device /dev/sdX --save drive.vhdx # save a drive (.img/.vhd/.vhdx/.iso)
rufus --hash -i image.iso                 # MD5/SHA-1/SHA-256 (--sha512)
```

Windows images: `--wue=tpm,account,user=NAME,...` preselects the Windows
customization options; `--persistence=4G` adds a persistent partition to
Linux live ISOs; `--zip=extra.zip` copies extra files; `--bad-blocks=4,tlc`
checks the drive first. See `rufus --help` for everything.

Testing without root: pass a regular file as `--device` (a "virtual
drive"); all code paths work on files as on devices.

## Status

See [FEATURES.md](FEATURES.md) for the per-feature status (what was
verified, and what can't exist on Linux) and [PORTING.md](PORTING.md) for
the engineering notes.

## Credits

Rufus is © 2011-2025 Pete Batard and its contributors, and includes code from
the projects listed in its source files (libcdio, wimlib, Syslinux, ms-sys,
BusyBox/bled, ezxml, cregex, ...), each under its own license, as noted in
the files under `upstream/` and `wimlib-unix/`.
