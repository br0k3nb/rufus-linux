/*
 * Rufus Linux port: native FAT32 formatter.
 *
 * Port of upstream format_fat32.c (FormatLargeFAT32()), itself derived from
 * fat32format by Tom Thornhill. The Windows version obtains the drive
 * geometry through DeviceIoControl(); the port uses the partition data the
 * device layer already collected.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ms-sys (vendored) */
#include "br.h"
#include "file.h"
#include "fat16.h"
#include "fat32.h"

#define die(msg, err) do { uprintf(msg); ErrorStatus = RUFUS_ERROR(err); goto out; } while (0)

#pragma pack(push, 1)
typedef struct tagFAT_BOOTSECTOR32 {
    /* Common fields */
    BYTE sJmpBoot[3];
    BYTE sOEMName[8];
    WORD wBytsPerSec;
    BYTE bSecPerClus;
    WORD wRsvdSecCnt;
    BYTE bNumFATs;
    WORD wRootEntCnt;
    WORD wTotSec16;           /* if zero, use dTotSec32 instead */
    BYTE bMedia;
    WORD wFATSz16;
    WORD wSecPerTrk;
    WORD wNumHeads;
    DWORD dHiddSec;
    DWORD dTotSec32;
    /* Fat 32/16 only */
    DWORD dFATSz32;
    WORD wExtFlags;
    WORD wFSVer;
    DWORD dRootClus;
    WORD wFSInfo;
    WORD wBkBootSec;
    BYTE Reserved[12];
    BYTE bDrvNum;
    BYTE Reserved1;
    BYTE bBootSig;            /* == 0x29 if next three fields are ok */
    DWORD dBS_VolID;
    BYTE sVolLab[11];
    BYTE sBS_FilSysType[8];
} FAT_BOOTSECTOR32;

typedef struct {
    DWORD dLeadSig;           /* 0x41615252 */
    BYTE sReserved1[480];     /* zeros */
    DWORD dStrucSig;          /* 0x61417272 */
    DWORD dFree_Count;        /* 0xFFFFFFFF */
    DWORD dNxt_Free;          /* 0xFFFFFFFF */
    BYTE sReserved2[12];      /* zeros */
    DWORD dTrailSig;          /* 0xAA550000 */
} FAT_FSINFO;
#pragma pack(pop)

/*
 * 28.2 CALCULATING THE VOLUME SERIAL NUMBER
 * (same computation as upstream format_fat32.c)
 */
static DWORD GetVolumeID(void)
{
    struct tm tmv;
    time_t now = time(NULL);
    DWORD d;
    WORD lo, hi, tmp;

    localtime_r(&now, &tmv);

    lo = (WORD)(tmv.tm_mday + (tmv.tm_mon << 8));
    tmp = (WORD)((0 / 10) + (tmv.tm_sec << 8));
    lo += tmp;

    hi = (WORD)(tmv.tm_min + (tmv.tm_hour << 8));
    hi += (WORD)(tmv.tm_year + 1900);

    d = (DWORD)lo + ((DWORD)hi << 16);
    return d;
}

/*
 * Proper computation of FAT size
 * See: http://www.syslinux.org/archives/2016-February/024850.html
 */
static DWORD GetFATSizeSectors(DWORD DskSize, DWORD ReservedSecCnt, DWORD SecPerClus, DWORD NumFATs, DWORD BytesPerSect)
{
    uint64_t Numerator, Denominator;
    uint64_t FatElementSize = 4;
    uint64_t ReservedClusCnt = 2;
    uint64_t FatSz;

    Numerator = DskSize - ReservedSecCnt + ReservedClusCnt * SecPerClus;
    Denominator = (uint64_t)SecPerClus * BytesPerSect / FatElementSize + NumFATs;
    FatSz = Numerator / Denominator + 1;    /* +1 to ensure we are rounded up */

    return (DWORD)FatSz;
}

/*
 * Large FAT32 volume formatting from fat32format by Tom Thornhill
 * http://www.ridgecrop.demon.co.uk/index.htm?fat32format.htm
 */
BOOL FormatLargeFAT32(DWORD DriveIndex, uint64_t PartitionOffset, DWORD ClusterSize, LPCSTR FSName, LPCSTR Label, DWORD Flags)
{
    BOOL r = FALSE;
    DWORD i;
    HANDLE hLogicalVolume = NULL;
    /* Recommended values */
    DWORD ReservedSectCount = 32;
    DWORD NumFATs = 2;
    DWORD BackupBootSect = 6;
    DWORD VolumeId = 0;     /* calculated before format */
    DWORD BytesPerSect = 0;
    DWORD SectorsPerCluster = 0;
    DWORD TotalSectors = 0;
    DWORD AlignSectors = 0;
    DWORD SystemAreaSize = 0;
    DWORD UserAreaSize = 0;
    DWORD BurstSize = 128;  /* Zero in blocks of 64K typically */
    uint64_t PartitionLength;
    DWORD FatSize = 0;

    /* Structures to be written to the disk */
    FAT_BOOTSECTOR32* pFAT32BootSect = NULL;
    FAT_FSINFO* pFAT32FsInfo = NULL;
    DWORD* pFirstSectOfFat = NULL;
    BYTE* pZeroSect = NULL;
    char VolId[12] = "NO NAME    ";

    /* Debug temp vars */
    uint64_t FatNeeded, ClusterCount;

    if (strncmp(FSName, "FAT", 3) != 0) {
        ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
        goto out;
    }
    if (!(Flags & FP_NO_PROGRESS)) {
        PrintInfoDebug(0, MSG_222, "Large FAT32");
        UpdateProgressWithInfoInit(NULL, TRUE);
    }
    VolumeId = GetVolumeID();

    /* Open the drive and lock it */
    hLogicalVolume = GetLogicalHandle(DriveIndex, PartitionOffset, TRUE, TRUE, FALSE);
    if (IS_ERROR(ErrorStatus))
        goto out;
    if ((hLogicalVolume == INVALID_HANDLE_VALUE) || (hLogicalVolume == NULL))
        die("Invalid logical volume handle", ERROR_INVALID_HANDLE);

    /* Try to disappear the volume while we're formatting it */
    UnmountVolume(hLogicalVolume);

    /* Work out drive params (upstream: IOCTL_DISK_GET_DRIVE_GEOMETRY[_EX]) */
    BytesPerSect = SelectedDrive.SectorSize;
    if (BytesPerSect < 512)
        BytesPerSect = 512;
    PartitionLength = GetPartitionSize(DriveIndex, PartitionOffset);
    if (PartitionLength == 0)
        die("Could not get partition size", ERROR_NOT_SUPPORTED);
    if (IS_ERROR(ErrorStatus)) goto out;

    /* Checks on Disk Size */
    if (PartitionLength / BytesPerSect < 65536) {
        /* Most FAT32 implementations would probably mount this volume just fine,
         * but the spec says that we shouldn't do this, so we won't */
        die("This drive is too small for FAT32 - there must be at least 64K clusters", APPERR(ERROR_INVALID_CLUSTER_SIZE));
    }
    if (PartitionLength / BytesPerSect >= 0xFFFFFFFFull) {
        die("This drive is too big for FAT32 - max 2TB supported", APPERR(ERROR_INVALID_VOLUME_SIZE));
    }

    /* Set default cluster size
     * https://support.microsoft.com/en-us/help/140365/default-cluster-size-for-ntfs-fat-and-exfat */
    if (ClusterSize == 0) {
        if (PartitionLength < 64 * MB)
            ClusterSize = 512;
        else if (PartitionLength < 128 * MB)
            ClusterSize = 1 * KB;
        else if (PartitionLength < 256 * MB)
            ClusterSize = 2 * KB;
        else if (PartitionLength < 8 * GB)
            ClusterSize = 4 * KB;
        else if (PartitionLength < 16 * GB)
            ClusterSize = 8 * KB;
        else if (PartitionLength < 32 * GB)
            ClusterSize = 16 * KB;
        else if (PartitionLength < 2 * TB)
            ClusterSize = 32 * KB;
        else
            ClusterSize = 64 * KB;
    }

    pFAT32BootSect = (FAT_BOOTSECTOR32*)calloc(BytesPerSect, 1);
    pFAT32FsInfo = (FAT_FSINFO*)calloc(BytesPerSect, 1);
    pFirstSectOfFat = (DWORD*)calloc(BytesPerSect, 1);
    if (!pFAT32BootSect || !pFAT32FsInfo || !pFirstSectOfFat) {
        die("Failed to allocate memory", ERROR_NOT_ENOUGH_MEMORY);
    }

    /* fill out the boot sector and fs info */
    pFAT32BootSect->sJmpBoot[0] = 0xEB;
    pFAT32BootSect->sJmpBoot[1] = 0x58;    /* jmp.s $+0x5a is 0xeb 0x58, not 0xeb 0x5a */
    pFAT32BootSect->sJmpBoot[2] = 0x90;
    memcpy(pFAT32BootSect->sOEMName, "MSWIN4.1", 8);
    pFAT32BootSect->wBytsPerSec = (WORD)BytesPerSect;
    SectorsPerCluster = ClusterSize / BytesPerSect;

    pFAT32BootSect->bSecPerClus = (BYTE)SectorsPerCluster;
    pFAT32BootSect->bNumFATs = (BYTE)NumFATs;
    pFAT32BootSect->wRootEntCnt = 0;
    pFAT32BootSect->wTotSec16 = 0;
    pFAT32BootSect->bMedia = 0xF8;
    pFAT32BootSect->wFATSz16 = 0;
    pFAT32BootSect->wSecPerTrk = (WORD)(SelectedDrive.SectorsPerTrack ? SelectedDrive.SectorsPerTrack : 63);
    pFAT32BootSect->wNumHeads = 255;
    pFAT32BootSect->dHiddSec = (DWORD)(PartitionOffset / BytesPerSect);
    TotalSectors = (DWORD)(PartitionLength / BytesPerSect);
    pFAT32BootSect->dTotSec32 = TotalSectors;

    FatSize = GetFATSizeSectors(pFAT32BootSect->dTotSec32, ReservedSectCount,
        pFAT32BootSect->bSecPerClus, pFAT32BootSect->bNumFATs, BytesPerSect);

    /* Update reserved sector count so that the start of data region is aligned to a MB boundary */
    SystemAreaSize = ReservedSectCount + NumFATs * FatSize;
    AlignSectors = (1 * MB) / BytesPerSect;
    SystemAreaSize = (SystemAreaSize + AlignSectors - 1) / AlignSectors * AlignSectors;
    ReservedSectCount = SystemAreaSize - NumFATs * FatSize;

    pFAT32BootSect->wRsvdSecCnt = (WORD)ReservedSectCount;
    pFAT32BootSect->dFATSz32 = FatSize;
    pFAT32BootSect->wExtFlags = 0;
    pFAT32BootSect->wFSVer = 0;
    pFAT32BootSect->dRootClus = 2;
    pFAT32BootSect->wFSInfo = 1;
    pFAT32BootSect->wBkBootSec = (WORD)BackupBootSect;
    pFAT32BootSect->bDrvNum = 0x80;
    pFAT32BootSect->Reserved1 = 0;
    pFAT32BootSect->bBootSig = 0x29;

    /* Apply the requested label (upstream does this through SetVolumeLabelA()
     * after formatting, which ultimately ends up patching the same field) */
    if (Label != NULL && Label[0] != 0) {
        char* VolLabel = strdup(Label);
        if (VolLabel != NULL) {
            ToValidLabel(VolLabel, TRUE);
            memset(VolId, ' ', 11);
            for (i = 0; i < 11 && VolLabel[i] != 0; i++)
                VolId[i] = (char)toupper((unsigned char)VolLabel[i]);
            free(VolLabel);
        }
    }

    pFAT32BootSect->dBS_VolID = VolumeId;
    memcpy(pFAT32BootSect->sVolLab, VolId, 11);
    memcpy(pFAT32BootSect->sBS_FilSysType, "FAT32   ", 8);
    ((BYTE*)pFAT32BootSect)[510] = 0x55;
    ((BYTE*)pFAT32BootSect)[511] = 0xaa;

    /* FATGEN103.DOC says the 0xAA55 signature is at offsets 510/511 whatever the
     * sector size; other OSs may check the end of the sector, so set both. */
    if (BytesPerSect != 512) {
        ((BYTE*)pFAT32BootSect)[BytesPerSect - 2] = 0x55;
        ((BYTE*)pFAT32BootSect)[BytesPerSect - 1] = 0xaa;
    }

    /* FSInfo sect */
    pFAT32FsInfo->dLeadSig = 0x41615252;
    pFAT32FsInfo->dStrucSig = 0x61417272;
    pFAT32FsInfo->dFree_Count = (DWORD)-1;
    pFAT32FsInfo->dNxt_Free = (DWORD)-1;
    pFAT32FsInfo->dTrailSig = 0xaa550000;

    /* First FAT Sector */
    pFirstSectOfFat[0] = 0x0ffffff8;    /* Reserved cluster 1 media id in low byte */
    pFirstSectOfFat[1] = 0x0fffffff;    /* Reserved cluster 2 EOC */
    pFirstSectOfFat[2] = 0x0fffffff;    /* end of cluster chain for root dir */

    /* Write boot sector, fats
     * Sector 0 Boot Sector
     * Sector 1 FSInfo
     * Sector 2 More boot code - we write zeros here
     * Sector 6 Backup boot sector
     * Sector 7 Backup FSInfo sector
     * zeroed sectors up to ReservedSectCount
     * FAT1  ReservedSectCount to ReservedSectCount + FatSize
     * ...
     * FATn  ReservedSectCount to ReservedSectCount + FatSize
     * RootDir - allocated to cluster2 */

    UserAreaSize = TotalSectors - ReservedSectCount - (NumFATs * FatSize);
    assert(SectorsPerCluster > 0);
    ClusterCount = UserAreaSize / SectorsPerCluster;

    /* Sanity check for a cluster count of >2^28, since the upper 4 bits of the
     * cluster values in the FAT are reserved. */
    if (ClusterCount > 0x0FFFFFFF) {
        die("This drive has more than 2^28 clusters, try to specify a larger cluster size or use the default",
            ERROR_INVALID_CLUSTER_SIZE);
    }

    /* Sanity check - < 64K clusters means that the volume will be misdetected as FAT16 */
    if (ClusterCount < 65536) {
        die("FAT32 must have at least 65536 clusters, try to specify a smaller cluster size or use the default",
            ERROR_INVALID_CLUSTER_SIZE);
    }

    /* Sanity check, make sure the fat is big enough */
    FatNeeded = ClusterCount * 4;
    FatNeeded += (BytesPerSect - 1);
    FatNeeded /= BytesPerSect;
    if (FatNeeded > FatSize) {
        die("This drive is too big for large FAT32 format", APPERR(ERROR_INVALID_VOLUME_SIZE));
    }

    /* Now we're committed - print some info first */
    uprintf("Size : %s %u sectors", SizeToHumanReadable(PartitionLength, TRUE, FALSE), TotalSectors);
    uprintf("Cluster size %u bytes, %u bytes per sector", SectorsPerCluster * BytesPerSect, BytesPerSect);
    uprintf("Volume ID is %lx:%lx", (unsigned long)(VolumeId >> 16), (unsigned long)(VolumeId & 0xffff));
    uprintf("%u Reserved sectors, %u sectors per FAT, %u FATs", ReservedSectCount, FatSize, NumFATs);
    uprintf("%llu Total clusters", (unsigned long long)ClusterCount);

    /* Fix up the FSInfo sector */
    pFAT32FsInfo->dFree_Count = (UserAreaSize / SectorsPerCluster) - 1;
    pFAT32FsInfo->dNxt_Free = 3;    /* clusters 0-1 reserved, we used cluster 2 for the root dir */

    uprintf("%lu Free clusters", (unsigned long)pFAT32FsInfo->dFree_Count);

    /* First zero out ReservedSect + FatSize * NumFats + SectorsPerCluster */
    SystemAreaSize = ReservedSectCount + (NumFATs * FatSize) + SectorsPerCluster;
    uprintf("Clearing out %u sectors for reserved sectors, FATs and root cluster...", SystemAreaSize);

    /* Not the most effective, but easy on RAM */
    pZeroSect = (BYTE*)calloc(BytesPerSect, BurstSize);
    if (!pZeroSect) {
        die("Failed to allocate memory", ERROR_NOT_ENOUGH_MEMORY);
    }

    for (i = 0; i < (SystemAreaSize + BurstSize - 1); i += BurstSize) {
        if (!(Flags & FP_NO_PROGRESS))
            UpdateProgressWithInfo(OP_FORMAT, MSG_217, (uint64_t)i * BytesPerSect,
                ((uint64_t)SystemAreaSize + BurstSize) * BytesPerSect);
        CHECK_FOR_USER_CANCEL;
        if (write_sectors(hLogicalVolume, BytesPerSect, i, BurstSize, pZeroSect) != (BytesPerSect * BurstSize)) {
            die("Error clearing reserved sectors", ERROR_WRITE_FAULT);
        }
    }

    uprintf("Initializing reserved sectors and FATs...");
    /* Now we should write the boot sector and fsinfo twice, once at 0 and once at the backup boot sect position */
    for (i = 0; i < 2; i++) {
        int SectorStart = (i == 0) ? 0 : BackupBootSect;
        write_sectors(hLogicalVolume, BytesPerSect, SectorStart, 1, pFAT32BootSect);
        write_sectors(hLogicalVolume, BytesPerSect, SectorStart + 1, 1, pFAT32FsInfo);
    }

    /* Write the first fat sector in the right places */
    for (i = 0; i < NumFATs; i++) {
        int SectorStart = ReservedSectCount + (i * FatSize);
        uprintf("FAT #%lu sector at address: %d", (unsigned long)i, SectorStart);
        write_sectors(hLogicalVolume, BytesPerSect, SectorStart, 1, pFirstSectOfFat);
    }

    if (!(Flags & FP_NO_BOOT)) {
        /* Must do it here, as we may have issues trying to write the PBR after a remount */
        if (!(Flags & FP_NO_PROGRESS))
            PrintInfoDebug(0, MSG_229);
        if (!WritePBR(hLogicalVolume)) {
            /* Non fatal error, but the drive probably won't boot */
            uprintf("Could not write partition boot record - drive may not boot...");
        }
    }

    if (!(Flags & FP_NO_PROGRESS)) {
        PrintInfo(0, MSG_221, lmprintf(MSG_307));
        uprintf("Setting label...");
    }
    /* Upstream calls SetVolumeLabelA() here, which patches the label in the
     * boot sector. Since WritePBR() above replaced our boot sector with the
     * standard ms-sys one (which carries "NO NAME"), re-apply the volume id
     * and the label to both the primary and the backup boot sector. */
    {
        uint8_t* sect = (uint8_t*)calloc(BytesPerSect, 1);
        if (sect == NULL) {
            uprintf("Could not set label: not enough memory");
        } else {
            for (i = 0; i < 2; i++) {
                uint32_t sect_start = (i == 0) ? 0 : BackupBootSect;
                if ((read_sectors(hLogicalVolume, BytesPerSect, sect_start, 1, sect) > 0) &&
                    (memcmp(&sect[71], VolId, 11) != 0)) {
                    memcpy(&sect[67], &VolumeId, 4);
                    memcpy(&sect[71], VolId, 11);
                    if (write_sectors(hLogicalVolume, BytesPerSect, sect_start, 1, sect) != (BytesPerSect))
                        uprintf("Could not set label: write error");
                }
            }
            safe_free(sect);
        }
    }
    uprintf("Label '%s' set", VolId);

    /* SetVolumeLabel() also records the label as a root directory entry, so
     * do the same here (this is what fsck/mount look at for the volume name) */
    if (memcmp(VolId, "NO NAME    ", 11) != 0) {
        uint32_t root_sect = ReservedSectCount + (NumFATs * FatSize);
        uint8_t* root = (uint8_t*)calloc(BytesPerSect, 1);
        if (root == NULL) {
            uprintf("Could not set label: not enough memory");
        } else {
            if ((read_sectors(hLogicalVolume, BytesPerSect, root_sect, 1, root) > 0) &&
                (memcmp(&root[0], VolId, 11) != 0)) {
                struct tm tmv;
                time_t now = time(NULL);
                uint16_t fdate, ftime;
                localtime_r(&now, &tmv);
                fdate = (uint16_t)((((tmv.tm_year + 1900) - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
                ftime = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
                memcpy(&root[0], VolId, 11);
                root[11] = 0x08;    /* ATTR_VOLUME_ID */
                root[14] = (uint8_t)(ftime & 0xff);
                root[15] = (uint8_t)(ftime >> 8);
                root[16] = (uint8_t)(fdate & 0xff);
                root[17] = (uint8_t)(fdate >> 8);
                root[18] = (uint8_t)(fdate & 0xff);
                root[19] = (uint8_t)(fdate >> 8);
                root[22] = (uint8_t)(ftime & 0xff);
                root[23] = (uint8_t)(ftime >> 8);
                root[24] = (uint8_t)(fdate & 0xff);
                root[25] = (uint8_t)(fdate >> 8);
                if (write_sectors(hLogicalVolume, BytesPerSect, root_sect, 1, root) != (BytesPerSect))
                    uprintf("Could not set label: write error");
            }
            safe_free(root);
        }
    }

    uprintf("Format completed.");
    r = TRUE;

out:
    safe_closehandle(hLogicalVolume);
    safe_free(pFAT32BootSect);
    safe_free(pFAT32FsInfo);
    safe_free(pFirstSectOfFat);
    safe_free(pZeroSect);
    return r;
}
