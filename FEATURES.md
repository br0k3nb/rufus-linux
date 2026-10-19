# Linux port — feature parity matrix

Status legend: `[x]` done · `[~]` done, with a documented difference ·
`[n/a]` can't exist on Linux (reason given)

"Verified" notes refer to checks that were actually run: `make test`
(tests/run-tests.sh, root-free, image files as drives) and boot tests of the
produced drives in QEMU (tests/qemu-shot.sh, SeaBIOS and OVMF/UEFI).

## Core formatting engine

| # | Feature | Status | Notes / Linux implementation |
|---|---------|--------|------------------------------|
| 1 | Device list (model, VID/PID, size, removable flag) | [x] | `/sys/block` + `/sys/bus/usb` walk; system disk filtered |
| 2 | Exclusive device access / flush caches | [x] | `O_EXCL` open, `fsync`, `BLKRRPART` |
| 3 | Unmount all volumes on target drive | [x] | done at the start of FormatThread (`/proc/mounts`, udisks for user mounts, loop devices of image files) |
| 4 | GPT creation (GUIDs, attributes) | [x] | port of `CreatePartition()` + native GPT writer |
| 5 | MBR creation (UEFI marker, bootable flag) | [x] | native MBR writer + ms-sys MBR blobs |
| 6 | Zeroing ("ClearMBRGPT", full/fast zeroing) | [x] | port of `format.c` |
| 7 | FAT16/FAT32 format | [x] | FAT32: native port of `format_fat32.c`; FAT16: `mkfs.fat` (fmifs equivalent) |
| 8 | Large FAT32 (>32 GB) | [x] | same native writer |
| 9 | NTFS format (+compression) | [x] | `mkntfs` with the partition geometry (hidden sectors) |
| 10 | exFAT format | [x] | `mkfs.exfat` |
| 11 | UDF format | [x] | `mkudffs` (udftools, reported if missing) |
| 12 | ext2/ext3/ext4 format | [x] | `mke2fs` |
| 13 | ReFS | [n/a] | Windows-only file system |
| 14 | Cluster size selection | [x] | passed to the formatter/mkfs |
| 15 | Volume label | [x] | `ToValidLabel()` as upstream |
| 16 | Quick / full format | [x] | |
| 17 | Bad blocks check (1/2/4 passes, SLC/MLC/TLC patterns, fake drive detection) | [x] | upstream `badblocks.c` |
| 18 | Bad block log file (`rufus_*.log`) | [x] | in `$HOME`, as upstream uses `%USERPROFILE%` |
| 19 | HDD vs flash drive classification | [x] | upstream `IsHDD()` heuristic and tables; USB HDDs listed on request |

On image files (virtual drives) the external mkfs tools format a sparse
temporary file of the partition's size and geometry, which is then copied
into the image: no root or loop device is needed. Verified: FAT16, FAT32,
NTFS, exFAT, ext4.

## Boot loading

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 20 | FreeDOS | [x] | upstream `dos.c`/`dos_locale.c`; keyboard/codepage from XKB and locale. Verified boot (QEMU) |
| 21 | MS-DOS | [x] | `diskcopy.dll` from the Microsoft symbol server, as upstream. Verified boot (QEMU) |
| 22 | Syslinux 4.07 / 6.04 | [x] | vendored libinstaller/libfat; `ldlinux.c32` and version-specific files downloaded and signature-checked. Verified boot (QEMU). NTFS targets not supported (upstream maps file extents with a Windows API) |
| 23 | GRUB 2 (ISOs) | [x] | `core.img` embedding, version-matched download from rufus.ie |
| 24 | Grub4DOS | [x] | `grldr` download + MBR/SBR. Verified boot (QEMU) |
| 25 | ReactOS | [x] | ms-sys ReactOS boot records, `mboot.c32` setup |
| 26 | KolibriOS | [x] | `MTLD_F32` loader extraction |
| 27 | UEFI:NTFS partition | [x] | Verified: Windows 11 on NTFS booting through UEFI:NTFS (OVMF) |
| 28-30 | MBR/PBR analysis, UEFI marker, SBR/protective message | [x] | |

## ISO / image handling

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 31 | ISO mode (ISO9660/Joliet/Rock Ridge) | [x] | vendored libcdio, upstream `iso.c` |
| 32 | UDF ISOs | [x] | Verified with a Windows 11 ISO |
| 33 | DD mode | [x] | double-buffered asynchronous writes (upstream `winio.h` API over pthreads) |
| 34 | Compressed images (gz, xz, bz2, zstd, zip, lzma, Z, vtsi) | [x] | vendored bled. Verified byte-identical writes |
| 35 | VHD/VHDX images | [x] | native VHD (fixed/dynamic) and VHDX reader instead of the Windows Virtual Disk API. Verified byte-identical (qemu-img made images) |
| 36 | FFU images | [n/a] | needs Windows' `dism` |
| 37 | Image scan report | [x] | upstream `ImageScanThread()` logic (`image.c`) |
| 38 | install.wim > 4 GB split for FAT32 | [x] | vendored wimlib, built natively. Verified (Windows 11, booted to Setup) |
| 39 | Windows To Go | [n/a] | needs Windows' `bcdboot`/`bcdedit` to build the BCD store; reported as unsupported |
| 40 | WinPE (XP) setup | [x] | upstream `SetupWinPE()` |
| 41 | Persistence partition | [x] | casper/live; Verified (ext3 `casper-rw`, kernel option added) |
| 42 | Distro fixups | [x] | upstream `fix_config()` |
| 43 | ISO → ESP | [x] | `--esp` / selection dialog |
| 44 | EFI FAT image (`efi.img`) dump | [x] | upstream code over libfat. Verified |
| 45 | Save optical disc → ISO | [x] | raw copy of `/dev/srN` (`--save` with an optical drive) |
| 46 | Save drive → VHD/VHDX/ISO | [~] | native fixed VHD and dynamic VHDX writers (verified with qemu-img), raw `.img`; ISO via `xorriso` (ISO-9660 level 3 + Joliet/RR) instead of oscdimg's UDF |
| 47 | Zip archive of extra files | [x] | `--zip`, upstream `ExtractZip()` |
| 48 | md5sum.txt update / UEFI media validation | [x] | upstream `UpdateMD5Sum()`, `--uefi-validation` |
| 49 | Windows User Experience (unattend.xml) | [~] | upstream `wue.c`; the TPM/Secure Boot/RAM bypass always uses unattend.xml (upstream's own fallback: there is no offline registry API on Linux). Verified with Windows 11 |

## Checksums & security

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 50 | MD5/SHA-1/SHA-256/SHA-512 | [x] | upstream `hash.c` (with SHA-NI paths). Verified against coreutils |
| 51 | Known file DB (✓/✗) | [x] | |
| 52 | Signed downloads | [x] | OpenSSL check with Rufus' public key |
| 53 | UEFI bootloader revocation (DBX, SBAT, SVN, certs) | [x] | PKCS#7 parsing with OpenSSL instead of CryptoAPI; remote SBAT/cert lists; DBX updates |
| 54 | Authenticode of downloaded Rufus executables | [n/a] | the port doesn't self-update the Windows executable |

## Downloading

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 55 | HTTP(S) downloads with progress | [x] | libcurl loaded at runtime (curl(1) fallback) |
| 56 | Update check | [~] | signed version file, as upstream; reports a newer upstream release (no Windows executable download) |
| 57 | Windows ISO download (Fido) | [n/a] | Fido is a PowerShell script built for Windows |

## Hardware / info

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 58 | Conflicting processes on the drive | [x] | `/proc` scan (fds, cwd, maps), upstream warning |
| 59 | Device change detection | [x] | GUI polls the device list |
| 60 | USB port cycling | [n/a] | upstream only uses it with its experimental VDS mode |

## UX / misc

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 61 | GUI | [x] | GTK3 (`make gui`), upstream layout/texts; operations run through `rufus --worker` (pkexec for devices) |
| 62 | CLI | [x] | see `rufus --help` |
| 63 | Localization | [x] | `res/loc/rufus.loc`, all languages, GUI language menu |
| 64 | Settings | [x] | `~/.config/rufus/rufus.ini` (or `rufus.ini` next to the binary) |
| 65 | Log | [x] | GUI log window, `--log FILE` |
| 66 | Autorun.inf + icon | [x] | |
| 67 | Virtual drives for tests | [x] | `--device FILE` (CLI and GUI) |
