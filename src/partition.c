/*
 * Rufus Linux port: partition table creation.
 *
 * Direct port of upstream drive.c:CreatePartition(), which computes the
 * partition layout (offsets, sizes, roles, UEFI:NTFS content) and then asks
 * the storage stack to apply it. On Linux the layout is applied by writing
 * the MBR/GPT structures ourselves (parttable.c) and by asking the kernel to
 * re-read them (BLKRRPART).
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#include <string.h>
#include <unistd.h>
#include <wchar.h>

BOOL mbr_uefi_marker = FALSE;

/*
 * Zero the first 'size' bytes of a partition. This is needed because the
 * OS may try to read file system data at an old location even if the
 * partition was just deleted.
 */
static BOOL ClearPartition(HANDLE hDrive, uint64_t offset, DWORD size)
{
    BOOL r = FALSE;
    uint8_t* buffer = (uint8_t*)calloc(size, 1);

    if (buffer == NULL)
        return FALSE;

    if (!SetFilePointerEx(hDrive, (LARGE_INTEGER){ .QuadPart = (int64_t)offset }, NULL, FILE_BEGIN)) {
        free(buffer);
        return FALSE;
    }

    r = WriteFileWithRetry(hDrive, buffer, size, NULL, WRITE_RETRIES);
    free(buffer);
    return r;
}

static const char* partition_type_name(int partition_style)
{
    switch (partition_style) {
    case PARTITION_STYLE_MBR: return "MBR";
    case PARTITION_STYLE_GPT: return "GPT";
    default: return "SFD";
    }
}

/*
 * Create a partition table
 * See http://technet.microsoft.com/en-us/library/cc739412.aspx for some background info
 */
BOOL CreatePartition(HANDLE hDrive, int partition_style, int file_system, BOOL b_mbr_uefi_marker, uint8_t extra_partitions)
{
    const LONGLONG bytes_per_track = ((LONGLONG)SelectedDrive.SectorsPerTrack) * SelectedDrive.SectorSize;
    const DWORD size_to_clear = MAX_SECTORS_TO_CLEAR * SelectedDrive.SectorSize;
    uint8_t* buffer;
    uint64_t last_offset = SelectedDrive.DiskSize;
    size_t uefi_ntfs_size = 0;
    DWORD pi = 0, mi, i, bufsize;
    // Go for a 260 MB sized ESP by default to keep everyone happy, including 4K sector users:
    // https://docs.microsoft.com/en-us/windows-hardware/manufacture/desktop/configure-uefigpt-based-hard-drive-partitions
    // and folks using MacOS: https://github.com/pbatard/rufus/issues/979
    LONGLONG esp_size = 260 * MB, main_size;
    LONGLONG ClusterSize = (LONGLONG)cluster_size;

    PrintInfoDebug(0, MSG_238, partition_type_name(partition_style));

    mbr_uefi_marker = b_mbr_uefi_marker;

    if (ClusterSize == 0)
        ClusterSize = 0x200;

    if (partition_style == PARTITION_STYLE_SFD)
        // Nothing to do
        return TRUE;

    if (extra_partitions & XP_UEFI_NTFS) {
        uefi_ntfs_size = GetResourceSize(hMainInstance, MAKEINTRESOURCEA(IDR_UEFI_NTFS), _RT_RCDATA, "uefi-ntfs.img");
        if (uefi_ntfs_size == 0) {
            uprintf("Could not access embedded 'uefi-ntfs.img'");
            return FALSE;
        }
    }

    memset(partition_index, 0, sizeof(partition_index));
    memset(SelectedDrive.Partition, 0, sizeof(SelectedDrive.Partition));

    // Compute the starting offset of the first partition
    if ((partition_style == PARTITION_STYLE_GPT) || (!old_bios_fixes)) {
        // Go with the MS 1 MB wastage at the beginning...
        SelectedDrive.Partition[pi].Offset = 1 * MB;
    } else {
        // For 'Fixes for old BIOSes', align to a cylinder size that is itself
        // aligned to the cluster size (see upstream drive.c for the rationale)
        SelectedDrive.Partition[pi].Offset = CEILING_ALIGN(bytes_per_track, ClusterSize);
        // GRUB2 no longer fits in the usual 31½ KB that the above computation
        // provides so just unconditionally double that size
        SelectedDrive.Partition[pi].Offset *= 2;
    }

    // Having the ESP up front may help (and is the Microsoft recommended way) but this
    // is only achievable if we can mount more than one partition at once. Linux always
    // can, so behave like fixed media / Windows 10 1703 or later.
    if (extra_partitions & XP_ESP) {
        assert(partition_style == PARTITION_STYLE_GPT);
        partition_index[PI_ESP] = pi;
        wcscpy(SelectedDrive.Partition[pi].Name, L"EFI System Partition");
        SelectedDrive.Partition[pi].Size = esp_size;
        SelectedDrive.Partition[pi + 1].Offset = SelectedDrive.Partition[pi].Offset + SelectedDrive.Partition[pi].Size;
        // Align next partition to track and cluster
        SelectedDrive.Partition[pi + 1].Offset = CEILING_ALIGN(SelectedDrive.Partition[pi + 1].Offset, bytes_per_track);
        if (ClusterSize % SelectedDrive.SectorSize == 0)
            SelectedDrive.Partition[pi + 1].Offset = FLOOR_ALIGN(SelectedDrive.Partition[pi + 1].Offset, ClusterSize);
        assert(SelectedDrive.Partition[pi + 1].Offset >= SelectedDrive.Partition[pi].Offset + SelectedDrive.Partition[pi].Size);
        pi++;
        // Clear the extra partition we processed
        extra_partitions &= ~(XP_ESP);
    }

    // If required, set the MSR partition (GPT only - must be created before the data part)
    if (extra_partitions & XP_MSR) {
        assert(partition_style == PARTITION_STYLE_GPT);
        wcscpy(SelectedDrive.Partition[pi].Name, L"Microsoft Reserved Partition");
        SelectedDrive.Partition[pi].Size = 128 * MB;
        SelectedDrive.Partition[pi + 1].Offset = SelectedDrive.Partition[pi].Offset + SelectedDrive.Partition[pi].Size;
        SelectedDrive.Partition[pi + 1].Offset = CEILING_ALIGN(SelectedDrive.Partition[pi + 1].Offset, bytes_per_track);
        if (ClusterSize % SelectedDrive.SectorSize == 0)
            SelectedDrive.Partition[pi + 1].Offset = FLOOR_ALIGN(SelectedDrive.Partition[pi + 1].Offset, ClusterSize);
        assert(SelectedDrive.Partition[pi + 1].Offset >= SelectedDrive.Partition[pi].Offset + SelectedDrive.Partition[pi].Size);
        pi++;
        extra_partitions &= ~(XP_MSR);
    }

    // Reserve an entry for the main partition
    partition_index[PI_MAIN] = pi++;
    // Shorthand for the main index.
    mi = partition_index[PI_MAIN];
    wcscpy(SelectedDrive.Partition[mi].Name, write_as_esp ? L"EFI System Partition" : L"Main Data Partition");

    if (extra_partitions) {
        // Adjust the size according to extra partitions (which we always align to a track)
        if (extra_partitions & XP_PERSISTENCE) {
            assert(persistence_size != 0);
            partition_index[PI_CASPER] = pi;
            wcscpy(SelectedDrive.Partition[pi].Name, L"Linux Persistence");
            SelectedDrive.Partition[pi++].Size = CEILING_ALIGN(persistence_size, bytes_per_track);
        }
        if (extra_partitions & XP_ESP) {
            partition_index[PI_ESP] = pi;
            wcscpy(SelectedDrive.Partition[pi].Name, L"EFI System Partition");
            SelectedDrive.Partition[pi++].Size = CEILING_ALIGN(esp_size, bytes_per_track);
        } else if (extra_partitions & XP_UEFI_NTFS) {
            partition_index[PI_UEFI_NTFS] = pi;
            wcscpy(SelectedDrive.Partition[pi].Name, L"UEFI:NTFS");
            SelectedDrive.Partition[pi++].Size = CEILING_ALIGN(uefi_ntfs_size, bytes_per_track);
        } else if (extra_partitions & XP_COMPAT) {
            wcscpy(SelectedDrive.Partition[pi].Name, L"BIOS Compatibility");
            SelectedDrive.Partition[pi++].Size = bytes_per_track;    // One track for the extra partition
        }
    }
    assert(pi <= MAX_PARTITIONS);
    if (pi > MAX_PARTITIONS)
        return FALSE;

    // Compute the offsets of the extra partitions (which we always align to a track)
    last_offset = SelectedDrive.DiskSize;
    if (partition_style == PARTITION_STYLE_GPT)
        last_offset -= 33ULL * SelectedDrive.SectorSize;
    for (i = pi - 1; i > mi; i--) {
        assert(SelectedDrive.Partition[i].Size < last_offset);
        SelectedDrive.Partition[i].Offset = FLOOR_ALIGN(last_offset - SelectedDrive.Partition[i].Size, bytes_per_track);
        last_offset = SelectedDrive.Partition[i].Offset;
    }

    // With the above, compute the main partition size (which we align to a track)
    main_size = last_offset - SelectedDrive.Partition[mi].Offset;
    assert(main_size > 0);
    if (write_as_esp)
        main_size = MIN(main_size, MAX_ISO_TO_ESP_SIZE);
    SelectedDrive.Partition[mi].Size = FLOOR_ALIGN(main_size, bytes_per_track);
    // Try to make sure that the main partition size is a multiple of the cluster size
    if (ClusterSize % SelectedDrive.SectorSize == 0)
        SelectedDrive.Partition[mi].Size = FLOOR_ALIGN(SelectedDrive.Partition[mi].Size, ClusterSize);
    if (SelectedDrive.Partition[mi].Size <= 0) {
        uprintf("Error: Invalid %ls size", SelectedDrive.Partition[mi].Name);
        return FALSE;
    }

    // Build the layout
    SelectedDrive.nPartitions = (int)pi;
    for (i = 0; i < pi; i++) {
        uprintf("● Creating %ls%s (offset: %lld, size: %s)", SelectedDrive.Partition[i].Name,
            (wcsstr(SelectedDrive.Partition[i].Name, L"Partition") == NULL) ? " Partition" : "",
            (long long)SelectedDrive.Partition[i].Offset,
            SizeToHumanReadable(SelectedDrive.Partition[i].Size, TRUE, FALSE));
        // Zero the first sectors of the partition to avoid file system caching issues
        if (!ClearPartition(hDrive, SelectedDrive.Partition[i].Offset,
            (DWORD)MIN(size_to_clear, SelectedDrive.Partition[i].Size)))
            uprintf("Could not zero %ls: %s", SelectedDrive.Partition[i].Name, WindowsErrorString());
    }

    // We need to write the UEFI:NTFS partition before we refresh the disk
    if (extra_partitions & XP_UEFI_NTFS) {
        uprintf("Writing UEFI:NTFS data...");
        if (!SetFilePointerEx(hDrive,
                (LARGE_INTEGER){ .QuadPart = (int64_t)SelectedDrive.Partition[partition_index[PI_UEFI_NTFS]].Offset },
                NULL, FILE_BEGIN)) {
            uprintf("  Could not set position");
            return FALSE;
        }
        buffer = GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_UEFI_NTFS), _RT_RCDATA,
            "uefi-ntfs.img", &bufsize, FALSE);
        if (buffer == NULL) {
            uprintf("  Could not access source image");
            return FALSE;
        }
        if (!WriteFileWithRetry(hDrive, buffer, bufsize, NULL, WRITE_RETRIES)) {
            uprintf("  Write error: %s", WindowsErrorString());
            return FALSE;
        }
    }

    CHECK_FOR_USER_CANCEL;

    // Write the partition table itself
    if (!write_partition_table(hDrive, partition_style)) {
        uprintf("Could not set drive layout: %s", WindowsErrorString());
        return FALSE;
    }

    if (!RefreshDriveLayout(hDrive))
        return FALSE;

    return TRUE;

out:
    /* Reached through CHECK_FOR_USER_CANCEL */
    return FALSE;
}

/*
 * Initialize (reset) a disk for partitioning: upstream uses
 * IOCTL_DISK_CREATE_DISK with a RAW partition style; on Linux the equivalent
 * is to zap the first sector and ask the kernel to drop its partition view.
 */
BOOL InitializeDisk(HANDLE hDrive)
{
    uint8_t sector[MAX_SECTOR_SIZE];

    if (SelectedDrive.SectorSize > MAX_SECTOR_SIZE)
        return FALSE;
    memset(sector, 0, sizeof(sector));
    if (!SetFilePointerEx(hDrive, (LARGE_INTEGER){ .QuadPart = 0 }, NULL, FILE_BEGIN) ||
        !WriteFileWithRetry(hDrive, sector, SelectedDrive.SectorSize, NULL, WRITE_RETRIES))
        return FALSE;
    RefreshDriveLayout(hDrive);
    return TRUE;
}
