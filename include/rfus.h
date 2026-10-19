/*
 * Rufus Linux port: core application model.
 *
 * Linux-flavoured equivalent of upstream src/rufus.h + src/drive.h +
 * src/format.h data model. Keeps upstream naming (enums, flags, structure
 * fields, HAS_* macros) so that adapted module copies stay diffable against
 * the Windows sources.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RUFUS_LINUX_RFUS_H
#define RUFUS_LINUX_RFUS_H

#include "win32compat.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>

// Upstream resource IDs (plain defines, fully portable)
#include "resource.h"
// Upstream helper macros/inlines (byte swapping, popcnt, remap, ...)
#include "missing.h"

#define if_assert_fails(cond)       assert(cond); if (!(cond))
#define if_assert_succeeds(cond)    assert(cond); if ((cond))
extern BOOL usb_debug;
#define uuprintf(...) do { if (usb_debug) uprintf(__VA_ARGS__); } while(0)
#define DRIVE_ACCESS_TIMEOUT        15000   // How long we should retry drive access (in ms)
BOOL CompareGUID(const GUID* guid1, const GUID* guid2);

/* Upstream version this port is built from (generated from rufus.rc) */
#include "version.h"
#define RUFUS_URL               "https://rufus.ie"
#define MAX_DRIVES              64
#define MAX_PARTITIONS          16
#define MAX_WININST             4
#define MAX_ISO_TO_ESP_SIZE     (1 * GB)
#define MAX_SECTORS_TO_CLEAR    (8 * MB / 512)
#define LARGE_FAT32_SIZE        (32 * GB)
#define DD_BUFFER_SIZE          (32 * MB)
#define MAX_ISO_SIZE            (64 * TB)
#define MAX_VHD_SIZE            (16 * TB)

#define KB                      1024LL
#define MB                      (1024LL * KB)
#define GB                      (1024LL * MB)
#define TB                      (1024LL * GB)
#define PB                      (1024LL * TB)

#define DRIVE_INDEX_MIN         0x00000080
#define DRIVE_INDEX_MAX         0x000000C0
#define MAX_DISPLAY_SIZE        999999999999999999ULL

#define WRITE_RETRIES           4

#ifndef MAX
#define MAX(a, b)               (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b)               (((a) < (b)) ? (a) : (b))
#endif
#define FLOOR_ALIGN(x, y)       (((x) / (y)) * (y))
#define CEILING_ALIGN(x, y)     ((((x) + (y) - 1) / (y)) * (y))
#define SIZE_MAX_SPARE

/* ------------------------------------------------------------------ */
/* Enumerations (values must match upstream exactly)                   */
/* ------------------------------------------------------------------ */
enum fs_type {
    FS_UNKNOWN = -1,
    FS_FAT16 = 0,
    FS_FAT32,
    FS_NTFS,
    FS_UDF,
    FS_EXFAT,
    FS_REFS,
    FS_EXT2,
    FS_EXT3,
    FS_EXT4,
    FS_MAX
};

enum boot_type {
    BT_NON_BOOTABLE = 0,
    BT_MSDOS,
    BT_FREEDOS,
    BT_IMAGE,
    BT_SYSLINUX_V4,
    BT_SYSLINUX_V6,
    BT_REACTOS,
    BT_GRUB4DOS,
    BT_GRUB2,
    BT_UEFI_NTFS,
    BT_MAX
};

enum target_type {
    TT_BIOS = 0,
    TT_UEFI,
    TT_MAX
};

enum image_option_type {
    IMOP_WIN_STANDARD = 0,
    IMOP_WIN_EXTENDED,
    IMOP_WIN_TO_GO,
    IMOP_MAX
};

enum file_io_type {
    FILE_IO_READ = 0,
    FILE_IO_WRITE,
    FILE_IO_APPEND
};

/* Partition styles: same values as Microsoft's PARTITION_STYLE_* */
#define PARTITION_STYLE_MBR     1
#define PARTITION_STYLE_GPT     2
#define PARTITION_STYLE_RAW     0
#define PARTITION_STYLE_SFD     PARTITION_STYLE_RAW

/* Drive types: same values as GetDriveType() */
#define DRIVE_UNKNOWN           0
#define DRIVE_NO_ROOT_DIR       1
#define DRIVE_REMOVABLE         2
#define DRIVE_FIXED             3
#define DRIVE_REMOTE            4
#define DRIVE_CDROM             5
#define DRIVE_RAMDISK           6

/* Media types (winioctl.h subset, values preserved) */
typedef enum _MEDIA_TYPE {
    Unknown = 0, F5_1Pt2_512, F3_1Pt44_512, F3_2Pt88_512, F3_20Pt8_512,
    F3_720_512, F5_360_512, F5_320_512, F5_320_1024, F5_180_512,
    F5_160_512, RemovableMedia, FixedMedia, F3_120M_512, F3_640_512 = 20,
    F5_640_512, F5_720_512, F3_1Pt2_512, F3_1Pt44_512_, F3_2Pt88_512_,
    F3_20Pt8_512_, F3_120M_512_, F3_640_512_, F5_640_512_, F5_720_512_,
    MEDIA_TYPE_MAX = 26
} MEDIA_TYPE;

/* Action type for progress bar breakdown (upstream rufus.h:285) */
enum action_type {
    OP_NOOP_WITH_TASKBAR = -3,
    OP_NOOP = -2,
    OP_INIT = -1,
    OP_ANALYZE_MBR = 0,
    OP_BADBLOCKS,
    OP_ZERO_MBR,
    OP_PARTITION,
    OP_FORMAT,
    OP_CREATE_FS,
    OP_FIX_MBR,
    OP_FILE_COPY,
    OP_PATCH,
    OP_FINALIZE,
    OP_EXTRACT_ZIP,
    OP_MAX
};

enum ArchType {
    ARCH_UNKNOWN = 0,
    ARCH_X86_32,
    ARCH_X86_64,
    ARCH_ARM_32,
    ARCH_ARM_64,
    ARCH_IA_64,
    ARCH_RISCV_64,
    ARCH_LOONGARCH_64,
    ARCH_EBC,
    ARCH_MAX
};

/* ------------------------------------------------------------------ */
/* Partition flags (upstream drive.h)                                  */
/* ------------------------------------------------------------------ */
#define XP_MSR                  0x01
#define XP_ESP                  0x02
#define XP_UEFI_NTFS            0x04
#define XP_COMPAT               0x08
#define XP_PERSISTENCE          0x10

#define PI_MAIN                 0
#define PI_ESP                  1
#define PI_CASPER               2
#define PI_UEFI_NTFS            3
#define PI_MAX                  4

#define RUFUS_EXTRA_PARTITION_TYPE 0xea

#define FP_FORCE                0x00000001
#define FP_QUICK                0x00000002
#define FP_COMPRESSION          0x00000004
#define FP_DUPLICATE_METADATA   0x00000008
#define FP_LARGE_FAT32          0x00010000
#define FP_NO_BOOT              0x00020000
#define FP_CREATE_PERSISTENCE_CONF 0x00040000
#define FP_NO_PROGRESS          0x00080000

/* GPT partitioning attributes */
#define GPT_PARTITION_NO_DRIVE_LETTER_ 0x1000000000000000ULL
#define GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER 0x8000000000000000ULL
#define GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY       0x1000000000000000ULL

/* MBR disk signature used by Rufus to flag an MBR+UEFI layout ('UEFI') */
#define MBR_UEFI_MARKER         0x49464555

/* Maximum sector size we handle in the partition/format code */
#ifndef MAX_SECTOR_SIZE
#define MAX_SECTOR_SIZE         4096
#endif

/* Upstream GUIDs (defined in parttypes.c through the vendored
 * src/gpt_types.h, declared here for the partition writers) */
extern const GUID PARTITION_GENERIC_ESP;
extern const GUID PARTITION_LINUX_DATA;
extern const GUID PARTITION_MICROSOFT_DATA;
extern const GUID PARTITION_MICROSOFT_RESERVED;

/* parttable.c */
BOOL write_partition_table(HANDLE hDrive, int partition_style);
extern BOOL mbr_uefi_marker;

/* ------------------------------------------------------------------ */
/* Image / drive reports (upstream rufus.h / drive.h)                  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t build;
    uint16_t revision;
} winver_t;

typedef struct {
    uint8_t type;
    char path[64];
} efi_boot_entry_t;

#define NB_OLD_C32              2
#define OLD_C32_NAMES           { "menu.c32", "vesamenu.c32" }
#define OLD_C32_THRESHOLD       { 53500, 148000 }

typedef struct {
    char label[192];
    char usb_label[192];
    char cfg_path[128];
    char reactos_path[128];
    char wininst_path[MAX_WININST][64];
    efi_boot_entry_t efi_boot_entry[64];
    char efi_img_path[128];
    uint64_t image_size;
    uint64_t projected_size;
    int64_t mismatch_size;
    uint32_t wininst_version;
    BOOLEAN is_iso;
    int8_t is_bootable_img;
    BOOLEAN is_vhd;
    BOOLEAN is_windows_img;
    BOOLEAN disable_iso;
    BOOLEAN rh8_derivative;
    uint16_t winpe;
    uint16_t has_efi;
    uint8_t has_secureboot_bootloader;
    uint8_t has_md5sum;
    uint8_t wininst_index;
    uint8_t has_symlinks;
    uint8_t has_4GB_file;
    BOOLEAN has_long_filename;
    BOOLEAN has_deep_directories;
    BOOLEAN has_bootmgr;
    BOOLEAN has_bootmgr_efi;
    BOOLEAN has_autorun;
    BOOLEAN has_old_c32[NB_OLD_C32];
    BOOLEAN has_old_vesamenu;
    BOOLEAN has_efi_syslinux;
    BOOLEAN has_grub4dos;
    uint8_t has_grub2;
    uint8_t has_grub2_fs;
    BOOLEAN has_compatresources_dll;
    BOOLEAN has_panther_unattend;
    BOOLEAN has_kolibrios;
    BOOLEAN needs_syslinux_overwrite;
    BOOLEAN needs_ntfs;
    BOOLEAN uses_casper;
    BOOLEAN uses_minint;
    uint8_t compression_type;
    winver_t win_version;
    uint16_t sl_version;
    char sl_version_str[12];
    char sl_version_ext[32];
    char grub2_version[192];
} RUFUS_IMG_REPORT;

#define WINPE_I386              0x0007
#define WINPE_AMD64             0x0023
#define WINPE_MININT            0x01C0
#define SPECIAL_WIM_VERSION     0x000E0000
#define HAS_KOLIBRIOS(r)        (r.has_kolibrios)
#define HAS_REACTOS(r)          (r.reactos_path[0] != 0)
#define HAS_GRUB(r)             ((r.has_grub2) || (r.has_grub4dos))
#define HAS_SYSLINUX(r)         (r.sl_version != 0)
#define HAS_BOOTMGR_BIOS(r)     (r.has_bootmgr)
#define HAS_BOOTMGR_EFI(r)      (r.has_bootmgr_efi)
#define HAS_BOOTMGR(r)          (HAS_BOOTMGR_BIOS(r) || HAS_BOOTMGR_EFI(r))
#define HAS_REGULAR_EFI(r)      (r.has_efi & 0x7FFE)
#define HAS_WININST(r)          (r.wininst_index != 0)
#define HAS_WINPE(r)            (((r.winpe & WINPE_I386) == WINPE_I386)||((r.winpe & WINPE_AMD64) == WINPE_AMD64)||((r.winpe & WINPE_MININT) == WINPE_MININT))
#define HAS_WINDOWS(r)          (HAS_BOOTMGR(r) || (r.uses_minint) || HAS_WINPE(r))
#define HAS_WIN7_EFI(r)         ((r.has_efi == 1) && HAS_WININST(r))
#define HAS_FATLESS_GRUB(r)     ((r.has_grub2 & 0x80) && !(r.has_grub2_fs & 0x1))
#define HAS_NTFSLESS_GRUB(r)    ((r.has_grub2 & 0x80) && !(r.has_grub2_fs & 0x4))
#define IS_WINDOWS_1X(r)        (r.has_bootmgr_efi && (r.win_version.major >= 10))
#define IS_WINDOWS_11(r)        (r.has_bootmgr_efi && (r.win_version.major >= 11))
#define HAS_EFI_IMG(r)          (r.efi_img_path[0] != 0)
#define IS_DD_BOOTABLE(r)       (r.is_bootable_img > 0)
#define IS_DD_ONLY(r)           ((r.is_bootable_img > 0) && (!r.is_iso || r.disable_iso))
#define IS_EFI_BOOTABLE(r)      (r.has_efi != 0)
#define IS_BIOS_BOOTABLE(r)     (HAS_BOOTMGR(r) || HAS_SYSLINUX(r) || HAS_WINPE(r) || HAS_GRUB(r) || HAS_REACTOS(r) || HAS_KOLIBRIOS(r))
#define HAS_WINTOGO(r)          (HAS_BOOTMGR(r) && IS_EFI_BOOTABLE(r) && HAS_WININST(r))
#define HAS_PERSISTENCE(r)      ((HAS_SYSLINUX(r) || HAS_GRUB(r)) && !(HAS_WINDOWS(r) || HAS_REACTOS(r) || HAS_KOLIBRIOS(r)))
#define IS_FAT(fs)              ((fs == FS_FAT16) || (fs == FS_FAT32))
#define IS_EXT(fs)              ((fs >= FS_EXT2) && (fs <= FS_EXT4))
#define IS_FAT32_COMPAT(r)      (((r.has_4GB_file == 0 && !HAS_FATLESS_GRUB(r)) || (r.has_4GB_file == 0x11 && allow_dual_uefi_bios)) && !r.needs_ntfs)
#define SYMLINKS_RR             0x01
#define SYMLINKS_UDF            0x02
#define SL_MAJOR(x)             ((uint8_t)((x)>>8))
#define SL_MINOR(x)             ((uint8_t)(x))

/* Basic String Array (upstream rufus.h) */
typedef struct {
    char** String;
    uint32_t Index;
    uint32_t Max;
} StrArray;
extern void StrArrayCreate(StrArray* arr, uint32_t initial_size);
extern int32_t StrArrayAdd(StrArray* arr, const char* str, BOOL duplicate);
extern int32_t StrArrayAddUnique(StrArray* arr, const char* str, BOOL duplicate);
extern int32_t StrArrayFind(StrArray* arr, const char* str);
extern void StrArrayClear(StrArray* arr);
extern void StrArrayDestroy(StrArray* arr);
#define IsStrArrayEmpty(arr) (arr.Index == 0)

enum EFI_BOOT_TYPE {
    EBT_MAIN = 0,
    EBT_GRUB,
    EBT_MOKMANAGER,
    EBT_BOOTMGR
};

/* Path display conversion (upstream rufus.h) */
static __inline void static_repchr(char* p, char s, char r) {
    if (p != NULL) while (*p != 0) { if (*p == s) *p = r; p++; }
}
#define to_unix_path(str) static_repchr(str, '\\', '/')
#define to_windows_path(str) static_repchr(str, '/', '\\')

typedef struct {
    wchar_t Name[36];
    uint64_t Offset;
    uint64_t Size;
} LINUX_PARTITION_ENTRY;

typedef struct {
    uint64_t DiskSize;
    DWORD DeviceNumber;
    DWORD SectorsPerTrack;
    DWORD SectorSize;
    DWORD FirstDataSector;
    MEDIA_TYPE MediaType;
    int PartitionStyle;
    int nPartitions;
    LINUX_PARTITION_ENTRY Partition[MAX_PARTITIONS];
    int FSType;
    char proposed_label[16];
    BOOL has_protective_mbr;
    BOOL has_mbr_uefi_marker;
    struct {
        ULONG Allowed;
        ULONG Default;
    } ClusterSize[FS_MAX];
} RUFUS_DRIVE_INFO;

typedef struct {
    char* id;
    char* name;
    char* display_name;
    char* label;
    char* hub;
    DWORD index;
    uint32_t port;
    uint64_t size;
} RUFUS_DRIVE;

typedef struct {
    DWORD Type;
    DWORD DeviceNum;
    DWORD BufSize;
    LONGLONG DeviceSize;
    char* DevicePath;
    char* ImagePath;
    char* Label;
} IMG_SAVE;

/* ------------------------------------------------------------------ */
/* Globals (same names as upstream)                                    */
/* ------------------------------------------------------------------ */
extern RUFUS_DRIVE_INFO SelectedDrive;
extern int partition_index[PI_MAX];
extern RUFUS_IMG_REPORT img_report;
extern const int nb_steps[FS_MAX];
extern const char* FileSystemLabel[FS_MAX];
extern const char* FileSystemName[FS_MAX];

extern DWORD DriveIndex;
extern BOOL IsDriveRemovable;
extern BOOL IsDriveUSB;
extern BOOL large_drive;
extern BOOL zero_drive;
extern BOOL quick_format;
extern BOOL write_as_image;
extern BOOL write_as_esp;
extern BOOL allow_dual_uefi_bios;
extern BOOL force_large_fat32;
extern BOOL enable_ntfs_compression;
extern BOOL use_vds;
extern BOOL use_extend_size;
extern BOOL save_image_to_file;
extern int fs_type, boot_type, target_type, cluster_size;
extern int partition_type;
extern int image_option;
extern char image_path[MAX_PATH];
extern char label[64];
extern char system_dir[MAX_PATH];
extern char app_dir[MAX_PATH];
extern DWORD ErrorStatus;
extern DWORD LastWriteError;
extern uint64_t persistence_size;
extern int ext_version;
extern int setup_type;
extern unsigned long ulBytesPerSector;  /* NB: unsigned long, as in ms-sys br.c */
extern const char* image_name;

#define CHECK_FOR_USER_CANCEL   if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)) goto out
#define EXIT_ON_USER_CANCEL     if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)) return

/* ------------------------------------------------------------------ */
/* Platform / module APIs (implemented under linux/src)                */
/* ------------------------------------------------------------------ */

/* devices.c */
BOOL    GetDevices(DWORD devnum);
void    ClearDrives(void);
int     GetDriveCount(void);
BOOL    SelectDriveByIndex(DWORD index);
BOOL    SelectDriveByPath(const char* path);
int     GetSelectedDriveIndex(void);
const char* GetDriveEntryPath(int index);
BOOL    GetDriveEntryInfo(int index, uint64_t* size, BOOL* usb, BOOL* removable,
                          uint16_t* vid, uint16_t* pid, const char** name);
BOOL    IsVolumeMounted(const char* device_path, char* mountpoint, size_t len);
void    probe_volume_fs(HANDLE h, char* name, DWORD name_size);
char*   GetPhysicalName(DWORD DriveIndex);
HANDLE  GetPhysicalHandle(DWORD DriveIndex, BOOL bLockDrive, BOOL bWriteAccess, BOOL bWriteShare);
HANDLE  GetLogicalHandle(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bLockDrive, BOOL bWriteAccess, BOOL bWriteShare);
char*   GetLogicalName(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bKeepTrailingSlash, BOOL bSilent);
char*   AltGetLogicalName(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bKeepTrailingBackslash, BOOL bSilent);
BOOL    GetDrivePartitionData(DWORD DriveIndex, char* FileSystemName, DWORD FileSystemNameSize, BOOL bSilent);
uint64_t GetDriveSize(DWORD DriveIndex);
BOOL    GetDriveLabel(DWORD DriveIndex, char* letters, char** label, BOOL bSilent);
char    GetUnusedDriveLetter(void);
BOOL    IsMediaPresent(DWORD DriveIndex);
BOOL    UnmountVolume(HANDLE hDrive);
BOOL    AltUnmountVolume(const char* drive_name, BOOL bSilent);
char*   AltMountVolume(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bSilent);
char*   GetMountedPath(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bSilent);
BOOL    RemountVolume(char* drive_name, BOOL bSilent);
BOOL    MountVolume(char* drive_name, char* drive_guid);
BOOL    AnalyzeMBR(HANDLE hPhysicalDrive, const char* TargetName, BOOL bSilent);
BOOL    AnalyzePBR(HANDLE hLogicalVolume);
BOOL    RefreshDriveLayout(HANDLE hDrive);
BOOL    InitializeDisk(HANDLE hDrive);
BOOL    CycleDevice(int index);
BOOL    CyclePort(int index);
BOOL    IsDriveUSBDevice(DWORD DriveIndex);
BOOL    IsFilteredDrive(DWORD DriveIndex);
UINT    GetDriveTypeFromIndex(DWORD DriveIndex);
BOOL    IsSourceImageLocatedOnTargetDrive(DWORD DriveIndex);
const char* GetMBRPartitionType(const uint8_t type);
const char* GetGPTPartitionType(const GUID* guid);
const char* GetExtFsLabel(DWORD DriveIndex, uint64_t PartitionOffset);

/* Device classification helpers (devices.c) */
BOOL    IsUsbDevice(DWORD DriveIndex);
BOOL    IsFixedMedia(DWORD DriveIndex);

/* partition.c */
BOOL    CreatePartition(HANDLE hDrive, int partition_style, int file_system, BOOL mbr_uefi_marker, uint8_t extra_partitions);
BOOL    ClearMBRGPT(HANDLE hDrive, uint64_t DiskSize, uint32_t SectorSize);
int64_t GetEspOffset(HANDLE hPhysical);
BOOL    ToggleEsp(DWORD DriveIndex, uint64_t PartitionOffset);
const char* GetFsName(HANDLE hPhysical, uint64_t StartingOffset);
uint64_t GetPartitionSize(DWORD DriveIndex, uint64_t PartitionOffset);

/* format.c */
BOOL    FormatPartition(DWORD DriveIndex, uint64_t PartitionOffset, DWORD UnitAllocationSize, USHORT FSType, LPCSTR Label, DWORD Flags);
BOOL    FormatLargeFAT32(DWORD DriveIndex, uint64_t PartitionOffset, DWORD ClusterSize, LPCSTR FSName, LPCSTR Label, DWORD Flags);
BOOL    FormatExtFs(DWORD DriveIndex, uint64_t PartitionOffset, DWORD BlockSize, LPCSTR FSName, LPCSTR Label, DWORD Flags);
DWORD WINAPI FormatThread(void* param);
BOOL    WriteMBR(HANDLE hDrive);
BOOL    WriteSBR(HANDLE hDrive);
BOOL    WritePBR(HANDLE hDrive);
BOOL    CheckDisk(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bSilent);
BOOL    SetAutoMount(BOOL enable);
void    ToValidLabel(char* Label, BOOL bFAT);

/* bled compression types and API */
#include "bled/bled.h"
#define IMG_COMPRESSION_FFU     (BLED_COMPRESSION_MAX)
#define IMG_COMPRESSION_VHD     (BLED_COMPRESSION_MAX + 1)
#define IMG_COMPRESSION_VHDX    (BLED_COMPRESSION_MAX + 2)
#define WRITE_TIMEOUT           5000
extern BOOL lock_drive;
extern BOOL fast_zeroing;

/* badblocks.c (see badblocks.h) */
#define TID_BADBLOCKS_UPDATE    0x1001
#define BADLOCKS_PATTERN_TYPES  5
#define BADBLOCK_PATTERN_COUNT  4
#define BADBLOCK_PATTERN_ONE_PASS   {0x55, 0x00, 0x00, 0x00}
#define BADBLOCK_PATTERN_TWO_PASSES {0x55, 0xaa, 0x00, 0x00}
#define BADBLOCK_PATTERN_SLC        {0x00, 0xff, 0x55, 0xaa}
#define BADCLOCK_PATTERN_MLC        {0x00, 0xff, 0x33, 0xcc}
#define BADBLOCK_PATTERN_TLC        {0x00, 0xff, 0x1c71c7, 0xe38e38}
#define BADBLOCK_BLOCK_SIZE     (512 * KB)
extern int nb_passes_sel, flash_type_sel;
extern BOOL detect_fakes;   /* upstream's IDC_NB_PASSES / pattern type combos */
extern HWND hMainDialog;        /* NULL in the CLI; the GUI's main window */

/* image.c */
/* format.c WriteDrive() is static (as upstream) */

/* boot.c */
BOOL    InstallSyslinux(DWORD DriveIndex, const char* volume_path, int fs_type);
BOOL    InstallGrub2(DWORD DriveIndex, const char* drive_name, int fs_type);
BOOL    InstallGrub4dos(DWORD DriveIndex, const char* drive_name);
BOOL    WriteUefiNtfsImage(HANDLE hDrive, uint64_t PartitionOffset, uint64_t PartitionSize);
BOOL    GetSyslinuxVersion(uint16_t* sl_version, char* str, size_t len);

/* dos.c */
BOOL    ExtractDOS(const char* drive_name);
BOOL    SetDOSLocale(const char* drive_name, BOOL bFreeDOS);

/* iso.c (upstream signatures; see src/iso.c) */
BOOL     ExtractISO(const char* src_iso, const char* dest_dir, BOOL scan);
int64_t  ExtractISOFile(const char* iso, const char* iso_file, const char* dest_file, DWORD attributes);
uint32_t ReadISOFileToBuffer(const char* iso, const char* iso_file, uint8_t** buf);
BOOL     HasEfiImgBootLoaders(void* iso);
BOOL     DumpFatDir(void* iso, const char* path, int32_t cluster);
void     GetGrubVersion(char* buf, size_t buf_size, const char* source);
void     GetGrubFs(char* buf, size_t buf_size);
void     GetEfiBootInfo(char* buf, size_t buf_size, const char* source);
/* Save-image support (upstream vhd.c): stubbed on Linux for now */
BOOL     IsoSaveImage(IMG_SAVE* img_save);
DWORD    WINAPI IsoSaveImageThread(void* param);
void     OpticalDiscSaveImage(void);

/* hash.c (as upstream rufus.h) */
#if !defined(ALIGNED)
#define ALIGNED(m) __attribute__ ((__aligned__(m)))
#endif
enum hash_type {
    HASH_MD5 = 0,
    HASH_SHA1,
    HASH_SHA256,
    HASH_SHA512,
    HASH_MAX
};
#define MD5_BLOCKSIZE       64
#define SHA1_BLOCKSIZE      64
#define SHA256_BLOCKSIZE    64
#define SHA512_BLOCKSIZE    128
#define MAX_BLOCKSIZE       SHA512_BLOCKSIZE
#define MD5_HASHSIZE        16
#define SHA1_HASHSIZE       20
#define SHA256_HASHSIZE     32
#define SHA512_HASHSIZE     64
#define MAX_HASHSIZE        SHA512_HASHSIZE
typedef struct ALIGNED(64) {
    uint8_t buf[MAX_BLOCKSIZE];
    uint64_t state[8];
    uint64_t bytecount;
} HASH_CONTEXT;
typedef struct {
    char name[256];
    uint8_t thumbprint[SHA1_HASHSIZE];
} cert_info_t;
typedef void hash_init_t(HASH_CONTEXT* ctx);
typedef void hash_write_t(HASH_CONTEXT* ctx, const uint8_t* buf, size_t len);
typedef void hash_final_t(HASH_CONTEXT* ctx);
extern hash_init_t* hash_init[HASH_MAX];
extern hash_write_t* hash_write[HASH_MAX];
extern hash_final_t* hash_final[HASH_MAX];
typedef struct {
    char* product;
    uint32_t version;
} sbat_entry_t;
typedef struct {
    uint32_t count;
    uint8_t list[0][SHA1_HASHSIZE];
} thumbprint_list_t;
extern char hash_str[HASH_MAX][150];
extern uint32_t hash_count[HASH_MAX];
extern BOOL enable_extra_hashes, cpu_has_sha1_accel, cpu_has_sha256_accel;
extern sbat_entry_t* sbat_entries;
extern thumbprint_list_t *sb_active_certs, *sb_revoked_certs;
extern char *sbat_level_txt, *sb_active_txt, *sb_revoked_txt;
extern void (*hash_results_hook)(void);
BOOL     DetectSHA1Acceleration(void);
BOOL     DetectSHA256Acceleration(void);
BOOL     HashFile(const unsigned type, const char* path, uint8_t* sum);
BOOL     PE256Buffer(uint8_t* buf, uint32_t len, uint8_t* hash);
BOOL     HashBuffer(const unsigned type, const uint8_t* buf, const size_t len, uint8_t* sum);
uint8_t* StringToHash(const char* str);
BOOL     FileMatchesHash(const char* path, const char* str);
BOOL     BufferMatchesHash(const uint8_t* buf, const size_t len, const char* str);
void     UpdateMD5Sum(const char* dest_dir, const char* md5sum_name);
int      IsBootloaderRevoked(uint8_t* buf, uint32_t len);
BOOL     IsSignedBySecureBootAuthority(uint8_t* buf, uint32_t len);
DWORD WINAPI HashThread(void* param);
int      TestHashes(void);
/* parser.c / pki.c */
sbat_entry_t* GetSbatEntries(char* sbatlevel);
thumbprint_list_t* GetThumbprintEntries(char* thumbprints_txt);
uint16_t GetPeArch(uint8_t* buf);
uint8_t* GetPeSection(uint8_t* buf, const char* name, uint32_t* len);
uint8_t* GetPeSignatureData(uint8_t* buf);
uint8_t* RvaToPhysical(uint8_t* buf, uint32_t rva);
uint32_t FindResourceRva(const wchar_t* name, uint8_t* root, uint8_t* dir, uint32_t* len);
enum ArchType MachineToArch(WORD machine);
int      GetIssuerCertificateInfo(uint8_t* cert, cert_info_t* info);
BOOL     UseLocalDbx(int arch);
void     FetchSecureBootData(void);
extern BOOL expert_mode;
extern int default_thread_priority;
extern const char* efi_archname[ARCH_MAX];

/* syslinux/libboot glue used by adapted syslinux.c */
int     syslinux_read_boot_sector(HANDLE hDrive, size_t sector_size, void* buf);
int     syslinux_write_boot_sector(HANDLE hDrive, size_t sector_size, const void* buf);

/* vhd.c (wimlib wrappers, as upstream) */
BOOL    WimExtractFile(const char* image, int index, const char* src, const char* dst);
const char* iso_target_relative_path(const char* path);
#ifndef MBR_SIZE
#define MBR_SIZE                512
#endif
/* vhdfile.c: VHD/VHDX reader (stands for the Windows Virtual Disk API) */
char*   VhdMountImageAndGetSize(const char* path, uint64_t* disk_size);
void    VhdUnmountImage(void);
int64_t VhdReadImage(void* buf, uint64_t offset, uint32_t len);
extern BOOL has_ffu_support;
uint64_t VhdGetSize(void);
/* badblocks.c (see badblocks.h) */
#define TID_BADBLOCKS_UPDATE    0x1001
#define BADLOCKS_PATTERN_TYPES  5
#define BADBLOCK_PATTERN_COUNT  4
#define BADBLOCK_PATTERN_ONE_PASS   {0x55, 0x00, 0x00, 0x00}
#define BADBLOCK_PATTERN_TWO_PASSES {0x55, 0xaa, 0x00, 0x00}
#define BADBLOCK_PATTERN_SLC        {0x00, 0xff, 0x55, 0xaa}
#define BADCLOCK_PATTERN_MLC        {0x00, 0xff, 0x33, 0xcc}
#define BADBLOCK_PATTERN_TLC        {0x00, 0xff, 0x1c71c7, 0xe38e38}
#define BADBLOCK_BLOCK_SIZE     (512 * KB)
extern int nb_passes_sel, flash_type_sel;
extern BOOL detect_fakes;   /* upstream's IDC_NB_PASSES / pattern type combos */
extern HWND hMainDialog;        /* NULL in the CLI; the GUI's main window */

/* image.c */
BOOL     ScanImage(void);
void     GetBootladerInfo(void);
void     DisplayISOProps(void);
void     GetPartitionSchemeAndTargetSystem(int selected_pt, BOOL allowed_pt[3], BOOL allowed_tt[3]);
int      GetPreferredFSForImage(uint32_t fs_mask);
/* devices.c */
char*    GetVolumeDevicePath(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bSilent);
BOOL     IsVirtualDrive(DWORD DriveIndex);
BYTE     GetProcessSearch(uint32_t timeout, uint8_t access_mask, BOOL bIgnoreStaleProcesses);
#define SEARCH_PROCESS_TIMEOUT  5000
/* saveimage.c */
BOOL     SaveImage(const char* device_path, DWORD DriveIndex, const char* image_path_out);
/* update.c */
BOOL     CheckForUpdates(BOOL force);
/* worker.c */
extern BOOL worker_mode;
void     worker_init(void);
/* zip.c */
extern char* archive_path;
BOOL     ExtractZip(const char* src_zip, const char* dest_dir);
/* icon.c */
extern BOOL extended_label;
BOOL     SetAutorun(const char* path);
BOOL     GetResourceFilePath(const char* rel_path, char* out, size_t out_len);
#define MORE_INFO_URL           0xFFFF
#define MB_CLOSE                0x0F

/* process.c (upstream stdio.c/stdfn.c: RunCommandWithProgress) */
DWORD    RunCommandWithProgress(const char* cmd, const char* dir, BOOL log, int msg, const char* pattern);
#define  RunCommand(cmd, dir, log) RunCommandWithProgress(cmd, dir, log, 0, NULL)

/* resources.c (upstream signature: name/type are MAKEINTRESOURCEA/string) */
uint8_t* GetResource(HMODULE module, char* name, char* type, const char* desc, DWORD* len, BOOL duplicate);
DWORD    GetResourceSize(HMODULE module, char* name, char* type, const char* desc);
void     free_resource_cache(void);
extern HMODULE hMainInstance;

/* badblocks.c */
extern volatile BOOL badblocks_running;

/* utils.c */
char*    SizeToHumanReadable(uint64_t size, BOOL copy_to_log, BOOL fake_units);
char*    TimestampToHumanReadable(uint64_t ts);
const char* GetDriveLettersStr(void);
BOOL     IsBufferInDB(const unsigned char* buf, const size_t len);
BOOL     IsFileInDB(const char* path);
int      IsHDD(DWORD DriveIndex, uint16_t vid, uint16_t pid, const char* strid);
char*    GuidToString(const GUID* guid, BOOL bDecorated);
GUID     StringToGuid(const char* str);
BOOL     GetGuidFromString(const char* str, GUID* guid);
uint64_t FtoU64(double f);
uint32_t read_file(const char* path, uint8_t** buf);
uint32_t write_file(const char* path, const uint8_t* buf, const uint32_t size);
char*    _printbits(size_t const size, void const* const ptr, int leading_zeroes);
void     DumpBufferHex(void* buf, size_t size);
extern BOOL right_to_left_mode;

/* Default-locale helper for error messages (upstream stdio.c) */
const char* _StrError(DWORD error_code);

/* ------------------------------------------------------------------ */
/* UI / logging API (messages.c + localization.c)                      */
/* ------------------------------------------------------------------ */
void     uprintf(const char* format, ...) __attribute__((format(printf, 1, 2)));
void     wuprintf(const wchar_t* format, ...);
void     uprintfs(const char* str);
void     uprint_progress(uint64_t cur_value, uint64_t max_value);
void     duprintf(const char* format, ...) __attribute__((format(printf, 1, 2)));
extern void (*log_hook)(const char* line);
extern void (*progress_hook)(const char* label, const char* text, double percent);
extern void (*status_hook)(BOOL info, const char* text);
extern int (*notification_hook)(int type, const char* title, const char* msg);
void     _uprintf(const char* file, int line, const char* function, const char* format, ...);

typedef struct {
    WORD id;
    union {
        void* callback;
        char* url;
    };
} notification_info;
void     PrintStatusInfo(BOOL info, BOOL debug, unsigned int duration, int msg_id, ...);
#define PrintStatus(...) PrintStatusInfo(FALSE, FALSE, __VA_ARGS__)
#define PrintStatusDebug(...) PrintStatusInfo(FALSE, TRUE, __VA_ARGS__)
#define PrintInfo(...) PrintStatusInfo(TRUE, FALSE, __VA_ARGS__)
#define PrintInfoDebug(...) PrintStatusInfo(TRUE, TRUE, __VA_ARGS__)
int      NotificationEx(int type, const char* dont_display_setting, const notification_info* more_info, const char* title, const char* format, ...);
#define Notification(type, title, ...) NotificationEx(type, NULL, NULL, title, __VA_ARGS__)
void     InitProgress(BOOL bOnlyFormat);
void     UpdateProgress(int op, float percent);
void     UpdateProgressStatus(int op, const char* status);
void     _UpdateProgressWithInfo(int op, int msg, uint64_t processed, uint64_t total, BOOL force);
#define UpdateProgressWithInfo(op, msg, processed, total) _UpdateProgressWithInfo(op, msg, processed, total, FALSE)
#define UpdateProgressWithInfoUpTo(upto, op, msg, processed, total) _UpdateProgressWithInfo(op, msg, (processed) * (upto), (total) * 100, FALSE)
#define UpdateProgressWithInfoForce(op, msg, processed, total) _UpdateProgressWithInfo(op, msg, processed, total, TRUE)
#define UpdateProgressWithInfoInit(hProgressDialog, bNoAltMode) _UpdateProgressWithInfo(OP_INIT, (int)bNoAltMode, (uint64_t)(uintptr_t)hProgressDialog, 0, FALSE)
void     CenterDialog(HWND hDlg);
void     SetTaskbarProgressState(int state);

/* localization */
char*    lmprintf(uint32_t msg_id, ...);
void     init_localization(void);
void     exit_localization(void);
void     reinit_localization(void);
BOOL     set_locale(const char* locale_name);
char*    get_current_locale_name(void);
void     toggle_default_locale(void);

/* settings (learned from upstream settings.h: INI-backed via parser.c).
 * The Linux port implements the parser natively in settings.c. */
extern char* ini_file;
char*    get_token_data_file_indexed(const char* token, const char* filename, int index);
#define  get_token_data_file(token, filename) get_token_data_file_indexed(token, filename, 1)
char*    set_token_data_file(const char* token, const char* data, const char* filename);
int64_t  ReadSetting64(const char* key);
BOOL     WriteSetting64(const char* key, int64_t val);
int32_t  ReadSetting32(const char* key);
BOOL     WriteSetting32(const char* key, int32_t val);
#define  ReadSettingBool(key) (ReadSetting32(key) != 0)
BOOL     WriteSettingBool(const char* key, BOOL val);
char*    ReadSettingStr(const char* key);
BOOL     WriteSettingStr(const char* key, const char* val);
void     init_settings(void);

/* ------------------------------------------------------------------ */
/* Misc helpers                                                        */
/* ------------------------------------------------------------------ */
#define APPLICATION_NAME        "Rufus"
#define APPLICATION_URL         "https://rufus.ie"
/* Folder for downloaded resources, as upstream (app_data_dir/FILES_DIR) */
#define FILES_DIR               APPLICATION_NAME
#define IGNORE_RETVAL(expr)     do { (void)(expr); } while (0)

/* Settings names (upstream settings.h) */
#define SETTING_ADVANCED_MODE               "AdvancedMode"
#define SETTING_DISABLE_RUFUS_MBR           "DisableRufusMBR"
#define SETTING_DISABLE_SECURE_BOOT_NOTICE  "DisableSecureBootNotice"
#define SETTING_DISABLE_VHDS                "DisableVHDs"
#define SETTING_ENABLE_EXTRA_HASHES         "EnableExtraHashes"
#define SETTING_ENABLE_FILE_INDEXING        "EnableFileIndexing"
#define SETTING_FORCE_LARGE_FAT32_FORMAT    "ForceLargeFat32Formatting"
#define SETTING_IGNORE_BOOT_MARKER          "IgnoreBootMarker"
#define SETTING_INCLUDE_BETAS               "CheckForBetas"
#define SETTING_LAST_UPDATE                 "LastUpdateCheck"
#define SETTING_LOCALE                      "Locale"
#define SETTING_UPDATE_INTERVAL             "UpdateCheckInterval"
#define SETTING_USE_EXT_VERSION             "UseExtVersion"
#define SETTING_USE_PROPER_SIZE_UNITS       "UseProperSizeUnits"
#define SETTING_USE_UDF_VERSION             "UseUdfVersion"
#define SETTING_PERSISTENT_LOG              "PersistentLog"
#define SETTING_PREFERRED_SAVE_IMAGE_TYPE   "PreferredSaveImageType"
#define SETTING_PRESERVE_TIMESTAMPS         "PreserveTimestamps"
#define SETTING_VERBOSE_UPDATES             "VerboseUpdateCheck"
#define SETTING_WUE_OPTIONS                 "WindowsUserExperienceOptions"

extern BOOL old_bios_fixes;         /* IDC_OLD_BIOS_FIXES */
extern BOOL append_silent;          /* IDC_APPEND_SILENT */
extern BOOL use_rufus_mbr;          /* SETTING_DISABLE_RUFUS_MBR (inverted) */
extern BOOL badblocks_check;        /* --bad-blocks */
/* ISO module (iso.c) globals, same names/semantics as upstream */
extern FILE* fd_md5sum;
extern uint64_t md5sum_totalbytes;
extern uint64_t total_blocks, extra_blocks, nb_blocks, last_nb_blocks;
extern BOOL preserve_timestamps, validate_md5sum;
extern BOOL enable_iso, enable_joliet, enable_rockridge, has_ldlinux_c32;
extern StrArray modified_files;
extern const char* md5sum_name[2];
extern const char* efi_dirname;
extern const char* efi_bootname[3];
extern const char* efi_archname[ARCH_MAX];
extern const char* bootmgr_efi_name;

#define safe_closehandle(h) do { if ((h) != NULL && (h) != INVALID_HANDLE_VALUE) { CloseHandle(h); (h) = INVALID_HANDLE_VALUE; } } while (0)
#define safe_free(p) do { free((void*)(p)); (p) = NULL; } while (0)
#define safe_mm_free(p) do { _mm_free((void*)(p)); (p) = NULL; } while (0)
#define safe_strlen(str) ((((char*)(str)) == NULL) ? 0 : strlen(str))
#define safe_strdup(str) ((((char*)(str)) == NULL) ? NULL : strdup(str))
static __inline void safe_strcp(char* dst, const size_t dst_max, const char* src, const size_t count) {
    memmove(dst, src, MIN(count, dst_max));
    if (dst != NULL) dst[MIN(count, dst_max) - 1] = 0;
}
#define safe_strcpy(dst, dst_max, src) safe_strcp(dst, dst_max, src, safe_strlen(src) + 1)
#define static_strcpy(dst, src) safe_strcpy(dst, sizeof(dst), src)
static __inline void safe_strcat_(char* dst, const size_t dst_max, const char* src) {
    size_t len = safe_strlen(dst);
    if (len < dst_max)
        safe_strcp(dst + len, dst_max - len, src, safe_strlen(src) + 1);
}
#define safe_strcat(dst, dst_max, src) safe_strcat_(dst, dst_max, src)
#define static_strcat(dst, src) safe_strcat(dst, sizeof(dst), src)
#define safe_strcmp(str1, str2) strcmp(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2))
#define safe_strstr(str1, str2) strstr(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2))
#define safe_stricmp(str1, str2) strcasecmp(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2))
#define safe_strncmp(str1, str2, count) strncmp(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2), count)
#define safe_strnicmp(str1, str2, count) strncasecmp(((str1 == NULL) ? "<NULL>" : str1), ((str2 == NULL) ? "<NULL>" : str2), count)
#define safe_sprintf(dst, count, ...) do { size_t _count = count; char* _dst = dst; snprintf(_dst, _count, __VA_ARGS__); \
    if (_dst != NULL) _dst[(_count) - 1] = 0; } while (0)
#define static_sprintf(dst, ...) safe_sprintf(dst, sizeof(dst), __VA_ARGS__)
#define safe_atoi(str) ((((char*)(str)) == NULL) ? 0 : atoi(str))

/* UTF-8 <-> wchar_t (UTF-32 on Linux) conversions; upstream's msapi_utf8.h
 * equivalents are Windows API based, these are native */
size_t   utf8_to_wchar_no_alloc(const char* src, wchar_t* dst, size_t dst_count);
wchar_t* utf8_to_wchar(const char* str);
char*    wchar_to_utf8(const wchar_t* wstr);
#define  safe_strtoupper(str) do { char* _s = (str); if (_s != NULL) for (; *_s != 0; _s++) *_s = (char)toupper((unsigned char)*_s); } while(0)
size_t   wchar_to_utf8_no_alloc(const wchar_t* src, char* dst, size_t dst_size);

int64_t  GetTime(void);
void     FormatTime(char* buf, size_t len, time_t t);
BOOL     IsElevated(void);
void     PrintUsageAndExit(int exit_code);
const char* GetLastErrorString(void);

/* Test/dev facilities */
extern BOOL enable_debug;
extern BOOL enable_logging;
extern BOOL test_mode;
extern BOOL cli_batch_mode;             /* --batch: never prompt */
extern BOOL cli_yes_to_all;             /* --yes: answer YES to all prompts */
extern char test_device_path[MAX_PATH];
extern char log_path[MAX_PATH];
void init_logging(const char* path);
void exit_logging(void);

/* Token parsing helpers (utils.c) */
extern char* get_token_data_buffer(const char* token, unsigned int n, const char* buffer, size_t buffer_size);
extern uint16_t embedded_sl_version[2];
extern char embedded_sl_version_str[2][12];
extern int64_t iso_blocking_status;
extern BOOL op_in_progress;
extern HANDLE format_thread;

/* Image analysis (vhd.c) */
#define WIM_MAGIC   0x0000004D4957534DULL   /* "MSWIM\0\0\0" */
extern int8_t IsBootableImage(const char* path);
extern BOOL ignore_boot_marker;

/* Syslinux installation (syslinux.c) */
extern unsigned char* syslinux_ldlinux[2];
extern unsigned long syslinux_ldlinux_len[2];
extern unsigned char* syslinux_mboot;
extern unsigned long syslinux_mboot_len;
extern uint32_t SECTOR_SHIFT, SECTOR_SIZE;
extern uint32_t LIBFAT_SECTOR_SHIFT, LIBFAT_SECTOR_SIZE, LIBFAT_SECTOR_MASK;

/* Networking (net.c) */
#define RUFUS_VERSION_STR       RUFUS_STRINGIFY(RUFUS_VERSION_MAJOR) "." RUFUS_STRINGIFY(RUFUS_VERSION_MINOR)
#define RUFUS_STRINGIFY_(x)     #x
#define RUFUS_STRINGIFY(x)      RUFUS_STRINGIFY_(x)
#define RSA_SIGNATURE_SIZE      256
#define RUFUS_URL               "https://rufus.ie"
#define DOWNLOAD_URL            RUFUS_URL "/downloads"
#define FILES_URL               RUFUS_URL "/files"
#define SYMBOL_SERVER_USER_AGENT "Microsoft-Symbol-Server/10.0.22621.755"
#define DISKCOPY_URL            "https://msdl.microsoft.com/download/symbols/diskcopy.dll/54505118173000/diskcopy.dll"
#define DISKCOPY_HASH           "95fc0786f5bc0a6db5c0604b31ac18fbed0502a2c6858e5fb02a647983ae03c7"
#define DISKCOPY_SIZE           0x16ee00
#define DISKCOPY_IMAGE_OFFSET   0x66d8
#define DISKCOPY_IMAGE_SIZE     0x168000
extern DWORD DownloadStatus;
uint64_t DownloadToFileOrBufferEx(const char* url, const char* file, const char* user_agent,
    uint8_t** buffer, HWND hProgressDialog, BOOL bTaskBarProgress);
#define DownloadToFileOrBuffer(url, file, buffer, hProgressDialog, bTaskBarProgress) \
    DownloadToFileOrBufferEx(url, file, NULL, buffer, hProgressDialog, bTaskBarProgress)
DWORD    DownloadSignedFile(const char* url, const char* file, HWND hProgressDialog, BOOL bPromptOnError);
BOOL     ValidateOpensslSignature(BYTE* pbBuffer, DWORD dwBufferLen, BYTE* pbSignature, DWORD dwSigLen);
BOOL     IsDownloadable(const char* url);

/* Per-user data directory (upstream: %LOCALAPPDATA%\Rufus), holding the
 * FILES_DIR folder of downloaded resources: $XDG_DATA_HOME/rufus */
extern char app_data_dir[MAX_PATH];
void     init_app_data_dir(void);

/* Pre-format checks (bootcheck.c, port of upstream BootCheckThread) */
enum {
    BOOTCHECK_PROCEED = 0,
    BOOTCHECK_CANCEL = -1,
    BOOTCHECK_DOWNLOAD_ERROR = -2,
    BOOTCHECK_GENERAL_ERROR = -3,
};
int      BootCheck(void);
extern uint8_t* grub2_buf;
extern long grub2_len;
extern char* embedded_sl_version_ext[2];
extern BOOL size_check;             /* SETTING_DISABLE_SIZE_CHECK inverted */
extern BOOL image_mode_selected;    /* ISO/DD/ESP mode was chosen by the user up front */
extern BOOL windows_to_go;          /* Windows To Go image option selected */
extern BOOL use_own_c32[NB_OLD_C32];
extern const char* old_c32_name[NB_OLD_C32];
/* Front-end selection dialog (upstream stdlg.c): radio buttons (returns
 * 1 << selected index) or check boxes (returns the mask of the checked
 * choices), -1 on cancel */
#ifndef BS_AUTOCHECKBOX
#define BS_AUTOCHECKBOX                 0x00000003L
#endif
#ifndef BS_AUTORADIOBUTTON
#define BS_AUTORADIOBUTTON              0x00000009L
#endif
#define SELECTION_NEEDS_ALL_TO_PROCEED  1
#define SELECTION_USE_WARNING_ICON      2
typedef struct {
    int style;
    int mask;
    int flags;
    int username_index;
    int edition_index;
    int regional_index;
    int privacy_index;
    StrArray choices;
    StrArray tooltips;
} selection_dialog_options_t;
int      SelectionDialog(char* title, char* message, selection_dialog_options_t* options);
extern BOOL cli_selection_preset;
extern int (*selection_hook)(char* title, char* message, selection_dialog_options_t* options);

/* Windows image handling (wue.c) */
#define MAX_USERNAME_LENGTH         128
#define USERNAME_INVALID_CHARS      "/\\[]:;|=.,+*?<>%@&\""
#define PATCH_PROGRESS_TOTAL        207
#define UNATTEND_SECUREBOOT_TPM_MINRAM      0x00001
#define UNATTEND_NO_ONLINE_ACCOUNT          0x00004
#define UNATTEND_NO_DATA_COLLECTION         0x00008
#define UNATTEND_OFFLINE_INTERNAL_DRIVES    0x00010
#define UNATTEND_DUPLICATE_LOCALE           0x00020
#define UNATTEND_SET_USER                   0x00040
#define UNATTEND_DISABLE_BITLOCKER          0x00080
#define UNATTEND_FORCE_S_MODE               0x00100
#define UNATTEND_USE_MS2023_BOOTLOADERS     0x00200
#define UNATTEND_APPLY_SKUSIPOLICY          0x00400
#define UNATTEND_SILENT_INSTALL             0x00800
#define UNATTEND_QOL_ENHANCEMENTS           0x01000
#define UNATTEND_FULL_MASK                  0x01FFF
#define UNATTEND_DEFAULT_MASK               0x016FF
#define UNATTEND_WINDOWS_TO_GO              0x10000
#define UNATTEND_WINPE_SETUP_MASK           (UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_SILENT_INSTALL)
#define UNATTEND_SPECIALIZE_DEPLOYMENT_MASK (UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_QOL_ENHANCEMENTS)
#define UNATTEND_OOBE_SHELL_SETUP_MASK      (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE | UNATTEND_SILENT_INSTALL)
#define UNATTEND_OOBE_INTERNATIONAL_MASK    (UNATTEND_DUPLICATE_LOCALE)
#define UNATTEND_OOBE_MASK                  (UNATTEND_OOBE_SHELL_SETUP_MASK | UNATTEND_OOBE_INTERNATIONAL_MASK | UNATTEND_DISABLE_BITLOCKER | \
                                             UNATTEND_USE_MS2023_BOOTLOADERS | UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_QOL_ENHANCEMENTS)
#define UNATTEND_OFFLINE_SERVICING_MASK     (UNATTEND_OFFLINE_INTERNAL_DRIVES | UNATTEND_FORCE_S_MODE)
#define UNATTEND_DEFAULT_SELECTION_MASK     (UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_OFFLINE_INTERNAL_DRIVES)
extern int unattend_xml_flags, unattend_xml_mask, wintogo_index, wininst_index, unattend_edition_index;
extern char *unattend_xml_path, unattend_username[MAX_USERNAME_LENGTH];
extern char temp_dir[MAX_PATH];
#define WPPRECORDER_MORE_INFO_URL   "https://github.com/pbatard/rufus/wiki/FAQ#bsods-with-windows-to-go-drives-created-from-windows-10-1809-isos"
char*    CreateUnattendXml(int arch, int flags);
char*    GetUILanguage(void);
BOOL     SetupWinPE(const char* drive_name);
BOOL     PopulateWindowsVersion(void);
int      GetEditions(StrArray* version_name, StrArray* version_index);
int      SetWinToGoIndex(void);
BOOL     SetupWinToGo(DWORD DriveIndex, const char* drive_name, BOOL use_esp);
BOOL     ApplyWindowsCustomization(const char* drive_name, int flags);
unsigned int GetKeyboardLayoutId(void);
/* vhd.c (WIM functions) */
uint32_t GetWimVersion(const char* image);
BOOL     WimSplitFile(const char* src, const char* dst);
BOOL     WimApplyImage(const char* image, int index, const char* dst);
/* parser.c */
char*    insert_section_data(const char* filename, const char* section, const char* data, BOOL dos2unix);
char*    replace_in_token_data(const char* filename, const char* token, const char* src, const char* rep, BOOL dos2unix);
char*    replace_char(const char* src, const char c, const char* rep);
void     filter_chars(char* str, const char* rem, const char rep);
char*    remove_substr(const char* src, const char* sub);
#define LIST_DIR_TYPE_FILE          0x01
#define LIST_DIR_TYPE_DIRECTORY     0x02
#define LIST_DIR_TYPE_RECURSIVE     0x80
DWORD    ListDirectoryContent(StrArray* arr, char* dir, uint8_t type);

#endif /* RUFUS_LINUX_RFUS_H */
