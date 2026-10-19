/*
 * Rufus Linux port: drive formatting pipeline.
 *
 * Port of upstream format.c. The flow is the same as on Windows:
 *
 *   1. (optionally) zero the whole drive
 *   2. (optionally) write a disk image ("DD mode")
 *   3. zap any existing MBR/GPT and re-read the partition table
 *   4. create the partition layout            (partition.c)
 *   5. format the main (and extra) partitions
 *   6. (optionally) write MBR/SBR/PBR boot code (ms-sys)
 *   7. finalize
 *
 * What differs from Windows is the medium layer only: instead of FMIFS /
 * VDS / drive letters, the port writes file systems through mkfs.* (or its
 * own native formatters) and manages volumes through mount(2)/umount2(2).
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "rfus.h"
#include "winio.h"
#include "badblocks.h"
#include <time.h>
#include "../res/grub/grub_version.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/* ms-sys (vendored) */
#include "br.h"
#include "file.h"
#include "fat16.h"
#include "fat32.h"
#include "ntfs.h"
#include "partition_info.h"


static int task_number = 0, actual_fs_type;

#define TASK_NB (actual_fs_type >= 0 && actual_fs_type < FS_MAX ? nb_steps[actual_fs_type] : 1)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Converts an UTF-8 label to a valid FAT/NTFS one
 * (direct port of upstream format.c:ToValidLabel())
 */
void ToValidLabel(char* Label, BOOL bFAT)
{
    size_t i, j, k;
    BOOL found;
    const wchar_t unauthorized[] = L"*?,;:/\\|+=<>[]\"";
    const wchar_t to_underscore[] = L"\t.";
    char label[16] = { 0 };
    wchar_t* wLabel = utf8_to_wchar(Label);

    if (wLabel == NULL)
        return;

    for (i = 0, k = 0; i < wcslen(wLabel); i++) {
        if (bFAT) { /* NTFS allows all the FAT unauthorized above */
            found = FALSE;
            for (j = 0; j < wcslen(unauthorized); j++) {
                if (wLabel[i] == unauthorized[j]) {
                    found = TRUE;
                    break;
                }
            }
            /* A FAT label that contains extended chars will be rejected */
            if (wLabel[i] >= 0x80) {
                wLabel[k++] = L'_';
                found = TRUE;
            }
            if (found)
                continue;
        }
        found = FALSE;
        for (j = 0; j < wcslen(to_underscore); j++) {
            if (wLabel[i] == to_underscore[j]) {
                wLabel[k++] = '_';
                found = TRUE;
                break;
            }
        }
        if (found)
            continue;
        wLabel[k++] = bFAT ? towupper(wLabel[i]) : wLabel[i];
    }
    wLabel[k] = 0;

    if (bFAT) {
        if (wcslen(wLabel) > 11)
            wLabel[11] = 0;
        for (i = 0, j = 0; wLabel[i] != 0; i++)
            if (wLabel[i] == '_')
                j++;
        if (i < 2 * j) {
            /* If the final label is mostly underscore, use an alternate label
             * according to the size (eg: "256 MB", "7.9 GB"). */
            static_sprintf(label, "%s", SizeToHumanReadable(SelectedDrive.DiskSize, TRUE, FALSE));
            for (i = 0; label[i] != 0; i++)
                wLabel[i] = (label[i] == '.') ? '_' : label[i];
            wLabel[i] = 0;
            uprintf("FAT label is mostly underscores. Using '%ls' label instead.", wLabel);
        }
    } else if (wcslen(wLabel) > 32) {
        wLabel[32] = 0;
    }

    /* Needed for disk by label isolinux.cfg workaround */
    wchar_to_utf8_no_alloc(wLabel, img_report.usb_label, sizeof(img_report.usb_label));
    safe_strcpy(Label, strlen(Label) + 1, img_report.usb_label);
    free(wLabel);
}

static const char* bt_to_name(void)
{
    switch (boot_type) {
    case BT_FREEDOS: return "FreeDOS";
    case BT_REACTOS: return "ReactOS";
    default:
        return ((boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report)) ? "KolibriOS" : "Standard";
    }
}

/* ------------------------------------------------------------------ */
/* MBR/GPT clearing                                                    */
/* ------------------------------------------------------------------ */
BOOL ClearMBRGPT(HANDLE hPhysicalDrive, uint64_t DiskSize, uint32_t SectorSize)
{
    BOOL r = FALSE;
    uint8_t* pZeroBuf = (uint8_t*)calloc(SectorSize, MAX_SECTORS_TO_CLEAR);

    if (pZeroBuf == NULL) {
        ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
        goto out;
    }

    /* We unconditionally zero MAX_SECTORS_TO_CLEAR and (MAX_SECTORS_TO_CLEAR / 8)
     * at the beginning and end of the drive respectively. This should encompass the
     * MBR, GPT, backup GPT and VBR for most devices. */
    PrintInfoDebug(0, MSG_224);
    if (!SetFilePointerEx(hPhysicalDrive, (LARGE_INTEGER){ .QuadPart = 0 }, NULL, FILE_BEGIN))
        uprintf("WARNING: Could not reset disk position: %s", WindowsErrorString());
    if (!WriteFileWithRetry(hPhysicalDrive, pZeroBuf, SectorSize * MAX_SECTORS_TO_CLEAR, NULL, WRITE_RETRIES))
        goto out;
    uprintf("Zeroed %s at the top of the drive", SizeToHumanReadable(MAX_SECTORS_TO_CLEAR * SectorSize, FALSE, FALSE));
    CHECK_FOR_USER_CANCEL;
    if (DiskSize > (uint64_t)SectorSize * (MAX_SECTORS_TO_CLEAR / 8)) {
        if (SetFilePointerEx(hPhysicalDrive,
                (LARGE_INTEGER){ .QuadPart = (int64_t)(DiskSize - (uint64_t)SectorSize * (MAX_SECTORS_TO_CLEAR / 8)) },
                NULL, FILE_BEGIN) &&
            WriteFileWithRetry(hPhysicalDrive, pZeroBuf, SectorSize * (MAX_SECTORS_TO_CLEAR / 8), NULL, WRITE_RETRIES))
            uprintf("Zeroed %s at the end of the drive", SizeToHumanReadable((MAX_SECTORS_TO_CLEAR / 8) * SectorSize, FALSE, FALSE));
        else
            uprintf("WARNING: Could not clear the backup GPT area: %s", WindowsErrorString());
    }

    r = TRUE;

out:
    safe_free(pZeroBuf);
    return r;
}

/* ------------------------------------------------------------------ */
/* Boot records (ms-sys)                                               */
/* ------------------------------------------------------------------ */

/*
 * Process the Master Boot Record
 */
BOOL WriteMBR(HANDLE hPhysicalDrive)
{
    BOOL r = FALSE;
    BOOL needs_masquerading = HAS_WINPE(img_report) && (!img_report.uses_minint);
    uint8_t* buffer = NULL;
    FAKE_FD fake_fd = { 0 };
    FILE* fp = (FILE*)&fake_fd;
    const char* using_msg = "Using %s MBR";

    if (SelectedDrive.SectorSize < 512)
        goto out;

    if (partition_type == PARTITION_STYLE_GPT) {
        /* Add a notice with a protective MBR */
        fake_fd._handle = (char*)hPhysicalDrive;
        set_bytes_per_sector(SelectedDrive.SectorSize);
        uprintf(using_msg, "Rufus protective");
        r = write_rufus_msg_mbr(fp);
        goto notify;
    }

    /* The port's partition writer already uses LBA partition types, but keep
     * upstream's normalization so that a pre-existing table behaves too. */
    buffer = (uint8_t*)_mm_malloc(SelectedDrive.SectorSize, 16);
    if (buffer == NULL) {
        uprintf("Could not allocate memory for MBR");
        ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
        goto out;
    }

    if (!read_sectors(hPhysicalDrive, SelectedDrive.SectorSize, 0, 1, buffer)) {
        uprintf("Could not read MBR");
        ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
        goto out;
    }

    switch (fs_type) {
    case FS_FAT16:
        if (buffer[0x1c2] == 0x0e) {
            uprintf("Partition is already FAT16 LBA...");
        } else if ((buffer[0x1c2] != 0x04) && (buffer[0x1c2] != 0x06)) {
            uprintf("WARNING: converting a non FAT16 partition to FAT16 LBA: FS type=0x%02x", buffer[0x1c2]);
        }
        buffer[0x1c2] = 0x0e;
        break;
    case FS_FAT32:
        if (buffer[0x1c2] == 0x0c) {
            uprintf("Partition is already FAT32 LBA...");
        } else if (buffer[0x1c2] != 0x0b) {
            uprintf("WARNING: converting a non FAT32 partition to FAT32 LBA: FS type=0x%02x", buffer[0x1c2]);
        }
        buffer[0x1c2] = 0x0c;
        break;
    }
    if ((boot_type != BT_NON_BOOTABLE) && (target_type == TT_BIOS)) {
        /* Set first partition bootable or masquerade as second disk */
        buffer[0x1be] = needs_masquerading ? 0x81 : 0x80;
        uprintf("Set bootable USB partition as 0x%02X", buffer[0x1be]);
    }

    if (!write_sectors(hPhysicalDrive, SelectedDrive.SectorSize, 0, 1, buffer)) {
        uprintf("Could not write MBR");
        ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
        goto out;
    }

    fake_fd._handle = (char*)hPhysicalDrive;
    set_bytes_per_sector(SelectedDrive.SectorSize);

    /* What follows is really a case statement with complex conditions listed
     * by order of preference */
    if ((boot_type == BT_IMAGE) && HAS_WINDOWS(img_report) && (allow_dual_uefi_bios) && (target_type == TT_BIOS))
        goto windows_mbr;

    /* Non bootable or forced UEFI (zeroed MBR) */
    if ((boot_type == BT_NON_BOOTABLE) || (target_type == TT_UEFI)) {
        uprintf(using_msg, "Zeroed");
        r = write_zero_mbr(fp);
        goto notify;
    }

    /* Syslinux */
    if ((boot_type == BT_SYSLINUX_V4) || (boot_type == BT_SYSLINUX_V6) ||
        ((boot_type == BT_IMAGE) && HAS_SYSLINUX(img_report))) {
        uprintf(using_msg, "Syslinux");
        r = write_syslinux_mbr(fp);
        goto notify;
    }

    /* Grub 2.0 */
    if (((boot_type == BT_IMAGE) && (img_report.has_grub2)) || (boot_type == BT_GRUB2)) {
        uprintf(using_msg, "Grub 2.0");
        r = write_grub2_mbr(fp);
        goto notify;
    }

    /* Grub4DOS */
    if (((boot_type == BT_IMAGE) && (img_report.has_grub4dos)) || (boot_type == BT_GRUB4DOS)) {
        uprintf(using_msg, "Grub4DOS");
        r = write_grub4dos_mbr(fp);
        goto notify;
    }

    /* ReactOS */
    if (boot_type == BT_REACTOS) {
        uprintf(using_msg, "ReactOS");
        r = write_reactos_mbr(fp);
        goto notify;
    }

    /* KolibriOS */
    if ((boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report) && (IS_FAT(fs_type))) {
        uprintf(using_msg, "KolibriOS");
        r = write_kolibrios_mbr(fp);
        goto notify;
    }

    /* If everything else failed, fall back to a conventional Windows/Rufus MBR */
windows_mbr:
    if (needs_masquerading || use_rufus_mbr) {
        uprintf(using_msg, APPLICATION_NAME);
        r = write_rufus_mbr(fp);
    } else {
        uprintf(using_msg, "Windows 7");
        r = write_win7_mbr(fp);
    }

notify:
    /* Tell the kernel we've updated the disk properties */
    RefreshDriveLayout(hPhysicalDrive);

out:
    safe_mm_free(buffer);
    return r;
}

/*
 * Write Secondary Boot Record (usually right after the MBR)
 */
BOOL WriteSBR(HANDLE hPhysicalDrive)
{
    DWORD size = 0, max_size, br_size = 0x200;
    int r, sub_type = boot_type;
    uint8_t* buf = NULL;
    FAKE_FD fake_fd = { 0 };
    FILE* fp = (FILE*)&fake_fd;

    fake_fd._handle = (char*)hPhysicalDrive;
    set_bytes_per_sector(SelectedDrive.SectorSize);
    /* Syslinux has precedence over Grub */
    if ((boot_type == BT_IMAGE) && (!HAS_SYSLINUX(img_report))) {
        if (img_report.has_grub4dos)
            sub_type = BT_GRUB4DOS;
        if (img_report.has_grub2)
            sub_type = BT_GRUB2;
    }

    /* Use BT_MAX for the protective message */
    if ((boot_type != BT_NON_BOOTABLE) && (partition_type == PARTITION_STYLE_GPT))
        sub_type = BT_MAX;

    switch (sub_type) {
    case BT_GRUB4DOS:
        uprintf("Writing Grub4Dos SBR");
        buf = GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_GR_GRUB_GRLDR_MBR), _RT_RCDATA, "grldr.mbr", &size, FALSE);
        if ((buf == NULL) || (size <= br_size)) {
            uprintf("grldr.mbr is either not present or too small");
            return FALSE;
        }
        buf = &buf[br_size];
        size -= br_size;
        break;
    case BT_GRUB2:
        if (grub2_buf != NULL) {
            uprintf("Writing Grub 2.0 SBR (from download) %s",
                IsBufferInDB(grub2_buf, grub2_len) ? "✓" : "✗");
            buf = grub2_buf;
            size = (DWORD)grub2_len;
        } else {
            uprintf("Writing Grub 2.0 SBR (from embedded)");
            buf = GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_GR_GRUB2_CORE_IMG), _RT_RCDATA, "core.img", &size, FALSE);
            if (buf == NULL) {
                uprintf("Could not access core.img");
                return FALSE;
            }
        }
        break;
    case BT_MAX:
        uprintf("Writing protective message SBR");
        size = 4 * KB;
        br_size = 17 * KB;    /* 34 sectors are reserved for protective MBR + primary GPT */
        buf = GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_SBR_MSG), _RT_RCDATA, "msg.txt", &size, TRUE);
        if (buf == NULL) {
            uprintf("Could not access message");
            return FALSE;
        }
        break;
    default:
        /* No need to write secondary block */
        return TRUE;
    }

    /* Ensure that we have sufficient space for the SBR */
    max_size = (DWORD)SelectedDrive.Partition[0].Offset;
    if (br_size + size > max_size) {
        uprintf("  SBR size is too large - You may need to uncheck 'Add fixes for old BIOSes'.");
        if (sub_type == BT_MAX)
            safe_free(buf);
        return FALSE;
    }

    r = write_data(fp, br_size, buf, (uint64_t)size);
    if (sub_type == BT_MAX)
        safe_free(buf);
    return (r != 0);
}

/*
 * Process the Partition Boot Record
 */
BOOL WritePBR(HANDLE hLogicalVolume)
{
    int i;
    FAKE_FD fake_fd = { 0 };
    FILE* fp = (FILE*)&fake_fd;
    const char* using_msg = "Using %s %s partition boot record";

    fake_fd._handle = (char*)hLogicalVolume;
    set_bytes_per_sector(SelectedDrive.SectorSize);

    switch (actual_fs_type) {
    case FS_FAT16:
        uprintf(using_msg, bt_to_name(), "FAT16");
        if (!is_fat_16_fs(fp)) {
            uprintf("New volume does not have a FAT16 boot sector - aborting");
            break;
        }
        uprintf("Confirmed new volume has a FAT16 boot sector");
        if (boot_type == BT_FREEDOS) {
            if (!write_fat_16_fd_br(fp, 0)) break;
        } else if (boot_type == BT_REACTOS) {
            if (!write_fat_16_ros_br(fp, 0)) break;
        } else if ((boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report)) {
            uprintf("FAT16 is not supported for KolibriOS"); break;
        } else {
            if (!write_fat_16_br(fp, 0)) break;
        }
        /* Disk Drive ID needs to be corrected on XP */
        if (!write_partition_physical_disk_drive_id_fat16(fp))
            break;
        return TRUE;
    case FS_FAT32:
        uprintf(using_msg, bt_to_name(), "FAT32");
        for (i = 0; i < 2; i++) {
            if (!is_fat_32_fs(fp)) {
                uprintf("New volume does not have a %s FAT32 boot sector - aborting", i ? "secondary" : "primary");
                break;
            }
            uprintf("Confirmed new volume has a %s FAT32 boot sector", i ? "secondary" : "primary");
            uprintf("Setting %s FAT32 boot sector for boot...", i ? "secondary" : "primary");
            if (boot_type == BT_FREEDOS) {
                if (!write_fat_32_fd_br(fp, 0)) break;
            } else if (boot_type == BT_REACTOS) {
                if (!write_fat_32_ros_br(fp, 0)) break;
            } else if ((boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report)) {
                if (!write_fat_32_kos_br(fp, 0)) break;
            } else if ((boot_type == BT_IMAGE) && HAS_BOOTMGR(img_report)) {
                if (!write_fat_32_pe_br(fp, 0)) break;
            } else if ((boot_type == BT_IMAGE) && HAS_WINPE(img_report)) {
                if (!write_fat_32_nt_br(fp, 0)) break;
            } else {
                if (!write_fat_32_br(fp, 0)) break;
            }
            /* Disk Drive ID needs to be corrected on XP */
            if (!write_partition_physical_disk_drive_id_fat32(fp))
                break;
            fake_fd._offset += 6 * SelectedDrive.SectorSize;
        }
        return TRUE;
    case FS_NTFS:
        uprintf(using_msg, bt_to_name(), "NTFS");
        if (!is_ntfs_fs(fp)) {
            uprintf("New volume does not have an NTFS boot sector - aborting");
            break;
        }
        uprintf("Confirmed new volume has an NTFS boot sector");
        if (!write_ntfs_br(fp)) break;
        return TRUE;
    case FS_EXT2:
    case FS_EXT3:
    case FS_EXT4:
        return TRUE;
    default:
        uprintf("Unsupported FS for FS BR processing - aborting");
        break;
    }
    ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* File system creation                                                */
/* ------------------------------------------------------------------ */

/* Run a mkfs.*-style command line, keeping the log clean on success */
static BOOL run_format_command(const char* cmd, const char* what)
{
    DWORD ret = RunCommand(cmd, NULL, FALSE);

    if (ret == 127) {
        /* The shell could not find the tool */
        uprintf("Could not format %s: '%.*s' was not found - please install it (%s)", what,
            (int)strcspn(cmd, " "), cmd,
            (strncmp(cmd, "mkudffs", 7) == 0) ? "udftools" : (strncmp(cmd, "mkntfs", 6) == 0) ? "ntfs-3g" :
            (strncmp(cmd, "mkfs.exfat", 10) == 0) ? "exfatprogs" : (strncmp(cmd, "mkfs.fat", 8) == 0) ? "dosfstools" : "e2fsprogs");
        ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (ret != 0) {
        uprintf("Could not format %s: command '%s' failed with error %lu", what, cmd, (unsigned long)ret);
        ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return TRUE;
}

/*
 * Format a partition with the system's mkfs.* tools. This is the port's
 * equivalent of FMIFS' FormatEx(), except that the file systems are created
 * by the distribution's utilities (mkfs.vfat, mkntfs, mkfs.exfat, mkudffs).
 */
/*
 * Copy the data extents of a (sparse) file into another file at 'offset',
 * zeroing the holes, so that 'size' bytes are written in total.
 */
static BOOL CopySparseFile(const char* src, const char* dst, uint64_t offset, uint64_t size)
{
    int in = -1, out = -1;
    uint8_t *buf = NULL, *zero = NULL;
    off_t data, hole, pos = 0;
    BOOL r = FALSE;
    const size_t bufsize = 4 * MB;

    in = open(src, O_RDONLY | O_CLOEXEC);
    out = open(dst, O_WRONLY | O_CLOEXEC);
    buf = malloc(bufsize);
    zero = calloc(1, bufsize);
    if (in < 0 || out < 0 || buf == NULL || zero == NULL)
        goto out;
    while ((uint64_t)pos < size) {
        data = lseek(in, pos, SEEK_DATA);
        if (data < 0)
            data = (off_t)size;   /* only a hole remains */
        /* Zero the hole [pos, data) in the destination */
        while (pos < data) {
            size_t n = (size_t)MIN((uint64_t)bufsize, (uint64_t)(data - pos));
            if (pwrite(out, zero, n, (off_t)offset + pos) != (ssize_t)n)
                goto out;
            pos += n;
        }
        if ((uint64_t)pos >= size)
            break;
        hole = lseek(in, pos, SEEK_HOLE);
        if (hole < 0 || (uint64_t)hole > size)
            hole = (off_t)size;
        while (pos < hole) {
            size_t n = (size_t)MIN((uint64_t)bufsize, (uint64_t)(hole - pos));
            ssize_t rd = pread(in, buf, n, pos);
            if (rd <= 0 || pwrite(out, buf, rd, (off_t)offset + pos) != rd)
                goto out;
            pos += rd;
        }
    }
    r = (fsync(out) == 0);
out:
    if (!r)
        uprintf("Could not copy the formatted volume into the image: %s", strerror(errno));
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    free(buf);
    free(zero);
    return r;
}

static BOOL FormatNativeMkfs(DWORD DriveIndex, uint64_t PartitionOffset, DWORD UnitAllocationSize,
                             USHORT FSType, LPCSTR Label, DWORD Flags)
{
    /*
     * PORT: upstream formats FAT16/NTFS/exFAT/UDF/ReFS through fmifs.dll
     * (FormatEx). The Linux equivalents are the mkfs tools, which need a
     * block device: the partition node, or a loop device for image files.
     */
    char cmd[768], opts[128] = "";
    char vol_label[72] = "";
    char path[MAX_PATH];
    const char* what = FileSystemLabel[FSType];
    uint64_t part_size = 0, hidden;
    int tmp_fd;
    BOOL r, is_file;

    if (FSType == FS_REFS) {
        uprintf("ReFS is not supported on Linux");
        ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    /* Image files: format a sparse temporary file of the partition size,
     * then copy it into the image (no loop device needed, works as a user) */
    is_file = IsVirtualDrive(DriveIndex);
    if (is_file) {
        int i;
        for (i = 0; i < SelectedDrive.nPartitions; i++)
            if (SelectedDrive.Partition[i].Offset == PartitionOffset)
                part_size = SelectedDrive.Partition[i].Size;
        if (part_size == 0) {
            uprintf("Could not find the size of the partition to format");
            ErrorStatus = RUFUS_ERROR(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        static_sprintf(path, "%s.rufus-XXXXXX", GetPhysicalName(DriveIndex));
        tmp_fd = mkstemp(path);
        if ((tmp_fd < 0) || (ftruncate(tmp_fd, (off_t)part_size) != 0)) {
            uprintf("Could not create temporary volume '%s': %s", path, strerror(errno));
            if (tmp_fd >= 0) {
                close(tmp_fd);
                unlink(path);
            }
            ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
            return FALSE;
        }
        close(tmp_fd);
    } else {
        static_strcpy(path, GetVolumeDevicePath(DriveIndex, PartitionOffset, FALSE));
        if (path[0] == 0) {
            uprintf("Could not access the volume to format it");
            ErrorStatus = RUFUS_ERROR(ERROR_INVALID_HANDLE);
            return FALSE;
        }
    }
    hidden = PartitionOffset / SelectedDrive.SectorSize;

    if (Label != NULL && Label[0] != 0) {
        safe_strcpy(vol_label, sizeof(vol_label), Label);
        ToValidLabel(vol_label, (FSType == FS_FAT16) || (FSType == FS_FAT32) || (FSType == FS_EXFAT));
    }

    switch (FSType) {
    case FS_FAT16:
        if (UnitAllocationSize >= 512)
            static_sprintf(opts, " -s %u", (unsigned)(UnitAllocationSize / SelectedDrive.SectorSize));
        snprintf(cmd, sizeof(cmd), "mkfs.fat -F 16 -S %u -h %llu%s%s%s%s '%s'", (unsigned)SelectedDrive.SectorSize,
            (unsigned long long)hidden, opts,
            (vol_label[0] != 0) ? " -n \"" : "", vol_label, (vol_label[0] != 0) ? "\"" : "", path);
        break;
    case FS_NTFS:
        if (UnitAllocationSize >= 512)
            static_sprintf(opts, " -c %u", (unsigned)UnitAllocationSize);
        /* Geometry (hidden sectors/heads/sectors per track) is what the NTFS
         * boot record uses to locate the volume: give it explicitly, since
         * mkntfs can't query it from an image file */
        snprintf(cmd, sizeof(cmd), "mkntfs -F%s%s -s %u -p %llu -H 255 -S 63%s%s%s%s '%s'", (Flags & FP_QUICK) ? " -Q" : "",
            (Flags & FP_COMPRESSION) ? " -C" : "", (unsigned)SelectedDrive.SectorSize, (unsigned long long)hidden, opts,
            (vol_label[0] != 0) ? " -L \"" : "", vol_label, (vol_label[0] != 0) ? "\"" : "", path);
        break;
    case FS_EXFAT:
        if (UnitAllocationSize >= 512)
            static_sprintf(opts, " -c %u", (unsigned)UnitAllocationSize);
        snprintf(cmd, sizeof(cmd), "mkfs.exfat%s%s%s%s%s '%s'", (Flags & FP_QUICK) ? "" : " --full-format", opts,
            (vol_label[0] != 0) ? " -L \"" : "", vol_label, (vol_label[0] != 0) ? "\"" : "", path);
        break;
    case FS_UDF:
        snprintf(cmd, sizeof(cmd), "mkudffs --media-type=hd --blocksize=%u --utf8%s%s%s '%s'",
            (unsigned)SelectedDrive.SectorSize,
            (vol_label[0] != 0) ? " --label=\"" : "", vol_label, (vol_label[0] != 0) ? "\"" : "", path);
        break;
    default:
        ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    uprintf("Formatting %s as %s...", is_file ? "partition" : path, what);
    r = run_format_command(cmd, what);
    if (is_file) {
        if (r)
            r = CopySparseFile(path, GetPhysicalName(DriveIndex), PartitionOffset, part_size);
        unlink(path);
        if (!r && !IS_ERROR(ErrorStatus))
            ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
    }
    return r;
}

/*
 * Format a partition as ext2/ext3/ext4
 */
BOOL FormatExtFs(DWORD DriveIndex, uint64_t PartitionOffset, DWORD BlockSize, LPCSTR FSName, LPCSTR Label, DWORD Flags)
{
    char cmd[768];
    char vol_label[72] = "";
    char offset_opt[64] = "";
    char block_opt[64] = "";
    char* path;
    HANDLE hLogicalVolume;
    struct stat st;
    int fsversion = FSName[3] - '0';    /* "ext2" -> 2 ... */

    if (IS_ERROR(ErrorStatus))
        return FALSE;
    if (!(Flags & FP_NO_PROGRESS)) {
        PrintInfoDebug(0, MSG_222, FSName);
        UpdateProgressWithInfoInit(NULL, TRUE);
    }

    path = GetLogicalName(DriveIndex, PartitionOffset, FALSE, TRUE);
    if (path == NULL || path[0] == 0) {
        ErrorStatus = RUFUS_ERROR(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if ((stat(path, &st) == 0) && S_ISREG(st.st_mode)) {
        /* mke2fs supports formatting at a byte offset ("-E offset=") */
        snprintf(offset_opt, sizeof(offset_opt), " -E offset=%llu", (unsigned long long)PartitionOffset);
    }
    if (BlockSize >= 1024 && (BlockSize % 1024) == 0 && BlockSize <= 65536)
        snprintf(block_opt, sizeof(block_opt), " -b %lu", (unsigned long)BlockSize);
    if (Label != NULL && Label[0] != 0) {
        safe_strcpy(vol_label, sizeof(vol_label), Label);
        ToValidLabel(vol_label, FALSE);
    }

    if (vol_label[0] != 0)
        snprintf(cmd, sizeof(cmd), "mke2fs -F -q -t ext%d%s%s -L \"%s\" '%s'",
            fsversion, block_opt, offset_opt, vol_label, path);
    else
        snprintf(cmd, sizeof(cmd), "mke2fs -F -q -t ext%d%s%s '%s'",
            fsversion, block_opt, offset_opt, path);

    if (!(Flags & FP_NO_PROGRESS))
        PrintInfo(0, MSG_221, lmprintf(MSG_307));
    uprintf("Formatting %s as %s...", path, FSName);
    if (!run_format_command(cmd, FSName))
        return FALSE;

    actual_fs_type = FS_EXT2 + (fsversion - 2);
    if (!(Flags & FP_NO_BOOT)) {
        if (!(Flags & FP_NO_PROGRESS))
            PrintInfoDebug(0, MSG_229);
        hLogicalVolume = GetLogicalHandle(DriveIndex, PartitionOffset, TRUE, TRUE, FALSE);
        if ((hLogicalVolume != NULL) && (hLogicalVolume != INVALID_HANDLE_VALUE)) {
            if (!WritePBR(hLogicalVolume))
                uprintf("Could not finalize the ext partition boot record");
            safe_closehandle(hLogicalVolume);
        }
    }
    return TRUE;
}

/*
 * Format a partition. Same dispatch as upstream FormatPartition():
 * FAT32 always goes through the native formatter, ext through mke2fs,
 * everything else through the system's mkfs.
 */
BOOL FormatPartition(DWORD DriveIndex, uint64_t PartitionOffset, DWORD UnitAllocationSize, USHORT FSType, LPCSTR Label, DWORD Flags)
{
    if ((DriveIndex < DRIVE_INDEX_MIN) || (DriveIndex > DRIVE_INDEX_MAX) || (FSType >= FS_MAX) ||
        ((UnitAllocationSize != 0) && ((UnitAllocationSize & (UnitAllocationSize - 1)) != 0))) {
        ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    actual_fs_type = FSType;
    task_number = 0;
    if (FSType == FS_FAT32)
        return FormatLargeFAT32(DriveIndex, PartitionOffset, UnitAllocationSize, FileSystemLabel[FSType], Label, Flags);
    else if (IS_EXT(FSType))
        return FormatExtFs(DriveIndex, PartitionOffset, UnitAllocationSize, FileSystemLabel[FSType], Label, Flags);
    else
        return FormatNativeMkfs(DriveIndex, PartitionOffset, UnitAllocationSize, FSType, Label, Flags);
}

/* ------------------------------------------------------------------ */
/* Image writing                                                       */
/* ------------------------------------------------------------------ */

/* Write an image file to a drive, or zero a drive */
#define NUM_BUFFERS 3   /* 2 + 1 as a mere double buffered async I/O would modify the buffer being processed */

/* Sector aligned writes for bled (upstream format.c sector_write()) */
static uint8_t* sec_buf = NULL;
static unsigned int sec_buf_pos = 0;
static uint64_t sec_written = 0;    /* PORT: bytes written, to restore the handle position */
static unsigned long bled_cancel = 0;

static int sector_write(int fd, const void* _buf, unsigned int count)
{
    const uint8_t* buf = (const uint8_t*)_buf;
    unsigned int sec_size = (unsigned int)SelectedDrive.SectorSize;
    int written, fill_size = 0;
    uint32_t sec_num;

    if (sec_size == 0)
        sec_size = 512;
    if ((sec_size > 64 * KB) || (count > 1 * GB))
        return -1;

    /* If we are on a sector boundary and count is multiple of the
     * sector size, just issue a regular write */
    if ((sec_buf_pos == 0) && (count % sec_size == 0)) {
        written = _write(fd, buf, count);
        if (written > 0)
            sec_written += written;
        return written;
    }

    /* If we have an existing partial sector, fill and write it */
    if (sec_buf_pos > 0) {
        fill_size = MIN(sec_size - sec_buf_pos, count);
        memcpy(&sec_buf[sec_buf_pos], buf, fill_size);
        sec_buf_pos += fill_size;
        /* If we don't have a full sector just buffer it for next call */
        if (sec_buf_pos < sec_size)
            return (int)count;
        sec_buf_pos = 0;
        written = _write(fd, sec_buf, sec_size);
        if (written != (int)sec_size)
            return written;
        sec_written += written;
    }

    /* Now write as many full sectors as we can */
    sec_num = (count - fill_size) / sec_size;
    written = _write(fd, &buf[fill_size], sec_num * sec_size);
    if (written < 0)
        return written;
    sec_written += written;
    if (written != (int)(sec_num * sec_size))
        return fill_size + written;
    sec_buf_pos = count - fill_size - written;
    if (sec_buf_pos >= sec_size)
        return -1;

    /* Keep leftover bytes, if any, in the sector buffer */
    if (sec_buf_pos != 0)
        memcpy(sec_buf, &buf[fill_size + written], sec_buf_pos);
    return (int)count;
}

/* Progress for bled (upstream format.c update_progress()) */
static void update_progress(const int64_t processed_bytes)
{
    static uint64_t total_bytes;

    /* PORT: propagate user cancellation to bled */
    bled_cancel = IS_ERROR(ErrorStatus) ? 1 : 0;
    if (processed_bytes < 0) {
        total_bytes = (uint64_t)-processed_bytes;
        UpdateProgressWithInfo(OP_FORMAT, MSG_261, 0, total_bytes);
        uprint_progress(0, total_bytes);
    } else {
        UpdateProgressWithInfo(OP_FORMAT, MSG_261, (uint64_t)processed_bytes, total_bytes);
        uprint_progress((uint64_t)processed_bytes, total_bytes);
    }
}

static BOOL WriteDrive(HANDLE hPhysicalDrive, BOOL bZeroDrive)
{
    BOOL s, ret = FALSE;
    HANDLE hSourceImage = INVALID_HANDLE_VALUE;
    DWORD i, read_size = 0, write_size = 0, buf_size;
    uint64_t wb, target_size = bZeroDrive ? SelectedDrive.DiskSize : MIN((uint64_t)SelectedDrive.DiskSize, img_report.image_size);
    uint8_t* buffer = NULL;
    uint32_t zero_data;
    uint8_t* cmp_buffer = NULL;
    int throttle_fast_zeroing = 0;

    if (SelectedDrive.SectorSize < 512) {
        uprintf("Unexpected sector size (%u) - Aborting", SelectedDrive.SectorSize);
        return FALSE;
    }

    /* We poked the MBR and other stuff, so we need to rewind */
    if (!SetFilePointerEx(hPhysicalDrive, (LARGE_INTEGER){ .QuadPart = 0 }, NULL, FILE_BEGIN))
        uprintf("WARNING: Unable to rewind image position - wrong data might be copied!");
    UpdateProgressWithInfoInit(NULL, FALSE);

    if (bZeroDrive) {
        uprintf(fast_zeroing ? "Fast-zeroing drive:" : "Zeroing drive:");
        /* Our buffer size must be a multiple of the sector size and *ALIGNED* to the sector size */
        buf_size = (DWORD)(((DD_BUFFER_SIZE + SelectedDrive.SectorSize - 1) / SelectedDrive.SectorSize) * SelectedDrive.SectorSize);
        buffer = (uint8_t*)_mm_malloc(buf_size, SelectedDrive.SectorSize);
        if (buffer == NULL) {
            ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
            uprintf("Could not allocate disk zeroing buffer");
            goto out;
        }
        memset(buffer, fast_zeroing ? 0xff : 0x00, buf_size);

        if (fast_zeroing) {
            cmp_buffer = (uint8_t*)_mm_malloc(buf_size, SelectedDrive.SectorSize);
            if (cmp_buffer == NULL) {
                ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
                uprintf("Could not allocate disk comparison buffer");
                goto out;
            }
        }

        read_size = buf_size;
        uprint_progress(0, 0);
        for (wb = 0; wb < target_size; wb += read_size) {
            UpdateProgressWithInfo(OP_FORMAT, fast_zeroing ? MSG_306 : MSG_286, wb, target_size);
            uprint_progress(wb, target_size);
            /* Don't overflow our projected size */
            if (wb + read_size > target_size)
                read_size = (DWORD)(target_size - wb);
            /* Writes fail unless the size is a multiple of sector size */
            if (read_size % SelectedDrive.SectorSize != 0)
                read_size = (DWORD)(((read_size + SelectedDrive.SectorSize - 1) / SelectedDrive.SectorSize) * SelectedDrive.SectorSize);

            if (throttle_fast_zeroing) {
                throttle_fast_zeroing--;
            } else if (fast_zeroing) {
                DWORD comp_size = 0;
                CHECK_FOR_USER_CANCEL;
                s = ReadFile(hPhysicalDrive, cmp_buffer, read_size, &comp_size, NULL);
                if ((!s) || (comp_size != read_size)) {
                    uprintf("\r\nRead error: Could not read data for fast zeroing comparison - %s", WindowsErrorString());
                    goto out;
                }
                zero_data = *(uint32_t*)cmp_buffer;
                if ((zero_data == 0) || (zero_data == 0xffffffff)) {
                    for (i = 1; (i < read_size / sizeof(uint32_t)) && (*(uint32_t*)(cmp_buffer + i * 4) == zero_data); i++);
                    if (i >= read_size / sizeof(uint32_t))
                        continue;    /* Block is empty, skip write */
                }
                if (!SetFilePointerEx(hPhysicalDrive, (LARGE_INTEGER){ .QuadPart = (int64_t)wb }, NULL, FILE_BEGIN)) {
                    uprintf("\r\nError: Could not reset position - %s", WindowsErrorString());
                    goto out;
                }
                throttle_fast_zeroing = 15;
            }

            for (i = 1; i <= WRITE_RETRIES; i++) {
                CHECK_FOR_USER_CANCEL;
                s = WriteFile(hPhysicalDrive, buffer, read_size, &write_size, NULL);
                if ((s) && (write_size == read_size))
                    break;
                if (s)
                    uprintf("\r\nWrite error: Wrote %u bytes, expected %u bytes", write_size, read_size);
                else
                    uprintf("\r\nWrite error at sector %llu: %s", (unsigned long long)(wb / SelectedDrive.SectorSize), WindowsErrorString());
                if (i < WRITE_RETRIES) {
                    uprintf("Retrying in %d seconds...", WRITE_TIMEOUT / 1000);
                    Sleep(WRITE_TIMEOUT);
                    if (!SetFilePointerEx(hPhysicalDrive, (LARGE_INTEGER){ .QuadPart = (int64_t)wb }, NULL, FILE_BEGIN)) {
                        uprintf("Write error: Could not reset position - %s", WindowsErrorString());
                        goto out;
                    }
                } else {
                    ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
                    goto out;
                }
                Sleep(200);
            }
        }
        uprintfs("\r\n");
    } else if (img_report.compression_type != BLED_COMPRESSION_NONE && img_report.compression_type < BLED_COMPRESSION_MAX) {
        int64_t bled_ret;
        uprintf("Writing compressed image:");
        hSourceImage = CreateFileU(image_path, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hSourceImage == INVALID_HANDLE_VALUE) {
            uprintf("Could not open image '%s': %s", image_path, WindowsErrorString());
            ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
            goto out;
        }
        sec_buf = (uint8_t*)_mm_malloc(SelectedDrive.SectorSize, SelectedDrive.SectorSize);
        if (sec_buf == NULL) {
            ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
            uprintf("Could not allocate disk write buffer");
            goto out;
        }
        sec_buf_pos = 0;
        sec_written = 0;
        bled_cancel = 0;
        /* PORT: bled's cancel flag is an unsigned long, whereas ErrorStatus is a
         * 32-bit DWORD: update_progress() mirrors ErrorStatus into bled_cancel */
        bled_init(256 * KB, uprintf, NULL, sector_write, update_progress, NULL, &bled_cancel);
        bled_ret = bled_uncompress_with_handles(hSourceImage, hPhysicalDrive, img_report.compression_type);
        bled_exit();
        uprintfs("\r\n");
        if ((bled_ret >= 0) && (sec_buf_pos != 0)) {
            /* A disk image that doesn't end up on disk boundary should be a rare
             * enough case, so we dont bother checking the write operation and
             * just issue a notice about it in the log. */
            uprintf("Notice: Compressed image data didn't end on block boundary.");
            /* PORT: bled wrote through a duplicated descriptor, so the handle's
             * position must be set explicitly (upstream shares the offset) */
            memset(&sec_buf[sec_buf_pos], 0, SelectedDrive.SectorSize - sec_buf_pos);
            SetFilePointerEx(hPhysicalDrive, (LARGE_INTEGER){ .QuadPart = (int64_t)sec_written }, NULL, FILE_BEGIN);
            WriteFile(hPhysicalDrive, sec_buf, SelectedDrive.SectorSize, &write_size, NULL);
        }
        safe_mm_free(sec_buf);
        if ((bled_ret < 0) && (SCODE_CODE(ErrorStatus) != ERROR_CANCELLED)) {
            /* Unfortunately, different compression backends return different negative error codes */
            uprintf("Could not write compressed image: %lld", (long long)bled_ret);
            ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
            goto out;
        }
        CHECK_FOR_USER_CANCEL;
    } else {
        DWORD rsize[NUM_BUFFERS] = { 0 };
        int read_bufnum = 0, proc_bufnum = 1;
        BOOL is_vhd = (img_report.compression_type == IMG_COMPRESSION_VHD) ||
                      (img_report.compression_type == IMG_COMPRESSION_VHDX);

        if (img_report.compression_type == IMG_COMPRESSION_FFU) {
            uprintf("FFU images require Windows' dism and can not be written on Linux");
            ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
            goto out;
        }
        /* VHD/VHDX require mounting the image first (PORT: through vhdfile.c) */
        if (is_vhd) {
            if (VhdMountImageAndGetSize(image_path, &target_size) == NULL || target_size == 0)
                goto out;
            target_size = MIN(target_size, (uint64_t)SelectedDrive.DiskSize);
        } else {
            hSourceImage = CreateFileAsync(image_path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
            if (hSourceImage == NULL) {
                hSourceImage = INVALID_HANDLE_VALUE;
                uprintf("Could not open image '%s': %s", image_path, WindowsErrorString());
                ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
                goto out;
            }
        }

        /* Our buffer size must be a multiple of the sector size and *ALIGNED* to the sector size */
        buf_size = (DWORD)(((DD_BUFFER_SIZE + SelectedDrive.SectorSize - 1) / SelectedDrive.SectorSize) * SelectedDrive.SectorSize);
        buffer = (uint8_t*)_mm_malloc((size_t)buf_size * NUM_BUFFERS, SelectedDrive.SectorSize);
        if (buffer == NULL) {
            ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
            uprintf("Could not allocate disk write buffer");
            goto out;
        }

        /* Start the initial read */
        if (!is_vhd)
            ReadFileAsync(hSourceImage, &buffer[read_bufnum * buf_size], (DWORD)MIN(buf_size, target_size));

        rsize[proc_bufnum] = 1;     /* To avoid early loop exit */
        uprint_progress(0, 0);
        for (wb = 0; rsize[proc_bufnum] != 0; wb += rsize[proc_bufnum]) {
            /* 0. Update the progress */
            UpdateProgressWithInfo(OP_FORMAT, MSG_261, wb, target_size);
            uprint_progress(wb, target_size);

            if (wb >= target_size)
                break;

            /* 1. Wait for the current read operation to complete (and update the read size) */
            if (is_vhd) {
                int64_t r = VhdReadImage(&buffer[read_bufnum * buf_size], wb, (DWORD)MIN(buf_size, target_size - wb));
                if (r <= 0) {
                    uprintf("\r\nRead error: Could not read virtual disk data");
                    ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
                    goto out;
                }
                rsize[read_bufnum] = (DWORD)r;
            } else if ((!WaitFileAsync(hSourceImage, DRIVE_ACCESS_TIMEOUT)) ||
                (!GetSizeAsync(hSourceImage, &rsize[read_bufnum]))) {
                uprintf("\r\nRead error: %s", WindowsErrorString());
                ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
                goto out;
            }
            if (rsize[read_bufnum] == 0) {
                uprintf("\r\nRead error: Unexpected end of image");
                ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
                goto out;
            }

            /* 2. WriteFile fails unless the size is a multiple of sector size */
            if (rsize[read_bufnum] % SelectedDrive.SectorSize != 0) {
                DWORD aligned = (DWORD)CEILING_ALIGN(rsize[read_bufnum], SelectedDrive.SectorSize);
                if (aligned > buf_size)
                    goto out;
                memset(&buffer[read_bufnum * buf_size + rsize[read_bufnum]], 0, aligned - rsize[read_bufnum]);
                rsize[read_bufnum] = aligned;
            }

            /* 3. Switch to the next reading buffer */
            proc_bufnum = read_bufnum;
            read_bufnum = (read_bufnum + 1) % NUM_BUFFERS;

            /* 3. Launch the next asynchronous read operation, without reading past
             * the source or target sizes */
            if (!is_vhd && (wb + rsize[proc_bufnum] < target_size))
                ReadFileAsync(hSourceImage, &buffer[read_bufnum * buf_size],
                    (DWORD)MIN(buf_size, target_size - (wb + rsize[proc_bufnum])));

            /* 4. Synchronously write the current data buffer */
            for (i = 1; i <= WRITE_RETRIES; i++) {
                CHECK_FOR_USER_CANCEL;
                s = WriteFile(hPhysicalDrive, &buffer[proc_bufnum * buf_size], rsize[proc_bufnum], &write_size, NULL);
                if ((s) && (write_size == rsize[proc_bufnum]))
                    break;
                if (s)
                    uprintf("\r\nWrite error: Wrote %u bytes, expected %u bytes", write_size, rsize[proc_bufnum]);
                else
                    uprintf("\r\nWrite error at sector %llu: %s", (unsigned long long)(wb / SelectedDrive.SectorSize), WindowsErrorString());
                if (i < WRITE_RETRIES) {
                    uprintf("Retrying in %d seconds...", WRITE_TIMEOUT / 1000);
                    Sleep(WRITE_TIMEOUT);
                    if (!SetFilePointerEx(hPhysicalDrive, (LARGE_INTEGER){ .QuadPart = (int64_t)wb }, NULL, FILE_BEGIN)) {
                        uprintf("Write error: Could not reset position - %s", WindowsErrorString());
                        goto out;
                    }
                } else {
                    ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
                    goto out;
                }
                Sleep(200);
            }
            /* The last (sector padded) block may go past the image end */
            if (wb + rsize[proc_bufnum] >= target_size) {
                wb = target_size;
                break;
            }
        }
        uprintfs("\r\n");
    }
    RefreshDriveLayout(hPhysicalDrive);
    ret = TRUE;
out:
    if ((img_report.compression_type == IMG_COMPRESSION_VHD) || (img_report.compression_type == IMG_COMPRESSION_VHDX))
        VhdUnmountImage();
    if ((hSourceImage != INVALID_HANDLE_VALUE) && !bZeroDrive && (img_report.compression_type >= BLED_COMPRESSION_MAX ||
        img_report.compression_type == BLED_COMPRESSION_NONE))
        CloseFileAsync(hSourceImage);
    else
        safe_closehandle(hSourceImage);
    safe_mm_free(buffer);
    safe_mm_free(cmp_buffer);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Format thread                                                       */
/* ------------------------------------------------------------------ */

static BOOL wait_for_logical(DWORD DriveIndex, uint64_t PartitionOffset)
{
    /* The kernel may need a moment before the partition node appears */
    int i;
    for (i = 0; i < 50; i++) {
        char* name = GetLogicalName(DriveIndex, PartitionOffset, TRUE, TRUE);
        if (name != NULL && name[0] != 0 && access(name, F_OK) == 0)
            return TRUE;
        if (i < 5)    /* virtual drives need no waiting */
            break;
        Sleep(100);
    }
    return TRUE;    /* fall through: the format code can still use the raw handle */
}

DWORD WINAPI FormatThread(void* param)
{
    DWORD DriveIndex = (DWORD)(uintptr_t)param;
    BOOL r = FALSE;
    HANDLE hPhysicalDrive = INVALID_HANDLE_VALUE;
    HANDLE hLogicalVolume = INVALID_HANDLE_VALUE;
    char fs_name[32] = "";
    char* volume_name = NULL;
    uint8_t extra_partitions = 0;
    BOOL actual_lock_drive = lock_drive;

    task_number = 0;
    large_drive = (SelectedDrive.DiskSize > (1 * TB));
    if (large_drive)
        uprintf("Notice: Large drive detected (may produce short writes)");

    /* Clear any error that a previous operation (e.g. the analysis of an
     * image that turned out not to be an ISO) may have left behind */
    ErrorStatus = 0;

    /* Find out if we need to add any extra partitions */
    if ((boot_type == BT_IMAGE) && !write_as_image && HAS_PERSISTENCE(img_report) && persistence_size)
        extra_partitions |= XP_PERSISTENCE;
    if ((((boot_type == BT_IMAGE) && IS_EFI_BOOTABLE(img_report)) &&
         ((fs_type == FS_NTFS) || (fs_type == FS_EXFAT))) ||
        (boot_type == BT_UEFI_NTFS)) {
        extra_partitions |= XP_UEFI_NTFS;
        /* ...but only if we're not dealing with a Windows image in installer
         * mode with target system set to BIOS and without dual BIOS+UEFI
         * boot enabled. */
        if ((boot_type == BT_IMAGE) && HAS_BOOTMGR_BIOS(img_report) &&
            (target_type == TT_BIOS) && (!allow_dual_uefi_bios))
            extra_partitions &= ~XP_UEFI_NTFS;
    }
    if (old_bios_fixes)
        extra_partitions |= XP_COMPAT;

    /* On Linux a single handle can span all the partitions */
    if ((extra_partitions & (XP_ESP | XP_PERSISTENCE)) || IS_EXT(fs_type))
        actual_lock_drive = FALSE;

    /* As upstream removes the drive letters and locks the volumes before
     * writing, release every volume of the drive (and, for an image file,
     * its loop devices): a stale mount would otherwise survive the format
     * and be reused, and exclusive access to a device would fail. */
    AltUnmountVolume(GetPhysicalName(DriveIndex), FALSE);

    PrintInfoDebug(0, MSG_225);
    hPhysicalDrive = GetPhysicalHandle(DriveIndex, actual_lock_drive, TRUE, !actual_lock_drive);
    if (hPhysicalDrive == INVALID_HANDLE_VALUE) {
        ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
        goto out;
    }

    if (!zero_drive && !write_as_image) {
        PrintInfoDebug(0, MSG_226);
        AnalyzeMBR(hPhysicalDrive, "Drive", FALSE);
        UpdateProgress(OP_ANALYZE_MBR, -1.0f);
    }

    if (zero_drive) {
        r = WriteDrive(hPhysicalDrive, TRUE);
        if (!r && !IS_ERROR(ErrorStatus))
            ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
        goto out;
    }

    /* Zap partition records. This may help prevent access errors. */
    if (boot_type != BT_IMAGE || !write_as_image) {
        if (!ClearMBRGPT(hPhysicalDrive, SelectedDrive.DiskSize, SelectedDrive.SectorSize) ||
            !InitializeDisk(hPhysicalDrive)) {
            uprintf("Could not reset the drive: %s", WindowsErrorString());
            ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
            goto out;
        }
        GetDrivePartitionData(SelectedDrive.DeviceNumber, fs_name, sizeof(fs_name), TRUE);
    }
    CHECK_FOR_USER_CANCEL;

    if (badblocks_check) {
        badblocks_report report = { 0 };
        int bb_r;
        do {
            FILE* log_fd;
            int sel = nb_passes_sel;
            char logfile[MAX_PATH], *bb_msg;
            const char* userdir = getenv("HOME");
            time_t now = time(NULL);
            struct tm lt;
            /* Create a log file for the bad blocks report, in the user's
             * home directory as upstream does (%USERPROFILE%) */
            localtime_r(&now, &lt);
            static_sprintf(logfile, "%s/rufus_%04d%02d%02d_%02d%02d%02d.log", (userdir != NULL) ? userdir : "/tmp",
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
            log_fd = fopen(logfile, "w+");
            if (log_fd == NULL) {
                uprintf("Error: Could not create log file for bad blocks check");
                goto out;
            }
            fprintf(log_fd, APPLICATION_NAME " bad blocks check started on: %04d.%02d.%02d %02d:%02d:%02d\n",
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
            fflush(log_fd);

            if (!BadBlocks(hPhysicalDrive, SelectedDrive.DiskSize, (sel >= 2) ? 4 : sel + 1, sel, &report, log_fd)) {
                uprintf("Bad blocks: Check failed.");
                if (!IS_ERROR(ErrorStatus))
                    ErrorStatus = RUFUS_ERROR(APPERR(ERROR_BADBLOCKS_FAILURE));
                ClearMBRGPT(hPhysicalDrive, SelectedDrive.DiskSize, SelectedDrive.SectorSize);
                fclose(log_fd);
                DeleteFileU(logfile);
                goto out;
            }
            uprintf("Bad Blocks: Check completed, %d bad block%s found. (%d/%d/%d errors)",
                report.bb_count, (report.bb_count == 1) ? "" : "s",
                report.num_read_errors, report.num_write_errors, report.num_corruption_errors);
            bb_r = IDOK;
            if (report.bb_count) {
                bb_msg = lmprintf(MSG_011, report.bb_count, report.num_read_errors, report.num_write_errors,
                    report.num_corruption_errors);
                fprintf(log_fd, "%s", bb_msg);
                now = time(NULL);
                localtime_r(&now, &lt);
                fprintf(log_fd, APPLICATION_NAME " bad blocks check ended on: %04d.%02d.%02d %02d:%02d:%02d\n",
                    lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
                fclose(log_fd);
                bb_r = Notification(MB_ABORTRETRYIGNORE | MB_ICONWARNING, lmprintf(MSG_010), lmprintf(MSG_012, bb_msg, logfile));
            } else {
                /* We didn't get any errors => delete the log file */
                fclose(log_fd);
                DeleteFileU(logfile);
            }
        } while (bb_r == IDRETRY);
        if (bb_r == IDABORT) {
            ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
            goto out;
        }

        /* Especially after destructive badblocks test, you must zero the MBR/GPT completely
         * before repartitioning. Else, all kind of bad things happen. */
        if (!ClearMBRGPT(hPhysicalDrive, SelectedDrive.DiskSize, SelectedDrive.SectorSize)) {
            uprintf("Could not zero MBR/GPT");
            if (!IS_ERROR(ErrorStatus))
                ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
            goto out;
        }
    }

    if ((boot_type == BT_IMAGE) && write_as_image) {
        if (!WriteDrive(hPhysicalDrive, FALSE))
            goto out;
        r = TRUE;
        goto out;
    }

    UpdateProgress(OP_ZERO_MBR, -1.0f);
    CHECK_FOR_USER_CANCEL;

    /* Create the partition layout (CreatePartition prints MSG_238 itself) */
    if (!CreatePartition(hPhysicalDrive, partition_type, fs_type,
        (partition_type == PARTITION_STYLE_MBR) && (target_type == TT_UEFI), extra_partitions)) {
        if (!IS_ERROR(ErrorStatus))
            ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
        goto out;
    }
    UpdateProgress(OP_PARTITION, -1.0f);
    GetDrivePartitionData(SelectedDrive.DeviceNumber, fs_name, sizeof(fs_name), FALSE);

    /* Format the Casper/persistence partition first, if any */
    if (extra_partitions & XP_PERSISTENCE) {
        int ext_ver = ReadSetting32(SETTING_USE_EXT_VERSION);
        if ((ext_ver < 2) || (ext_ver > 4))
            ext_ver = 3;
        uprintf("Using %s-like method to enable persistence", img_report.uses_casper ? "Ubuntu" : "Debian");
        if (!FormatPartition(DriveIndex, SelectedDrive.Partition[partition_index[PI_CASPER]].Offset, 0,
            FS_EXT2 + (ext_ver - 2),
            img_report.uses_casper ? "casper-rw" : "persistence",
            (img_report.uses_casper ? 0 : FP_CREATE_PERSISTENCE_CONF) | (quick_format ? FP_QUICK : 0))) {
            if (!IS_ERROR(ErrorStatus))
                ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
            goto out;
        }
    }

    /* The label global was populated by the front-end; apply it as upstream does */
    if (append_silent && strstr(label, " (SILENT)") == NULL)
        static_strcat(label, " (SILENT)");
    if (fs_type < FS_EXT2)
        ToValidLabel(label, (fs_type == FS_FAT16) || (fs_type == FS_FAT32) || (fs_type == FS_EXFAT));
    DWORD ClusterSize = (DWORD)cluster_size;
    if ((ClusterSize < 0x200) || (write_as_esp))
        ClusterSize = 0;    /* 0 = default cluster size */

    DWORD Flags = FP_FORCE;
    if (quick_format)
        Flags |= FP_QUICK;
    if ((fs_type == FS_NTFS) && (enable_ntfs_compression))
        Flags |= FP_COMPRESSION;
    if (write_as_esp)
        Flags |= FP_LARGE_FAT32;

    UpdateProgress(OP_FORMAT, -1.0f);
    r = FormatPartition(DriveIndex, SelectedDrive.Partition[partition_index[PI_MAIN]].Offset,
        ClusterSize, fs_type, label, Flags);
    if (!r) {
        uprintf("Format error: %s", StrError(ErrorStatus, TRUE));
        goto out;
    }

    /* Thanks to Microsoft, we must fix the MBR AFTER the drive has been formatted */
    if ((partition_type == PARTITION_STYLE_MBR) || ((boot_type != BT_NON_BOOTABLE) && (partition_type == PARTITION_STYLE_GPT))) {
        PrintInfoDebug(0, MSG_228);    /* "Writing master boot record..." */
        if ((!WriteMBR(hPhysicalDrive)) || (!WriteSBR(hPhysicalDrive))) {
            if (!IS_ERROR(ErrorStatus))
                ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
            goto out;
        }
        UpdateProgress(OP_FIX_MBR, -1.0f);
    }
    Sleep(200);

    if (!write_as_esp) {
        wait_for_logical(DriveIndex, SelectedDrive.Partition[partition_index[PI_MAIN]].Offset);
        /* Try to continue */
        CHECK_FOR_USER_CANCEL;

        volume_name = GetLogicalName(DriveIndex, SelectedDrive.Partition[partition_index[PI_MAIN]].Offset, TRUE, TRUE);
        if (volume_name == NULL || volume_name[0] == 0) {
            uprintf("Could not get volume name");
            ErrorStatus = RUFUS_ERROR(ERROR_NO_VOLUME_ID);
            goto out;
        }
        uprintf("Found volume %s", volume_name);
    }

    /* Refresh the drive label (upstream does this because Windows may have
     * altered it from the name we proposed) */
    if (!IS_EXT(fs_type) && volume_name != NULL) {
        char* lbl = NULL;
        if (GetDriveLabel(DriveIndex, NULL, &lbl, TRUE) && (lbl != NULL))
            safe_strcpy(img_report.usb_label, sizeof(img_report.usb_label), lbl);
    }

    if (boot_type != BT_NON_BOOTABLE) {
        if (boot_type == BT_UEFI_NTFS) {
            /* All good */
        } else if (target_type == TT_UEFI) {
            /* For once, no need to do anything - just check our sanity */
            if (!write_as_image && (boot_type == BT_IMAGE) && !IS_EFI_BOOTABLE(img_report)) {
                uprintf("The image does not appear to be UEFI-bootable");
                ErrorStatus = RUFUS_ERROR(ERROR_INSTALL_FAILURE);
                goto out;
            }
        } else if ((boot_type == BT_SYSLINUX_V4) || (boot_type == BT_SYSLINUX_V6) ||
                   ((boot_type == BT_IMAGE) && (HAS_SYSLINUX(img_report) || HAS_REACTOS(img_report)) &&
                    (!HAS_WINDOWS(img_report) || !allow_dual_uefi_bios))) {
            char* volume_path;
            /* The volume must be mounted for Syslinux to be able to create
             * 'ldlinux.sys' on it (upstream writes to "D:\\ldlinux.sys"
             * through the OS, which keeps the volume mounted) */
            volume_path = GetMountedPath(DriveIndex,
                SelectedDrive.Partition[partition_index[PI_MAIN]].Offset, FALSE);
            if (volume_path[0] == 0) {
                uprintf("Could not mount the target partition for the Syslinux installation");
                ErrorStatus = RUFUS_ERROR(ERROR_INSTALL_FAILURE);
                goto out;
            }
            if (!InstallSyslinux(DriveIndex, volume_path, fs_type)) {
                ErrorStatus = RUFUS_ERROR(ERROR_INSTALL_FAILURE);
                goto out;
            }
        } else {
            hLogicalVolume = GetLogicalHandle(DriveIndex, SelectedDrive.Partition[partition_index[PI_MAIN]].Offset, FALSE, TRUE, FALSE);
            if ((hLogicalVolume == INVALID_HANDLE_VALUE) || (hLogicalVolume == NULL)) {
                uprintf("Could not re-mount volume for partition boot record access");
                ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
                goto out;
            }
            PrintInfoDebug(0, MSG_229);
            if (!WritePBR(hLogicalVolume)) {
                if (!IS_ERROR(ErrorStatus))
                    ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
                goto out;
            }
            safe_closehandle(hLogicalVolume);
        }
    }
    CHECK_FOR_USER_CANCEL;

    /*
     * Copy the files that make the drive bootable (upstream: after the
     * RemountVolume() call). Upstream writes them through the drive letter
     * the volume is mounted on; the port mounts the partition through
     * udisks2 (regular users) or mount(2) (root) and writes through that
     * mount point, which is what 'drive_name' is below.
     */
    if ((boot_type != BT_NON_BOOTABLE) && (boot_type != BT_UEFI_NTFS) && !write_as_esp &&
        ((boot_type == BT_MSDOS) || (boot_type == BT_FREEDOS) || (boot_type == BT_GRUB4DOS) ||
         ((boot_type == BT_IMAGE) && (image_path[0] != 0) && (img_report.is_iso || img_report.is_windows_img)))) {
        char drive_name[MAX_PATH], dst[MAX_PATH], src[MAX_PATH];

        static_strcpy(drive_name, GetMountedPath(DriveIndex,
            SelectedDrive.Partition[partition_index[PI_MAIN]].Offset, FALSE));
        if (drive_name[0] == 0) {
            uprintf("Could not mount the target partition");
            ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_MOUNT_VOLUME));
            goto out;
        }
        CHECK_FOR_USER_CANCEL;

        if ((boot_type == BT_MSDOS) || (boot_type == BT_FREEDOS)) {
            UpdateProgress(OP_FILE_COPY, -1.0f);
            PrintInfoDebug(0, MSG_230);
            /* ExtractDOS() expects a path with a trailing separator */
            static_sprintf(dst, "%s/", drive_name);
            if (!ExtractDOS(dst)) {
                if (!IS_ERROR(ErrorStatus))
                    ErrorStatus = RUFUS_ERROR(ERROR_CANNOT_COPY);
                goto out;
            }
        } else if (boot_type == BT_GRUB4DOS) {
            static_sprintf(src, "%s/%s/grub4dos-%s/grldr", app_data_dir, FILES_DIR, GRUB4DOS_VERSION);
            static_sprintf(dst, "%s/grldr", drive_name);
            uprintf("Installing: %s (Grub4DOS loader) %s", dst, IsFileInDB(src) ? "✓" : "✗");
            if (!CopyFileU(src, dst, FALSE))
                uprintf("Failed to copy file: %s", WindowsErrorString());
        } else {
            UpdateProgress(OP_FILE_COPY, 0.0f);
            if (windows_to_go) {
                PrintInfoDebug(0, MSG_268);
                if (!SetupWinToGo(DriveIndex, drive_name, (extra_partitions & XP_ESP))) {
                    if (!IS_ERROR(ErrorStatus))
                        ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_EXTRACT));
                    goto out;
                }
                if (unattend_xml_path != NULL) {
                    if (!ApplyWindowsCustomization(drive_name, unattend_xml_flags | UNATTEND_WINDOWS_TO_GO))
                        ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
                }
            } else {
                uprintf("Extracting ISO image '%s' to '%s'", image_path, drive_name);
                if (!ExtractISO(image_path, drive_name, FALSE)) {
                    if (!IS_ERROR(ErrorStatus))
                        ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_EXTRACT));
                    goto out;
                }
                if (HAS_KOLIBRIOS(img_report)) {
                    static_sprintf(dst, "%s/MTLD_F32", drive_name);
                    uprintf("Installing: %s (KolibriOS loader)", dst);
                    if (ExtractISOFile(image_path, "HD_Load/USB_Boot/MTLD_F32", dst,
                        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) == 0) {
                        uprintf("WARNING: Loader installation failed - KolibriOS will not boot!");
                    }
                }
                /* EFI mode selected, with no 'boot###.efi' but Windows 7 x64's 'bootmgr.efi' (bit #0) */
                if (((target_type == TT_UEFI) || allow_dual_uefi_bios) && HAS_WIN7_EFI(img_report)) {
                    PrintInfo(0, MSG_232, lmprintf(MSG_307));
                    uprintf("Win7 EFI boot setup");
                    static_sprintf(dst, "%s/efi/boot", drive_name);
                    if (SHCreateDirectoryExU(NULL, dst, NULL) > ERROR_ALREADY_EXISTS) {
                        uprintf("Could not create directory '%s': %s", dst, WindowsErrorString());
                        ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
                    } else {
                        static_sprintf(src, "%s/%s", drive_name, img_report.wininst_path[0]);
                        static_strcat(dst, "/bootx64.efi");
                        if (!WimExtractFile(src, 1, "Windows/Boot/EFI/bootmgfw.efi", dst)) {
                            uprintf("Failed to setup Win7 EFI boot");
                            ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
                        }
                    }
                }
                if ((target_type == TT_BIOS) && HAS_WINPE(img_report)) {
                    /* Apply WinPE fixup */
                    if (!SetupWinPE(drive_name))
                        ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
                }
                if (unattend_xml_path != NULL) {
                    if (!ApplyWindowsCustomization(drive_name, unattend_xml_flags))
                        ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
                }
            }
        }

        UpdateProgress(OP_FINALIZE, -1.0f);
        PrintInfoDebug(0, MSG_233);
        if ((boot_type == BT_IMAGE) && (image_path[0] != 0) && (img_report.is_iso) && (!windows_to_go))
            UpdateMD5Sum(drive_name, md5sum_name[img_report.has_md5sum ? img_report.has_md5sum - 1 : 0]);
        if (extended_label)
            SetAutorun(drive_name);
        /* Copy any additonal files from an optional zip archive selected by the user */
        if (!IS_ERROR(ErrorStatus) && (archive_path != NULL) && (fs_type < FS_EXT2)) {
            UpdateProgress(OP_EXTRACT_ZIP, 0.0f);
            if (!ExtractZip(archive_path, drive_name) && !IS_ERROR(ErrorStatus))
                uprintf("WARNING: Could not copy additional files");
        }
        /* Make sure everything is on the media before releasing it */
        sync();
        AltUnmountVolume(drive_name, TRUE);
        /* NTFS fixup (WinPE/AIK images don't seem to boot without an extra checkdisk) */
        if ((boot_type == BT_IMAGE) && (img_report.is_iso) && (fs_type == FS_NTFS)) {
            CheckDisk(DriveIndex, SelectedDrive.Partition[partition_index[PI_MAIN]].Offset, FALSE);
            UpdateProgress(OP_FINALIZE, -1.0f);
        }
    } else {
        if (((boot_type == BT_NON_BOOTABLE) && extended_label && !IS_EXT(fs_type)) ||
            ((archive_path != NULL) && (fs_type < FS_EXT2) && !write_as_image)) {
            char mp[MAX_PATH];
            static_strcpy(mp, GetMountedPath(DriveIndex, SelectedDrive.Partition[partition_index[PI_MAIN]].Offset, FALSE));
            if ((mp[0] != 0) && (boot_type == BT_NON_BOOTABLE) && extended_label && !IS_EXT(fs_type))
                SetAutorun(mp);
            if ((mp[0] != 0) && (archive_path != NULL) && !IS_ERROR(ErrorStatus)) {
                UpdateProgress(OP_EXTRACT_ZIP, 0.0f);
                if (!ExtractZip(archive_path, mp) && !IS_ERROR(ErrorStatus))
                    uprintf("WARNING: Could not copy additional files");
            }
            sync();
        }
        UpdateProgress(OP_FINALIZE, -1.0f);
        PrintInfoDebug(0, MSG_233);
    }

out:
    if (hLogicalVolume != INVALID_HANDLE_VALUE)
        safe_closehandle(hLogicalVolume);
    safe_closehandle(hPhysicalDrive);
    /* PORT: Windows keeps the new volume mounted under its drive letter.
     * On Linux, release whatever we mounted (and the loop devices of virtual
     * drives) so that the media can be removed safely; the desktop's
     * automounter remounts it once automount is restored. */
    sync();
    AltUnmountVolume(GetPhysicalName(DriveIndex), TRUE);
    if (IS_ERROR(ErrorStatus))
        uprintf("Format failed: %s", StrError(ErrorStatus, TRUE));
    else if (r)
        PrintInfo(0, MSG_320, lmprintf(MSG_307));
    return r ? 0 : 1;
}

/*
 * Check disk. Upstream runs the Windows Chkdsk (fmifs.dll) on the NTFS volume
 * of WinPE/AIK images, which don't boot otherwise. The Linux equivalent for
 * an NTFS volume written through ntfs-3g/ntfs3 is 'ntfsfix', which resets
 * the journal and the volume dirty flag (-d), so that Windows considers the
 * volume clean. FAT and exFAT volumes are checked with their fsck tools.
 */
BOOL CheckDisk(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bSilent)
{
    char cmd[MAX_PATH + 64], *volume;
    const char* tool;
    DWORD ret;

    volume = GetLogicalName(DriveIndex, PartitionOffset, FALSE, TRUE);
    if ((volume == NULL) || (volume[0] == 0) || (strcmp(volume, GetPhysicalName(DriveIndex)) == 0)) {
        if (!bSilent)
            uprintf("Could not access the volume to check it - skipping");
        return FALSE;
    }
    switch (fs_type) {
    case FS_NTFS:  tool = "ntfsfix -d"; break;
    case FS_FAT16:
    case FS_FAT32: tool = "fsck.fat -a -w"; break;
    case FS_EXFAT: tool = "fsck.exfat -y"; break;
    default:
        return TRUE;
    }
    PrintInfoDebug(0, MSG_223);
    static_sprintf(cmd, "%s '%s'", tool, volume);
    uprintf("Running: %s", cmd);
    ret = RunCommand(cmd, NULL, TRUE);
    if (ret != 0 && !bSilent)
        uprintf("Check disk returned %lu", (unsigned long)ret);
    return (ret == 0);
}
