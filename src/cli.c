/*
 * Rufus Linux port: command line front-end.
 *
 * Replaces the Win32 entry point of upstream rufus.c for the Linux port.
 * Option names follow the upstream CLI (-x/--extra-devs, -i/--iso,
 * -l/--locale, -f/--filesystem, -w/--wait, -h/--help, -g/--gui) and add the
 * switches the Linux port needs to run headless (-l is taken by locale, so
 * device listing uses --list, and formatting uses --device plus explicit
 * options).
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"
#include "localization.h"

#include <getopt.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APPLICATION_NAME    "Rufus"



/* Command line state */
static BOOL opt_list = FALSE;
static BOOL opt_format = FALSE;
static BOOL opt_gui = FALSE;
static BOOL opt_extra_devs = FALSE;
static BOOL opt_user_fs = FALSE;    /* -f was given on the command line */
static BOOL opt_user_label = FALSE; /* --label was given on the command line */
static BOOL force_dd = FALSE;       /* --dd was given: disable ISO mode */
static BOOL force_iso = FALSE;      /* --iso-mode was given: don't ask about DD mode */
static BOOL opt_hash = FALSE;       /* --hash: compute the checksums of the image */
static BOOL opt_offline = FALSE;    /* --offline: don't access the network */
static const char* opt_save = NULL; /* --save: save the device to an image */
static int  opt_user_pt = -1;       /* --partition-scheme given on the command line */
static int  opt_user_tt = -1;       /* --target given on the command line */
static BOOL opt_user_boot = FALSE; /* --boot was given on the command line */
static const char* opt_device = NULL;
static int  opt_wait = 0;

/* Boot selection names, in upstream BT_* order (the boot selection dropdown) */
static const struct {
    const char* name;
    int type;
} boot_names[] = {
    { "none",       BT_NON_BOOTABLE },
    { "msdos",      BT_MSDOS },
    { "freedos",    BT_FREEDOS },
    { "image",      BT_IMAGE },
    { "syslinux4",  BT_SYSLINUX_V4 },
    { "syslinux6",  BT_SYSLINUX_V6 },
    { "reactos",    BT_REACTOS },
    { "grub4dos",   BT_GRUB4DOS },
    { "grub2",      BT_GRUB2 },
    { "uefi-ntfs",  BT_UEFI_NTFS },
};

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */
void PrintUsageAndExit(int exit_code)
{
    printf("\nUsage: rufus [OPTIONS] [--device DEVICE]\n");
    printf("\nGeneral options:\n");
    printf("  -h, --help                 This usage guide and exit\n");
    printf("  --version                  Print the version and exit\n");
    printf("  --list                     List available devices and exit\n");
    printf("  -x, --extra-devs           Also list non-USB devices (HDDs, SD cards...)\n");
    printf("  -g, --gui                  Start the GUI (default when available)\n");
    printf("  -l LOCALE, --locale=LOCALE Use the given locale (e.g. pt-BR, en-US)\n");
    printf("  --hash                     Compute the MD5/SHA-1/SHA-256 checksums of the image (-i)\n");
    printf("  --sha512                   Also compute SHA-512 with --hash\n");
    printf("  --check-updates            Check for a new Rufus release and UEFI DBX updates\n");
    printf("  --debug                    Enable debug output\n");
    printf("  --log FILE                 Append log output to FILE\n");
    printf("\nFormatting options:\n");
    printf("  --device PATH              Select the device or image file to use\n");
    printf("                             (a regular file is treated as a virtual drive)\n");
    printf("  --format                   Format the selected device\n");
    printf("  -i PATH, --iso=PATH        ISO image to use (without --device: analyze it)\n");
    printf("  -b TYPE, --boot=TYPE       Boot selection: none, msdos, freedos, syslinux4,\n");
    printf("                             syslinux6, reactos, grub4dos, grub2, uefi-ntfs\n");
    printf("                             (default: image when -i is given, else none)\n");
    printf("  --dd                       Write the image as-is (DD mode), even for ISOs\n");
    printf("  --iso-mode                 Write ISOHybrid images in ISO mode without asking\n");
    printf("  --esp                      Write the ISO as an EFI System Partition (GPT + FAT)\n");
    printf("  --force                    Ignore the image boot marker (use with any image)\n");
    printf("  -f FS, --filesystem=FS     File system: FAT, FAT32, NTFS, UDF, exFAT, ext2, ext3, ext4\n");
    printf("  --label=LABEL              Volume label\n");
    printf("  --partition-scheme=SCHEME  mbr or gpt\n");
    printf("  --target=TARGET            bios, uefi or both\n");
    printf("  --cluster-size=SIZE        Cluster size in bytes (0 = default)\n");
    printf("  --quick                    Quick format (default)\n");
    printf("  --full                     Zero the drive and perform a full format\n");
    printf("  --bad-blocks[=PASSES]      Check the device for bad blocks (slow!): 1, 2 or\n");
    printf("                             4 passes, with slc, mlc or tlc patterns (e.g. 4,tlc)\n");
    printf("  --zero                     Zero the whole drive before formatting\n");
    printf("  --persistence=SIZE         Persistent partition size for Linux live ISOs (e.g. 4G)\n");
    printf("  --wue=OPTIONS              Windows customization (comma separated): tpm, account,\n");
    printf("                             user[=NAME], locale, privacy, silent, bitlocker, qol,\n");
    printf("                             ms2023, skusipolicy, offline, none (default: tpm,account)\n");
    printf("  --zip=ARCHIVE              Copy the files from a zip archive to the drive\n");
    printf("  --save=FILE                Save the device to an image (.img, .vhd, .vhdx or .iso)\n");
    printf("  --no-extended-label        Don't create autorun.inf/icon (extended label and icon)\n");
    printf("  --old-bios-fixes           Add fixes for old BIOSes (extra partition, alignment)\n");
    printf("  --uefi-validation          Enable runtime UEFI media validation (UEFI-MD5Sum)\n");
    printf("  --offline                  Don't download UEFI revocation data or extra files\n");
    printf("  --yes                      Answer YES to all questions\n");
    printf("  --batch                    Never prompt (non-interactive)\n");
    printf("\nThe device must be given explicitly: Rufus on Linux never picks a\n");
    printf("device on its own, to prevent accidental data loss.\n");
    exit(exit_code);
}

static void PrintVersion(void)
{
    printf("Rufus %d.%d.%d.%d (Linux port)\n",
           RUFUS_VERSION_MAJOR, RUFUS_VERSION_MINOR, RUFUS_VERSION_BUILD, 0);
    printf("Port of https://github.com/pbatard/rufus\n");
}

/* ------------------------------------------------------------------ */
/* Device list                                                         */
/* ------------------------------------------------------------------ */
static void ListDevices(BOOL extra_devs)
{
    int i, count;

    GetDevices(DRIVE_INDEX_MIN);
    count = GetDriveCount();
    if (count == 0) {
        printf("No devices found.\n");
        if (!extra_devs)
            printf("(Rufus on Linux only lists removable/USB drives by default; "
                   "use -x to list all devices)\n");
        return;
    }

    printf("%-4s %-9s %-8s %-12s %s\n", "Dev", "Type", "Size", "Status", "Name");
    for (i = 0; i < count; i++) {
        uint64_t size = 0;
        BOOL usb = FALSE, removable = FALSE, filtered;
        const char* name = "";
        char size_str[32];

        uint16_t vid = 0, pid = 0;
        if (!GetDriveEntryInfo(i, &size, &usb, &removable, &vid, &pid, &name))
            continue;
        if (!extra_devs && !usb && !removable)
            continue;
        /* As upstream, USB hard drives/SSDs are only listed on request */
        if (!extra_devs && usb && (IsHDD(DRIVE_INDEX_MIN + i, vid, pid, name) > 0)) {
            uprintf("Device '%s' eliminated because it was detected as a Hard Drive or SSD (use -x to list it)", name);
            continue;
        }
        filtered = IsFilteredDrive(DRIVE_INDEX_MIN + i);
        snprintf(size_str, sizeof(size_str), "%s", SizeToHumanReadable(size, FALSE, FALSE));
        printf("%-4u %-9s %-8s %-12s %s\n",
               (unsigned)(DRIVE_INDEX_MIN + i),
               usb ? "USB" : (removable ? "Removable" : "Fixed"),
               size_str,
               filtered ? "System disk!" : "Available",
               name);
    }
    printf("\nDevice numbers are the ones to pass to --device=...\n");
    printf("(or use the device path directly, e.g. --device=/dev/sdb)\n");
}

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */
static int parse_fs(const char* arg)
{
    int i;
    if (arg == NULL)
        return FS_UNKNOWN;
    if (arg[0] >= '0' && arg[0] <= '9') {
        int v = (int)strtol(arg, NULL, 0);
        return (v >= 0 && v < FS_MAX) ? v : FS_UNKNOWN;
    }
    for (i = 0; i < FS_MAX; i++)
        if (strcasecmp(arg, FileSystemLabel[i]) == 0)
            return i;
    if (strcasecmp(arg, "FAT16") == 0)
        return FS_FAT16;
    return FS_UNKNOWN;
}

static int parse_boot(const char* arg)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(boot_names); i++)
        if (strcasecmp(arg, boot_names[i].name) == 0)
            return boot_names[i].type;
    if (strcasecmp(arg, "syslinux") == 0)
        return BT_SYSLINUX_V6;
    if (strcasecmp(arg, "grub") == 0)
        return BT_GRUB2;
    return -1;
}

/* Windows User Experience options (upstream's WUE dialog choices) */
static BOOL parse_wue(const char* arg)
{
    static const struct { const char* name; int flag; } wue_names[] = {
        { "tpm", UNATTEND_SECUREBOOT_TPM_MINRAM }, { "account", UNATTEND_NO_ONLINE_ACCOUNT },
        { "user", UNATTEND_SET_USER }, { "locale", UNATTEND_DUPLICATE_LOCALE },
        { "privacy", UNATTEND_NO_DATA_COLLECTION }, { "silent", UNATTEND_SILENT_INSTALL },
        { "bitlocker", UNATTEND_DISABLE_BITLOCKER }, { "qol", UNATTEND_QOL_ENHANCEMENTS },
        { "ms2023", UNATTEND_USE_MS2023_BOOTLOADERS }, { "skusipolicy", UNATTEND_APPLY_SKUSIPOLICY },
        { "offline", UNATTEND_OFFLINE_INTERNAL_DRIVES }, { "smode", UNATTEND_FORCE_S_MODE },
    };
    char buf[512], *tok, *val;
    size_t i;
    int mask = 0;

    snprintf(buf, sizeof(buf), "%s", arg);
    for (tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
        if (strcasecmp(tok, "none") == 0)
            continue;
        val = strchr(tok, '=');
        if (val != NULL)
            *val++ = 0;
        for (i = 0; i < ARRAYSIZE(wue_names); i++) {
            if (strcasecmp(tok, wue_names[i].name) == 0) {
                mask |= wue_names[i].flag;
                if ((wue_names[i].flag == UNATTEND_SET_USER) && (val != NULL))
                    snprintf(unattend_username, MAX_USERNAME_LENGTH, "%s", val);
                if (wue_names[i].flag == UNATTEND_FORCE_S_MODE)
                    expert_mode = TRUE;
                break;
            }
        }
        if (i >= ARRAYSIZE(wue_names)) {
            fprintf(stderr, "rufus: unknown Windows customization option '%s'\n", tok);
            return FALSE;
        }
    }
    /* The selection dialog uses this as its preset (and returns it in batch mode) */
    unattend_xml_mask = mask;
    cli_selection_preset = TRUE;
    return TRUE;
}

static void SelectDriveOrDie(const char* dev)
{
    /* Accept a numeric device number (as printed by --list) or a path */
    if (dev[0] >= '0' && dev[0] <= '9') {
        unsigned long num = strtoul(dev, NULL, 0);
        GetDevices(DRIVE_INDEX_MIN);
        if (!SelectDriveByIndex((DWORD)num)) {
            fprintf(stderr, "rufus: no device with number %lu\n", num);
            exit(1);
        }
        return;
    }
    if (!SelectDriveByPath(dev)) {
        fprintf(stderr, "rufus: could not open '%s'\n", dev);
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */
static BOOL confirm_destructive(const char* path)
{
    char buf[32];

    /* Worker: the GUI shows upstream's confirmation */
    if (worker_mode) {
        if (Notification(MB_OKCANCEL | MB_ICONWARNING, APPLICATION_NAME, lmprintf(MSG_003, path)) != IDOK)
            return FALSE;
        if ((SelectedDrive.nPartitions > 1) &&
            (Notification(MB_OKCANCEL | MB_ICONWARNING, lmprintf(MSG_094), lmprintf(MSG_093)) != IDOK))
            return FALSE;
        return TRUE;
    }
    if (cli_yes_to_all || cli_batch_mode)
        return TRUE;
    printf("\nWARNING: ALL DATA ON '%s' WILL BE DESTROYED.\n"
           "Type 'yes' to continue: ", path);
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin) == NULL)
        return FALSE;
    return ((strncasecmp(buf, "yes", 3) == 0) || (strncasecmp(buf, "y", 1) == 0));
}

/* Analyse the image file passed with -i: CLI equivalent of upstream's
 * ImageScanThread() + the dialog updates that follow it. Fills img_report
 * and decides between ISO (extraction) and DD (bit-for-bit) write modes. */
static void AnalyzeImage(void)
{
    struct stat st;

    if ((stat(image_path, &st) != 0) || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "rufus: could not access image '%s'\n", image_path);
        exit(1);
    }
    /* Upstream retrieves the latest UEFI revocation data on startup */
    if (!opt_offline)
        FetchSecureBootData();
    if (!ScanImage()) {
        fprintf(stderr, "rufus: '%s' is not a supported image\n", image_path);
        exit(1);
    }

    write_as_image = (force_dd || !img_report.is_iso) ? TRUE : FALSE;
    if (img_report.is_windows_img)
        write_as_image = FALSE;
    /* --dd, --iso-mode and --esp answer BootCheck()'s ISOHybrid question */
    image_mode_selected = (force_dd || force_iso || write_as_esp);
    if (write_as_image)
        write_as_esp = FALSE;
    printf("Using image: %s (%s)\n", image_path, SizeToHumanReadable(img_report.image_size, FALSE, FALSE));
    if (img_report.is_iso || img_report.is_windows_img)
        printf("  %s mode: the image will be extracted to the drive%s\n", img_report.is_iso ? "ISO" : "WIM",
               IS_DD_BOOTABLE(img_report) && !image_mode_selected ? " (or written in DD mode, if selected)" : "");
    else
        printf("  DD mode: the image will be written as-is\n");
}

static void RunFormat(DWORD drive_index)
{
    BOOL allowed_pt[3], allowed_tt[3];
    /* Indexed like upstream's partition scheme combo: MBR, GPT, SFD */
#define PT_IDX(pt)  (((pt) == PARTITION_STYLE_MBR) ? 0 : (((pt) == PARTITION_STYLE_GPT) ? 1 : 2))
    static const char* pt_name[3] = { "MBR", "GPT", "SFD" };

    if (image_path[0] != 0) {
        boot_type = BT_IMAGE;
        AnalyzeImage();
    } else if (!opt_user_boot) {
        boot_type = BT_NON_BOOTABLE;
    } else if (boot_type == BT_IMAGE) {
        fprintf(stderr, "rufus: --boot=image requires an image (-i)\n");
        exit(1);
    }

    /* Partition scheme and target system, as upstream's dialog computes them */
    GetPartitionSchemeAndTargetSystem(opt_user_pt, allowed_pt, allowed_tt);
    if (opt_user_pt >= 0) {
        if (!allowed_pt[PT_IDX(opt_user_pt)]) {
            fprintf(stderr, "rufus: the %s partition scheme can not be used with this boot selection\n", pt_name[PT_IDX(opt_user_pt)]);
            exit(1);
        }
        partition_type = opt_user_pt;
    }
    if (opt_user_tt >= 0) {
        if ((opt_user_tt == TT_UEFI) ? !allowed_tt[1] : !(allowed_tt[0] || allowed_tt[2])) {
            fprintf(stderr, "rufus: the %s target system can not be used with this selection\n",
                    (opt_user_tt == TT_UEFI) ? "UEFI" : "BIOS");
            exit(1);
        }
        target_type = opt_user_tt;
    }

    /* File system: same constraints as upstream's boot selection/file system combos */
    if ((boot_type == BT_MSDOS) || (boot_type == BT_FREEDOS) || (boot_type == BT_SYSLINUX_V4) || (boot_type == BT_SYSLINUX_V6) ||
        (boot_type == BT_GRUB4DOS) || (boot_type == BT_REACTOS)) {
        if (!opt_user_fs)
            fs_type = FS_FAT32;
        if ((fs_type != FS_FAT16) && (fs_type != FS_FAT32) && (fs_type != FS_NTFS) &&
            !((fs_type == FS_EXFAT) && (boot_type == BT_GRUB4DOS))) {
            fprintf(stderr, "rufus: %s can not be used with this boot selection\n", FileSystemLabel[fs_type]);
            exit(1);
        }
        if ((fs_type == FS_NTFS) && (boot_type != BT_GRUB4DOS) && (boot_type != BT_SYSLINUX_V6)) {
            fprintf(stderr, "rufus: NTFS can not be used with this boot selection\n");
            exit(1);
        }
    } else if (boot_type == BT_UEFI_NTFS) {
        if (!opt_user_fs)
            fs_type = FS_NTFS;
        if ((fs_type != FS_NTFS) && (fs_type != FS_EXFAT)) {
            fprintf(stderr, "rufus: UEFI:NTFS requires NTFS or exFAT\n");
            exit(1);
        }
    } else if ((boot_type == BT_IMAGE) && !write_as_image && !opt_user_fs) {
        int preferred_fs = GetPreferredFSForImage((1 << FS_FAT16) | (1 << FS_FAT32) | (1 << FS_NTFS) |
                                                  (1 << FS_UDF) | (1 << FS_EXFAT));
        if (preferred_fs != FS_UNKNOWN)
            fs_type = preferred_fs;
    }
    if ((boot_type == BT_IMAGE) && !write_as_image && img_report.needs_ntfs && (fs_type != FS_NTFS)) {
        fprintf(stderr, "rufus: this image requires NTFS\n");
        exit(1);
    }

    /* Propose the ISO label, like upstream's SetProposedLabel() */
    if (!opt_user_label && (boot_type == BT_IMAGE) && (img_report.label[0] != 0))
        snprintf(label, sizeof(label), "%s", img_report.label);

    if (!write_as_image || boot_type != BT_IMAGE)
        printf("Partition scheme: %s, target system: %s, file system: %s, label: '%s'\n",
               pt_name[PT_IDX(partition_type)],
               (target_type == TT_UEFI) ? "UEFI (non CSM)" : (allowed_tt[2] ? "BIOS or UEFI" : "BIOS (or UEFI-CSM)"),
               FileSystemLabel[fs_type], label);

    /* Upstream's BootCheck(): validate the selection against the image and
     * fetch any extra boot files (ldlinux.c32, grldr, core.img...) */
    switch (BootCheck()) {
    case BOOTCHECK_PROCEED:
        break;
    case BOOTCHECK_DOWNLOAD_ERROR:
        fprintf(stderr, "rufus: could not download the required files\n");
        exit(1);
    default:
        fprintf(stderr, "rufus: operation cancelled\n");
        exit(1);
    }
    if (write_as_esp && ((partition_type != PARTITION_STYLE_GPT) || !IS_FAT(fs_type))) {
        fprintf(stderr, "rufus: --esp requires --partition-scheme=gpt and a FAT file system\n");
        exit(1);
    }

    /* Detect processes that have write (0x2) or exec (0x4) rights against
     * our drive, as upstream does before starting */
    PrintStatus(0, MSG_278);
    if (GetProcessSearch(SEARCH_PROCESS_TIMEOUT, 0x06, TRUE)) {
        if (Notification(MB_ICONWARNING | MB_YESNO, GetPhysicalName(drive_index), lmprintf(MSG_132)) != IDYES)
            exit(1);
    }

    if (!confirm_destructive(GetPhysicalName(drive_index)))
        exit(1);

    /* Keep the mount manager out of our way, as upstream does */
    SetAutoMount(FALSE);

    if (write_as_image)
        printf("Writing image '%s' to '%s'...\n", image_path, GetPhysicalName(drive_index));
    else if (zero_drive)
        printf("Zeroing '%s'...\n", GetPhysicalName(drive_index));
    else if (boot_type == BT_IMAGE)
        printf("Formatting '%s' as %s (%s) and extracting '%s'...\n", GetPhysicalName(drive_index),
               FileSystemLabel[fs_type], (partition_type == PARTITION_STYLE_GPT) ? "GPT" : "MBR", image_path);
    else
        printf("Formatting '%s' as %s (%s)...\n", GetPhysicalName(drive_index),
               FileSystemLabel[fs_type], (partition_type == PARTITION_STYLE_GPT) ? "GPT" : "MBR");
    fflush(stdout);

    FormatThread((void*)(uintptr_t)drive_index);

    if (IS_ERROR(ErrorStatus)) {
        fprintf(stderr, "rufus: operation failed: %s\n", StrError(ErrorStatus, FALSE));
        exit(1);
    }
    printf("\nOperation completed successfully.\n");
}

int main(int argc, char** argv)
{
    int opt;
    char app_path[MAX_PATH];
    ssize_t link_len;

    setlocale(LC_ALL, "");

    /* CLI defaults: MBR scheme, BIOS target, FAT32, no automatic device */
    partition_type = PARTITION_STYLE_MBR;

    /* app_dir: directory of the executable */
    link_len = readlink("/proc/self/exe", app_path, sizeof(app_path) - 1);
    if (link_len > 0) {
        char* slash;
        app_path[link_len] = 0;
        slash = strrchr(app_path, '/');
        if (slash != NULL) {
            *slash = 0;
            snprintf(app_dir, sizeof(app_dir), "%s", app_path);
        }
    }

    init_settings();
    init_app_data_dir();
    cpu_has_sha1_accel = DetectSHA1Acceleration();
    cpu_has_sha256_accel = DetectSHA256Acceleration();
    init_localization();

    enum {
        OPT_LIST = 1000, OPT_DEVICE, OPT_FORMAT, OPT_VERSION, OPT_DEBUG,
        OPT_LOG, OPT_LABEL, OPT_SCHEME, OPT_TARGET, OPT_CLUSTER, OPT_QUICK,
        OPT_FULL, OPT_BADBLOCKS, OPT_YES, OPT_BATCH, OPT_ZERO, OPT_DD, OPT_FORCE,
        OPT_ISO_MODE, OPT_ESP, OPT_HASH, OPT_SHA512, OPT_TEST_HASHES,
        OPT_PERSISTENCE, OPT_WUE, OPT_NO_EXT_LABEL, OPT_OLD_BIOS, OPT_OFFLINE, OPT_ZIP, OPT_SAVE, OPT_UPDATES, OPT_WORKER, OPT_UEFI_VALIDATION
    };
    struct option long_options[] = {
        { "extra-devs",     no_argument,       NULL, 'x' },
        { "gui",            no_argument,       NULL, 'g' },
        { "help",           no_argument,       NULL, 'h' },
        { "iso",            required_argument, NULL, 'i' },
        { "boot",           required_argument, NULL, 'b' },
        { "locale",         required_argument, NULL, 'l' },
        { "filesystem",     required_argument, NULL, 'f' },
        { "wait",           required_argument, NULL, 'w' },
        { "list",           no_argument,       NULL, OPT_LIST },
        { "device",         required_argument, NULL, OPT_DEVICE },
        { "format",         no_argument,       NULL, OPT_FORMAT },
        { "version",        no_argument,       NULL, OPT_VERSION },
        { "debug",          no_argument,       NULL, OPT_DEBUG },
        { "log",            required_argument, NULL, OPT_LOG },
        { "label",          required_argument, NULL, OPT_LABEL },
        { "partition-scheme", required_argument, NULL, OPT_SCHEME },
        { "target",         required_argument, NULL, OPT_TARGET },
        { "cluster-size",   required_argument, NULL, OPT_CLUSTER },
        { "quick",          no_argument,       NULL, OPT_QUICK },
        { "full",           no_argument,       NULL, OPT_FULL },
        { "bad-blocks",     optional_argument, NULL, OPT_BADBLOCKS },
        { "zero",           no_argument,       NULL, OPT_ZERO },
        { "dd",             no_argument,       NULL, OPT_DD },
        { "iso-mode",       no_argument,       NULL, OPT_ISO_MODE },
        { "esp",            no_argument,       NULL, OPT_ESP },
        { "hash",           no_argument,       NULL, OPT_HASH },
        { "persistence",    required_argument, NULL, OPT_PERSISTENCE },
        { "wue",            required_argument, NULL, OPT_WUE },
        { "no-extended-label", no_argument,    NULL, OPT_NO_EXT_LABEL },
        { "old-bios-fixes", no_argument,       NULL, OPT_OLD_BIOS },
        { "offline",        no_argument,       NULL, OPT_OFFLINE },
        { "zip",            required_argument, NULL, OPT_ZIP },
        { "save",           required_argument, NULL, OPT_SAVE },
        { "check-updates",  no_argument,       NULL, OPT_UPDATES },
        { "worker",         no_argument,       NULL, OPT_WORKER },
        { "uefi-validation", no_argument,      NULL, OPT_UEFI_VALIDATION },
        { "sha512",         no_argument,       NULL, OPT_SHA512 },
        { "test-hashes",    no_argument,       NULL, OPT_TEST_HASHES },
        { "force",          no_argument,       NULL, OPT_FORCE },
        { "yes",            no_argument,       NULL, OPT_YES },
        { "batch",          no_argument,       NULL, OPT_BATCH },
        { 0, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "xghi:b:l:f:w:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'x':
            opt_extra_devs = TRUE;
            break;
        case 'g':
            opt_gui = TRUE;
            break;
        case 'h':
            PrintUsageAndExit(0);
            break;
        case 'i':
            snprintf(image_path, sizeof(image_path), "%s", optarg);
            break;
        case 'b': {
            int bt = parse_boot(optarg);
            if (bt < 0) {
                fprintf(stderr, "rufus: unknown boot selection '%s'\n", optarg);
                return 1;
            }
            boot_type = bt;
            opt_user_boot = TRUE;
            break;
        }
        case 'l':
            if (!set_locale(optarg))
                fprintf(stderr, "rufus: unknown locale '%s' (using %s)\n",
                        optarg, get_current_locale_name());
            break;
        case 'f': {
            int fs = parse_fs(optarg);
            if (fs == FS_UNKNOWN)
                fprintf(stderr, "rufus: unknown filesystem '%s'\n", optarg);
            else {
                fs_type = fs;
                opt_user_fs = TRUE;
            }
            break;
        }
        case 'w':
            opt_wait = atoi(optarg);
            break;
        case OPT_LIST:
            opt_list = TRUE;
            break;
        case OPT_DEVICE:
            opt_device = optarg;
            break;
        case OPT_FORMAT:
            opt_format = TRUE;
            break;
        case OPT_VERSION:
            PrintVersion();
            return 0;
        case OPT_DEBUG:
            enable_debug = TRUE;
            break;
        case OPT_LOG:
            snprintf(log_path, sizeof(log_path), "%s", optarg);
            init_logging(log_path);
            break;
        case OPT_LABEL:
            snprintf(label, sizeof(label), "%s", optarg);
            opt_user_label = TRUE;
            break;
        case OPT_SCHEME:
            if (strcasecmp(optarg, "mbr") == 0)
                opt_user_pt = PARTITION_STYLE_MBR;
            else if (strcasecmp(optarg, "gpt") == 0)
                opt_user_pt = PARTITION_STYLE_GPT;
            else if (strcasecmp(optarg, "sfd") == 0)
                opt_user_pt = PARTITION_STYLE_SFD;
            else {
                fprintf(stderr, "rufus: unknown partition scheme '%s'\n", optarg);
                return 1;
            }
            break;
        case OPT_TARGET:
            if (strcasecmp(optarg, "bios") == 0)
                opt_user_tt = TT_BIOS;
            else if (strcasecmp(optarg, "uefi") == 0)
                opt_user_tt = TT_UEFI;
            else if (strcasecmp(optarg, "both") == 0)
                allow_dual_uefi_bios = TRUE;    /* upstream's 'dual UEFI/BIOS' advanced option */
            else {
                fprintf(stderr, "rufus: unknown target '%s'\n", optarg);
                return 1;
            }
            break;
        case OPT_CLUSTER:
            cluster_size = (int)strtol(optarg, NULL, 0);
            break;
        case OPT_QUICK:
            quick_format = TRUE;
            break;
        case OPT_FULL:
            quick_format = FALSE;
            break;
        case OPT_BADBLOCKS:
            badblocks_check = TRUE;
            /* Same choices as upstream's passes dropdown */
            if (optarg != NULL) {
                if (strcmp(optarg, "1") == 0) nb_passes_sel = 0;
                else if (strcmp(optarg, "2") == 0) nb_passes_sel = 1;
                else if ((strcmp(optarg, "4") == 0) || (strcasecmp(optarg, "4,slc") == 0)) nb_passes_sel = 2;
                else if (strcasecmp(optarg, "4,mlc") == 0) nb_passes_sel = 3;
                else if (strcasecmp(optarg, "4,tlc") == 0) nb_passes_sel = 4;
                else {
                    fprintf(stderr, "rufus: invalid bad blocks passes '%s'\n", optarg);
                    return 1;
                }
            }
            break;
        case OPT_ZERO:
            zero_drive = TRUE;
            break;
        case OPT_DD:
            force_dd = TRUE;
            break;
        case OPT_ISO_MODE:
            force_iso = TRUE;
            break;
        case OPT_HASH:
            opt_hash = TRUE;
            break;
        case OPT_PERSISTENCE: {
            char* end;
            double v = strtod(optarg, &end);
            uint64_t mult = 1;
            switch (*end) {
            case 'k': case 'K': mult = KB; break;
            case 'm': case 'M': mult = MB; break;
            case 'g': case 'G': mult = GB; break;
            case 't': case 'T': mult = TB; break;
            case 0: mult = MB; break;
            default:
                fprintf(stderr, "rufus: invalid persistence size '%s'\n", optarg);
                return 1;
            }
            persistence_size = (uint64_t)(v * (double)mult);
            break;
        }
        case OPT_WUE:
            if (!parse_wue(optarg))
                return 1;
            break;
        case OPT_NO_EXT_LABEL:
            extended_label = FALSE;
            break;
        case OPT_OLD_BIOS:
            old_bios_fixes = TRUE;
            break;
        case OPT_OFFLINE:
            opt_offline = TRUE;
            break;
        case OPT_ZIP:
            archive_path = optarg;
            break;
        case OPT_SAVE:
            opt_save = optarg;
            break;
        case OPT_UEFI_VALIDATION:
            validate_md5sum = TRUE;
            break;
        case OPT_WORKER:
            /* Used by the GUI (see worker.c) */
            worker_init();
            break;
        case OPT_UPDATES:
            CheckForUpdates(TRUE);
            return 0;
        case OPT_SHA512:
            enable_extra_hashes = TRUE;
            break;
        case OPT_TEST_HASHES:
            return (TestHashes() == 0) ? 0 : 1;
        case OPT_ESP:
            write_as_esp = TRUE;
            break;
        case OPT_FORCE:
            /* Same as the GUI's hidden "ignore boot marker" option */
            ignore_boot_marker = TRUE;
            break;
        case OPT_YES:
            cli_yes_to_all = TRUE;
            break;
        case OPT_BATCH:
            cli_batch_mode = TRUE;
            break;
        default:
            PrintUsageAndExit(1);
            break;
        }
    }
    (void)opt_wait;
    (void)opt_gui;

    if (opt_hash) {
        DWORD_PTR affinity[HASH_MAX + 1] = { 0 };
        struct stat st;
        if (image_path[0] == 0) {
            fprintf(stderr, "rufus: --hash requires an image (-i)\n");
            return 1;
        }
        if (stat(image_path, &st) != 0) {
            fprintf(stderr, "rufus: could not access '%s'\n", image_path);
            return 1;
        }
        img_report.image_size = (uint64_t)st.st_size;
        if (HashThread(affinity) != 0) {
            fprintf(stderr, "rufus: could not compute the checksums\n");
            return 1;
        }
        printf("MD5:    %s\nSHA1:   %s\nSHA256: %s\n", hash_str[0], hash_str[1], hash_str[2]);
        if (enable_extra_hashes)
            printf("SHA512: %s\n", hash_str[3]);
        return 0;
    }

    /* Save a device (or an optical disc) to an image file */
    if (opt_save != NULL) {
        if (opt_device == NULL) {
            fprintf(stderr, "rufus: --save requires --device\n");
            return 1;
        }
        if (strncmp(opt_device, "/dev/sr", 7) != 0)
            SelectDriveOrDie(opt_device);
        return SaveImage((strncmp(opt_device, "/dev/sr", 7) == 0) ? opt_device : GetPhysicalName(DriveIndex),
                         DriveIndex, opt_save) ? 0 : 1;
    }

    /* An image without a device: just report what Rufus makes of it */
    if ((image_path[0] != 0) && (opt_device == NULL) && !opt_list) {
        boot_type = BT_IMAGE;
        AnalyzeImage();
        return 0;
    }

    if (opt_device != NULL)
        SelectDriveOrDie(opt_device);

    if (opt_list) {
        ListDevices(opt_extra_devs);
        return 0;
    }

    if (argc == 1) {
        /* No arguments: the GUI is the primary interface.
         * (The GTK front-end is provided by a later build phase.) */
        PrintUsageAndExit(0);
    }

    if (opt_format || opt_user_boot || image_path[0] != 0 || zero_drive || badblocks_check) {
        if (GetSelectedDriveIndex() < 0) {
            fprintf(stderr, "rufus: --format requires --device\n");
            return 1;
        }
        /* Both SelectDriveByIndex() and SelectDriveByPath() set the global
         * DriveIndex to the 0x80-based value used by the rest of the code */
        RunFormat(DriveIndex);
        return 0;
    }

    /* Device selected but nothing to do: show its details, as Rufus does
     * when a device is passed on the command line without a format. */
    if (opt_device != NULL) {
        char fs_name[64];
        printf("Device %s\n", GetPhysicalName(DriveIndex));
        GetDrivePartitionData(DriveIndex, fs_name, sizeof(fs_name), TRUE);
        printf("  Partition style: %s\n",
               (SelectedDrive.PartitionStyle == PARTITION_STYLE_GPT) ? "GPT" :
               (SelectedDrive.PartitionStyle == PARTITION_STYLE_MBR) ? "MBR" : "RAW");
        printf("  Partitions: %d\n", SelectedDrive.nPartitions);
        return 0;
    }

    PrintUsageAndExit(0);
    return 0;
}
