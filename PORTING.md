# Porting notes

Engineering reference for the Linux port. Upstream = the upstream Rufus tree in `upstream/` (see UPSTREAM.md).

## 1. Reuse strategy

Upstream sources are consumed in three ways:

1. **Compiled unchanged from `upstream/src`** — truly portable C:
   `ms-sys/` (writes MBR/PBR/boot records as byte arrays through a tiny
   `read_sectors`/`write_sectors` file layer), `syslinux/libfat` (FAT reader /
   sector mapper), `syslinux/libinstaller` (ldlinux patching), `ext2fs/`
   (e2fsprogs subset), `libcdio/` (ISO9660+UDF), `wimlib/` (WIM handling),
   `bled/` (compression), `cregex_*`, `xml.c`, `parser.c`.
   These are compiled with `-include linux/include/win32compat.h` and
   `-Ilinux/include` so that their `#include <windows.h>` etc. resolve to the
   port's shim headers. No upstream file is modified.

2. **Adapted copies in `linux/src`** — upstream logic that needs POSIX I/O or
   different platform calls. Each copy keeps a header comment pointing at the
   upstream revision, and the ported changes are marked `// PORT:`:
   `format_fat32.c`, `format_ext.c`, `badblocks.c`, `dos.c`, `iso.c`,
   `syslinux.c`, `hash.c`.

3. **New Linux code** — replacements for Windows-only facilities:
   `devices.c` (SetupAPI+IOCTLs+VDS), `partition.c` (VDS partition create),
   `format.c` (FormatThread orchestration incl. fmifs/VDS dispatch),
   `image.c` (WriteDrive), `fatfs.c` (native FAT16/32 file writer for
   mount-less population), `boot.c` (bootloader install glue), `misc`.

## 2. The I/O shim (win32compat.h + io.c)

Windows `HANDLE`s become pointers to `struct posix_fd { int fd; uint64_t pos;
unsigned flags; }`. The shim implements the exact API subset the ported /
vendored code uses:

- `CreateFileU/A`, `ReadFile`, `WriteFile`, `SetFilePointerEx`, `GetFileSizeEx`,
  `SetEndOfFile`, `FlushFileBuffers`, `CloseHandle`, `DeleteFileU`, `MoveFileU`,
  `CreateDirectoryU`, `CopyFileU`, `GetFileAttributesU`, `fopenU` family (UTF-8
  passthrough on Linux).
- `read_sectors`/`write_sectors` with the ms-sys `FAKE_FD` convention
  (`_handle` = `HANDLE`, `_offset` = partition base). Guarantees sector writes
  are aligned and sector-sized, mirroring the Windows code paths.
- `WriteFileWithRetry` (port of `stdio.c:615`), `CreateFileWithTimeout`,
  `CreateFileAsync`/`ReadFileAsync`/`WaitFileAsync`/`GetSizeAsync` (port of
  `winio.h` semantics: async front-end over pread/pwrite + worker thread).
- Threads/events/timers on pthreads; `GetTickCount64` on `CLOCK_MONOTONIC`.
- `DeviceIoControl` is **not** emulated generically. Ported code is adapted to
  call `linux_io` helpers (`blk_get_size`, `blk_get_sector_size`,
  `blk_reload`, `blk_flush`, `blk_discard`, `blk_is_optical`, ...) instead.

`wchar_t` stays 32-bit (glibc). Upstream only touches UTF-16 as *bytes* at
disk boundaries (FAT LFN, boot sectors, BCD), and those paths already use byte
arrays or explicit conversions, so no `-fshort-wchar` games are needed. All
user-visible strings remain UTF-8 as upstream keeps them.

## 3. Device model

`RUFUS_DRIVE_INFO` mirrors upstream (`drive.h:341`): `DiskSize`, `DeviceNumber`,
`SectorSize`, `PartitionStyle`, up to 16 `Partition[]` entries with
`{Name[36], Offset, Size}`, cluster-size tables. `SelectedDrive` is filled by
`devices.c`:

- enumerate `/sys/class/block/*` skipping `loop*`, `ram*`, `dm-*`, `md*`,
  `zram*`, `sr*` (optical handled separately), partitions (`sdX1..N`);
- read `size` (512-byte units *bdev logical block size), `removable`,
  `ro`, `device/model`, `device/vendor`, USB idVendor/idProduct via
  `/sys/bus/usb/devices/*` matching the host controller path;
- classify USB vs fixed via the transport path (`/sys/block/sdX/device` symlink
  containing `/usb`), like upstream `IsUsbDevice()` does with SetupAPI;
- "drive" for `SelectedDrive` = the whole disk (`/dev/sdX`), partitions are
  offsets, exactly like `\\.\PhysicalDriveN` semantics.

Mount handling: mounted volumes of the target disk are unmounted with
`umount2(MNT_DETACH)` before any write; volumes are (re)mounted via
`mount(2)` when post-processing requires a filesystem view (NTFS/exFAT/UDF
population), and FAT population uses the in-process `fatfs.c` writer instead
(same behavior class as upstream, which writes through the OS-mounted volume).

Virtual drives: `--device /path/file.img` treats a regular file as a disk.
Partitions are offsets; `BLK*` ioctls are skipped. This enables the full test
suite without root and mirrors Rufus's VHD-backed testing.

## 4. Partition layout fidelity

`partition.c` is a direct port of `drive.c:2307` (`CreatePartition`) and the
GPT/MBR structures from `mbr_types.h`/`gpt_types.h`:

- first partition offset 1 MB (or aligned `bytes_per_track*2` for old BIOS),
- ESP = 260 MB, MSR = 128 MB, UEFI:NTFS = `PARTITION_MICROSOFT_DATA` +
  `NO_DRIVE_LETTER`, persistence partition sized bytes→sectors,
- extra partitions grow from the end, GPT stores the UEFI marker in the
  protective MBR via the same `0x49464555` + bootable-flag logic,
- after writing, `BLKRRPART` (retried, since the kernel can be busy) replaces
  `IOCTL_DISK_UPDATE_PROPERTIES`; `udevadm settle`-equivalent is a short
  retry loop on `/sys/block/$dev/$part` existence.

## 5. Native FAT writer (`fatfs.c`)

Required because:
- ISO extraction, FreeDOS deployment and syslinux install must work without
  mounting (automount interference, and root-free testing), which matches
  upstream philosophy (Rufus writes FAT32 itself);
- syslinux install needs file→sector mapping anyway (libfat).

Implements FAT12/16/32: mount (BPB parse or freshly formatted), cluster chain
alloc, directory entries with LFN (UTF-16LE + checksum), subdirectories,
timestamp fields, file data clusters, `fsync`. It is deliberately the *only*
FAT writer used so the ISO extraction results are byte-reproducible.

## 6. Filesystem creation

- FAT16/FAT32: `format_fat32.c` port (BPB, FSInfo, FATs, backup boot sector,
  1 MB alignment, 64 KB cluster "large FAT32"), identical bytes to Windows.
- ext2/3/4: `format_ext.c` port over vendored `ext2fs`.
- NTFS/exFAT/UDF: `mkfs.ntfs`/`mkfs.exfat`/`mkfs.udf` are invoked with
  equivalent parameters (these are the Linux functional equivalents of
  `fmifs!FormatEx`, which upstream itself delegates to). `CheckDisk` →
  `ntfsfix`/`fsck` equivalents where applicable.

## 7. Progress / UI abstraction

Ported modules call the same UI hooks as upstream (`PrintInfo`, `PrintStatus`,
`Notification`, `UpdateProgress`, `InitProgress`). `messages.c` provides a
console implementation; the GTK front-end will provide a windowed
implementation of the same hooks. Cancel = `ErrorStatus` set to
`ERROR_CANCELLED`, checked via `CHECK_FOR_USER_CANCEL` exactly like upstream.

## 8. Build

`linux/Makefile` builds:

- `librufuscore.a` — ported modules incl. vendored sub-libraries,
- `rufus` — CLI front-end,
- `rufus-gui` — GTK3 front-end (optional, `make gui`),
- `tests/` — root-free integration tests over image files.

Vendored libraries are compiled from upstream sources with the shim include
path first; no external dependency beyond zlib/OpenSSL (present on the system)
and GTK3 for the GUI.
