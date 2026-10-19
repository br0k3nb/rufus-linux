/*
 * Rufus: The Reliable USB Formatting Utility
 * Image analysis and option defaults
 * Copyright © 2011-2025 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Rufus Linux port notes
 * ----------------------
 * The front-end independent parts of upstream rufus.c: ImageScanThread()
 * (as ScanImage()), GetBootladerInfo(), DisplayISOProps(), FindArch(), and
 * the logic of SetPartitionSchemeAndTargetSystem()/SetFSFromISO()/
 * SetProposedLabel(), which upstream applies to the dialog's combo boxes.
 * Here they compute the allowed values and defaults into the globals that
 * the CLI and GUI front-ends then present.
 */

#include "rfus.h"
#include "localization.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cregex.h"

static const char* arch_name[ARCH_MAX] = {
    "unknown", "x86_32", "x86_64", "ARM", "ARM64", "IA64", "RISC-V 64", "LoongArch 64", "EBC" };

#define PRINT_ISO_PROP(b, ...) do {if (b) uprintf(__VA_ARGS__);} while(0)

/* Parse a PE executable file and return its CPU architecture */
static uint8_t FindArch(const char* path)
{
    uint8_t* buf = NULL;
    uint32_t len = read_file(path, &buf);
    uint8_t ret = ARCH_UNKNOWN;

    if (len >= sizeof(IMAGE_DOS_HEADER))
        ret = (uint8_t)MachineToArch(GetPeArch(buf));
    free(buf);
    return ret;
}

void GetBootladerInfo(void)
{
    static const char* revocation_type[] = { "UEFI DBX", "Windows SSP", "Linux SBAT", "Windows SVN", "Cert DBX" };
    int r;
    BOOL sb_signed;
    uint32_t i, len;
    uint8_t* buf = NULL;

    /* Check UEFI bootloaders for revocation */
    if (!IS_EFI_BOOTABLE(img_report))
        return;

    PrintStatus(0, MSG_351);
    uprintf("UEFI bootloaders analysis:");
    for (i = 0; i < ARRAYSIZE(img_report.efi_boot_entry) && img_report.efi_boot_entry[i].path[0] != 0; i++) {
        len = ReadISOFileToBuffer(image_path, img_report.efi_boot_entry[i].path, &buf);
        if (len == 0) {
            uprintf("  Warning: Failed to extract '%s' to check for UEFI Secure Boot info", img_report.efi_boot_entry[i].path);
            continue;
        }
        sb_signed = IsSignedBySecureBootAuthority(buf, len);
        if (sb_signed)
            img_report.has_secureboot_bootloader |= 1;
        uprintf("  • %s%s", img_report.efi_boot_entry[i].path, sb_signed ? "*" : "");
        r = IsBootloaderRevoked(buf, len);
        if (r > 0) {
            uprintf("  WARNING: '%s' has been revoked by %s", img_report.efi_boot_entry[i].path, revocation_type[r - 1]);
            img_report.has_secureboot_bootloader |= 1 << r;
        }
        safe_free(buf);
    }
}

void DisplayISOProps(void)
{
    static char inst_str[] = " [1/#]";
    int i;

    uprintf("ISO label: '%s'", img_report.label);
    if (img_report.win_version.major != 0) {
        if (img_report.win_version.minor == 0)
            uprintf("  Detected: Windows %d ISO (Build %d.%d)", img_report.win_version.major,
                img_report.win_version.build, img_report.win_version.revision);
        else
            uprintf("  Detected: Windows %d.%d ISO (Build %d.%d)", img_report.win_version.major,
                img_report.win_version.minor, img_report.win_version.build, img_report.win_version.revision);
    }

    uprintf("  Size: %s (Projected)", SizeToHumanReadable(img_report.projected_size, FALSE, FALSE));
    if (img_report.mismatch_size > 0) {
        uprintf("  ERROR: Detected that file on disk has been truncated by %s!",
            SizeToHumanReadable(img_report.mismatch_size, FALSE, FALSE));
        Notification(MB_OK | MB_ICONWARNING, lmprintf(MSG_297),
            lmprintf(MSG_298, SizeToHumanReadable(img_report.mismatch_size, FALSE, FALSE)));
    } else if (img_report.mismatch_size < 0) {
        /* Not an error (ISOHybrid?), but we report it just in case */
        uprintf("  Note: File on disk is larger than reported ISO size by %s...",
            SizeToHumanReadable(-img_report.mismatch_size, FALSE, FALSE));
    }

    PRINT_ISO_PROP(img_report.has_4GB_file, "  Has a >4GB file");
    PRINT_ISO_PROP(img_report.has_long_filename, "  Has a >64 chars filename");
    PRINT_ISO_PROP(img_report.has_deep_directories, "  Has a Rock Ridge deep directory");
    PRINT_ISO_PROP(HAS_SYSLINUX(img_report), "  Uses: Syslinux/Isolinux v%s", img_report.sl_version_str);
    if (HAS_SYSLINUX(img_report) && (SL_MAJOR(img_report.sl_version) < 5)) {
        for (i = 0; i < NB_OLD_C32; i++) {
            PRINT_ISO_PROP(img_report.has_old_c32[i], "    With an old %s", old_c32_name[i]);
        }
    }
    PRINT_ISO_PROP(HAS_KOLIBRIOS(img_report), "  Uses: KolibriOS");
    PRINT_ISO_PROP(HAS_REACTOS(img_report), "  Uses: ReactOS");
    PRINT_ISO_PROP(img_report.has_grub4dos, "  Uses: Grub4DOS");
    PRINT_ISO_PROP(img_report.has_grub2, "  Uses: GRUB2 (%s)", img_report.grub2_version);
    if (img_report.has_efi == 0x80)
        uprintf("  Uses: EFI (through '%s')", img_report.efi_img_path);
    else
        PRINT_ISO_PROP(img_report.has_efi, "  Uses: EFI %s", HAS_WIN7_EFI(img_report) ? "(win7_x64)" : "");
    PRINT_ISO_PROP(HAS_BOOTMGR(img_report), "  Uses: Bootmgr (%s)",
        HAS_BOOTMGR_BIOS(img_report) ? (HAS_BOOTMGR_EFI(img_report) ? "BIOS and UEFI" : "BIOS only") : "UEFI only");
    PRINT_ISO_PROP(HAS_WINPE(img_report), "  Uses: WinPE %s", (img_report.uses_minint) ? "(with /minint)" : "");
    if (HAS_WININST(img_report)) {
        inst_str[4] = '0' + img_report.wininst_index;
        uprintf("  Uses: Install.%s%s (version %d.%d.%d%s)", &img_report.wininst_path[0][strlen(img_report.wininst_path[0]) - 3],
            (img_report.wininst_index > 1) ? inst_str : "", (img_report.wininst_version >> 24) & 0xff,
            (img_report.wininst_version >> 16) & 0xff, (img_report.wininst_version >> 8) & 0xff,
            (img_report.wininst_version >= SPECIAL_WIM_VERSION) ? "+" : "");
    }
    if (img_report.needs_ntfs) {
        uprintf("  Note: This ISO uses symbolic links and was not designed to work without them.");
        uprintf("  Because of this, only NTFS will be allowed as the target file system.");
    } else {
        PRINT_ISO_PROP(img_report.has_symlinks,
            "  Note: This ISO uses symbolic links, which may not be replicated due to file system");
        PRINT_ISO_PROP((img_report.has_symlinks == SYMLINKS_RR),
            "  limitations. Because of this, some features from this image may not work...");
        PRINT_ISO_PROP((img_report.has_symlinks == SYMLINKS_UDF),
            "  limitations. Because of this, the size required for the target media may be much");
        PRINT_ISO_PROP((img_report.has_symlinks == SYMLINKS_UDF),
            "  larger than size of the ISO...");
    }
}

/*
 * Analyze the image at image_path and fill img_report (port of the non UI
 * part of upstream's ImageScanThread()). Returns TRUE if the image can be
 * used, FALSE otherwise (in which case the user has been notified).
 */
BOOL ScanImage(void)
{
    /* Regexp patterns used to match ISO labels for the Red Hat 8 derivatives
     * where we should apply an inst.stage2 ➔ inst.repo workaround for ISO
     * mode (per: https://github.com/rhinstaller/anaconda/pull/3529). */
    const char* redhat8_derivative[] = {
        "^AlmaLinux-[8-9].*",       /* AlmaLinux 8.x and 9.x */
        "^Fedora.*-3[3-9].*",       /* Fedora 33-39 */
        "^CentOS.*-[8-9].*",        /* CentOS and CentOS Stream 8.x and 9.x */
        "^Circle.*-[8-9].*",        /* Circle Linux 8.x.and 9.x */
        "^OL-[8-9].*",              /* Oracle Linux 8.x and 9.x */
        "^RHEL-[8-9].*",            /* Red Hat 8.x and 9.x */
        "^Rocky-[8-9].*",           /* Rocky Linux 8.x and 9.x */
        "^MIRACLE-LINUX-[8-9].*",   /* MIRACLE LINUX 8.x and 9.x */
    };
    const char* suse_derivative[] = {
        "Install-SUSE",
        "Install-LEAP",
        "openSUSE-Tumbleweed"
    };
    int i;
    uint8_t arch;
    char tmp_path[MAX_PATH], tmp_str[64];
    const char* matches[REGEX_VM_MAX_MATCHES];
    cregex_node_t* node = NULL;
    cregex_program_t* program = NULL;

    if (image_path[0] == 0)
        return FALSE;
    PrintInfoDebug(0, MSG_202);
    memset(&img_report, 0, sizeof(img_report));
    ErrorStatus = 0;
    img_report.is_iso = (BOOLEAN)ExtractISO(image_path, "", TRUE);
    img_report.is_bootable_img = IsBootableImage(image_path);
    if (img_report.wininst_index > 0 || img_report.is_windows_img)
        PopulateWindowsVersion();

    if ((ErrorStatus == RUFUS_ERROR(ERROR_CANCELLED)) || (img_report.image_size == 0) ||
        (!img_report.is_iso && (img_report.is_bootable_img <= 0) && !img_report.is_windows_img)) {
        /* Failed to scan image */
        if (img_report.is_bootable_img < 0)
            Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_042), lmprintf(MSG_322, image_path));
        else
            Notification(MB_OK | MB_ICONINFORMATION, lmprintf(MSG_081), lmprintf(MSG_082));
        PrintInfoDebug(0, MSG_203);
        return FALSE;
    }

    if (img_report.is_windows_img) {
        if (GetTempFileNameU(temp_dir, APPLICATION_NAME, 0, tmp_path) != 0) {
            /* Only look at index 1 for now. If people complain, we may look for more. */
            if (WimExtractFile(image_path, 1, "Windows/Boot/EFI/bootmgr.efi", tmp_path)) {
                arch = FindArch(tmp_path);
                if (arch != 0) {
                    uprintf("  Image contains a%s %s EFI boot manager",
                        (arch == ARCH_RISCV_64) ? "" : "n", arch_name[arch]);
                    img_report.has_efi = 1 | (1 << arch);
                    img_report.has_bootmgr_efi = TRUE;
                    img_report.wininst_index = 1;
                } else {
                    uprintf("  Image does not contain an EFI boot manager");
                }
            }
            DeleteFileU(tmp_path);
        }
        tmp_str[0] = 0;
        if (img_report.win_version.major != 0) {
            if (img_report.win_version.minor == 0)
                static_sprintf(tmp_str, " %d (Build %d.%d)", img_report.win_version.major,
                    img_report.win_version.build, img_report.win_version.revision);
            else
                static_sprintf(tmp_str, " %d.%d (Build %d.%d)", img_report.win_version.major,
                    img_report.win_version.minor, img_report.win_version.build, img_report.win_version.revision);
        }
        uprintf("  Image is a %sUEFI bootable Windows%s installation image", img_report.has_efi ? "" : "NON-", tmp_str);
    } else if (IS_DD_BOOTABLE(img_report)) {
        if (img_report.is_bootable_img == 2)
            uprintf("  Image is a FORCED non-bootable image");
        else
            uprintf("  Image is a %sbootable %s image",
                (img_report.compression_type != BLED_COMPRESSION_NONE && img_report.compression_type < BLED_COMPRESSION_MAX) ?
                "compressed " : "", img_report.is_vhd ? "VHD" : "disk");
        if (img_report.projected_size > 0)
            uprintf("  Size: %s (Projected)", SizeToHumanReadable(img_report.projected_size, FALSE, FALSE));
    }

    if (img_report.is_iso) {
        GetBootladerInfo();
        DisplayISOProps();

        for (i = 0; i < (int)ARRAYSIZE(redhat8_derivative) && !img_report.rh8_derivative; i++) {
            node = cregex_parse(redhat8_derivative[i]);
            if (node != NULL) {
                program = cregex_compile_node(node);
                cregex_parse_free(node);
            }
            if (node == NULL || program == NULL) {
                uprintf("Internal error: Failed to parse '%s'", redhat8_derivative[i]);
                break;
            }
            if (cregex_program_run(program, img_report.label, matches, ARRAYSIZE(matches)) > 0)
                img_report.rh8_derivative = TRUE;
            cregex_compile_free(program);
            program = NULL;
        }

        for (i = 0; i < (int)ARRAYSIZE(suse_derivative); i++) {
            if (strncmp(img_report.label, suse_derivative[i], strlen(suse_derivative[i])) == 0) {
                img_report.disable_iso = TRUE;
                break;
            }
        }

        /* If we have EFI GRUB, detect whether we have FAT, exFAT or NTFS support (with exFAT
         * or NTFS support being mandatory if we have a file that is larger than 4 GB) */
        if (img_report.has_grub2 & 0x80) {
            if (!(img_report.has_grub2_fs & 0x7) || (img_report.has_4GB_file && !(img_report.has_grub2_fs & 0x6)))
                img_report.disable_iso = TRUE;
        }

        /* If we have an ISOHybrid, but without an ISO method we support, disable ISO support altogether */
        if (IS_DD_BOOTABLE(img_report) && (img_report.disable_iso ||
                (!IS_BIOS_BOOTABLE(img_report) && !IS_EFI_BOOTABLE(img_report)))) {
            Notification(MB_OK | MB_ICONINFORMATION, lmprintf(MSG_274, "ISOHybrid"), lmprintf(MSG_321));
            uprintf("Note: DD image mode enforced since this ISOHybrid is not ISO mode compatible.");
            img_report.is_iso = FALSE;
        }
    }
    if (!IS_DD_BOOTABLE(img_report) && !IS_BIOS_BOOTABLE(img_report) && !IS_EFI_BOOTABLE(img_report)) {
        /* No boot method that we support */
        PrintInfo(0, MSG_081);
        Notification(MB_OK | MB_ICONINFORMATION, lmprintf(MSG_081), lmprintf(MSG_082));
        return FALSE;
    }
    PrintInfo(0, MSG_210);
    return TRUE;
}

/*
 * Compute the partition scheme and target system for the current boot
 * selection, as upstream's SetPartitionSchemeAndTargetSystem() does for its
 * combo boxes: 'allowed_pt'/'allowed_tt' receive the choices (MBR, GPT, SFD /
 * BIOS, UEFI, BIOS+UEFI) and the globals partition_type/target_type are set
 * to the default. 'selected_pt' is the user's choice, if any (-1 for none).
 */
/* PORT: upstream indexes its partition scheme arrays with the Windows
 * PARTITION_STYLE values (MBR = 0, GPT = 1, RAW/SFD = 2), which differ
 * from the port's. PT_IDX() maps a partition style to that index. */
#define PT_IDX(pt)  (((pt) == PARTITION_STYLE_MBR) ? 0 : (((pt) == PARTITION_STYLE_GPT) ? 1 : 2))

void GetPartitionSchemeAndTargetSystem(int selected_pt, BOOL allowed_pt[3], BOOL allowed_tt[3])
{
    BOOL is_windows_to_go_selected = (boot_type == BT_IMAGE) && (image_path[0] != 0) && windows_to_go;
    int preferred_pt;

    allowed_pt[0] = TRUE; allowed_pt[1] = TRUE; allowed_pt[2] = FALSE;
    allowed_tt[0] = TRUE; allowed_tt[1] = TRUE; allowed_tt[2] = FALSE;
    switch (boot_type) {
    case BT_NON_BOOTABLE:
        allowed_pt[PT_IDX(PARTITION_STYLE_SFD)] = TRUE;
        allowed_tt[0] = FALSE;
        allowed_tt[1] = FALSE;
        allowed_tt[2] = TRUE;
        break;
    case BT_IMAGE:
        if (image_path[0] == 0)
            break;
        /* Check if image is EFI bootable */
        if (!IS_EFI_BOOTABLE(img_report)) {
            allowed_pt[PT_IDX(PARTITION_STYLE_GPT)] = FALSE;
            allowed_tt[1] = FALSE;
            break;
        }
        /* Image is EFI bootable => set dual BIOS + UEFI and so on... */
        if (IS_BIOS_BOOTABLE(img_report)) {
            if (!HAS_WINDOWS(img_report) || allow_dual_uefi_bios || is_windows_to_go_selected) {
                allowed_tt[0] = FALSE;
                allowed_tt[1] = TRUE;
                allowed_tt[2] = TRUE;
            }
            /* Syslinux 4.x or earlier has no support for NTFS so if an image is using Syslinux 4.x only
             * and has a 4 GB file (which forces us to use NTFS) then disable MBR altogether as we won't
             * be able to make a working MBR install of Syslinux. */
            if (HAS_SYSLINUX(img_report) && (SL_MAJOR(img_report.sl_version) < 5) && img_report.has_4GB_file &&
                !HAS_BOOTMGR(img_report) && !HAS_WINPE(img_report) && !HAS_GRUB(img_report))
                allowed_pt[PT_IDX(PARTITION_STYLE_MBR)] = FALSE;
        } else {
            allowed_tt[0] = FALSE;
        }
        break;
    case BT_MSDOS:
    case BT_FREEDOS:
    case BT_SYSLINUX_V4:
    case BT_SYSLINUX_V6:
    case BT_REACTOS:
    case BT_GRUB4DOS:
    case BT_GRUB2:
        allowed_pt[PT_IDX(PARTITION_STYLE_GPT)] = FALSE;
        allowed_tt[1] = FALSE;
        break;
    case BT_UEFI_NTFS:
        allowed_tt[0] = FALSE;
        break;
    }

    /* Override partition type selection to GPT for drives larger than 2TB */
    if (SelectedDrive.DiskSize > 2 * TB)
        selected_pt = PARTITION_STYLE_GPT;
    preferred_pt = (SelectedDrive.PartitionStyle == PARTITION_STYLE_GPT) ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;
    if (boot_type == BT_NON_BOOTABLE)
        preferred_pt = (selected_pt >= 0) ? selected_pt : PARTITION_STYLE_MBR;
    else if (boot_type == BT_UEFI_NTFS)
        preferred_pt = (selected_pt >= 0) ? selected_pt : PARTITION_STYLE_GPT;
    else if ((boot_type == BT_IMAGE) && (image_path[0] != 0) && (img_report.is_iso || img_report.is_windows_img)) {
        if (HAS_WINDOWS(img_report) && img_report.has_efi)
            preferred_pt = allow_dual_uefi_bios ? PARTITION_STYLE_MBR :
                ((selected_pt >= 0) ? selected_pt : PARTITION_STYLE_GPT);
        if (IS_DD_BOOTABLE(img_report))
            preferred_pt = (selected_pt >= 0) ? selected_pt : PARTITION_STYLE_MBR;
    } else if (selected_pt >= 0) {
        preferred_pt = selected_pt;
    }
    /* Like a combo box, fall back to the first allowed entry */
    if (!allowed_pt[PT_IDX(preferred_pt)])
        preferred_pt = allowed_pt[PT_IDX(PARTITION_STYLE_MBR)] ? PARTITION_STYLE_MBR : PARTITION_STYLE_GPT;
    partition_type = preferred_pt;

    /* Target system: the first entry of upstream's target system combo */
    if (allowed_tt[0] && (partition_type != PARTITION_STYLE_GPT))
        target_type = TT_BIOS;
    else if (allowed_tt[1] && !((partition_type == PARTITION_STYLE_MBR) && (boot_type == BT_IMAGE) &&
             IS_BIOS_BOOTABLE(img_report) && IS_EFI_BOOTABLE(img_report)))
        target_type = TT_UEFI;
    else
        target_type = TT_BIOS;  /* "BIOS or UEFI" */
    /* Adjust the allowed list to what the combo would show */
    if (partition_type == PARTITION_STYLE_GPT) {
        allowed_tt[0] = FALSE;
        if (boot_type != BT_NON_BOOTABLE)
            allowed_tt[2] = FALSE;
    } else if ((boot_type == BT_IMAGE) && IS_BIOS_BOOTABLE(img_report) && IS_EFI_BOOTABLE(img_report)) {
        allowed_tt[1] = FALSE;
    }
}

/* Select the preferred file system for an image (upstream SetFSFromISO()) */
int GetPreferredFSForImage(uint32_t fs_mask)
{
    int preferred_fs = FS_UNKNOWN;

    if (image_path[0] == 0)
        return FS_UNKNOWN;
    fs_mask |= (1 << FS_NTFS) | (img_report.has_4GB_file ? 0 : (1 << FS_FAT32));
    /* Syslinux and EFI have precedence over bootmgr (unless the user selected BIOS as target type) */
    if ((HAS_SYSLINUX(img_report)) || (HAS_REACTOS(img_report)) || HAS_KOLIBRIOS(img_report) ||
        (IS_EFI_BOOTABLE(img_report) && (target_type == TT_UEFI) && (!windows_to_go) && (!img_report.has_4GB_file))) {
        if (fs_mask & (1 << FS_FAT32))
            preferred_fs = FS_FAT32;
        else if ((fs_mask & (1 << FS_FAT16)) && !HAS_KOLIBRIOS(img_report))
            preferred_fs = FS_FAT16;
    } else if ((windows_to_go) || HAS_BOOTMGR(img_report) || HAS_WINPE(img_report)) {
        if ((fs_mask & (1 << FS_FAT32)) && (!img_report.has_4GB_file) && (allow_dual_uefi_bios))
            preferred_fs = FS_FAT32;
        else if (fs_mask & (1 << FS_NTFS))
            preferred_fs = FS_NTFS;
    }
    return preferred_fs;
}
