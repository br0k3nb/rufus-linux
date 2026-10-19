# Upstream sources

`upstream/` contains the files this port uses from the Rufus repository
(https://github.com/pbatard/rufus), unmodified:

- Rufus commit `3e696ab19010d7459c31537684522c98ba6a84a9` (Oct 15 2026),
  version 4.16.2412 (the version reported by this port comes from
  `upstream/src/rufus.rc`).
- `upstream/src`: `libcdio/`, `wimlib/` (without the `win32_*.c` backends),
  `bled/`, `syslinux/libfat` + `syslinux/libinstaller`, `ms-sys/`, `xml.c/.h`
  (ezxml), `cregex_*`, and the headers the port includes (`resource.h`,
  `rufus.rc`, `db.h`, `efi.h`, `dos.h`, `missing.h`, `hdd_vs_ufd.h`,
  `mbr_types.h`, `gpt_types.h`).
- `upstream/res`: the resources Rufus embeds in its executable (boot records,
  Syslinux/GRUB/Grub4DOS files, FreeDOS, UEFI:NTFS, UEFI-MD5Sum, DBX files,
  `setup.exe` wrappers, icon) and `loc/rufus.loc` (translations).

The few patches the Linux build needs are applied to a generated copy under
`vendor/` by `tools/prepare-vendor.sh` (documented there), never to
`upstream/`.

`wimlib-unix/unix_apply.c` and `unix_capture.c` come from wimlib v1.14.4
(https://wimlib.net, LGPL-3.0-or-later), unmodified.

To update to a newer Rufus, replace the files above with the new versions,
rebuild, and run `make test`.
