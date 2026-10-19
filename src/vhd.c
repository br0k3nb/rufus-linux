/*
 * Rufus: The Reliable USB Formatting Utility
 * Virtual Disk Handling functions
 * Copyright © 2013-2026 Pete Batard <pete@akeo.ie>
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
 * Compressed image detection and the WIM functions are upstream's (vhd.c),
 * over the vendored bled and wimlib libraries. Upstream mounts VHD/VHDX
 * images with the Windows Virtual Disk API; the port reads them with its own
 * VHD/VHDX parser (vhdfile.c), exposed through the same VhdMountImage...()
 * calls. FFU images require Windows' dism and are not supported.
 */

#include "rfus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wimlib.h"
#include "wimlib_port.h"

extern BOOL ignore_boot_marker;
extern FILE* fd_md5sum;
extern uint64_t total_blocks, extra_blocks, nb_blocks, last_nb_blocks;
BOOL has_ffu_support = FALSE;

static int progress_op = OP_FILE_COPY, progress_msg = MSG_267;
static struct wimlib_progress_info_split last_split_progress;

typedef struct {
	const char* ext;
	uint8_t type;
} comp_assoc;

static comp_assoc file_assoc[] = {
	{ ".zip", BLED_COMPRESSION_ZIP },
	{ ".Z", BLED_COMPRESSION_LZW },
	{ ".gz", BLED_COMPRESSION_GZIP },
	{ ".lzma", BLED_COMPRESSION_LZMA },
	{ ".bz2", BLED_COMPRESSION_BZIP2 },
	{ ".xz", BLED_COMPRESSION_XZ },
	{ ".vtsi", BLED_COMPRESSION_VTSI },
	{ ".zst", BLED_COMPRESSION_ZSTD },
	{ ".ffu", BLED_COMPRESSION_MAX },
	{ ".vhd", BLED_COMPRESSION_MAX + 1 },
	{ ".vhdx", BLED_COMPRESSION_MAX + 2 },
};

// Look for a boot marker in the MBR area of the image
static int8_t IsCompressedBootableImage(const char* path)
{
	char *ext = NULL, *physical_disk = NULL;
	(void)physical_disk;
	unsigned char *buf = NULL;
	int i;
	FILE* fd = NULL;
	BOOL r = 0;
	int64_t dc = 0;

	img_report.compression_type = BLED_COMPRESSION_NONE;
	if (safe_strlen(path) > 4)
		for (ext = (char*)&path[safe_strlen(path) - 1]; (*ext != '.') && (ext != path); ext--);

	for (i = 0; i < ARRAYSIZE(file_assoc); i++) {
		if (safe_stricmp(ext, file_assoc[i].ext) == 0) {
			img_report.compression_type = file_assoc[i].type;
			buf = malloc(MBR_SIZE);
			if (buf == NULL)
				return 0;
			ErrorStatus = 0;
			if (img_report.compression_type < BLED_COMPRESSION_MAX) {
				bled_init(0, uprintf, NULL, NULL, NULL, NULL, NULL);
				dc = bled_get_uncompressed_size(path, file_assoc[i].type);
				if (dc > 0)
					img_report.projected_size = dc;
				dc = bled_uncompress_to_buffer(path, (char*)buf, MBR_SIZE, file_assoc[i].type);
				bled_exit();
			} else if (img_report.compression_type == BLED_COMPRESSION_MAX) {
				// PORT: FFU images are applied by dism.exe on Windows, which does
				// not exist on Linux (see FEATURES.md)
				// Dism, through FfuProvider.dll, can mount a .ffu as a physicaldrive, which we
				// could then use to poke the MBR as we do for VHD... Except Microsoft did design
				// dism to FAIL AND EXIT, after mounting the ffu as a virtual drive, if it doesn't
				// find something that looks like Windows at the specified image index... which it
				// usually won't in our case. So, curse Microsoft and their incredible short-
				// sightedness (or, most likely in this case, intentional malice, by BREACHING the
				// OS contract to keep useful disk APIs for their usage, and their usage only).
				// Then again, considering that .ffu's are GPT based, the marker should always be
				// present, so just check for the FFU signature and pretend there's a marker then.
				if (has_ffu_support) {
					fd = fopenU(path, "rb");
					if (fd != NULL) {
						img_report.is_vhd = TRUE;
						dc = fread(buf, 1, MBR_SIZE, fd);
						fclose(fd);
						// The signature may not be constant, but since the only game in town to
						// create FFU is dism, and dism appears to use "SignedImage " always,.we
						// might as well use this to our advantage.
						if (strncmp(&buf[4], "SignedImage ", 12) == 0) {
							// At this stage, the buffer is only used for marker validation.
							buf[0x1FE] = 0x55;
							buf[0x1FF] = 0xAA;
						}
					} else
						uprintf("Could not open %s: %d", path, errno);
				} else {
					uprintf("  An FFU image was selected, but this system does not have FFU support!");
				}
			} else {
				// PORT: VHD/VHDX images are read through the port's own virtual disk
				// reader (vhdfile.c) instead of the Windows VirtDisk API
				physical_disk = VhdMountImageAndGetSize(path, &img_report.projected_size);
				if (physical_disk != NULL) {
					img_report.is_vhd = TRUE;
					dc = VhdReadImage(buf, 0, MBR_SIZE);
				}
				VhdUnmountImage();
			}
			if (dc != MBR_SIZE) {
				free(buf);
				return FALSE;
			}
			if ((buf[0x1FE] == 0x55) && (buf[0x1FF] == 0xAA))
				r = 1;
			else if (ignore_boot_marker)
				r = 2;
			free(buf);
			return r;
		}
	}

	return FALSE;
}

// 0: non-bootable, 1: bootable, 2: forced bootable
int8_t IsBootableImage(const char* path)
{
	HANDLE handle = INVALID_HANDLE_VALUE;
	LARGE_INTEGER liImageSize;
	DWORD size;
	uint64_t wim_magic = 0;
	LARGE_INTEGER ptr = { 0 };
	int8_t is_bootable_img;

	uprintf("Disk image analysis:");
	handle = CreateFileU(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		uprintf("  Could not open image '%s'", path);
		is_bootable_img = -1;
		goto out;
	}

	is_bootable_img = IsCompressedBootableImage(path) ? 1 : 0;
	if (img_report.compression_type == BLED_COMPRESSION_NONE)
		is_bootable_img = AnalyzeMBR(handle, "  Image", FALSE) ? 1 : (ignore_boot_marker ? 2 : 0);

	if (!GetFileSizeEx(handle, &liImageSize)) {
		uprintf("  Could not get image size: %s", WindowsErrorString());
		is_bootable_img = -2;
		goto out;
	}
	img_report.image_size = (uint64_t)liImageSize.QuadPart;
	if (img_report.projected_size == 0)
		img_report.projected_size = img_report.image_size;
	size = sizeof(wim_magic);
	IGNORE_RETVAL(SetFilePointerEx(handle, ptr, NULL, FILE_BEGIN));
	img_report.is_windows_img = ReadFile(handle, &wim_magic, size, &size, NULL) && (wim_magic == WIM_MAGIC);
	if (img_report.is_windows_img)
		goto out;

out:
	safe_closehandle(handle);
	return is_bootable_img;
}

// WIM operations progress callback
static enum wimlib_progress_status WimProgressFunc(enum wimlib_progress_msg msg_type,
	union wimlib_progress_info* info, void* progctx)
{
	static BOOL init[3] = { 0 };

	if IS_ERROR(ErrorStatus)
		return WIMLIB_PROGRESS_STATUS_ABORT;

	switch (msg_type) {
	case WIMLIB_PROGRESS_MSG_EXTRACT_IMAGE_BEGIN:
		memset(init, 0, sizeof(init));
		uprintf("Applying image %d (\"%s\") from '%s' to '%s'",
			info->extract.image, info->extract.image_name,
			info->extract.wimfile_name, info->extract.target);
		break;
	case WIMLIB_PROGRESS_MSG_EXTRACT_FILE_STRUCTURE:
		if (!init[0]) {
			uprintf("Creating file structure...");
			init[0] = TRUE;
			uprint_progress(0, 0);
		}
		UpdateProgressWithInfoUpTo(98, progress_op, progress_msg, info->extract.current_file_count, info->extract.end_file_count * 6);
		uprint_progress(info->extract.current_file_count, info->extract.end_file_count);
		break;
	case WIMLIB_PROGRESS_MSG_EXTRACT_STREAMS:
		if (!init[1]) {
			uprintf("\nExtracting file data...");
			init[1] = TRUE;
			uprint_progress(0, 0);
		}
		UpdateProgressWithInfoUpTo(98, progress_op, progress_msg, info->extract.total_bytes + (4 * info->extract.completed_bytes), info->extract.total_bytes * 6);
		uprint_progress(info->extract.completed_bytes, info->extract.total_bytes);
		break;
	case WIMLIB_PROGRESS_MSG_EXTRACT_METADATA:
		if (!init[2]) {
			uprintf("\nApplying metadata to files...");
			init[2] = TRUE;
			uprint_progress(0, 0);
		}
		UpdateProgressWithInfoUpTo(98, progress_op, progress_msg, info->extract.current_file_count + (5 * info->extract.end_file_count), info->extract.end_file_count * 6);
		uprint_progress(info->extract.current_file_count, info->extract.end_file_count);
		if (info->extract.current_file_count >= info->extract.end_file_count)
			uprintf("\n");
		break;
	case WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART:
		last_split_progress = info->split;
		uprintf("● %s", info->split.part_name);
		break;
	case WIMLIB_PROGRESS_MSG_SPLIT_END_PART:
		if (fd_md5sum != NULL) {
			// Don't bother computing the hash at write time - just do it post creation
			uint8_t sum[MD5_HASHSIZE];
			// PORT: md5sum.txt paths are relative to the root of the target
			const char* filename = info->split.part_name;
			HashFile(HASH_MD5, filename, sum);
			for (int j = 0; j < MD5_HASHSIZE; j++)
				fprintf(fd_md5sum, "%02x", sum[j]);
			fprintf(fd_md5sum, "  ./%s\n", iso_target_relative_path(filename));
		}
		break;
	case WIMLIB_PROGRESS_MSG_WRITE_STREAMS: {
		uint64_t completed_bytes = last_split_progress.completed_bytes + info->write_streams.completed_compressed_bytes;
		nb_blocks = last_nb_blocks + completed_bytes / 2048;
		UpdateProgressWithInfo(OP_FILE_COPY, MSG_231, nb_blocks, total_blocks + extra_blocks);
		break;
	}
	default:
		break;
	}

	return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

// Return the WIM version of an image
uint32_t GetWimVersion(const char* image)
{
	int r;
	WIMStruct* wim;
	struct wimlib_wim_info info;

	if (image == NULL)
		return 0;

	r = wimlib_open_wimU(image, 0, &wim);
	if (r == 0) {
		r = wimlib_get_wim_info(wim, &info);
		wimlib_free(wim);
		if (r == 0)
			return info.wim_version;
	}
	uprintf("WARNING: Could not get WIM version: Error %d", r);
	return 0;
}

// Extract a file from a WIM image. Returns the allocated path of the extracted file or NULL on error.
BOOL WimExtractFile(const char* image, int index, const char* src, const char* dst)
{
	int r = 1;
	WIMStruct* wim;
	char tmp[MAX_PATH] = "", *p;

	if ((image == NULL) || (src == NULL) || (dst == NULL))
		goto out;

	// PORT: '/' is the path separator (in the WIM too, for wimlib on UNIX)
	if (strrchr(src, '/') == NULL || strrchr(dst, '/') == NULL)
		goto out;
	p = strrchr(dst, '/');
	*p = '\0';

	wimlib_global_init(0);
	wimlib_set_print_errors(true);
	r = wimlib_open_wimU(image, 0, &wim);
	if (r == 0) {
		r = wimlib_extract_pathsU(wim, index, dst, &src, 1, WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE);
		wimlib_free(wim);
		static_strcpy(tmp, dst);
		static_strcat(tmp, strrchr(src, '/'));
		*p = '/';
		if (!MoveFileExU(tmp, dst, MOVEFILE_REPLACE_EXISTING)) {
			uprintf("  Could not rename %s to %s: %s", tmp, dst, WindowsErrorString());
			r = 1;
		}
	}
	wimlib_global_cleanup();

out:
	return (r == 0);
}

// Split an install.wim for FAT32 limits
BOOL WimSplitFile(const char* src, const char* dst)
{
	int r = 1;
	WIMStruct* wim;

	if ((src == NULL) || (dst == NULL))
		goto out;

	wimlib_global_init(0);
	wimlib_set_print_errors(true);
	r = wimlib_open_wimU(src, 0, &wim);
	if (r == 0) {
		wimlib_register_progress_function(wim, WimProgressFunc, NULL);
		r = wimlib_splitU(wim, dst, 4094ULL * MB, WIMLIB_WRITE_FLAG_FSYNC);
		wimlib_free(wim);
	}
	wimlib_global_cleanup();

out:
	return (r == 0);
}

BOOL WimApplyImage(const char* image, int index, const char* dst)
{
	int r = 1;
	WIMStruct* wim;

	wimlib_global_init(0);
	wimlib_set_print_errors(true);

	uprintf("Opening: %s:[%d]", image, index);
	r = wimlib_open_wimU(image, 0, &wim);
	if (r == 0) {
		progress_op = OP_FILE_COPY;
		progress_msg = MSG_267;
		wimlib_register_progress_function(wim, WimProgressFunc, NULL);
		r = wimlib_extract_imageU(wim, index, dst, 0);
		wimlib_free(wim);
	} else {
		uprintf("Failed to open '%s': Wimlib error %d", image, r);
	}
	wimlib_global_cleanup();
	return (r == 0);
}

