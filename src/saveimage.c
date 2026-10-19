/*
 * Rufus Linux port: save a drive (or optical disc) to an image file.
 *
 * Upstream (vhd.c/iso.c SaveImage()) creates VHD/VHDX images of a drive with
 * the Windows Virtual Disk API (CreateVirtualDisk() cloning the physical
 * drive), FFU images with dism, ISO images of a drive's file system with
 * Microsoft's oscdimg, and ISO images of optical media with a raw copy.
 * The port writes the virtual disk formats itself:
 * - .vhd:  fixed VHD (raw data + footer), uncompressed, so that it can be
 *          used as a DD image (as upstream, FULL_PHYSICAL_ALLOCATION),
 * - .vhdx: dynamic VHDX, where blocks that only contain zeroes are not
 *          stored (as CreateVirtualDisk() does),
 * - .img:  raw disk image,
 * - .iso:  raw copy for optical media; for other drives, an ISO-9660 image
 *          (level 3, so that files larger than 4 GB are supported, with
 *          Rock Ridge and Joliet) of the mounted volume, made with xorriso.
 * FFU requires Windows' dism and is not available.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"
#include "localization.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <time.h>
#include <unistd.h>

#define VHDX_BLOCK_SIZE     (32 * MB)

static void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_be64(uint8_t* p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(&p[4], (uint32_t)v); }
static void put_le16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put_le64(uint8_t* p, uint64_t v) { put_le32(p, (uint32_t)v); put_le32(&p[4], (uint32_t)(v >> 32)); }

static void random_guid(uint8_t guid[16])
{
    FILE* fd = fopen("/dev/urandom", "rb");
    if (fd == NULL || fread(guid, 1, 16, fd) != 16) {
        int i;
        for (i = 0; i < 16; i++)
            guid[i] = (uint8_t)rand();
    }
    if (fd != NULL)
        fclose(fd);
    guid[7] = (guid[7] & 0x0f) | 0x40;  /* version 4 */
    guid[8] = (guid[8] & 0x3f) | 0x80;  /* variant */
}

static uint32_t crc32c(const uint8_t* buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    size_t i;
    int k;
    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82F63B78 & (0 - (crc & 1)));
    }
    return ~crc;
}

static BOOL write_at(int fd, const void* buf, size_t len, uint64_t offset)
{
    size_t done = 0;
    while (done < len) {
        ssize_t w = pwrite(fd, (const uint8_t*)buf + done, len - done, (off_t)(offset + done));
        if (w <= 0)
            return FALSE;
        done += (size_t)w;
    }
    return TRUE;
}

/* VHD footer (Microsoft Virtual Hard Disk Image Format Specification) */
static void make_vhd_footer(uint8_t footer[512], uint64_t size)
{
    uint64_t total_sectors = size / 512;
    uint32_t cyl, heads, spt, cyl_times_heads, checksum = 0;
    int i;

    memset(footer, 0, 512);
    memcpy(footer, "conectix", 8);
    put_be32(&footer[8], 0x00000002);           /* features: reserved bit set */
    put_be32(&footer[12], 0x00010000);          /* file format version */
    put_be64(&footer[16], 0xFFFFFFFFFFFFFFFFULL);   /* data offset (fixed disk) */
    put_be32(&footer[24], (uint32_t)(time(NULL) - 946684800));  /* seconds since 2000-01-01 */
    memcpy(&footer[28], "rufs", 4);             /* creator application */
    put_be32(&footer[32], (RUFUS_VERSION_MAJOR << 16) | RUFUS_VERSION_MINOR);
    memcpy(&footer[36], "Wi2k", 4);             /* creator host OS, as Windows writes it */
    put_be64(&footer[40], size);                /* original size */
    put_be64(&footer[48], size);                /* current size */
    /* CHS geometry, as computed in the specification's appendix */
    if (total_sectors > 65535ULL * 16 * 255)
        total_sectors = 65535ULL * 16 * 255;
    if (total_sectors >= 65535ULL * 16 * 63) {
        spt = 255;
        heads = 16;
        cyl_times_heads = (uint32_t)(total_sectors / spt);
    } else {
        spt = 17;
        cyl_times_heads = (uint32_t)(total_sectors / spt);
        heads = (cyl_times_heads + 1023) / 1024;
        if (heads < 4)
            heads = 4;
        if (cyl_times_heads >= (heads * 1024) || heads > 16) {
            spt = 31;
            heads = 16;
            cyl_times_heads = (uint32_t)(total_sectors / spt);
        }
        if (cyl_times_heads >= (heads * 1024)) {
            spt = 63;
            heads = 16;
            cyl_times_heads = (uint32_t)(total_sectors / spt);
        }
    }
    cyl = cyl_times_heads / heads;
    put_be16(&footer[56], (uint16_t)cyl);
    footer[58] = (uint8_t)heads;
    footer[59] = (uint8_t)spt;
    put_be32(&footer[60], 2);                   /* disk type: fixed */
    random_guid(&footer[68]);                   /* unique id */
    for (i = 0; i < 512; i++)
        checksum += footer[i];
    put_be32(&footer[64], ~checksum);
}

/* VHDX GUIDs, in their on-disk byte order */
static const uint8_t guid_bat[16] = { 0x66, 0x77, 0xC2, 0x2D, 0x23, 0xF6, 0x00, 0x42, 0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08 };
static const uint8_t guid_metadata[16] = { 0x06, 0xA2, 0x7C, 0x8B, 0x90, 0x47, 0x9A, 0x4B, 0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E };
static const uint8_t guid_file_params[16] = { 0x37, 0x67, 0xA1, 0xCA, 0x36, 0xFA, 0x43, 0x4D, 0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B };
static const uint8_t guid_disk_size[16] = { 0x24, 0x42, 0xA5, 0x2F, 0x1B, 0xCD, 0x76, 0x48, 0xB2, 0x11, 0x5D, 0xBE, 0xD8, 0x3B, 0xF4, 0xB8 };
static const uint8_t guid_page83[16] = { 0xAB, 0x12, 0xCA, 0xBE, 0xE6, 0xB2, 0x23, 0x45, 0x93, 0xEF, 0xC3, 0x09, 0xE0, 0x00, 0xC7, 0x46 };
static const uint8_t guid_logical_ss[16] = { 0x1D, 0xBF, 0x41, 0x81, 0x6F, 0xA9, 0x09, 0x47, 0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F };
static const uint8_t guid_physical_ss[16] = { 0xC7, 0x48, 0xA3, 0xCD, 0x5D, 0x44, 0x71, 0x44, 0x9C, 0xC9, 0xE9, 0x88, 0x52, 0x51, 0xC5, 0x56 };

/* Write the VHDX structures (everything but the payload blocks) */
static BOOL write_vhdx_metadata(int fd, uint64_t size, uint32_t sector_size, const uint64_t* bat,
                                uint64_t bat_entries, uint64_t bat_offset, uint32_t bat_length)
{
    uint8_t* buf = calloc(1, 1 * MB);
    uint8_t guid[16];
    const uint64_t md_offset = 2 * MB;
    const struct { const uint8_t* guid; uint32_t len; uint32_t flags; } items[] = {
        { guid_file_params, 8, 4 }, { guid_disk_size, 8, 6 }, { guid_page83, 16, 6 },
        { guid_logical_ss, 4, 6 }, { guid_physical_ss, 4, 6 },
    };
    uint32_t i, data_off = 64 * KB;
    uint64_t j;
    BOOL r = FALSE;

    if (buf == NULL)
        return FALSE;
    /* File type identifier */
    memcpy(buf, "vhdxfile", 8);
    for (i = 0; i < strlen("Rufus"); i++)
        put_le16(&buf[8 + 2 * i], (uint16_t)"Rufus"[i]);
    if (!write_at(fd, buf, 64 * KB, 0))
        goto out;

    /* Headers (sequence numbers 1 and 2: the second one is current) */
    for (i = 0; i < 2; i++) {
        memset(buf, 0, 4 * KB);
        memcpy(buf, "head", 4);
        put_le64(&buf[8], i + 1);
        random_guid(guid);
        memcpy(&buf[16], guid, 16);             /* FileWriteGuid */
        random_guid(guid);
        memcpy(&buf[32], guid, 16);             /* DataWriteGuid */
        /* LogGuid = 0: no log replay needed */
        put_le16(&buf[64], 0);                  /* LogVersion */
        put_le16(&buf[66], 1);                  /* Version */
        put_le32(&buf[68], 1 * MB);             /* LogLength */
        put_le64(&buf[72], 1 * MB);             /* LogOffset */
        put_le32(&buf[4], crc32c(buf, 4 * KB));
        if (!write_at(fd, buf, 4 * KB, (uint64_t)(i + 1) * 64 * KB))
            goto out;
    }

    /* Region table, and its copy */
    memset(buf, 0, 64 * KB);
    memcpy(buf, "regi", 4);
    put_le32(&buf[8], 2);
    memcpy(&buf[16], guid_bat, 16);
    put_le64(&buf[32], bat_offset);
    put_le32(&buf[40], bat_length);
    put_le32(&buf[44], 1);
    memcpy(&buf[48], guid_metadata, 16);
    put_le64(&buf[64], md_offset);
    put_le32(&buf[72], 1 * MB);
    put_le32(&buf[76], 1);
    put_le32(&buf[4], crc32c(buf, 64 * KB));
    if (!write_at(fd, buf, 64 * KB, 192 * KB) || !write_at(fd, buf, 64 * KB, 256 * KB))
        goto out;

    /* Empty log */
    memset(buf, 0, 1 * MB);
    if (!write_at(fd, buf, 1 * MB, 1 * MB))
        goto out;

    /* Metadata region */
    memcpy(buf, "metadata", 8);
    put_le16(&buf[10], (uint16_t)ARRAYSIZE(items));
    for (i = 0; i < ARRAYSIZE(items); i++) {
        uint8_t* e = &buf[32 + 32 * i];
        memcpy(e, items[i].guid, 16);
        put_le32(&e[16], data_off);
        put_le32(&e[20], items[i].len);
        put_le32(&e[24], items[i].flags);
        switch (i) {
        case 0: put_le32(&buf[data_off], VHDX_BLOCK_SIZE); put_le32(&buf[data_off + 4], 0); break;
        case 1: put_le64(&buf[data_off], size); break;
        case 2: random_guid(&buf[data_off]); break;
        case 3: put_le32(&buf[data_off], sector_size); break;
        case 4: put_le32(&buf[data_off], sector_size); break;
        }
        data_off += 4 * KB;
    }
    if (!write_at(fd, buf, 1 * MB, md_offset))
        goto out;

    /* BAT */
    for (j = 0; j < bat_entries; j++)
        put_le64((uint8_t*)&bat[j], bat[j]);
    r = write_at(fd, bat, bat_length, bat_offset);
    for (j = 0; j < bat_entries; j++)
        ((uint64_t*)bat)[j] = 0;
out:
    free(buf);
    return r;
}

/* Copy a drive to an image file of the given type */
static BOOL save_drive(const char* device, const char* image, uint64_t size, int type)
{
    enum { T_RAW, T_VHD, T_VHDX };
    uint8_t* buf = NULL;
    uint64_t* bat = NULL;
    uint64_t pos = 0, data_offset = 0, bat_entries = 0, bat_offset = 3 * MB;
    uint32_t chunk_ratio = 0, bat_length = 0, sector_size = SelectedDrive.SectorSize ? SelectedDrive.SectorSize : 512;
    size_t buf_size = (type == T_VHDX) ? VHDX_BLOCK_SIZE : (size_t)DD_BUFFER_SIZE;
    int in = -1, out = -1;
    BOOL r = FALSE;

    in = open(device, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        uprintf("Could not open '%s': %s", device, strerror(errno));
        ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
        goto out;
    }
    out = open(image, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        uprintf("Could not create '%s': %s", image, strerror(errno));
        ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
        goto out;
    }
    buf = _mm_malloc(buf_size, 4096);
    if (buf == NULL) {
        ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
        goto out;
    }
    if (type == T_VHDX) {
        uint64_t data_blocks = (size + VHDX_BLOCK_SIZE - 1) / VHDX_BLOCK_SIZE;
        chunk_ratio = (uint32_t)(((uint64_t)1 << 23) * sector_size / VHDX_BLOCK_SIZE);
        bat_entries = data_blocks + (data_blocks - 1) / chunk_ratio;
        bat_length = (uint32_t)CEILING_ALIGN(bat_entries * 8, 1 * MB);
        bat = calloc(1, bat_length);
        if (bat == NULL) {
            ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
            goto out;
        }
        data_offset = bat_offset + bat_length;
    }

    UpdateProgressWithInfoInit(NULL, FALSE);
    uprint_progress(0, 0);
    while (pos < size) {
        size_t n = (size_t)MIN((uint64_t)buf_size, size - pos), got = 0;
        CHECK_FOR_USER_CANCEL;
        while (got < n) {
            ssize_t rd = pread(in, &buf[got], n - got, (off_t)(pos + got));
            if (rd <= 0) {
                uprintf("Read error at offset %llu: %s", (unsigned long long)(pos + got), (rd < 0) ? strerror(errno) : "end of device");
                ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
                goto out;
            }
            got += (size_t)rd;
        }
        if (type == T_VHDX) {
            uint64_t block = pos / VHDX_BLOCK_SIZE, idx = block + block / chunk_ratio, i;
            for (i = 0; i < n && buf[i] == 0; i++);
            if (i < n) {
                /* Only blocks with data are stored, the others read back as zero */
                if (n < VHDX_BLOCK_SIZE)
                    memset(&buf[n], 0, VHDX_BLOCK_SIZE - n);
                if (!write_at(out, buf, VHDX_BLOCK_SIZE, data_offset))
                    goto write_error;
                bat[idx] = 6 | ((data_offset / MB) << 20);  /* PAYLOAD_BLOCK_FULLY_PRESENT */
                data_offset += VHDX_BLOCK_SIZE;
            }
        } else if (!write_at(out, buf, n, pos)) {
            goto write_error;
        }
        pos += n;
        UpdateProgressWithInfo(OP_FORMAT, MSG_261, pos, size);
        uprint_progress(pos, size);
    }
    uprintfs("\r\n");
    if (type == T_VHD) {
        uint8_t footer[512];
        make_vhd_footer(footer, size);
        if (!write_at(out, footer, sizeof(footer), size))
            goto write_error;
    } else if (type == T_VHDX) {
        if (!write_vhdx_metadata(out, size, sector_size, bat, bat_entries, bat_offset, bat_length))
            goto write_error;
    }
    r = (fsync(out) == 0);
    goto out;

write_error:
    uprintf("Could not write '%s': %s", image, strerror(errno));
    ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
out:
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    if (buf != NULL) _mm_free(buf);
    free(bat);
    if (!r)
        unlink(image);
    return r;
}

/* Create an ISO image of the (mounted) file system of a drive */
static BOOL save_volume_as_iso(DWORD DriveIndex, const char* image)
{
    char cmd[3 * MAX_PATH], mp[MAX_PATH], vol_label[33] = "", *lbl = NULL;
    uint64_t offset = SelectedDrive.Partition[0].Offset;
    DWORD r;

    if (SelectedDrive.nPartitions == 0) {
        uprintf("The drive has no partition to create an ISO image from");
        return FALSE;
    }
    if (GetDriveLabel(DriveIndex, NULL, &lbl, TRUE) && (lbl != NULL))
        static_strcpy(vol_label, lbl);
    static_strcpy(mp, GetMountedPath(DriveIndex, offset, FALSE));
    if (mp[0] == 0) {
        uprintf("Could not mount the drive's file system");
        return FALSE;
    }
    /* ISO-9660 level 3 (multi-extent files, for files > 4 GB), with Rock
     * Ridge and Joliet: the functional equivalent of upstream's UDF image */
    static_sprintf(cmd, "xorriso -as mkisofs -iso-level 3 -r -J -joliet-long -V '%s' -o '%s' '%s'",
        (vol_label[0] != 0) ? vol_label : "RUFUS", image, mp);
    uprintf("Running command: '%s'", cmd);
    r = RunCommandWithProgress(cmd, NULL, FALSE, MSG_261, ".*[ \t]([0-9\\.]+)% done.*");
    AltUnmountVolume(GetPhysicalName(DriveIndex), TRUE);
    if (r == 127) {
        uprintf("'xorriso' is required to create ISO images - please install it");
        ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
    } else if (r != 0 && !IS_ERROR(ErrorStatus)) {
        uprintf("Failed to write ISO image (error %lu)", (unsigned long)r);
        ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
    }
    return (r == 0);
}

/*
 * Save a drive or an optical disc to an image file. The type comes from the
 * extension, as upstream's save dialog does: .vhd, .vhdx, .img or .iso.
 */
BOOL SaveImage(const char* device_path, DWORD DriveIndex, const char* image_path_out)
{
    const char* ext = strrchr(image_path_out, '.');
    struct stat st;
    uint64_t size = 0;
    BOOL is_optical = (strncmp(device_path, "/dev/sr", 7) == 0);
    int fd, type;
    BOOL r;

    ErrorStatus = 0;
    if (ext != NULL && strcasecmp(ext, ".vhdx") == 0)
        type = 2;
    else if (ext != NULL && strcasecmp(ext, ".vhd") == 0)
        type = 1;
    else if (ext != NULL && strcasecmp(ext, ".ffu") == 0) {
        uprintf("FFU images require Windows' dism and can not be created on Linux");
        ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
        return FALSE;
    } else if (ext != NULL && strcasecmp(ext, ".iso") == 0)
        type = is_optical ? 0 : 3;
    else if (ext != NULL && strcasecmp(ext, ".img") == 0)
        type = 0;
    else {
        uprintf("WARNING: Can not determine image type from extension - Saving to uncompressed VHD.");
        type = 1;
    }

    if (type == 3)
        r = save_volume_as_iso(DriveIndex, image_path_out);
    else {
        /* Size of the source (device or virtual drive file) */
        fd = open(device_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
                size = (uint64_t)st.st_size;
            else
                ioctl(fd, BLKGETSIZE64, &size);
            close(fd);
        }
        if (size == 0) {
            uprintf("Could not get the size of '%s'", device_path);
            ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
            return FALSE;
        }
        uprintf("Saving '%s' (%s) to '%s'", device_path, SizeToHumanReadable(size, FALSE, FALSE), image_path_out);
        r = save_drive(device_path, image_path_out, size, type);
    }
    if (r)
        uprintf("Saved '%s'", image_path_out);
    return r;
}
