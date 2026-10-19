/*
 * Rufus Linux port: MBR and GPT partition table writers.
 *
 * On Windows, Rufus hands its computed layout to the storage stack
 * (IOCTL_DISK_SET_DRIVE_LAYOUT_EX) and lets the driver write the table.
 * Linux has no such call, so the port writes the MBR/GPT structures
 * directly. The layout itself (offsets, sizes, type selection, disk GUID)
 * is computed exactly as upstream drive.c:CreatePartition() does.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define GPT_ENTRIES             128
#define GPT_ENTRY_SIZE          128
#define GPT_HEADER_SIZE         92
#define GPT_REVISION            0x00010000
#define MBR_PARTITION_TABLE     0x1be

/* ------------------------------------------------------------------ */
/* CRC32 (IEEE 802.3, as used by GPT) - same polynomial as zlib's      */
/* ------------------------------------------------------------------ */
static uint32_t crc32_table[256];
static BOOL crc32_table_ready = FALSE;

static void crc32_init(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = TRUE;
}

static uint32_t crc32_ieee(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    if (!crc32_table_ready)
        crc32_init();
    for (i = 0; i < len; i++)
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* GUID helpers                                                        */
/* ------------------------------------------------------------------ */
static void random_guid(uint8_t* out)
{
    /* RFC 4122 version 4 UUID, little-endian per field (GUID on-disk form) */
    uint8_t b[16];
    FILE* f = fopen("/dev/urandom", "rb");
    if (f == NULL || fread(b, 1, sizeof(b), f) != sizeof(b)) {
        time_t t = time(NULL);
        srand((unsigned)t ^ (unsigned)getpid());
        for (int i = 0; i < 16; i++)
            b[i] = (uint8_t)rand();
    }
    if (f != NULL)
        fclose(f);
    b[7] = (uint8_t)((b[7] & 0x0F) | 0x40);    /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);    /* variant 1 */
    out[0] = b[3]; out[1] = b[2]; out[2] = b[1]; out[3] = b[0];
    out[4] = b[5]; out[5] = b[4];
    out[6] = b[7]; out[7] = b[6];
    memcpy(out + 8, b + 8, 8);
}

/* Express a GUID (host form) in on-disk little-endian form */
static void guid_to_bytes(const GUID* guid, uint8_t* out)
{
    out[0] = (uint8_t)(guid->Data1 & 0xFF);
    out[1] = (uint8_t)((guid->Data1 >> 8) & 0xFF);
    out[2] = (uint8_t)((guid->Data1 >> 16) & 0xFF);
    out[3] = (uint8_t)((guid->Data1 >> 24) & 0xFF);
    out[4] = (uint8_t)(guid->Data2 & 0xFF);
    out[5] = (uint8_t)((guid->Data2 >> 8) & 0xFF);
    out[6] = (uint8_t)(guid->Data3 & 0xFF);
    out[7] = (uint8_t)((guid->Data3 >> 8) & 0xFF);
    memcpy(out + 8, guid->Data4, 8);
}

static const GUID* partition_role_guid(const wchar_t* name, uint64_t* attributes)
{
    *attributes = 0;
    if (wcscmp(name, L"EFI System Partition") == 0)
        return &PARTITION_GENERIC_ESP;
    if (wcscmp(name, L"Microsoft Reserved Partition") == 0)
        return &PARTITION_MICROSOFT_RESERVED;
    if (wcscmp(name, L"Linux Persistence") == 0)
        return &PARTITION_LINUX_DATA;
    if (wcscmp(name, L"UEFI:NTFS") == 0) {
        /* Upstream: declared as MS Data (not ESP!) with the no-drive-letter
         * attribute, so that the Windows installer doesn't choke on two
         * ESPs on the same system */
        *attributes = GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER;
        return &PARTITION_MICROSOFT_DATA;
    }
    return &PARTITION_MICROSOFT_DATA;
}

/* ------------------------------------------------------------------ */
/* MBR                                                                 */
/* ------------------------------------------------------------------ */
static void chs_from_lba(uint32_t lba, uint8_t* chs)
{
    /* Classic Microsoft geometry: 255 heads, 63 sectors/track */
    uint32_t cylinder = lba / (255 * 63);
    uint32_t head = (lba / 63) % 255;
    uint32_t sector = (lba % 63) + 1;
    if (cylinder > 1023) {
        chs[0] = 0xFE; chs[1] = 0xFF; chs[2] = 0xFF;
        return;
    }
    chs[0] = (uint8_t)(head & 0xFF);
    chs[1] = (uint8_t)((sector & 0x3F) | ((cylinder >> 2) & 0xC0));
    chs[2] = (uint8_t)(cylinder & 0xFF);
}

static void mbr_set_entry(uint8_t* table, int index, uint8_t boot_flag, uint8_t type,
                          uint32_t start_lba, uint32_t sectors)
{
    uint8_t* e = table + index * 16;
    memset(e, 0, 16);
    e[0] = boot_flag;
    if (sectors > 0) {
        uint32_t end_lba = start_lba + sectors - 1;
        chs_from_lba(start_lba, e + 1);
        e[4] = type;
        chs_from_lba(end_lba, e + 5);
        e[8] = (uint8_t)(start_lba & 0xFF);
        e[9] = (uint8_t)((start_lba >> 8) & 0xFF);
        e[10] = (uint8_t)((start_lba >> 16) & 0xFF);
        e[11] = (uint8_t)((start_lba >> 24) & 0xFF);
        e[12] = (uint8_t)(sectors & 0xFF);
        e[13] = (uint8_t)((sectors >> 8) & 0xFF);
        e[14] = (uint8_t)((sectors >> 16) & 0xFF);
        e[15] = (uint8_t)((sectors >> 24) & 0xFF);
    }
}

/*
 * Write the MBR partition table computed by CreatePartition().
 * Boot code is written later by WriteMBR() (through ms-sys), which only
 * touches the first 440 bytes, so both operations compose.
 */
static BOOL write_mbr_table(HANDLE hDrive)
{
    uint8_t sector[MAX_SECTOR_SIZE];
    uint32_t disk_signature;
    uint64_t sector_size = SelectedDrive.SectorSize;
    int i, n = SelectedDrive.nPartitions;

    if (sector_size < 512 || sector_size > sizeof(sector)) {
        uprintf("Unsupported sector size %llu for MBR", (unsigned long long)sector_size);
        return FALSE;
    }
    memset(sector, 0, sizeof(sector));

    /* Disk signature (0x1B8): the Rufus UEFI marker when targetting UEFI
     * from an MBR layout, else a random/time based signature */
    disk_signature = mbr_uefi_marker ? MBR_UEFI_MARKER : (uint32_t)GetTickCount64();
    sector[0x1B8] = (uint8_t)(disk_signature & 0xFF);
    sector[0x1B9] = (uint8_t)((disk_signature >> 8) & 0xFF);
    sector[0x1BA] = (uint8_t)((disk_signature >> 16) & 0xFF);
    sector[0x1BB] = (uint8_t)((disk_signature >> 24) & 0xFF);

    for (i = 0; i < n && i < 4; i++) {
        const wchar_t* name = SelectedDrive.Partition[i].Name;
        uint8_t type;
        uint8_t boot_flag = 0;
        uint64_t start_lba = SelectedDrive.Partition[i].Offset / sector_size;
        uint64_t sectors = SelectedDrive.Partition[i].Size / sector_size;

        if (wcscmp(name, L"EFI System Partition") == 0 || wcscmp(name, L"UEFI:NTFS") == 0) {
            type = 0xef;
        } else if (wcscmp(name, L"Linux Persistence") == 0) {
            type = 0x83;
        } else if (wcscmp(name, L"BIOS Compatibility") == 0) {
            type = RUFUS_EXTRA_PARTITION_TYPE;
        } else {
            /* Main data partition: type follows the target file system */
            switch (fs_type) {
            case FS_FAT16:
                type = 0x0e;    /* FAT16 LBA */
                break;
            case FS_FAT32:
                type = 0x0c;    /* FAT32 LBA */
                break;
            case FS_NTFS:
            case FS_EXFAT:
            case FS_UDF:
            case FS_REFS:
                type = 0x07;
                break;
            case FS_EXT2:
            case FS_EXT3:
            case FS_EXT4:
                type = 0x83;
                break;
            default:
                uprintf("Unsupported file system");
                return FALSE;
            }
            if (boot_type != BT_NON_BOOTABLE)
                boot_flag = 0x80;
        }
        if (sectors > 0xFFFFFFFFull)
            sectors = 0xFFFFFFFFull;
        mbr_set_entry(sector + MBR_PARTITION_TABLE, i, boot_flag, type,
                      (uint32_t)start_lba, (uint32_t)sectors);
    }
    sector[510] = 0x55;
    sector[511] = 0xAA;

    if (!SetFilePointerEx(hDrive, (LARGE_INTEGER){ .QuadPart = 0 }, NULL, FILE_BEGIN) ||
        !WriteFileWithRetry(hDrive, sector, 512, NULL, WRITE_RETRIES)) {
        uprintf("Could not write MBR: %s", WindowsErrorString());
        return FALSE;
    }
    uprintf("Wrote MBR partition table (%d partition%s)", n, (n == 1) ? "" : "s");
    return TRUE;
}

/*
 * Write the GPT (protective MBR, primary and backup headers + entries),
 * mirroring what IOCTL_DISK_SET_DRIVE_LAYOUT_EX produces on Windows.
 */
static BOOL write_gpt_table(HANDLE hDrive)
{
    uint8_t* sector = NULL;
    uint8_t* entries = NULL;
    uint32_t sector_size = SelectedDrive.SectorSize;
    uint64_t disk_size = SelectedDrive.DiskSize;
    uint64_t total_sectors, last_lba, first_usable, last_usable;
    uint8_t disk_guid[16];
    uint32_t entries_crc, header_crc;
    int i, n = SelectedDrive.nPartitions;
    BOOL r = FALSE;

    if (sector_size < 512 || (sector_size & (sector_size - 1)) != 0) {
        uprintf("Unsupported sector size %u for GPT", sector_size);
        return FALSE;
    }
    total_sectors = disk_size / sector_size;
    if (total_sectors < 34 + 33) {
        uprintf("Drive is too small for a GPT layout");
        return FALSE;
    }
    last_lba = total_sectors - 1;
    first_usable = 34;                  /* 1 (PMBR) + 1 (header) + 32 (entries) */
    last_usable = last_lba - 33;        /* 32 entries + 1 header */

    sector = (uint8_t*)calloc(1, sector_size);
    entries = (uint8_t*)calloc(GPT_ENTRIES, GPT_ENTRY_SIZE);
    if (sector == NULL || entries == NULL) {
        uprintf("Could not allocate GPT structures");
        ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
        goto out;
    }

    random_guid(disk_guid);

    /* --- Partition entries --- */
    for (i = 0; i < n && i < GPT_ENTRIES; i++) {
        uint8_t* e = entries + i * GPT_ENTRY_SIZE;
        uint64_t attributes;
        const GUID* role = partition_role_guid(SelectedDrive.Partition[i].Name, &attributes);
        uint64_t first = SelectedDrive.Partition[i].Offset / sector_size;
        uint64_t last = first + SelectedDrive.Partition[i].Size / sector_size - 1;
        const wchar_t* name = SelectedDrive.Partition[i].Name;
        int j;

        guid_to_bytes(role, e);
        random_guid(e + 16);
        e[32] = (uint8_t)(first & 0xFF);
        e[33] = (uint8_t)((first >> 8) & 0xFF);
        e[34] = (uint8_t)((first >> 16) & 0xFF);
        e[35] = (uint8_t)((first >> 24) & 0xFF);
        e[36] = (uint8_t)((first >> 32) & 0xFF);
        e[37] = (uint8_t)((first >> 40) & 0xFF);
        e[38] = (uint8_t)((first >> 48) & 0xFF);
        e[39] = (uint8_t)((first >> 56) & 0xFF);
        e[40] = (uint8_t)(last & 0xFF);
        e[41] = (uint8_t)((last >> 8) & 0xFF);
        e[42] = (uint8_t)((last >> 16) & 0xFF);
        e[43] = (uint8_t)((last >> 24) & 0xFF);
        e[44] = (uint8_t)((last >> 32) & 0xFF);
        e[45] = (uint8_t)((last >> 40) & 0xFF);
        e[46] = (uint8_t)((last >> 48) & 0xFF);
        e[47] = (uint8_t)((last >> 56) & 0xFF);
        for (j = 0; j < 8; j++)
            e[48 + j] = (uint8_t)((attributes >> (8 * j)) & 0xFF);
        /* UTF-16LE name (wchar_t on Linux is UTF-32: keep the BMP as-is) */
        for (j = 0; j < 36 - 1 && name[j] != 0; j++) {
            e[56 + j * 2] = (uint8_t)(name[j] & 0xFF);
            e[56 + j * 2 + 1] = (uint8_t)((name[j] >> 8) & 0xFF);
        }
        uprintf("  Partition %d: first LBA %llu, last LBA %llu",
                i + 1, (unsigned long long)first, (unsigned long long)last);
    }
    entries_crc = crc32_ieee(entries, GPT_ENTRIES * GPT_ENTRY_SIZE);

    /* --- Protective MBR --- */
    {
        uint64_t pmbr_sectors = total_sectors - 1;
        if (pmbr_sectors > 0xFFFFFFFFull)
            pmbr_sectors = 0xFFFFFFFFull;
        mbr_set_entry(sector + MBR_PARTITION_TABLE, 0, 0, 0xEE, 1, (uint32_t)pmbr_sectors);
        sector[510] = 0x55;
        sector[511] = 0xAA;
        if (!SetFilePointerEx(hDrive, (LARGE_INTEGER){ .QuadPart = 0 }, NULL, FILE_BEGIN) ||
            !WriteFileWithRetry(hDrive, sector, 512, NULL, WRITE_RETRIES)) {
            uprintf("Could not write protective MBR: %s", WindowsErrorString());
            goto out;
        }
    }

    /* --- Primary GPT header (LBA 1) --- */
    memset(sector, 0, sector_size);
    memcpy(sector, "EFI PART", 8);
    sector[8] = 0x00; sector[9] = 0x00; sector[10] = 0x01; sector[11] = 0x00;   /* revision 1.0 */
    sector[12] = GPT_HEADER_SIZE & 0xFF;
    sector[13] = 0;                                                             /* header size = 92 */
    /* header CRC (bytes 16..19) is computed with that field zeroed */
    sector[24] = 0x01;                                                          /* current LBA = 1 */
    sector[32] = (uint8_t)(last_lba & 0xFF);
    sector[33] = (uint8_t)((last_lba >> 8) & 0xFF);
    sector[34] = (uint8_t)((last_lba >> 16) & 0xFF);
    sector[35] = (uint8_t)((last_lba >> 24) & 0xFF);
    sector[36] = (uint8_t)((last_lba >> 32) & 0xFF);
    sector[37] = (uint8_t)((last_lba >> 40) & 0xFF);
    sector[38] = (uint8_t)((last_lba >> 48) & 0xFF);
    sector[39] = (uint8_t)((last_lba >> 56) & 0xFF);
    sector[40] = (uint8_t)(first_usable & 0xFF);
    sector[41] = (uint8_t)((first_usable >> 8) & 0xFF);
    sector[42] = (uint8_t)((first_usable >> 16) & 0xFF);
    sector[43] = (uint8_t)((first_usable >> 24) & 0xFF);
    sector[44] = (uint8_t)((first_usable >> 32) & 0xFF);
    sector[45] = (uint8_t)((first_usable >> 40) & 0xFF);
    sector[46] = (uint8_t)((first_usable >> 48) & 0xFF);
    sector[47] = (uint8_t)((first_usable >> 56) & 0xFF);
    sector[48] = (uint8_t)(last_usable & 0xFF);
    sector[49] = (uint8_t)((last_usable >> 8) & 0xFF);
    sector[50] = (uint8_t)((last_usable >> 16) & 0xFF);
    sector[51] = (uint8_t)((last_usable >> 24) & 0xFF);
    sector[52] = (uint8_t)((last_usable >> 32) & 0xFF);
    sector[53] = (uint8_t)((last_usable >> 40) & 0xFF);
    sector[54] = (uint8_t)((last_usable >> 48) & 0xFF);
    sector[55] = (uint8_t)((last_usable >> 56) & 0xFF);
    memcpy(sector + 56, disk_guid, 16);
    sector[72] = 0x02;                                                          /* entries start at LBA 2 */
    sector[80] = GPT_ENTRIES & 0xFF;
    sector[81] = (GPT_ENTRIES >> 8) & 0xFF;
    sector[82] = (GPT_ENTRIES >> 16) & 0xFF;
    sector[83] = (GPT_ENTRIES >> 24) & 0xFF;
    sector[84] = GPT_ENTRY_SIZE & 0xFF;
    sector[85] = (GPT_ENTRY_SIZE >> 8) & 0xFF;
    sector[86] = (GPT_ENTRY_SIZE >> 16) & 0xFF;
    sector[87] = (GPT_ENTRY_SIZE >> 24) & 0xFF;
    sector[88] = (uint8_t)(entries_crc & 0xFF);
    sector[89] = (uint8_t)((entries_crc >> 8) & 0xFF);
    sector[90] = (uint8_t)((entries_crc >> 16) & 0xFF);
    sector[91] = (uint8_t)((entries_crc >> 24) & 0xFF);
    header_crc = crc32_ieee(sector, GPT_HEADER_SIZE);
    sector[16] = (uint8_t)(header_crc & 0xFF);
    sector[17] = (uint8_t)((header_crc >> 8) & 0xFF);
    sector[18] = (uint8_t)((header_crc >> 16) & 0xFF);
    sector[19] = (uint8_t)((header_crc >> 24) & 0xFF);

    if (!SetFilePointerEx(hDrive, (LARGE_INTEGER){ .QuadPart = sector_size }, NULL, FILE_BEGIN) ||
        !WriteFileWithRetry(hDrive, sector, sector_size, NULL, WRITE_RETRIES)) {
        uprintf("Could not write GPT header: %s", WindowsErrorString());
        goto out;
    }
    /* Entries at LBA 2 */
    if (!SetFilePointerEx(hDrive, (LARGE_INTEGER){ .QuadPart = 2 * (int64_t)sector_size }, NULL, FILE_BEGIN) ||
        !WriteFileWithRetry(hDrive, entries, GPT_ENTRIES * GPT_ENTRY_SIZE, NULL, WRITE_RETRIES)) {
        uprintf("Could not write GPT entries: %s", WindowsErrorString());
        goto out;
    }

    /* --- Backup entries (last_lba - 32) and header (last_lba) --- */
    if (!SetFilePointerEx(hDrive, (LARGE_INTEGER){ .QuadPart = (int64_t)((last_lba - 32) * sector_size) }, NULL, FILE_BEGIN) ||
        !WriteFileWithRetry(hDrive, entries, GPT_ENTRIES * GPT_ENTRY_SIZE, NULL, WRITE_RETRIES)) {
        uprintf("Could not write backup GPT entries: %s", WindowsErrorString());
        goto out;
    }
    {
        uint64_t primary_lba = 1;
        /* Backup header: same content, but current LBA and backup LBA swap */
        memset(sector + 16, 0, 4);
        sector[24] = (uint8_t)(last_lba & 0xFF);
        sector[25] = (uint8_t)((last_lba >> 8) & 0xFF);
        sector[26] = (uint8_t)((last_lba >> 16) & 0xFF);
        sector[27] = (uint8_t)((last_lba >> 24) & 0xFF);
        sector[28] = (uint8_t)((last_lba >> 32) & 0xFF);
        sector[29] = (uint8_t)((last_lba >> 40) & 0xFF);
        sector[30] = (uint8_t)((last_lba >> 48) & 0xFF);
        sector[31] = (uint8_t)((last_lba >> 56) & 0xFF);
        memset(sector + 32, 0, 8);
        sector[32] = (uint8_t)(primary_lba & 0xFF);
        /* backup entries LBA goes into bytes 72..79 */
        memset(sector + 72, 0, 8);
        {
            uint64_t backup_entries_lba = last_lba - 32;
            sector[72] = (uint8_t)(backup_entries_lba & 0xFF);
            sector[73] = (uint8_t)((backup_entries_lba >> 8) & 0xFF);
            sector[74] = (uint8_t)((backup_entries_lba >> 16) & 0xFF);
            sector[75] = (uint8_t)((backup_entries_lba >> 24) & 0xFF);
            sector[76] = (uint8_t)((backup_entries_lba >> 32) & 0xFF);
            sector[77] = (uint8_t)((backup_entries_lba >> 40) & 0xFF);
            sector[78] = (uint8_t)((backup_entries_lba >> 48) & 0xFF);
            sector[79] = (uint8_t)((backup_entries_lba >> 56) & 0xFF);
        }
        header_crc = crc32_ieee(sector, GPT_HEADER_SIZE);
        sector[16] = (uint8_t)(header_crc & 0xFF);
        sector[17] = (uint8_t)((header_crc >> 8) & 0xFF);
        sector[18] = (uint8_t)((header_crc >> 16) & 0xFF);
        sector[19] = (uint8_t)((header_crc >> 24) & 0xFF);
        if (!SetFilePointerEx(hDrive, (LARGE_INTEGER){ .QuadPart = (int64_t)(last_lba * sector_size) }, NULL, FILE_BEGIN) ||
            !WriteFileWithRetry(hDrive, sector, sector_size, NULL, WRITE_RETRIES)) {
            uprintf("Could not write backup GPT header: %s", WindowsErrorString());
            goto out;
        }
    }

    uprintf("Wrote GPT partition table (%d partition%s)", n, (n == 1) ? "" : "s");
    r = TRUE;

out:
    free(sector);
    free(entries);
    return r;
}

BOOL write_partition_table(HANDLE hDrive, int partition_style)
{
    if (partition_style == PARTITION_STYLE_GPT)
        return write_gpt_table(hDrive);
    return write_mbr_table(hDrive);
}
