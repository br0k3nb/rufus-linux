/*
 * Rufus Linux port: VHD/VHDX virtual disk reader.
 *
 * Upstream attaches VHD/VHDX images with the Windows Virtual Disk API
 * (OpenVirtualDisk/AttachVirtualDisk) and then reads them as a physical
 * drive. Linux has no such API (short of qemu-nbd, which needs root and the
 * nbd module), so the port reads the virtual disk formats directly:
 * - VHD: fixed and dynamic disks (Microsoft VHD Image Format Specification),
 * - VHDX: fixed and dynamic disks ([MS-VHDX]).
 * Differencing disks (which need their parent) are not supported, as they
 * can't be meaningfully selected as a standalone image anyway.
 *
 * The calls mirror upstream's: VhdMountImageAndGetSize() "attaches" the
 * image and returns a pseudo physical path, VhdUnmountImage() releases it,
 * and VhdReadImage() reads virtual disk data.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { VHD_NONE = 0, VHD_FIXED, VHD_DYNAMIC, VHDX };

static struct {
    int fd;
    int type;
    char path[MAX_PATH];
    uint64_t disk_size;     /* virtual size */
    uint64_t file_size;
    /* VHD dynamic */
    uint32_t block_size;
    uint32_t bitmap_size;   /* bytes, rounded to a sector */
    uint32_t* bat;          /* VHD: sector numbers (host order) */
    uint32_t max_entries;
    /* VHDX */
    uint64_t* vbat;
    uint64_t vbat_entries;
    uint32_t chunk_ratio;
} vhd = { .fd = -1 };

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(&p[4]); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | ((uint64_t)le32(&p[4]) << 32); }

static BOOL read_at(void* buf, uint64_t offset, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t r = pread(vhd.fd, (uint8_t*)buf + done, len - done, (off_t)(offset + done));
        if (r <= 0)
            return FALSE;
        done += (size_t)r;
    }
    return TRUE;
}

/* CRC-32C (Castagnoli), as used by VHDX headers/region tables */
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

static BOOL crc_ok(uint8_t* buf, size_t len, size_t crc_offset)
{
    uint32_t stored = le32(&buf[crc_offset]);
    memset(&buf[crc_offset], 0, 4);
    BOOL r = (crc32c(buf, len) == stored);
    buf[crc_offset] = (uint8_t)stored; buf[crc_offset + 1] = (uint8_t)(stored >> 8);
    buf[crc_offset + 2] = (uint8_t)(stored >> 16); buf[crc_offset + 3] = (uint8_t)(stored >> 24);
    return r;
}

/* VHDX GUIDs, in their on-disk (mixed endian) byte order */
static const uint8_t guid_bat[16] = { 0x66, 0x77, 0xC2, 0x2D, 0x23, 0xF6, 0x00, 0x42, 0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08 };
static const uint8_t guid_metadata[16] = { 0x06, 0xA2, 0x7C, 0x8B, 0x90, 0x47, 0x9A, 0x4B, 0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E };
static const uint8_t guid_file_params[16] = { 0x37, 0x67, 0xA1, 0xCA, 0x36, 0xFA, 0x43, 0x4D, 0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B };
static const uint8_t guid_disk_size[16] = { 0x24, 0x42, 0xA5, 0x2F, 0x1B, 0xCD, 0x76, 0x48, 0xB2, 0x11, 0x5D, 0xBE, 0xD8, 0x3B, 0xF4, 0xB8 };
static const uint8_t guid_sector_size[16] = { 0x1D, 0xBF, 0x41, 0x81, 0x6F, 0xA9, 0x09, 0x47, 0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F };

static BOOL open_vhdx(void)
{
    uint8_t hdr[2][4096], rt[64 * KB], *md = NULL;
    uint64_t seq[2] = { 0, 0 }, bat_off = 0, md_off = 0, log_guid_lo, log_guid_hi;
    uint32_t bat_len = 0, md_len = 0, block_size = 0, sector_size = 512, count, i;
    BOOL valid[2];
    int cur;

    /* Headers at 64 KB and 128 KB: use the valid one with the highest sequence */
    for (i = 0; i < 2; i++) {
        valid[i] = read_at(hdr[i], (i + 1) * 64 * KB, 4096) && (memcmp(hdr[i], "head", 4) == 0) &&
                   crc_ok(hdr[i], 4096, 4);
        if (valid[i])
            seq[i] = le64(&hdr[i][8]);
    }
    if (!valid[0] && !valid[1]) {
        uprintf("  VHDX: no valid header");
        return FALSE;
    }
    cur = (!valid[0] || (valid[1] && seq[1] > seq[0])) ? 1 : 0;
    log_guid_lo = le64(&hdr[cur][48]);
    log_guid_hi = le64(&hdr[cur][56]);
    if ((log_guid_lo | log_guid_hi) != 0)
        uprintf("  VHDX: WARNING - the image has a pending log (it was not cleanly closed)");

    /* Region table (192 KB, backup at 256 KB) */
    if (!(read_at(rt, 192 * KB, sizeof(rt)) && memcmp(rt, "regi", 4) == 0 && crc_ok(rt, sizeof(rt), 4)) &&
        !(read_at(rt, 256 * KB, sizeof(rt)) && memcmp(rt, "regi", 4) == 0 && crc_ok(rt, sizeof(rt), 4))) {
        uprintf("  VHDX: no valid region table");
        return FALSE;
    }
    count = le32(&rt[8]);
    for (i = 0; i < count && (16 + (i + 1) * 32) <= sizeof(rt); i++) {
        const uint8_t* e = &rt[16 + i * 32];
        if (memcmp(e, guid_bat, 16) == 0) {
            bat_off = le64(&e[16]);
            bat_len = le32(&e[24]);
        } else if (memcmp(e, guid_metadata, 16) == 0) {
            md_off = le64(&e[16]);
            md_len = le32(&e[24]);
        }
    }
    if (bat_off == 0 || md_off == 0 || md_len < 32 || md_len > 16 * MB) {
        uprintf("  VHDX: missing BAT or metadata region");
        return FALSE;
    }

    /* Metadata */
    md = malloc(md_len);
    if (md == NULL || !read_at(md, md_off, md_len) || memcmp(md, "metadata", 8) != 0) {
        uprintf("  VHDX: invalid metadata region");
        free(md);
        return FALSE;
    }
    count = le16(&md[10]);
    for (i = 0; i < count && (32 + (i + 1) * 32) <= md_len; i++) {
        const uint8_t* e = &md[32 + i * 32];
        uint32_t off = le32(&e[16]);
        if (off + 8 > md_len)
            continue;
        if (memcmp(e, guid_file_params, 16) == 0) {
            block_size = le32(&md[off]);
            if (le32(&md[off + 4]) & 2) {
                uprintf("  VHDX: differencing disks are not supported");
                free(md);
                return FALSE;
            }
        } else if (memcmp(e, guid_disk_size, 16) == 0) {
            vhd.disk_size = le64(&md[off]);
        } else if (memcmp(e, guid_sector_size, 16) == 0) {
            sector_size = le32(&md[off]);
        }
    }
    free(md);
    if (block_size == 0 || vhd.disk_size == 0 || (sector_size != 512 && sector_size != 4096)) {
        uprintf("  VHDX: invalid metadata");
        return FALSE;
    }

    /* BAT: payload entries, with a sector bitmap entry after every chunk_ratio of them */
    vhd.block_size = block_size;
    vhd.chunk_ratio = (uint32_t)(((uint64_t)1 << 23) * sector_size / block_size);
    vhd.vbat_entries = bat_len / 8;
    vhd.vbat = malloc(bat_len);
    if (vhd.vbat == NULL || !read_at(vhd.vbat, bat_off, bat_len)) {
        uprintf("  VHDX: could not read BAT");
        return FALSE;
    }
    for (i = 0; i < vhd.vbat_entries; i++)
        vhd.vbat[i] = le64((uint8_t*)&vhd.vbat[i]);
    vhd.type = VHDX;
    return TRUE;
}

static BOOL open_vhd(void)
{
    uint8_t footer[512], dyn[1024];
    uint32_t disk_type, i;

    if (vhd.file_size < 512 || !read_at(footer, vhd.file_size - 512, 512))
        return FALSE;
    /* Older VHDs may have a 511 byte footer */
    if (memcmp(footer, "conectix", 8) != 0) {
        if (!read_at(footer, vhd.file_size - 511, 511) || memcmp(footer, "conectix", 8) != 0) {
            /* Fall back to the copy at the start (dynamic disks) */
            if (!read_at(footer, 0, 512) || memcmp(footer, "conectix", 8) != 0) {
                uprintf("  VHD: no valid footer");
                return FALSE;
            }
        }
    }
    vhd.disk_size = be64(&footer[48]);
    disk_type = be32(&footer[60]);
    if (disk_type == 2) {
        vhd.type = VHD_FIXED;
        return TRUE;
    }
    if (disk_type != 3) {
        uprintf("  VHD: %s disks are not supported", (disk_type == 4) ? "differencing" : "unknown type of");
        return FALSE;
    }
    if (!read_at(dyn, be64(&footer[16]), sizeof(dyn)) || memcmp(dyn, "cxsparse", 8) != 0) {
        uprintf("  VHD: invalid dynamic disk header");
        return FALSE;
    }
    vhd.max_entries = be32(&dyn[28]);
    vhd.block_size = be32(&dyn[32]);
    if (vhd.block_size == 0 || vhd.max_entries == 0 || vhd.max_entries > (1U << 28))
        return FALSE;
    vhd.bitmap_size = ((vhd.block_size / 512 / 8) + 511) & ~511U;
    vhd.bat = malloc((size_t)vhd.max_entries * 4);
    if (vhd.bat == NULL || !read_at(vhd.bat, be64(&dyn[16]), (size_t)vhd.max_entries * 4))
        return FALSE;
    for (i = 0; i < vhd.max_entries; i++)
        vhd.bat[i] = be32((uint8_t*)&vhd.bat[i]);
    vhd.type = VHD_DYNAMIC;
    return TRUE;
}

void VhdUnmountImage(void)
{
    if (vhd.fd >= 0)
        close(vhd.fd);
    free(vhd.bat);
    free(vhd.vbat);
    memset(&vhd, 0, sizeof(vhd));
    vhd.fd = -1;
}

char* VhdMountImageAndGetSize(const char* path, uint64_t* disk_size)
{
    uint8_t sig[8];
    BOOL r;

    VhdUnmountImage();
    vhd.fd = open(path, O_RDONLY | O_CLOEXEC);
    if (vhd.fd < 0) {
        uprintf("Could not open image '%s': %s", path, strerror(errno));
        return NULL;
    }
    vhd.file_size = (uint64_t)lseek(vhd.fd, 0, SEEK_END);
    r = read_at(sig, 0, sizeof(sig)) && (memcmp(sig, "vhdxfile", 8) == 0) ? open_vhdx() : open_vhd();
    if (!r) {
        uprintf("Could not mount image '%s'", path);
        VhdUnmountImage();
        return NULL;
    }
    static_strcpy(vhd.path, path);
    if (disk_size != NULL)
        *disk_size = vhd.disk_size;
    uprintf("  Virtual disk: %s, %s", (vhd.type == VHDX) ? "VHDX" : ((vhd.type == VHD_FIXED) ? "fixed VHD" : "dynamic VHD"),
        SizeToHumanReadable(vhd.disk_size, FALSE, FALSE));
    return vhd.path;
}

uint64_t VhdGetSize(void)
{
    return vhd.disk_size;
}

/* Read 'len' bytes of virtual disk data at 'offset'. Returns the number of
 * bytes read (short at the end of the disk) or -1 on error. */
int64_t VhdReadImage(void* buf, uint64_t offset, uint32_t len)
{
    uint8_t* p = (uint8_t*)buf;
    uint64_t done = 0;

    if (vhd.fd < 0 || vhd.type == VHD_NONE)
        return -1;
    if (offset >= vhd.disk_size)
        return 0;
    if (offset + len > vhd.disk_size)
        len = (uint32_t)(vhd.disk_size - offset);

    while (done < len) {
        uint64_t pos = offset + done, chunk;
        if (vhd.type == VHD_FIXED) {
            if (!read_at(&p[done], pos, len - done))
                return -1;
            break;
        } else if (vhd.type == VHD_DYNAMIC) {
            uint64_t block = pos / vhd.block_size, in_block = pos % vhd.block_size;
            chunk = MIN(len - done, vhd.block_size - in_block);
            if (block >= vhd.max_entries || vhd.bat[block] == 0xFFFFFFFF) {
                memset(&p[done], 0, (size_t)chunk);
            } else if (!read_at(&p[done], (uint64_t)vhd.bat[block] * 512 + vhd.bitmap_size + in_block, (size_t)chunk)) {
                return -1;
            }
        } else {
            uint64_t block = pos / vhd.block_size, in_block = pos % vhd.block_size;
            uint64_t idx = block + block / vhd.chunk_ratio, entry, state;
            chunk = MIN(len - done, vhd.block_size - in_block);
            entry = (idx < vhd.vbat_entries) ? vhd.vbat[idx] : 0;
            state = entry & 7;
            if ((state == 6) || (state == 7)) {     /* PAYLOAD_BLOCK_FULLY/PARTIALLY_PRESENT */
                if (!read_at(&p[done], (entry >> 20) * MB + in_block, (size_t)chunk))
                    return -1;
            } else {                                /* NOT_PRESENT/UNDEFINED/ZERO/UNMAPPED */
                memset(&p[done], 0, (size_t)chunk);
            }
        }
        done += chunk;
    }
    return (int64_t)len;
}
