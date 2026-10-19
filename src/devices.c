/*
 * Rufus Linux port: device layer.
 *
 * Replacement for upstream drive.c/dev.c: block device enumeration via
 * sysfs, partition table parsing (MBR/GPT), mount management, device
 * classification (USB / removable / fixed), MBR/PBR analysis through the
 * vendored ms-sys code, and the physical/logical handle entry points used
 * by the ported modules.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <mntent.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <linux/usbdevice_fs.h>

/* ms-sys boot record analysis (vendored, compiled unchanged) */
#include "file.h"
#include "br.h"
#include "fat12.h"
#include "fat16.h"
#include "fat32.h"
#include "partition_info.h"

#define SYSFS_BLOCK         "/sys/class/block"
#define SYSFS_USB           "/sys/bus/usb/devices"
#define ARRAYSIZE_(x)       (sizeof(x)/sizeof((x)[0]))

typedef struct {
    BOOL     valid;
    char     kernel[32];        /* "sdb" */
    char     path[MAX_PATH];    /* "/dev/sdb" or test image path */
    char     vendor[40];
    char     model[48];
    char     display_name[128];
    uint64_t size;              /* bytes */
    uint32_t sector_size;
    BOOL     removable;
    BOOL     usb;
    BOOL     optical;
    BOOL     is_file;           /* virtual drive backed by a file */
    BOOL     read_only;
    uint16_t vid, pid;
    char     usb_node[32];
} drive_entry_t;

static drive_entry_t drive_entries[MAX_DRIVES];
static int drive_count = 0;
static int selected_entry = -1;

/* Partition type info from the last GetDrivePartitionData() scan: the GPT
 * type GUID (raw, on-disk byte order) or the MBR type byte per partition */
static uint8_t part_type_guids[MAX_PARTITIONS][16];
static uint8_t mbr_partition_types[MAX_PARTITIONS];

/* Map an upstream DriveIndex (DRIVE_INDEX_MIN..DRIVE_INDEX_MAX) or a raw
 * 0-based position to an index into drive_entries[] */
static int pos_of(DWORD DriveIndex)
{
    int pos = (DriveIndex >= DRIVE_INDEX_MIN) ? (int)(DriveIndex - DRIVE_INDEX_MIN)
                                              : (int)DriveIndex;
    if (pos < 0 || pos >= drive_count)
        return -1;
    return pos;
}

static drive_entry_t* drive_entry_of(DWORD DriveIndex)
{
    int pos = pos_of(DriveIndex);
    return (pos < 0) ? NULL : &drive_entries[pos];
}

static DWORD drive_index_of(const drive_entry_t* e)
{
    return (DWORD)(DRIVE_INDEX_MIN + (DWORD)(e - drive_entries));
}

/* ------------------------------------------------------------------ */
/* Small sysfs helpers                                                 */
/* ------------------------------------------------------------------ */
static BOOL sysfs_read(const char* path, char* buf, size_t len)
{
    FILE* f = fopen(path, "r");
    if (f == NULL)
        return FALSE;
    if (fgets(buf, (int)len, f) == NULL) {
        fclose(f);
        return FALSE;
    }
    fclose(f);
    {   /* strip trailing whitespace */
        size_t l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == ' ' || buf[l-1] == '\t'))
            buf[--l] = 0;
    }
    return TRUE;
}

static uint64_t sysfs_read_u64(const char* path, uint64_t default_val)
{
    char buf[64];
    if (!sysfs_read(path, buf, sizeof(buf)))
        return default_val;
    return strtoull(buf, NULL, 0);
}

/* Read a link target of a sysfs node into buf */
static BOOL sysfs_readlink(const char* path, char* buf, size_t len)
{
    ssize_t r = readlink(path, buf, len - 1);
    if (r <= 0)
        return FALSE;
    buf[r] = 0;
    return TRUE;
}

static int compare_drives(const void* a, const void* b)
{
    const drive_entry_t* da = (const drive_entry_t*)a;
    const drive_entry_t* db = (const drive_entry_t*)b;
    /* Test file first, then USB/removable, then fixed; stable by name */
    if (da->is_file != db->is_file)
        return da->is_file ? -1 : 1;
    if (da->usb != db->usb)
        return da->usb ? -1 : 1;
    if (da->removable != db->removable)
        return da->removable ? -1 : 1;
    return strcmp(da->kernel, db->kernel);
}

/* Extract the USB device node (e.g. "1-4") from a sysfs path like
 * /sys/devices/pci0000:00/0000:00:14.0/usb1/1-4/1-4:1.0/host6/.../block/sdb */
static void usb_node_from_path(const char* syspath, char* node, size_t len)
{
    char buf[MAX_PATH * 2];
    char* p;
    node[0] = 0;
    snprintf(buf, sizeof(buf), "%s", syspath);
    if (strstr(buf, "/usb") == NULL)
        return;
    p = strstr(buf, "/usb");
    /* p points at "/usbX/1-4/..."; find next '/' after "usbN" */
    p = strchr(p + 1, '/');
    if (p == NULL)
        return;
    p++;
    {   /* take up to next '/' */
        char* end = strchr(p, '/');
        if (end != NULL)
            *end = 0;
        snprintf(node, len, "%s", p);
    }
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */
static BOOL is_whole_disk(const char* dir_path)
{
    char path[MAX_PATH * 2];
    struct stat st;
    snprintf(path, sizeof(path), "%s/partition", dir_path);
    if (stat(path, &st) == 0)
        return FALSE;   /* has partition attribute => it is a partition */
    return TRUE;
}

static BOOL is_skipped_device(const char* name)
{
    static const char* prefixes[] = {
        "loop", "ram", "zram", "dm-", "md", "sr", "fd", "nbd", "rbd", "vd"
    };
    size_t i;
    for (i = 0; i < ARRAYSIZE_(prefixes); i++) {
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0)
            return TRUE;
    }
    return FALSE;
}

BOOL GetDevices(DWORD devnum)
{
    DIR* dir;
    struct dirent* de;
    unsigned idx = 0; /* visit counter (devnum semantics from upstream) */

    ClearDrives();

    dir = opendir(SYSFS_BLOCK);
    if (dir == NULL)
        return FALSE;

    while ((de = readdir(dir)) != NULL && drive_count < MAX_DRIVES) {
        char dir_path[MAX_PATH * 2], tmp[MAX_PATH * 2], link[MAX_PATH * 2];
        drive_entry_t* e;

        if (de->d_name[0] == '.')
            continue;
        if (is_skipped_device(de->d_name))
            continue;
        snprintf(dir_path, sizeof(dir_path), SYSFS_BLOCK "/%s", de->d_name);
        if (!is_whole_disk(dir_path))
            continue;

        /* Upstream GetDevices(devnum) distinguishes full rescans from
         * single-device updates; the Linux layer always rescans. */
        idx++;
        (void)idx;
        (void)devnum;

        e = &drive_entries[drive_count];
        memset(e, 0, sizeof(*e));
        snprintf(e->kernel, sizeof(e->kernel), "%s", de->d_name);
        snprintf(e->path, sizeof(e->path), "/dev/%s", de->d_name);

        snprintf(tmp, sizeof(tmp), "%s/size", dir_path);
        e->size = sysfs_read_u64(tmp, 0) * 512ULL;
        if (e->size == 0)
            continue;

        snprintf(tmp, sizeof(tmp), "%s/queue/logical_block_size", dir_path);
        e->sector_size = (uint32_t)sysfs_read_u64(tmp, 512);
        snprintf(tmp, sizeof(tmp), "%s/removable", dir_path);
        e->removable = (sysfs_read_u64(tmp, 0) != 0);
        snprintf(tmp, sizeof(tmp), "%s/ro", dir_path);
        e->read_only = (sysfs_read_u64(tmp, 0) != 0);

        /* Resolve the device node to find out whether it is USB */
        if (sysfs_readlink(dir_path, link, sizeof(link))) {
            if (strstr(link, "/usb") != NULL) {
                e->usb = TRUE;
                usb_node_from_path(link, e->usb_node, sizeof(e->usb_node));
                if (e->usb_node[0] != 0) {
                    snprintf(tmp, sizeof(tmp), SYSFS_USB "/%s/idVendor", e->usb_node);
                    {
                        char vid[16];
                        if (sysfs_read(tmp, vid, sizeof(vid)))
                            e->vid = (uint16_t)strtoul(vid, NULL, 16);
                    }
                    snprintf(tmp, sizeof(tmp), SYSFS_USB "/%s/idProduct", e->usb_node);
                    {
                        char pid[16];
                        if (sysfs_read(tmp, pid, sizeof(pid)))
                            e->pid = (uint16_t)strtoul(pid, NULL, 16);
                    }
                }
            }
        }

        snprintf(tmp, sizeof(tmp), "%s/device/vendor", dir_path);
        sysfs_read(tmp, e->vendor, sizeof(e->vendor));
        snprintf(tmp, sizeof(tmp), "%s/device/model", dir_path);
        sysfs_read(tmp, e->model, sizeof(e->model));
        if (e->vendor[0] == 0)
            snprintf(e->vendor, sizeof(e->vendor), "%s", e->usb ? "USB" : "Unknown");
        if (e->model[0] == 0)
            snprintf(e->model, sizeof(e->model), "%s", e->kernel);
        snprintf(e->display_name, sizeof(e->display_name), "%s %s (%s)",
                 e->vendor, e->model, e->kernel);

        e->valid = TRUE;
        drive_count++;
    }
    closedir(dir);

    /* Test/virtual drive (regular file selected as --device) */
    if (test_mode && test_device_path[0] != 0 && drive_count < MAX_DRIVES) {
        drive_entry_t* e = &drive_entries[drive_count];
        struct stat st;
        memset(e, 0, sizeof(*e));
        if (stat(test_device_path, &st) == 0) {
            snprintf(e->path, sizeof(e->path), "%s", test_device_path);
            snprintf(e->kernel, sizeof(e->kernel), "%s", basename(test_device_path));
            e->size = (uint64_t)st.st_size;
            e->sector_size = 512;
            e->removable = TRUE;
            e->usb = FALSE;
            e->is_file = TRUE;
            snprintf(e->display_name, sizeof(e->display_name),
                     "Virtual drive (%s)", e->path);
            e->valid = TRUE;
            drive_count++;
        }
    }

    qsort(drive_entries, drive_count, sizeof(drive_entry_t), compare_drives);
    return TRUE;
}

void ClearDrives(void)
{
    memset(drive_entries, 0, sizeof(drive_entries));
    drive_count = 0;
    selected_entry = -1;
}

int GetDriveCount(void)
{
    return drive_count;
}

const char* GetDriveEntryPath(int index)
{
    if (index < 0 || index >= drive_count)
        return NULL;
    return drive_entries[index].path;
}

BOOL GetDriveEntryInfo(int index, uint64_t* size, BOOL* usb, BOOL* removable,
                       uint16_t* vid, uint16_t* pid, const char** name)
{
    drive_entry_t* e;
    if (index < 0 || index >= drive_count)
        return FALSE;
    e = &drive_entries[index];
    if (size) *size = e->size;
    if (usb) *usb = e->usb;
    if (removable) *removable = e->removable;
    if (vid) *vid = e->vid;
    if (pid) *pid = e->pid;
    if (name) *name = e->display_name;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Partition table parsing                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  boot;
    uint8_t  type;
    uint32_t start_lba;
    uint32_t sectors;
} mbr_entry_t;

static BOOL parse_mbr(const uint8_t* sector, mbr_entry_t entries[4])
{
    int i;
    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return FALSE;
    for (i = 0; i < 4; i++) {
        const uint8_t* e = sector + 0x1BE + i * 16;
        entries[i].boot = e[0];
        entries[i].type = e[4];
        entries[i].start_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                               ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        entries[i].sectors = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                             ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
    }
    return TRUE;
}

static BOOL parse_gpt(HANDLE h, uint32_t sector_size, uint64_t disk_size)
{
    uint8_t hdr[512];
    uint8_t* entries_buf = NULL;
    DWORD read = 0;
    uint64_t entries_lba, entries_count;
    uint32_t entry_size;
    int i, n = 0;
    BOOL r = FALSE;

    SetFilePointerEx(h, (LARGE_INTEGER){ .QuadPart = sector_size }, NULL, FILE_BEGIN);
    if (!ReadFile(h, hdr, 512, &read, NULL) || read != 512)
        return FALSE;
    if (memcmp(hdr, "EFI PART", 8) != 0)
        return FALSE;

    entries_lba = *(uint64_t*)(hdr + 72);
    entries_count = *(uint32_t*)(hdr + 80);
    entry_size = *(uint32_t*)(hdr + 84);
    if (entry_size < 128 || entry_size > 512)
        entry_size = 128;
    if (entries_count > 128)
        entries_count = 128;
    if (entries_count == 0)
        return FALSE;

    entries_buf = (uint8_t*)calloc(1, (size_t)(entries_count * entry_size));
    if (entries_buf == NULL)
        return FALSE;

    SetFilePointerEx(h, (LARGE_INTEGER){ .QuadPart = (int64_t)(entries_lba * sector_size) }, NULL, FILE_BEGIN);
    if (!ReadFile(h, entries_buf, (DWORD)(entries_count * entry_size), &read, NULL))
        goto out;

    for (i = 0; i < (int)(read / entry_size) && n < MAX_PARTITIONS; i++) {
        const uint8_t* e = entries_buf + i * entry_size;
        uint64_t first, last, sectors;
        static const uint8_t null_guid[16] = { 0 };
        if (memcmp(e, null_guid, 16) == 0)
            continue;
        first = *(uint64_t*)(e + 32);
        last = *(uint64_t*)(e + 40);
        sectors = (last >= first) ? (last - first + 1) : 0;
        SelectedDrive.Partition[n].Offset = first * sector_size;
        SelectedDrive.Partition[n].Size = sectors * sector_size;
        /* Partition name is UTF-16LE at offset 56, 72 bytes max */
        {
            const uint16_t* name16 = (const uint16_t*)(e + 56);
            wchar_t* out = SelectedDrive.Partition[n].Name;
            int j;
            for (j = 0; j < 35 && name16[j] != 0; j++)
                out[j] = (wchar_t)name16[j];
            out[j] = 0;
        }
        /* Record the partition type GUID so callers (ESP detection) can
         * match on it exactly */
        memcpy(part_type_guids[n], e, 16);
        n++;
    }
    SelectedDrive.nPartitions = n;
    SelectedDrive.PartitionStyle = PARTITION_STYLE_GPT;
    (void)disk_size;
    r = (n > 0);
out:
    free(entries_buf);
    return r;
}

BOOL GetDrivePartitionData(DWORD DriveIndex, char* FileSystemName, DWORD FileSystemNameSize, BOOL bSilent)
{
    HANDLE h;
    uint8_t sector[512];
    DWORD read = 0;
    int i;

    h = GetPhysicalHandle(DriveIndex, FALSE, FALSE, TRUE);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    SetFilePointerEx(h, (LARGE_INTEGER){ .QuadPart = 0 }, NULL, FILE_BEGIN);
    if (!ReadFile(h, sector, sizeof(sector), &read, NULL) || read != 512)
        goto out_fail;

    SelectedDrive.has_protective_mbr = FALSE;
    SelectedDrive.has_mbr_uefi_marker = FALSE;
    SelectedDrive.nPartitions = 0;
    SelectedDrive.PartitionStyle = PARTITION_STYLE_RAW;
    memset(part_type_guids, 0, sizeof(part_type_guids));
    memset(mbr_partition_types, 0, sizeof(mbr_partition_types));

    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        mbr_entry_t mbr[4];
        parse_mbr(sector, mbr);
        if (mbr[0].type == 0xEE)
            SelectedDrive.has_protective_mbr = TRUE;
        /* Rufus' MBR UEFI marker: 0x49464555 at offset 0xE8 of the
         * first 512 bytes (see ms-sys write_bootmark) */
        if (*(uint32_t*)(sector + 0x1B8) == 0x49464555)
            SelectedDrive.has_mbr_uefi_marker = TRUE;

        if (!parse_gpt(h, SelectedDrive.SectorSize, SelectedDrive.DiskSize)) {
            int n = 0;
            for (i = 0; i < 4; i++) {
                if (mbr[i].type == 0 || mbr[i].sectors == 0)
                    continue;
                SelectedDrive.Partition[n].Offset = (uint64_t)mbr[i].start_lba * 512;
                SelectedDrive.Partition[n].Size = (uint64_t)mbr[i].sectors * 512;
                mbstowcs(SelectedDrive.Partition[n].Name,
                         GetMBRPartitionType(mbr[i].type), 36);
                mbr_partition_types[n] = mbr[i].type;
                n++;
            }
            SelectedDrive.nPartitions = n;
            if (n > 0)
                SelectedDrive.PartitionStyle = PARTITION_STYLE_MBR;
        }
    }

    /* Filesystem type of the first partition (used by the UI and checks) */
    if (FileSystemName != NULL && FileSystemNameSize > 0) {
        FileSystemName[0] = 0;
        if (SelectedDrive.nPartitions > 0) {
            HANDLE hp = GetLogicalHandle(DriveIndex, SelectedDrive.Partition[0].Offset,
                                         FALSE, FALSE, TRUE);
            if (hp != INVALID_HANDLE_VALUE) {
                probe_volume_fs(hp, FileSystemName, FileSystemNameSize);
                CloseHandle(hp);
            }
        }
    }
    safe_closehandle(h);
    return TRUE;

out_fail:
    if (!bSilent)
        uprintf("Could not get partition data for drive %s: %s\n",
                GetHandlePath(h), WindowsErrorString());
    safe_closehandle(h);
    return FALSE;
}

/* Detect the filesystem in use at the start of a volume (offset-adjusted) */
void probe_volume_fs(HANDLE h, char* name, DWORD name_size)
{
    uint8_t buf[4096];
    DWORD read = 0;
    name[0] = 0;

    SetFilePointerEx(h, (LARGE_INTEGER){ .QuadPart = 0 }, NULL, FILE_BEGIN);
    if (!ReadFile(h, buf, sizeof(buf), &read, NULL) || read < 512)
        return;

    if (memcmp(buf + 3, "NTFS    ", 8) == 0) { snprintf(name, name_size, "NTFS"); return; }
    if (memcmp(buf + 3, "EXFAT   ", 8) == 0) { snprintf(name, name_size, "exFAT"); return; }
    if (memcmp(buf + 0x52, "FAT32   ", 8) == 0) { snprintf(name, name_size, "FAT32"); return; }
    if (memcmp(buf + 0x36, "FAT", 3) == 0 || memcmp(buf + 0x36, "FAT16   ", 8) == 0 ||
        memcmp(buf + 0x36, "FAT12   ", 8) == 0) { snprintf(name, name_size, "FAT"); return; }
    /* ext2/3/4: magic at 0x438 (+1024 from volume start) or 0x838 */
    if (buf[0x438] == 0x53 && buf[0x439] == 0xEF) { snprintf(name, name_size, "ext"); return; }
    if (buf[0x438] == 0x00 && buf[0x439] == 0x00 && read >= 1024 + 0x400 &&
        buf[0x838] == 0x53 && buf[0x839] == 0xEF) { snprintf(name, name_size, "ext"); return; }
    /* UDF volume recognition sequence at sector 16 */
    {
        HANDLE hd = h;
        uint8_t vrs[512];
        SetFilePointerEx(hd, (LARGE_INTEGER){ .QuadPart = 32768 }, NULL, FILE_BEGIN);
        if (ReadFile(hd, vrs, sizeof(vrs), &read, NULL) && read >= 5) {
            if (memcmp(vrs + 1, "BEA01", 5) == 0 || memcmp(vrs + 1, "NSR02", 5) == 0 ||
                memcmp(vrs + 1, "NSR03", 5) == 0) {
                snprintf(name, name_size, "UDF");
                return;
            }
        }
    }
    snprintf(name, name_size, "Unknown");
}

BOOL GetDriveLabel(DWORD DriveIndex, char* letters, char** label, BOOL bSilent)
{
    HANDLE h;
    uint8_t buf[4096];
    DWORD read = 0;
    static char volume_label[64];
    (void)letters;
    (void)bSilent;

    volume_label[0] = 0;
    if (label != NULL)
        *label = volume_label;
    h = GetLogicalHandle(DriveIndex, 0, FALSE, FALSE, TRUE);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    SetFilePointerEx(h, (LARGE_INTEGER){ .QuadPart = 0 }, NULL, FILE_BEGIN);
    if (ReadFile(h, buf, sizeof(buf), &read, NULL) && read >= 512) {
        if (memcmp(buf + 0x52, "FAT32   ", 8) == 0)
            snprintf(volume_label, sizeof(volume_label), "%.11s", buf + 0x47);
        else if (memcmp(buf + 0x36, "FAT", 3) == 0)
            snprintf(volume_label, sizeof(volume_label), "%.11s", buf + 0x2B);
        else if (buf[0x438] == 0x53 && buf[0x439] == 0xEF && read >= 1024 + 0x80)
            snprintf(volume_label, sizeof(volume_label), "%.16s", buf + 0x478);
    }
    CloseHandle(h);
    {   /* strip trailing spaces */
        size_t l = strlen(volume_label);
        while (l > 0 && volume_label[l-1] == ' ')
            volume_label[--l] = 0;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */
static void fill_selected_drive(const drive_entry_t* e)
{
    memset(&SelectedDrive, 0, sizeof(SelectedDrive));
    SelectedDrive.DiskSize = e->size;
    SelectedDrive.DeviceNumber = drive_index_of(e);
    SelectedDrive.SectorSize = e->sector_size;
    SelectedDrive.SectorsPerTrack = 63;
    SelectedDrive.MediaType = e->removable ? RemovableMedia : FixedMedia;
    SelectedDrive.PartitionStyle = PARTITION_STYLE_RAW;
    IsDriveRemovable = e->removable;
    IsDriveUSB = e->usb;
}

BOOL SelectDriveByIndex(DWORD index)
{
    int pos = pos_of(index);
    if (pos < 0)
        return FALSE;
    selected_entry = pos;
    DriveIndex = drive_index_of(&drive_entries[pos]);
    fill_selected_drive(&drive_entries[pos]);
    GetDrivePartitionData(DriveIndex, NULL, 0, TRUE);
    return TRUE;
}

/* Select a device by explicit path: matching enumerated entry or raw path
 * (regular file => virtual drive, block device => direct use). */
BOOL SelectDriveByPath(const char* path)
{
    int i;
    struct stat st;

    if (path == NULL || *path == 0)
        return FALSE;
    if (stat(path, &st) != 0)
        return FALSE;

    if (S_ISREG(st.st_mode)) {
        /* Virtual drive on a regular file */
        drive_entry_t* e;
        ClearDrives();
        e = &drive_entries[0];
        memset(e, 0, sizeof(*e));
        snprintf(e->path, sizeof(e->path), "%s", path);
        snprintf(e->kernel, sizeof(e->kernel), "%s", basename((char*)path));
        e->size = (uint64_t)st.st_size;
        e->sector_size = 512;
        e->removable = TRUE;
        e->is_file = TRUE;
        snprintf(e->display_name, sizeof(e->display_name), "Virtual drive (%s)", path);
        e->valid = TRUE;
        drive_count = 1;
        test_mode = TRUE;
        snprintf(test_device_path, sizeof(test_device_path), "%s", path);
        selected_entry = 0;
        DriveIndex = drive_index_of(e);
        fill_selected_drive(e);
        GetDrivePartitionData(DriveIndex, NULL, 0, TRUE);
        return TRUE;
    }

    /* Block device (or char device): try the enumerated list first */
    for (i = 0; i < drive_count; i++) {
        if (strcmp(drive_entries[i].path, path) == 0) {
            selected_entry = i;
            DriveIndex = drive_index_of(&drive_entries[i]);
            fill_selected_drive(&drive_entries[i]);
            GetDrivePartitionData(DriveIndex, NULL, 0, TRUE);
            return TRUE;
        }
    }
    /* Not in the list (e.g. an image loop device): enumerate, then fuzzy
     * match on the canonical path. */
    GetDevices(DRIVE_INDEX_MIN);
    for (i = 0; i < drive_count; i++) {
        char* resolved = realpath_dup(drive_entries[i].path);
        if (resolved != NULL && strcmp(resolved, path) == 0) {
            free(resolved);
            selected_entry = i;
            DriveIndex = drive_index_of(&drive_entries[i]);
            fill_selected_drive(&drive_entries[i]);
            GetDrivePartitionData(DriveIndex, NULL, 0, TRUE);
            return TRUE;
        }
        free(resolved);
    }
    return FALSE;
}

int GetSelectedDriveIndex(void)
{
    return selected_entry;
}

BOOL IsFilteredDrive(DWORD DriveIndex)
{
    /* Filter out the drive that holds the running system's root filesystem */
    static char root_disk[64] = "";
    FILE* f;
    struct mntent* me;
    drive_entry_t* e;

    if (root_disk[0] == 0) {
        f = setmntent("/proc/mounts", "r");
        if (f != NULL) {
            while ((me = getmntent(f)) != NULL) {
                if (strcmp(me->mnt_dir, "/") == 0) {
                    char tmp[MAX_PATH], *base;
                    char* resolved;
                    snprintf(tmp, sizeof(tmp), "%s", me->mnt_fsname);
                    resolved = realpath_dup(tmp);
                    if (resolved != NULL) {
                        snprintf(tmp, sizeof(tmp), "%s", resolved);
                        free(resolved);
                    }
                    base = basename(tmp);
                    /* Strip the partition suffix to get the whole disk:
                     * sda3 -> sda, nvme0n1p2 -> nvme0n1, mmcblk0p1 -> mmcblk0 */
                    {
                        size_t l = strlen(base);
                        while (l > 0 && isdigit((unsigned char)base[l - 1]))
                            base[--l] = 0;
                        if (l > 1 && base[l - 1] == 'p' && isdigit((unsigned char)base[l - 2]))
                            base[--l] = 0;
                    }
                    snprintf(root_disk, sizeof(root_disk), "%s", base);
                    break;
                }
            }
            endmntent(f);
        }
    }
    if (root_disk[0] == 0)
        return FALSE;
    e = drive_entry_of(DriveIndex);
    if (e == NULL)
        return FALSE;
    return (strcmp(e->kernel, root_disk) == 0);
}

BOOL IsUsbDevice(DWORD DriveIndex)
{
    drive_entry_t* e = drive_entry_of(DriveIndex);
    return (e != NULL) ? e->usb : FALSE;
}

BOOL IsFixedMedia(DWORD DriveIndex)
{
    drive_entry_t* e = drive_entry_of(DriveIndex);
    return (e != NULL) ? !e->removable : FALSE;
}

BOOL IsDriveUSBDevice(DWORD DriveIndex)
{
    return IsUsbDevice(DriveIndex);
}

UINT GetDriveTypeFromIndex(DWORD DriveIndex)
{
    drive_entry_t* e = drive_entry_of(DriveIndex);
    if (e == NULL)
        return DRIVE_UNKNOWN;
    if (e->optical)
        return DRIVE_CDROM;
    return e->removable ? DRIVE_REMOVABLE : DRIVE_FIXED;
}

uint64_t GetDriveSize(DWORD DriveIndex)
{
    drive_entry_t* e = drive_entry_of(DriveIndex);
    return (e != NULL) ? e->size : 0;
}

/*
 * Size of the partition starting at the given offset, as currently known to
 * the device layer. Falls back to querying the kernel (or the size of the
 * underlying file for image-based virtual drives).
 */
uint64_t GetPartitionSize(DWORD DriveIndex, uint64_t PartitionOffset)
{
    int i;
    for (i = 0; i < SelectedDrive.nPartitions; i++) {
        if (SelectedDrive.Partition[i].Offset == PartitionOffset)
            return SelectedDrive.Partition[i].Size;
    }
    {
        HANDLE h = GetLogicalHandle(DriveIndex, PartitionOffset, FALSE, FALSE, TRUE);
        LARGE_INTEGER li;
        if ((h != NULL) && (h != INVALID_HANDLE_VALUE) && GetFileSizeEx(h, &li) && (li.QuadPart > 0)) {
            CloseHandle(h);
            return (uint64_t)li.QuadPart;
        }
        safe_closehandle(h);
    }
    return 0;
}

BOOL IsMediaPresent(DWORD DriveIndex)
{
    drive_entry_t* e = drive_entry_of(DriveIndex);
    return (e != NULL) && (e->size > 0);
}

BOOL IsSourceImageLocatedOnTargetDrive(DWORD DriveIndex)
{
    BOOL r = FALSE;
    drive_entry_t* e = drive_entry_of(DriveIndex);
    char* resolved;

    if (e == NULL || image_path[0] == 0)
        return FALSE;
    resolved = realpath_dup(image_path);
    if (resolved != NULL) {
        FILE* f = setmntent("/proc/mounts", "r");
        struct mntent* me;
        if (f != NULL) {
            while ((me = getmntent(f)) != NULL) {
                char dev[MAX_PATH];
                snprintf(dev, sizeof(dev), "%s", me->mnt_fsname);
                if (strncmp(dev, e->path, strlen(e->path)) == 0) {
                    /* Mounted volume belongs to the target disk: check
                     * whether the image lives under its mountpoint */
                    if (strncmp(resolved, me->mnt_dir, strlen(me->mnt_dir)) == 0)
                        r = TRUE;
                }
            }
            endmntent(f);
        }
        free(resolved);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Handles                                                             */
/* ------------------------------------------------------------------ */
char* GetPhysicalName(DWORD DriveIndex)
{
    static char name[MAX_PATH];
    drive_entry_t* e = drive_entry_of(DriveIndex);
    if (e == NULL)
        return NULL;
    snprintf(name, sizeof(name), "%s", e->path);
    return name;
}

static BOOL unmount_path(const char* device_path, BOOL bSilent);

HANDLE GetPhysicalHandle(DWORD DriveIndex, BOOL bLockDrive, BOOL bWriteAccess, BOOL bWriteShare)
{
    HANDLE h;
    DWORD share;
    drive_entry_t* e = drive_entry_of(DriveIndex);

    if (e == NULL)
        return INVALID_HANDLE_VALUE;

    share = bWriteShare ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : 0;
    h = CreateFileU(e->path,
                    GENERIC_READ | (bWriteAccess ? GENERIC_WRITE : 0),
                    share, NULL, OPEN_EXISTING,
                    FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return h;
    if (bLockDrive)
        ; /* exclusive access is handled by O_EXCL in CreateFileU */
    if (e->size > 2ULL * TB)
        large_drive = TRUE;
    return h;
}

/* Find the partition device node for a given partition offset */
static BOOL find_partition_node(DWORD DriveIndex, uint64_t offset, char* out, size_t out_len)
{
    char dir_path[MAX_PATH * 2], tmp[MAX_PATH * 2], name[MAX_PATH];
    DIR* dir;
    struct dirent* de;
    drive_entry_t* e = drive_entry_of(DriveIndex);
    const char* base;

    out[0] = 0;
    if (e == NULL || e->is_file)
        return FALSE;
    base = e->kernel;
    dir = opendir(SYSFS_BLOCK);
    if (dir == NULL)
        return FALSE;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, base, strlen(base)) != 0)
            continue;
        if (strlen(de->d_name) == strlen(base))
            continue;   /* the disk itself */
        snprintf(dir_path, sizeof(dir_path), SYSFS_BLOCK "/%s", de->d_name);
        if (is_whole_disk(dir_path))
            continue;
        snprintf(tmp, sizeof(tmp), "%s/start", dir_path);
        if (sysfs_read_u64(tmp, UINT64_MAX) != offset / 512)
            continue;
        snprintf(name, sizeof(name), "/dev/%s", de->d_name);
        snprintf(out, out_len, "%s", name);
        closedir(dir);
        return TRUE;
    }
    closedir(dir);
    return FALSE;
}

HANDLE GetLogicalHandle(DWORD DriveIndex, uint64_t PartitionOffset,
                        BOOL bLockDrive, BOOL bWriteAccess, BOOL bWriteShare)
{
    HANDLE h;
    char node[MAX_PATH];
    DWORD share = bWriteShare ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : 0;

    if (drive_entry_of(DriveIndex) == NULL)
        return INVALID_HANDLE_VALUE;
    if (PartitionOffset == 0)
        return GetPhysicalHandle(DriveIndex, bLockDrive, bWriteAccess, bWriteShare);

    if (find_partition_node(DriveIndex, PartitionOffset, node, sizeof(node))) {
        return CreateFileU(node, GENERIC_READ | (bWriteAccess ? GENERIC_WRITE : 0),
                           share, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    }
    /* No partition node: use the whole device with a logical offset
     * (works for image files and for devices whose table has not been
     * re-read yet). */
    h = GetPhysicalHandle(DriveIndex, bLockDrive, bWriteAccess, bWriteShare);
    if (h != INVALID_HANDLE_VALUE)
        SetHandleOffset(h, PartitionOffset);
    return h;
}

HANDLE AltGetLogicalHandle(DWORD DriveIndex, uint64_t PartitionOffset,
                           BOOL bLockDrive, BOOL bWriteAccess, BOOL bWriteShare)
{
    return GetLogicalHandle(DriveIndex, PartitionOffset, bLockDrive, bWriteAccess, bWriteShare);
}

char* GetLogicalName(DWORD DriveIndex, uint64_t PartitionOffset,
                     BOOL bKeepTrailingSlash, BOOL bSilent)
{
    static char name[MAX_PATH];
    char node[MAX_PATH];
    drive_entry_t* e = drive_entry_of(DriveIndex);
    (void)bKeepTrailingSlash;
    (void)bSilent;
    name[0] = 0;
    if (e == NULL)
        return name;
    if (PartitionOffset == 0) {
        snprintf(name, sizeof(name), "%s", e->path);
        return name;
    }
    if (find_partition_node(DriveIndex, PartitionOffset, node, sizeof(node)))
        snprintf(name, sizeof(name), "%s", node);
    else
        snprintf(name, sizeof(name), "%s", e->path);
    return name;
}

char* AltGetLogicalName(DWORD DriveIndex, uint64_t PartitionOffset,
                        BOOL bKeepTrailingBackslash, BOOL bSilent)
{
    return GetLogicalName(DriveIndex, PartitionOffset, bKeepTrailingBackslash, bSilent);
}

/* ------------------------------------------------------------------ */
/* Mount management                                                    */
/* ------------------------------------------------------------------ */
static BOOL udisks_unmount(const char* node, BOOL bSilent);
static void release_created_loops(void);
static BOOL keep_created_loops = FALSE;

/* Unmount all volumes backed by 'device_path'. The path may be a device
 * node, a mount point, or a regular file used as a virtual drive (in which
 * case the loop devices backed by that file are unmounted). */
static BOOL unmount_path(const char* device_path, BOOL bSilent)
{
    FILE* f;
    struct mntent* me;
    char targets[16][MAX_PATH], sources[16][MAX_PATH], matches[8][MAX_PATH];
    int n = 0, nmatch = 0, i, retries;
    BOOL ret = TRUE;
    struct stat st;

    if ((device_path == NULL) || (device_path[0] == 0))
        return FALSE;

    snprintf(matches[nmatch++], sizeof(matches[0]), "%s", device_path);
    if ((stat(device_path, &st) == 0) && S_ISREG(st.st_mode)) {
        /* Virtual drive: also match every loop device backed by the file */
        char abs_path[PATH_MAX], path[MAX_PATH], backing[PATH_MAX];
        char* rp = realpath_dup(device_path);
        int l;

        snprintf(abs_path, sizeof(abs_path), "%s", (rp != NULL) ? rp : device_path);
        safe_free(rp);
        for (l = 0; (l < 256) && (nmatch < 8); l++) {
            FILE* bf;
            size_t ln;

            snprintf(path, sizeof(path), "/sys/block/loop%d/loop/backing_file", l);
            bf = fopen(path, "r");
            if (bf == NULL)
                continue;
            if (fgets(backing, sizeof(backing), bf) == NULL) {
                fclose(bf);
                continue;
            }
            fclose(bf);
            ln = strlen(backing);
            while (ln > 0 && (backing[ln - 1] == '\n' || backing[ln - 1] == '\r'))
                backing[--ln] = 0;
            if (strcmp(backing, abs_path) != 0 && strcmp(backing, device_path) != 0)
                continue;
            snprintf(matches[nmatch++], sizeof(matches[0]), "/dev/loop%d", l);
        }
    }

    f = setmntent("/proc/mounts", "r");
    if (f == NULL)
        return FALSE;
    while ((me = getmntent(f)) != NULL && n < 16) {
        for (i = 0; i < nmatch; i++) {
            if ((strncmp(me->mnt_fsname, matches[i], strlen(matches[i])) == 0) ||
                (strcmp(me->mnt_dir, matches[i]) == 0)) {
                snprintf(targets[n], sizeof(targets[n]), "%s", me->mnt_dir);
                snprintf(sources[n], sizeof(sources[n]), "%s", me->mnt_fsname);
                n++;
                break;
            }
        }
    }
    endmntent(f);

    for (i = 0; i < n; i++) {
        for (retries = 0; retries < 5; retries++) {
            if (umount2(targets[i], MNT_DETACH) == 0)
                break;
            if (errno == EINVAL || errno == ENOENT)
                break;
            /* Volumes mounted by the user (udisks, GNOME, ...) can't be
             * unmounted by us: ask udisks2 to do it on our behalf */
            if ((errno == EPERM) && udisks_unmount(sources[i], bSilent || (retries < 4)))
                break;
            if (!bSilent)
                uprintf("Could not unmount '%s': %s", targets[i], strerror(errno));
            Sleep(200);
        }
        if ((retries == 5) && (errno != EINVAL) && (errno != ENOENT))
            ret = FALSE;
    }

    /* Now that no volume uses them, get rid of the loop devices that we
     * had to set up ourselves */
    if (!keep_created_loops)
        release_created_loops();
    return ret;
}

BOOL AltUnmountVolume(const char* drive_name_str, BOOL bSilent)
{
    if (drive_name_str == NULL || *drive_name_str == 0)
        return FALSE;
    return unmount_path(drive_name_str, bSilent);
}

/* Port-only helper: make sure that the partition is mounted and return
 * its mount point without a trailing slash (the Linux equivalent of the
 * Windows "D:" drive name that upstream uses in FormatThread) */
char* GetMountedPath(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bSilent)
{
    static char ret[MAX_PATH];
    char* mp;

    ret[0] = 0;
    mp = AltMountVolume(DriveIndex, PartitionOffset, bSilent);
    if (mp == NULL || mp[0] == 0)
        return ret;
    snprintf(ret, sizeof(ret), "%s", mp);
    /* Strip the trailing slash: callers join with a separator of their
     * own, and paths like "//foo" don't match what upstream produces */
    while ((ret[0] != 0) && (ret[strlen(ret) - 1] == '/'))
        ret[strlen(ret) - 1] = 0;
    return ret;
}

BOOL UnmountVolume(HANDLE hDrive)
{
    const char* path = GetHandlePath(hDrive);
    if (path[0] == 0)
        return FALSE;
    return unmount_path(path, FALSE);
}

BOOL IsVolumeMounted(const char* device_path, char* mountpoint, size_t len)
{
    FILE* f = setmntent("/proc/mounts", "r");
    struct mntent* me;
    BOOL found = FALSE;
    if (f == NULL)
        return FALSE;
    while ((me = getmntent(f)) != NULL) {
        if (strcmp(me->mnt_fsname, device_path) == 0) {
            if (mountpoint != NULL && len > 0)
                snprintf(mountpoint, len, "%s", me->mnt_dir);
            found = TRUE;
            break;
        }
    }
    endmntent(f);
    return found;
}

/* ------------------------------------------------------------------ */
/* udisks2 helpers: allow mounting volumes as a regular user          */
/* ------------------------------------------------------------------ */
static BOOL udisks_mount(const char* node, char* mountpoint, size_t len, BOOL bSilent);
static BOOL udisks_unmount(const char* node, BOOL bSilent);

static BOOL run_capture(const char* cmd, char* out, size_t out_len)
{
    FILE* f = popen(cmd, "r");
    size_t total = 0;
    int status;

    if (out_len == 0)
        return FALSE;
    out[0] = 0;
    if (f == NULL)
        return FALSE;
    while (total + 1 < out_len) {
        size_t got = fread(out + total, 1, out_len - total - 1, f);
        if (got == 0)
            break;
        total += got;
    }
    out[total] = 0;
    status = pclose(f);
    return (status == 0);
}

/* Find an existing loop device backed by 'file' at 'offset' (readable
 * without root) */
/* Loop devices that we set up ourselves, so that they can be released
 * after the volumes backed by them have been unmounted */
static char created_loops[8][32];
static int created_loop_count = 0;

static void track_created_loop(const char* loop_dev)
{
    int i;
    for (i = 0; i < created_loop_count; i++)
        if (strcmp(created_loops[i], loop_dev) == 0)
            return;
    if (created_loop_count < 8)
        snprintf(created_loops[created_loop_count++], sizeof(created_loops[0]), "%s", loop_dev);
}

/* Release the loop devices that setup_loop_device() created. This must be
 * done after the volumes have been unmounted, as a busy loop can't be
 * detached. */
static void release_created_loops(void)
{
    int i;

    for (i = 0; i < created_loop_count; i++) {
        if (geteuid() == 0) {
            int lfd = open(created_loops[i], O_RDWR);
            if (lfd >= 0) {
                if (ioctl(lfd, LOOP_CLR_FD, 0) != 0)
                    duprintf("Could not release %s: %s", created_loops[i], strerror(errno));
                close(lfd);
            }
        } else {
            char cmd[128], out[256];
            /* A loop can be in high demand: don't hang forever on it */
            snprintf(cmd, sizeof(cmd), "timeout 10 udisksctl loop-delete --block-device %s 2>&1",
                     created_loops[i]);
            run_capture(cmd, out, sizeof(out));
        }
        duprintf("Released loop device %s", created_loops[i]);
    }
    created_loop_count = 0;
}

static const char* find_loop_for_file(const char* file, uint64_t offset, uint64_t size, char* buf, size_t len)
{
    char abs_path[PATH_MAX], path[MAX_PATH], backing[PATH_MAX], off_str[64], sz_str[64];
    uint64_t loop_offset, sizelimit, fsize = 0;
    struct stat st;
    char* rp;
    int i;

    rp = realpath_dup(file);
    snprintf(abs_path, sizeof(abs_path), "%s", (rp != NULL) ? rp : file);
    safe_free(rp);

    for (i = 0; i < 256; i++) {
        FILE* f;
        size_t n;
        snprintf(path, sizeof(path), "/sys/block/loop%d/loop/backing_file", i);
        f = fopen(path, "r");
        if (f == NULL)
            continue;
        if (fgets(backing, sizeof(backing), f) == NULL) {
            fclose(f);
            continue;
        }
        fclose(f);
        n = strlen(backing);
        while (n > 0 && (backing[n - 1] == '\n' || backing[n - 1] == '\r'))
            backing[--n] = 0;
        if (strcmp(backing, abs_path) != 0 && strcmp(backing, file) != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/block/loop%d/loop/offset", i);
        if (sysfs_read(path, off_str, sizeof(off_str))) {
            loop_offset = strtoull(off_str, NULL, 10);
            if (loop_offset != offset)
                continue;
        }
        /* The loop must also cover the whole requested range, or the
         * partition would appear truncated */
        snprintf(path, sizeof(path), "/sys/block/loop%d/loop/sizelimit", i);
        sizelimit = 0;
        if (sysfs_read(path, sz_str, sizeof(sz_str)))
            sizelimit = strtoull(sz_str, NULL, 10);
        if (sizelimit != 0) {
            if ((size != 0) && (sizelimit < size))
                continue;
            if ((size == 0) && (fsize == 0) && (stat(abs_path, &st) == 0) && S_ISREG(st.st_mode))
                fsize = (uint64_t)st.st_size;
            if ((size == 0) && (fsize > offset) && (sizelimit < fsize - offset))
                continue;
        }
        snprintf(buf, len, "/dev/loop%d", i);
        return buf;
    }
    return NULL;
}

/* Make sure a loop device exists for a byte range of a regular file
 * (root-free through udisks when possible) */
static BOOL setup_loop_device(const char* file, uint64_t offset, uint64_t size,
                              char* loop_dev, size_t len, BOOL bSilent)
{
    char cmd[MAX_PATH * 4], out[2048], *p;
    size_t n;

    if (find_loop_for_file(file, offset, size, loop_dev, len) != NULL)
        return TRUE;

    if (geteuid() != 0) {
        if (size != 0)
            snprintf(cmd, sizeof(cmd), "udisksctl loop-setup --file '%s' --offset %llu --size %llu 2>&1",
                     file, (unsigned long long)offset, (unsigned long long)size);
        else
            snprintf(cmd, sizeof(cmd), "udisksctl loop-setup --file '%s' --offset %llu 2>&1",
                     file, (unsigned long long)offset);
        if (!run_capture(cmd, out, sizeof(out))) {
            if (!bSilent)
                uprintf("Could not set up a loop device for '%s': %s", file, out);
            return FALSE;
        }
        p = strstr(out, "/dev/loop");
        if (p == NULL) {
            if (!bSilent)
                uprintf("Could not parse the loop device from: %s", out);
            return FALSE;
        }
        for (n = 0; n + 1 < len && p[n] != 0 && p[n] != ' ' && p[n] != '\n' && p[n] != '.'; n++)
            loop_dev[n] = p[n];
        loop_dev[n] = 0;
        track_created_loop(loop_dev);
        return TRUE;
    }

    /* Root: set up the loop device ourselves */
    {
        int ctl, lfd, ffd, nr;
        struct loop_info64 info;

        ctl = open("/dev/loop-control", O_RDWR);
        if (ctl < 0) {
            if (!bSilent)
                uprintf("Could not open /dev/loop-control: %s", strerror(errno));
            return FALSE;
        }
        nr = ioctl(ctl, LOOP_CTL_GET_FREE);
        close(ctl);
        if (nr < 0)
            return FALSE;
        snprintf(loop_dev, len, "/dev/loop%d", nr);
        lfd = open(loop_dev, O_RDWR);
        ffd = open(file, O_RDWR);
        if (lfd < 0 || ffd < 0) {
            if (lfd >= 0) close(lfd);
            if (ffd >= 0) close(ffd);
            return FALSE;
        }
        if (ioctl(lfd, LOOP_SET_FD, ffd) != 0) {
            if (!bSilent)
                uprintf("LOOP_SET_FD failed: %s", strerror(errno));
            close(lfd);
            close(ffd);
            return FALSE;
        }
        memset(&info, 0, sizeof(info));
        info.lo_offset = offset;
        if (size != 0)
            info.lo_sizelimit = size;
        ioctl(lfd, LOOP_SET_STATUS64, &info);
        close(lfd);
        close(ffd);
        track_created_loop(loop_dev);
    }
    return TRUE;
}

/* Wait for a device node to appear (udev can be slow) */
static BOOL wait_for_node(const char* node, int timeout_ms)
{
    int i;
    struct stat st;

    for (i = 0; i < timeout_ms / 100; i++) {
        if (stat(node, &st) == 0)
            return TRUE;
        Sleep(100);
    }
    return FALSE;
}

/* Mount a partition (by device node or via a loop device for image
 * targets) and return its mountpoint in a static buffer. */
/*
 * Block device node for a volume, for tools that need one (mkfs.*): the
 * partition node of a device or, for a virtual drive (image file), a loop
 * device over the partition, which is released by AltUnmountVolume().
 */
/* TRUE if the drive is an image file used as a virtual drive */
BOOL IsVirtualDrive(DWORD DriveIndex)
{
    drive_entry_t* e = drive_entry_of(DriveIndex);
    return (e != NULL) && e->is_file;
}

char* GetVolumeDevicePath(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bSilent)
{
    static char node[MAX_PATH];
    drive_entry_t* e = drive_entry_of(DriveIndex);
    uint64_t part_size = 0;
    int i;

    node[0] = 0;
    if (e == NULL)
        return node;
    if (find_partition_node(DriveIndex, PartitionOffset, node, sizeof(node)))
        return node;
    if (!e->is_file) {
        snprintf(node, sizeof(node), "%s", (PartitionOffset == 0) ? e->path : "");
        return node;
    }
    for (i = 0; i < SelectedDrive.nPartitions && i < MAX_PARTITIONS; i++) {
        if (SelectedDrive.Partition[i].Offset == PartitionOffset) {
            part_size = SelectedDrive.Partition[i].Size;
            break;
        }
    }
    if (!setup_loop_device(e->path, PartitionOffset, part_size, node, sizeof(node), bSilent) ||
        !wait_for_node(node, 5000)) {
        node[0] = 0;
        return node;
    }
    /* A desktop automounter may grab the new loop device: keep it free */
    keep_created_loops = TRUE;
    unmount_path(node, TRUE);
    keep_created_loops = FALSE;
    return node;
}

char* AltMountVolume(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bSilent)
{
    static char mountpoint[MAX_PATH];
    char node[MAX_PATH];
    drive_entry_t* e = drive_entry_of(DriveIndex);
    uint64_t part_size = 0;
    int r;

    mountpoint[0] = 0;
    if (e == NULL)
        return mountpoint;

    node[0] = 0;
    if (!find_partition_node(DriveIndex, PartitionOffset, node, sizeof(node))) {
        if (e->is_file) {
            char loop_dev[64];
            int i;

            for (i = 0; i < SelectedDrive.nPartitions && i < MAX_PARTITIONS; i++) {
                if (SelectedDrive.Partition[i].Offset == PartitionOffset) {
                    part_size = SelectedDrive.Partition[i].Size;
                    break;
                }
            }
            if (!setup_loop_device(e->path, PartitionOffset, part_size, loop_dev, sizeof(loop_dev), bSilent))
                return mountpoint;
            snprintf(node, sizeof(node), "%s", loop_dev);
            if (!wait_for_node(node, 5000)) {
                if (!bSilent)
                    uprintf("Loop device '%s' did not appear", node);
                return mountpoint;
            }
        } else {
            snprintf(node, sizeof(node), "%s", e->path);
        }
    }
    if (node[0] == 0)
        return mountpoint;

    /* Already mounted? */
    if (IsVolumeMounted(node, mountpoint, sizeof(mountpoint)))
        return mountpoint;

    if (geteuid() == 0) {
        snprintf(mountpoint, sizeof(mountpoint), "/run/media/rufus/%s",
                 (label[0] != 0) ? label : e->kernel);
        {   /* sanitize */
            char* p = mountpoint + strlen("/run/media/rufus/");
            for (; *p != 0; p++)
                if (*p == '/' || *p == ' ')
                    *p = '_';
        }
        mkdir("/run/media/rufus", 0755);
        mkdir(mountpoint, 0755);

        r = mount(node, mountpoint, "", MS_NOATIME, "");
        if (r == 0) {
            if (!bSilent)
                uprintf("Mounted '%s' on '%s'", node, mountpoint);
            return mountpoint;
        }
        if (!bSilent)
            uprintf("Could not mount '%s' on '%s': %s", node, mountpoint, strerror(errno));
        mountpoint[0] = 0;
    }

    /* Regular user: mount through udisks2 (polkit authorizes removable
     * media and disk images for the active session) */
    if (!udisks_mount(node, mountpoint, sizeof(mountpoint), bSilent))
        return mountpoint;
    if (!bSilent)
        uprintf("Mounted '%s' on '%s'", node, mountpoint);
    return mountpoint;
}

static BOOL udisks_mount(const char* node, char* mountpoint, size_t len, BOOL bSilent)
{
    char cmd[MAX_PATH * 4], out[2048], *p, *end;
    size_t n;

    mountpoint[0] = 0;
    snprintf(cmd, sizeof(cmd), "udisksctl mount --block-device '%s' 2>&1", node);
    if (!run_capture(cmd, out, sizeof(out))) {
        if (!bSilent)
            uprintf("Could not mount '%s' with udisks: %s", node, out);
        return FALSE;
    }
    p = strstr(out, " at ");
    if (p == NULL) {
        if (!bSilent)
            uprintf("Unexpected udisks output: %s", out);
        return FALSE;
    }
    p += 4;
    end = p + strcspn(p, "\r\n");
    n = (size_t)(end - p);
    while (n > 0 && (p[n - 1] == '.' || p[n - 1] == ' '))
        n--;
    if (n == 0 || n >= len)
        return FALSE;
    memcpy(mountpoint, p, n);
    mountpoint[n] = 0;
    return TRUE;
}

static BOOL udisks_unmount(const char* node, BOOL bSilent)
{
    char cmd[MAX_PATH * 4], out[2048];

    snprintf(cmd, sizeof(cmd), "udisksctl unmount --block-device '%s' 2>&1", node);
    if (!run_capture(cmd, out, sizeof(out))) {
        if (!bSilent)
            uprintf("Could not unmount '%s' with udisks: %s", node, out);
        return FALSE;
    }
    return TRUE;
}

BOOL RemountVolume(char* drive_name_str, BOOL bSilent)
{
    (void)drive_name_str;
    (void)bSilent;
    return TRUE;    /* nothing to do on Linux: mounts are handled per need */
}

BOOL MountVolume(char* drive_name_str, char* drive_guid)
{
    (void)drive_name_str;
    (void)drive_guid;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Layout / initialization                                             */
/* ------------------------------------------------------------------ */
BOOL RefreshDriveLayout(HANDLE hDrive)
{
    int retries, fd;
    struct stat st;

    if (!hDrive || hDrive == INVALID_HANDLE_VALUE)
        return FALSE;
    fd = GetHandleFD(hDrive);
    (void)st;
    /* FLUSH + BLKRRPART asks the kernel to re-read the partition table */
    for (retries = 0; retries < 10; retries++) {
        if (ioctl(fd, BLKRRPART) == 0)
            break;
        if (errno == EBUSY) {
            Sleep(500);
            continue;
        }
        break;  /* non-block devices return EINVAL/ENOTTY: fine */
    }
    ioctl(fd, BLKFLSBUF);
    return TRUE;
}

BOOL SetAutoMount(BOOL enable)
{
    /* Linux has no mount manager to disable; udisks may re-mount
     * volumes, which we handle by unmounting right before use. */
    (void)enable;
    return TRUE;
}

BOOL GetAutoMount(BOOL* enabled)
{
    if (enabled != NULL)
        *enabled = TRUE;
    return TRUE;
}

BOOL CycleDevice(int index)
{
    if (index < 0 || index >= drive_count)
        return FALSE;
    return CyclePort(index);
}

BOOL CyclePort(int index)
{
    char path[MAX_PATH * 2], buf[8];
    FILE* f;
    int fd;
    drive_entry_t* e = drive_entry_of((DWORD)index);

    if (e == NULL)
        return FALSE;
    if (e->usb_node[0] == 0)
        return FALSE;

    /* USB power cycle through the "authorized" sysfs attribute */
    snprintf(path, sizeof(path), SYSFS_USB "/%s/authorized", e->usb_node);
    f = fopen(path, "w");
    if (f == NULL) {
        /* Fallback: USBDEVFS_RESET through usbfs */
        char dev_path[64];
        snprintf(path, sizeof(path), SYSFS_USB "/%s/dev", e->usb_node);
        if (!sysfs_read(path, buf, sizeof(buf)))
            return FALSE;
        {
            unsigned bus, dev;
            if (sscanf(buf, "%u:%u", &bus, &dev) != 2)
                return FALSE;
            snprintf(dev_path, sizeof(dev_path), "/dev/bus/usb/%03u/%03u", bus, dev);
        }
        fd = open(dev_path, O_RDWR);
        if (fd < 0)
            return FALSE;
        if (ioctl(fd, USBDEVFS_RESET, 0) < 0) {
            close(fd);
            return FALSE;
        }
        close(fd);
        return TRUE;
    }
    fputs("0", f);
    fclose(f);
    Sleep(1000);
    f = fopen(path, "w");
    if (f == NULL)
        return FALSE;
    fputs("1", f);
    fclose(f);
    Sleep(1000);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* MBR/PBR analysis (ported from drive.c:1426/1444/1479)               */
/* ------------------------------------------------------------------ */
static const struct { int (*fn)(FILE* fp); char* str; } known_mbr[] = {
    { is_dos_mbr, "DOS/NT/95A" },
    { is_dos_f2_mbr, "DOS/NT/95A (F2)" },
    { is_95b_mbr, "Windows 95B/98/98SE/ME" },
    { is_2000_mbr, "Windows 2000/XP/2003" },
    { is_vista_mbr, "Windows Vista" },
    { is_win7_mbr, "Windows 7" },
    { is_rufus_mbr, "Rufus" },
    { is_syslinux_mbr, "Syslinux" },
    { is_isolinux_mbr, "Isolinux" },
    { is_reactos_mbr, "ReactOS" },
    { is_kolibrios_mbr, "KolibriOS" },
    { is_grub4dos_mbr, "Grub4DOS" },
    { is_grub2_mbr, "Grub 2.0" },
    { is_zero_mbr_not_including_disk_signature_or_copy_protect, "Zeroed" },
};

static const struct { int (*fn)(FILE* fp); char* str; } known_pbr[] = {
    { entire_fat_16_br_matches, "FAT16 DOS" },
    { entire_fat_16_fd_br_matches, "FAT16 FreeDOS" },
    { entire_fat_16_ros_br_matches, "FAT16 ReactOS" },
    { entire_fat_32_br_matches, "FAT32 DOS" },
    { entire_fat_32_nt_br_matches, "FAT32 NT" },
    { entire_fat_32_fd_br_matches, "FAT32 FreeDOS" },
    { entire_fat_32_ros_br_matches, "FAT32 ReactOS" },
    { entire_fat_32_kos_br_matches, "FAT32 KolibriOS" },
};

BOOL AnalyzeMBR(HANDLE hPhysicalDrive, const char* TargetName, BOOL bSilent)
{
    FAKE_FD fake_fd = { 0 };
    FILE* fp = (FILE*)&fake_fd;
    int i;

    fake_fd._handle = (char*)hPhysicalDrive;
    set_bytes_per_sector(SelectedDrive.SectorSize);

    if (!is_br(fp)) {
        if (!bSilent)
            uprintf("%s does not have a Boot Marker", TargetName);
        return FALSE;
    }
    for (i = 0; i < (int)ARRAYSIZE_(known_mbr); i++) {
        if (known_mbr[i].fn(fp)) {
            if (!bSilent)
                uprintf("%s has a %s Master Boot Record", TargetName, known_mbr[i].str);
            return TRUE;
        }
    }
    if (!bSilent)
        uprintf("%s has an unknown Master Boot Record", TargetName);
    return TRUE;
}

BOOL AnalyzePBR(HANDLE hLogicalVolume)
{
    FAKE_FD fake_fd = { 0 };
    FILE* fp = (FILE*)&fake_fd;
    int i;

    fake_fd._handle = (char*)hLogicalVolume;
    set_bytes_per_sector(SelectedDrive.SectorSize);

    if (!is_br(fp))
        return FALSE;
    if (is_fat_16_br(fp) || is_fat_32_br(fp)) {
        for (i = 0; i < (int)ARRAYSIZE_(known_pbr); i++) {
            if (known_pbr[i].fn(fp))
                return TRUE;
        }
    }
    return TRUE;
}

int64_t GetEspOffset(HANDLE hPhysical)
{
    /* Look for an ESP (MBR type 0xef / GPT ESP GUID) in the current
     * partition table of the selected drive. Upstream drive.c does the same
     * by comparing the partition type, which we recorded while parsing. */
    static const uint8_t esp_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
    };
    int i;
    (void)hPhysical;

    for (i = 0; i < SelectedDrive.nPartitions && i < MAX_PARTITIONS; i++) {
        if (SelectedDrive.PartitionStyle == PARTITION_STYLE_GPT) {
            if (memcmp(part_type_guids[i], esp_guid, 16) == 0)
                return (int64_t)SelectedDrive.Partition[i].Offset;
        } else if (SelectedDrive.PartitionStyle == PARTITION_STYLE_MBR) {
            if (mbr_partition_types[i] == 0xef)
                return (int64_t)SelectedDrive.Partition[i].Offset;
        }
    }
    return -1;
}

BOOL ToggleEsp(DWORD DriveIndex, uint64_t PartitionOffset)
{
    (void)DriveIndex;
    (void)PartitionOffset;
    return FALSE;   /* ported from drive.c: needs settings backend */
}

const char* GetFsName(HANDLE hPhysical, uint64_t StartingOffset)
{
    static char fs_name[32];
    HANDLE h = GetLogicalHandle(DriveIndex, StartingOffset, FALSE, FALSE, TRUE);
    (void)hPhysical;
    if (h == INVALID_HANDLE_VALUE)
        return "";
    probe_volume_fs(h, fs_name, sizeof(fs_name));
    CloseHandle(h);
    return fs_name;
}

const char* GetExtFsLabel(DWORD DriveIndex, uint64_t PartitionOffset)
{
    static char ext_label[32];
    HANDLE h = GetLogicalHandle(DriveIndex, PartitionOffset, FALSE, FALSE, TRUE);
    uint8_t buf[2048];
    DWORD read = 0;
    ext_label[0] = 0;
    if (h == INVALID_HANDLE_VALUE)
        return ext_label;
    /* ext superblock: offset 1024; s_volume_name at +0x78 */
    SetFilePointerEx(h, (LARGE_INTEGER){ .QuadPart = 1024 }, NULL, FILE_BEGIN);
    if (ReadFile(h, buf, sizeof(buf), &read, NULL) && read >= 0x88) {
        if (buf[0x38] == 0x53 && buf[0x39] == 0xEF)
            snprintf(ext_label, sizeof(ext_label), "%.16s", buf + 0x78);
    }
    CloseHandle(h);
    return ext_label;
}

char GetUnusedDriveLetter(void) { return 0; }

/* ------------------------------------------------------------------ */
/* Conflicting processes (upstream process.c GetProcessSearch())       */
/* ------------------------------------------------------------------ */
/*
 * Upstream enumerates the handles of every process (NtQuerySystemInformation)
 * to find the ones that access the target drive. On Linux, the equivalent
 * is /proc: the open file descriptors, current directory, root and memory
 * mappings of each process are checked against the drive's device nodes
 * (disk and partitions, or the image file and its loop devices) and the
 * mount points of its volumes. Access rights are reported like upstream
 * (r: read, w: write, x: executable/directory in use). Processes of other
 * users are only visible when running as root.
 */
#include <dirent.h>

#define MAX_DRIVE_PATHS 32

static int collect_drive_paths(drive_entry_t* e, char paths[MAX_DRIVE_PATHS][MAX_PATH], BOOL is_mount[MAX_DRIVE_PATHS])
{
    char tmp[MAX_PATH * 2], backing[PATH_MAX];
    int n = 0, l, k;
    DIR* d;
    struct dirent* de;
    FILE* f;
    struct mntent* me;

    snprintf(paths[n], MAX_PATH, "%s", e->path);
    is_mount[n++] = FALSE;
    if (e->is_file) {
        /* Loop devices backed by the image file */
        for (l = 0; l < 256 && n < MAX_DRIVE_PATHS; l++) {
            snprintf(tmp, sizeof(tmp), "/sys/block/loop%d/loop/backing_file", l);
            if (!sysfs_read(tmp, backing, sizeof(backing)) || strcmp(backing, e->path) != 0)
                continue;
            snprintf(paths[n], MAX_PATH, "/dev/loop%d", l);
            is_mount[n++] = FALSE;
        }
    } else {
        /* Partitions of the disk */
        snprintf(tmp, sizeof(tmp), SYSFS_BLOCK "/%s", e->kernel);
        d = opendir(tmp);
        while (d != NULL && (de = readdir(d)) != NULL && n < MAX_DRIVE_PATHS) {
            if (strncmp(de->d_name, e->kernel, strlen(e->kernel)) != 0)
                continue;
            snprintf(paths[n], MAX_PATH, "/dev/%s", de->d_name);
            is_mount[n++] = FALSE;
        }
        if (d != NULL)
            closedir(d);
    }
    /* Mount points of all these nodes */
    k = n;
    f = setmntent("/proc/mounts", "r");
    while (f != NULL && (me = getmntent(f)) != NULL && n < MAX_DRIVE_PATHS) {
        for (l = 0; l < k; l++) {
            size_t len = strlen(paths[l]);
            if (strncmp(me->mnt_fsname, paths[l], len) == 0 &&
                (me->mnt_fsname[len] == 0 || isdigit((unsigned char)me->mnt_fsname[len]) || me->mnt_fsname[len] == 'p')) {
                snprintf(paths[n], MAX_PATH, "%s", me->mnt_dir);
                is_mount[n++] = TRUE;
                break;
            }
        }
    }
    if (f != NULL)
        endmntent(f);
    return n;
}

static BOOL path_matches(const char* target, char paths[MAX_DRIVE_PATHS][MAX_PATH], BOOL is_mount[MAX_DRIVE_PATHS], int n)
{
    int i;
    for (i = 0; i < n; i++) {
        size_t len = strlen(paths[i]);
        if (is_mount[i]) {
            if (strncmp(target, paths[i], len) == 0 && (target[len] == 0 || target[len] == '/'))
                return TRUE;
        } else if (strcmp(target, paths[i]) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

BYTE GetProcessSearch(uint32_t timeout, uint8_t access_mask, BOOL bIgnoreStaleProcesses)
{
    const char* access_rights_str[8] = { "n", "r", "w", "rw", "x", "rx", "wx", "rwx" };
    char paths[MAX_DRIVE_PATHS][MAX_PATH], link[PATH_MAX], tmp[MAX_PATH], cmdline[512], line[1024];
    BOOL is_mount[MAX_DRIVE_PATHS];
    drive_entry_t* e = drive_entry_of(SelectedDrive.DeviceNumber);
    DIR *proc, *fds;
    struct dirent *pe, *fe;
    pid_t self = getpid();
    int n, j = 0;
    BYTE returned_mask = 0;
    FILE* f;
    ssize_t len;

    (void)timeout; (void)bIgnoreStaleProcesses;
    if (e == NULL)
        return 0;
    n = collect_drive_paths(e, paths, is_mount);
    proc = opendir("/proc");
    if (proc == NULL)
        return 0;
    while ((pe = readdir(proc)) != NULL) {
        BYTE rights = 0;
        pid_t pid = (pid_t)atoi(pe->d_name);
        if (pid <= 0 || pid == self)
            continue;
        /* Open files */
        snprintf(tmp, sizeof(tmp), "/proc/%d/fd", pid);
        fds = opendir(tmp);
        while (fds != NULL && (fe = readdir(fds)) != NULL) {
            if (fe->d_name[0] == '.')
                continue;
            snprintf(tmp, sizeof(tmp), "/proc/%d/fd/%s", pid, fe->d_name);
            len = readlink(tmp, link, sizeof(link) - 1);
            if (len <= 0)
                continue;
            link[len] = 0;
            if (!path_matches(link, paths, is_mount, n))
                continue;
            /* Access mode from fdinfo */
            snprintf(tmp, sizeof(tmp), "/proc/%d/fdinfo/%s", pid, fe->d_name);
            rights |= 0x01;
            f = fopen(tmp, "r");
            while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
                unsigned int flags;
                if (sscanf(line, "flags: %o", &flags) == 1 && (flags & O_ACCMODE) != O_RDONLY)
                    rights |= 0x02;
            }
            if (f != NULL)
                fclose(f);
        }
        if (fds != NULL)
            closedir(fds);
        /* Current directory, root and executable/mapped files: 'exec' rights */
        {
            const char* links[] = { "cwd", "root", "exe" };
            size_t i;
            for (i = 0; i < ARRAYSIZE(links); i++) {
                snprintf(tmp, sizeof(tmp), "/proc/%d/%s", pid, links[i]);
                len = readlink(tmp, link, sizeof(link) - 1);
                if (len > 0) {
                    link[len] = 0;
                    if (path_matches(link, paths, is_mount, n))
                        rights |= 0x04;
                }
            }
        }
        snprintf(tmp, sizeof(tmp), "/proc/%d/maps", pid);
        f = fopen(tmp, "r");
        while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
            char* p = strchr(line, '/');
            if (p == NULL)
                continue;
            p[strcspn(p, "\n")] = 0;
            if (path_matches(p, paths, is_mount, n)) {
                rights |= 0x04;
                break;
            }
        }
        if (f != NULL)
            fclose(f);
        if ((rights & access_mask) == 0)
            continue;
        /* Command line */
        cmdline[0] = 0;
        snprintf(tmp, sizeof(tmp), "/proc/%d/cmdline", pid);
        f = fopen(tmp, "r");
        if (f != NULL) {
            size_t k, r = fread(cmdline, 1, sizeof(cmdline) - 1, f);
            fclose(f);
            for (k = 0; k + 1 < r; k++)
                if (cmdline[k] == 0)
                    cmdline[k] = ' ';
            cmdline[r] = 0;
        }
        returned_mask |= rights;
        if (j++ == 0)
            uprintf("WARNING: The following application(s) or service(s) are accessing the drive:");
        uprintf("● [%d] %s (%s)", pid, (cmdline[0] != 0) ? cmdline : pe->d_name, access_rights_str[rights & 0x7]);
    }
    closedir(proc);
    if (j != 0)
        uprintf("You should close these applications before retrying the operation.");
    return returned_mask & access_mask;
}
