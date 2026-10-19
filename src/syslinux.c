/*
 * Rufus: The Reliable USB Formatting Utility
 * Syslinux installation
 * Copyright © 2003 Lars Munch Christensen
 * Copyright © 1998-2008 H. Peter Anvin
 * Copyright © 2012-2024 Pete Batard <pete@akeo.ie>
 *
 * Based on the Linux installer program for SYSLINUX by H. Peter Anvin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Rufus Linux port notes
 * ----------------------
 * - Upstream identifies the target volume with a drive letter ("D:\"), as the
 *   OS keeps the volume mounted for the whole operation. Linux has no drive
 *   letters and volumes are mounted on demand, so the port's
 *   InstallSyslinux() takes the *mount point* of the target volume instead
 *   (format.c passes the result of GetMountedPath()).
 * - ldlinux.sys is created through the mounted filesystem (the equivalent of
 *   upstream creating it through "D:") so that the FAT driver allocates the
 *   file, and the file's sector chain is then read with the *same* libfat
 *   code that upstream uses, through a raw volume handle.
 * - ldlinux.sys/ldlinux.bss come from res/syslinux (resources.c), which
 *   plays the role of upstream's embedded RCDATA resources. A copy under the
 *   files dir takes precedence, like upstream's downloaded resources do.
 * - NTFS targets are not supported yet: upstream maps the extents of
 *   ldlinux.sys through the Windows defragmentation API (ntfssect.c), which
 *   has no Linux equivalent (a native NTFS MFT parser would be required).
 */

#include "rfus.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syslinux.h"
#include "syslxfs.h"
#include "libfat.h"
#include "setadv.h"

unsigned char* syslinux_ldlinux[2] = { NULL, NULL };
unsigned long syslinux_ldlinux_len[2];
unsigned char* syslinux_mboot = NULL;
unsigned long syslinux_mboot_len;

/* Workaround for 4K support */
uint32_t SECTOR_SHIFT = 9;
uint32_t SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_SHIFT = 9;
uint32_t LIBFAT_SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_MASK = 511;

/*
 * Wrapper for ReadFile suitable for libfat
 */
int libfat_readfile(intptr_t pp, void *buf, size_t secsize, libfat_sector_t sector)
{
    LARGE_INTEGER offset;
    DWORD bytes_read;

    offset.QuadPart = (int64_t)sector * (int64_t)secsize;
    if (!SetFilePointerEx((HANDLE)pp, offset, NULL, FILE_BEGIN)) {
        uprintf("Could not set pointer to position %llu: %s", (unsigned long long)offset.QuadPart, WindowsErrorString());
        return 0;
    }

    if (!ReadFile((HANDLE)pp, buf, (DWORD)secsize, &bytes_read, NULL)) {
        uprintf("Could not read sector %llu: %s", (unsigned long long)sector, WindowsErrorString());
        return 0;
    }

    if (bytes_read != secsize) {
        uprintf("Sector %llu: Read %u bytes instead of %zu requested", (unsigned long long)sector, bytes_read, secsize);
        return 0;
    }

    return (int)secsize;
}

/*
 * Extract the ldlinux.sys and ldlinux.bss from resources,
 * then patch and install them
 */
BOOL InstallSyslinux(DWORD drive_index, const char* volume_path, int file_system)
{
    const LARGE_INTEGER liZero = { {0, 0} };
    HANDLE f_handle = INVALID_HANDLE_VALUE;
    HANDLE d_handle = INVALID_HANDLE_VALUE;
    DWORD bytes_read, res_len;
    uint64_t partition_offset;
    BOOL r = FALSE;
    FILE* fd;
    size_t length;

    static unsigned char* sectbuf = NULL;
    static char* resource[2][2] = {
        { MAKEINTRESOURCEA(IDR_SL_LDLINUX_V4_SYS), MAKEINTRESOURCEA(IDR_SL_LDLINUX_V4_BSS) },
        { MAKEINTRESOURCEA(IDR_SL_LDLINUX_V6_SYS), MAKEINTRESOURCEA(IDR_SL_LDLINUX_V6_BSS) } };
    const char* ldlinux = "ldlinux";
    const char* syslinux = "syslinux";
    const char* ldlinux_ext[3] = { "sys", "bss", "c32" };
    const char* mboot_c32 = "mboot.c32";
    char path[MAX_PATH], tmp[64], ldlinux_path[MAX_PATH];
    const char* errmsg;
    struct libfat_filesystem* lf_fs;
    libfat_sector_t s, *secp;
    libfat_sector_t* sectors = NULL;
    int ldlinux_sectors;
    uint32_t ldlinux_cluster;
    int i, w, nsectors, sl_fs_stype;
    BOOL use_v5 = (boot_type == BT_SYSLINUX_V6) || ((boot_type == BT_IMAGE) && (SL_MAJOR(img_report.sl_version) >= 5));
    /* Version directory used for the (downloaded) Syslinux files */
    const char* sl_ver_str = (boot_type == BT_IMAGE) ? img_report.sl_version_str : embedded_sl_version_str[use_v5 ? 1 : 0];
    const char* sl_ver_ext = (boot_type == BT_IMAGE) ? img_report.sl_version_ext : "";

    PrintInfoDebug(0, MSG_234, (boot_type == BT_IMAGE) ? img_report.sl_version_str : embedded_sl_version_str[use_v5 ? 1 : 0]);

    /* 4K sector size workaround */
    SECTOR_SHIFT = 0;
    SECTOR_SIZE = SelectedDrive.SectorSize;
    while (SECTOR_SIZE >>= 1)
        SECTOR_SHIFT++;
    SECTOR_SIZE = SelectedDrive.SectorSize;
    LIBFAT_SECTOR_SHIFT = SECTOR_SHIFT;
    LIBFAT_SECTOR_SIZE = SECTOR_SIZE;
    LIBFAT_SECTOR_MASK = SECTOR_SIZE - 1;

    /* sectbuf should be aligned to at least 8 bytes - see github #767 */
    sectbuf = _mm_malloc(SECTOR_SIZE, 16);
    if (sectbuf == NULL)
        goto out;

    /*
     * First, reopen the volume. Upstream keeps a lock on the volume here;
     * the port's raw handle is opened (and closed) for the occasion.
     */
    partition_offset = (SelectedDrive.nPartitions > 0) ?
        SelectedDrive.Partition[partition_index[PI_MAIN]].Offset : 0;
    d_handle = GetLogicalHandle(drive_index, partition_offset, FALSE, TRUE, TRUE);
    if ((d_handle == INVALID_HANDLE_VALUE) || (d_handle == NULL)) {
        uprintf("Could not open volume for Syslinux installation");
        goto out;
    }

    /* Make sure we can read the boot sector */
    if (!ReadFile(d_handle, sectbuf, SECTOR_SIZE, &bytes_read, NULL)) {
        uprintf("Could not read VBR");
        goto out;
    }
    if (bytes_read != SECTOR_SIZE) {
        uprintf("Could not read the whole VBR");
        goto out;
    }
    if ((errmsg = syslinux_check_bootsect(sectbuf, &sl_fs_stype))) {
        uprintf("Error: %s", errmsg);
        goto out;
    }

    /* Initialize the ADV -- this should be smarter */
    syslinux_reset_adv(syslinux_adv);

    /* Access a copy of the ldlinux.sys & ldlinux.bss resources (downloaded or embedded) */
    IGNORE_RETVAL(_chdirU(app_data_dir));
    static_sprintf(path, "%s/%s-%s%s/%s.%s", FILES_DIR, syslinux, sl_ver_str, sl_ver_ext, ldlinux, ldlinux_ext[0]);
    fd = (sl_ver_str[0] == 0) ? NULL : fopen(path, "rb");
    if (fd != NULL) {
        fclose(fd);
        for (i = 0; i < 2; i++) {
            static_sprintf(path, "%s/%s-%s%s/%s.%s", FILES_DIR, syslinux, sl_ver_str, sl_ver_ext, ldlinux, ldlinux_ext[i]);
            fd = fopen(path, "rb");
            if (fd == NULL) {
                uprintf("Could not open %s/%s", app_data_dir, path);
                goto out;
            }
            fseek(fd, 0, SEEK_END);
            length = (size_t)ftell(fd);
            fseek(fd, 0, SEEK_SET);
            syslinux_ldlinux_len[i] = (unsigned long)length;
            syslinux_ldlinux[i] = (unsigned char*)malloc(length);
            if (syslinux_ldlinux[i] == NULL) {
                fclose(fd);
                goto out;
            }
            length = fread(syslinux_ldlinux[i], 1, length, fd);
            fclose(fd);
            if (length != (size_t)syslinux_ldlinux_len[i]) {
                uprintf("Could not read %s/%s", app_data_dir, path);
                goto out;
            }
            uprintf("Using existing '%s/%s' %s", app_data_dir, path,
                IsBufferInDB(syslinux_ldlinux[i], (size_t)syslinux_ldlinux_len[i]) ? "✓" : "✗");
        }
    } else {
        for (i = 0; i < 2; i++) {
            static_sprintf(tmp, "%s.%s", ldlinux, ldlinux_ext[i]);
            /* PORT: DWORD is 32-bit, unsigned long is 64-bit on LP64 */
            res_len = 0;
            syslinux_ldlinux[i] = GetResource(hMainInstance, resource[use_v5 ? 1 : 0][i],
                _RT_RCDATA, tmp, &res_len, TRUE);
            syslinux_ldlinux_len[i] = res_len;
            if (syslinux_ldlinux[i] == NULL)
                goto out;
        }
    }

    /* Create ldlinux.sys file (in the root of the mounted volume) */
    static_sprintf(ldlinux_path, "%s/%s.%s", volume_path, ldlinux, ldlinux_ext[0]);
    f_handle = CreateFileU(ldlinux_path, GENERIC_READ | GENERIC_WRITE,
              FILE_SHARE_READ | FILE_SHARE_WRITE,
              NULL, CREATE_ALWAYS,
              FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM |
              FILE_ATTRIBUTE_HIDDEN, NULL);

    if (f_handle == INVALID_HANDLE_VALUE) {
        uprintf("Unable to create '%s': %s", ldlinux_path, WindowsErrorString());
        goto out;
    }

    /* Write ldlinux.sys file */
    if (!WriteFileWithRetry(f_handle, (const char _force *)syslinux_ldlinux[0],
           syslinux_ldlinux_len[0], NULL, WRITE_RETRIES)) {
        uprintf("Could not write '%s': %s", ldlinux_path, WindowsErrorString());
        goto out;
    }
    if (!WriteFileWithRetry(f_handle, syslinux_adv, 2 * ADV_SIZE, NULL, WRITE_RETRIES)) {
        uprintf("Could not write ADV to '%s': %s", ldlinux_path, WindowsErrorString());
        goto out;
    }

    uprintf("Successfully wrote '%s'", ldlinux_path);
    if (boot_type != BT_IMAGE)
        UpdateProgress(OP_FILE_COPY, -1.0f);

    /* Now flush the media */
    if (!FlushFileBuffers(f_handle)) {
        uprintf("FlushFileBuffers failed");
        goto out;
    }

    /* Map the file (is there a better way to do this?) */
    ldlinux_sectors = (int)((syslinux_ldlinux_len[0] + 2 * ADV_SIZE + SECTOR_SIZE - 1) >> SECTOR_SHIFT);
    sectors = (libfat_sector_t*)calloc(ldlinux_sectors, sizeof *sectors);
    if (sectors == NULL)
        goto out;

    switch (file_system) {
    case FS_NTFS:
        /* Upstream uses the Windows defragmentation API (ntfssect.c) to map
         * the clusters of ldlinux.sys on NTFS volumes */
        uprintf("Syslinux installation on NTFS volumes is not supported by the Linux port yet");
        goto out;
    case FS_FAT16:
    case FS_FAT32:
    case FS_EXFAT:
        lf_fs = libfat_open(libfat_readfile, (intptr_t)d_handle);
        if (lf_fs == NULL) {
            uprintf("Syslinux FAT access error");
            goto out;
        }
        ldlinux_cluster = libfat_searchdir(lf_fs, 0, "LDLINUX SYS", NULL);
        secp = sectors;
        nsectors = 0;
        s = libfat_clustertosector(lf_fs, ldlinux_cluster);
        while (s && nsectors < ldlinux_sectors) {
            *secp++ = s;
            nsectors++;
            s = libfat_nextsector(lf_fs, s);
        }
        libfat_close(lf_fs);
        break;
    default:
        uprintf("Unsupported Syslinux filesystem");
        goto out;
    }

    /* Set the base directory and patch ldlinux.sys and the boot sector */
    for (i = (int)strlen(img_report.cfg_path); (i > 0) && (img_report.cfg_path[i] != '/'); i--);
    if (i > 0)
        img_report.cfg_path[i] = 0;
    w = syslinux_patch(sectors, nsectors, 0, 0, img_report.cfg_path, NULL);
    if (i > 0)
        img_report.cfg_path[i] = '/';
    if (w < 0) {
        uprintf("WARNING: Could not patch Syslinux files.");
        uprintf("This could be caused by your firewall having modified downloaded content, such as 'ldlinux.sys'...");
        goto out;
    }

    /* Rewrite the file */
    if (!SetFilePointerEx(f_handle, liZero, NULL, FILE_BEGIN) ||
        !WriteFileWithRetry(f_handle, syslinux_ldlinux[0], syslinux_ldlinux_len[0], NULL, WRITE_RETRIES)) {
        uprintf("Could not rewrite '%s': %s", ldlinux_path, WindowsErrorString());
        goto out;
    }

    /* Close file */
    safe_closehandle(f_handle);

    if (boot_type == BT_SYSLINUX_V6) {
        IGNORE_RETVAL(_chdirU(app_data_dir));
        static_sprintf(path, "%s/%s-%s", FILES_DIR, syslinux, embedded_sl_version_str[1]);
        IGNORE_RETVAL(_chdirU(path));
        static_sprintf(tmp, "%s.%s", ldlinux, ldlinux_ext[2]);
        static_sprintf(ldlinux_path, "%s/%s.%s", volume_path, ldlinux, ldlinux_ext[2]);
        fd = fopen(tmp, "rb");
        if (fd == NULL) {
            uprintf("Caution: No '%s' was provided. The target will be missing a required Syslinux file!", tmp);
        } else {
            fclose(fd);
            if (CopyFileU(tmp, ldlinux_path, TRUE)) {
                uprintf("Created '%s' (from '%s/%s/%s-%s/%s') %s", ldlinux_path, app_data_dir, FILES_DIR, syslinux,
                    embedded_sl_version_str[1], tmp, IsFileInDB(ldlinux_path) ? "✓" : "✗");
            } else {
                uprintf("Failed to create '%s': %s", ldlinux_path, WindowsErrorString());
            }
        }
        IGNORE_RETVAL(_chdirU(app_data_dir));
    } else if (HAS_REACTOS(img_report)) {
        uprintf("Setting up ReactOS...");
        res_len = 0;
        syslinux_mboot = GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_SL_MBOOT_C32),
            _RT_RCDATA, "mboot.c32", &res_len, FALSE);
        syslinux_mboot_len = res_len;
        if (syslinux_mboot == NULL) {
            goto out;
        }
        /* Create mboot.c32 file */
        static_sprintf(ldlinux_path, "%s/%s", volume_path, mboot_c32);
        f_handle = CreateFileU(ldlinux_path, GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                  NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f_handle == INVALID_HANDLE_VALUE) {
            uprintf("Unable to create '%s'", ldlinux_path);
            goto out;
        }
        if (!WriteFileWithRetry(f_handle, syslinux_mboot, syslinux_mboot_len, NULL, WRITE_RETRIES)) {
            uprintf("Could not write '%s'", ldlinux_path);
            goto out;
        }
        safe_closehandle(f_handle);
        static_sprintf(ldlinux_path, "%s/syslinux.cfg", volume_path);
        fd = fopen(ldlinux_path, "w");
        if (fd == NULL) {
            uprintf("Could not create ReactOS 'syslinux.cfg'");
            goto out;
        }
        /* Write the syslinux.cfg for ReactOS */
        fprintf(fd, "DEFAULT ReactOS\nLABEL ReactOS\n  KERNEL %s\n  APPEND %s",
            mboot_c32, img_report.reactos_path);
        fclose(fd);
    }

    /*
     * PORT: The boot sector must be written with the volume unmounted. The
     * Linux FAT driver keeps the boot sector in its own cache (to toggle the
     * 'dirty' flag) and writes it back on unmount, which would overwrite the
     * Syslinux boot record that we write through the raw volume handle.
     */
    if (!AltUnmountVolume(volume_path, FALSE)) {
        uprintf("Could not unmount '%s' to write the Syslinux boot record", volume_path);
        goto out;
    }

    /* Read existing FAT data into boot sector */
    if (!SetFilePointerEx(d_handle, liZero, NULL, FILE_BEGIN) ||
        !ReadFile(d_handle, sectbuf, SECTOR_SIZE, &bytes_read, NULL)) {
        uprintf("Could not read Syslinux boot record: %s", WindowsErrorString());
        goto out;
    }
    if (bytes_read < SECTOR_SIZE) {
        uprintf("Partial read of Syslinux boot record: read %u bytes but requested %u", bytes_read, SECTOR_SIZE);
        goto out;
    }

    /* Make the syslinux boot sector */
    syslinux_make_bootsect(sectbuf, (file_system == FS_NTFS) ? NTFS : VFAT);

    /* Write boot sector back */
    if (!SetFilePointerEx(d_handle, liZero, NULL, FILE_BEGIN) ||
        !WriteFileWithRetry(d_handle, sectbuf, SECTOR_SIZE, NULL, WRITE_RETRIES)) {
        uprintf("Could not write Syslinux boot record: %s", WindowsErrorString());
        goto out;
    }
    uprintf("Successfully wrote Syslinux boot record");

    if (boot_type != BT_IMAGE)
        UpdateProgress(OP_FILE_COPY, -1.0f);

    r = TRUE;

out:
    safe_mm_free(sectbuf);
    safe_free(syslinux_ldlinux[0]);
    safe_free(syslinux_ldlinux[1]);
    safe_free(sectors);
    safe_closehandle(d_handle);
    safe_closehandle(f_handle);
    return r;
}
