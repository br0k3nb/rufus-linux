/*
 * Rufus Linux port: application-wide globals.
 *
 * Values and names match upstream rufus.c / drive.c so that ported code
 * reads the same way.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

RUFUS_DRIVE_INFO SelectedDrive;
int partition_index[PI_MAX];
RUFUS_IMG_REPORT img_report;

const int nb_steps[FS_MAX] = { 5, 5, 12, 1, 10, 1, 1, 1, 1 };
const char* FileSystemLabel[FS_MAX] = { "FAT", "FAT32", "NTFS", "UDF", "exFAT", "ReFS", "ext2", "ext3", "ext4" };
const char* FileSystemName[FS_MAX]  = { "FAT", "FAT32", "NTFS", "UDF", "exFAT", "ReFS", "ext2", "ext3", "ext4" };

DWORD DriveIndex = 0;
HMODULE hMainInstance = NULL;
BOOL  IsDriveRemovable = FALSE;
BOOL  IsDriveUSB = FALSE;
BOOL  zero_drive = FALSE;
BOOL  quick_format = TRUE;
BOOL  write_as_image = FALSE;
BOOL  write_as_esp = FALSE;
BOOL  allow_dual_uefi_bios = FALSE;
BOOL  force_large_fat32 = FALSE;
BOOL  enable_ntfs_compression = FALSE;
BOOL  use_vds = FALSE;              /* no VDS on Linux; kept for ported code paths */
BOOL  use_extend_size = FALSE;
BOOL  save_image_to_file = FALSE;
int   fs_type = FS_FAT32;
int   boot_type = BT_IMAGE;
int   target_type = TT_BIOS;
int   cluster_size = 0;
int   partition_type = 0;
int   image_option = IMOP_WIN_STANDARD;
char  image_path[MAX_PATH] = "";
char  label[64] = "";
char  system_dir[MAX_PATH] = "";
char  app_dir[MAX_PATH] = "";
char  app_data_dir[MAX_PATH] = "";
char  temp_dir[MAX_PATH] = "/tmp";
HWND  hMainDialog = NULL;
int   nb_passes_sel = 0;    /* 0: 1 pass, 1: 2 passes, 2: 4 passes (and SLC/MLC/TLC patterns) */
int   flash_type_sel = 0;
BOOL  detect_fakes = TRUE;  /* IDC_... "check for fake drives" in upstream's bad blocks options */
char  drive_name[128] = "";
DWORD ErrorStatus = 0;
uint64_t persistence_size = 0;
int   ext_version = 3;
int   setup_type = 0;
const char* image_name = "";
BOOL  quit_requested = FALSE;
BOOL  use_fake_units = FALSE;
BOOL  right_to_left_mode = FALSE;
BOOL  lock_drive = TRUE;
BOOL  fast_zeroing = FALSE;
BOOL  old_bios_fixes = FALSE;
BOOL  append_silent = FALSE;
BOOL  use_rufus_mbr = TRUE;
BOOL  size_check = TRUE;
BOOL  usb_debug = FALSE;
BOOL  expert_mode = FALSE;
int   default_thread_priority = 0;
BOOL  image_mode_selected = FALSE;
BOOL  windows_to_go = FALSE;

volatile BOOL badblocks_running = FALSE;
BOOL  badblocks_check = FALSE;

/* ISO module (iso.c) globals */
FILE*  fd_md5sum = NULL;
uint64_t md5sum_totalbytes = 0;
uint64_t total_blocks = 0, extra_blocks = 0, nb_blocks = 0, last_nb_blocks = 0;
BOOL   preserve_timestamps = FALSE;
BOOL   validate_md5sum = FALSE;
BOOL   enable_iso = TRUE, enable_joliet = TRUE, enable_rockridge = TRUE;
BOOL   has_ldlinux_c32 = FALSE;
StrArray modified_files;

/* Syslinux versions embedded in resources (upstream detects these from
 * res/syslinux/ldlinux_v{4,6}.sys at start-up; the values match those
 * blobs: Syslinux 4.07 and 6.04). TODO: parse the blobs like upstream. */
uint16_t embedded_sl_version[2] = { 0x0407, 0x0604 };
char embedded_sl_version_str[2][12] = { "4.07", "6.04" };
int64_t iso_blocking_status = -1;
BOOL op_in_progress = FALSE;        /* a format/extract operation is running */
HANDLE format_thread = NULL;        /* handle of the format thread (CLI: unused) */
BOOL ignore_boot_marker = FALSE;    /* SETTING_IGNORE_BOOT_MARKER */
char  test_device_path[MAX_PATH] = "";   /* virtual drive passed with --device */
