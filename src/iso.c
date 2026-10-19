/*
 * Rufus: The Reliable USB Formatting Utility
 * ISO file extraction
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Based on libcdio's iso & udf samples:
 * Copyright © 2003-2014 Rocky Bernstein <rocky@gnu.org>
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
 * Rufus Linux port: ISO image analysis and file extraction.
 *
 * Port of upstream src/iso.c. The flow is the same as on Windows (scan the
 * ISO/UDF file system properties, then extract the files while patching a few
 * config files), but the OS-specific parts are adapted:
 *
 *  - there is no GUI, so the UM_xxx/SendMessage() calls are dropped,
 *  - the WIM and libfat (EFI FAT image) helpers are not vendored yet and are
 *    stubbed out with TODO notes,
 *  - directory timestamps are restored through utimensat(),
 *  - the MD5 code needed for md5sum validation is embedded below (upstream
 *    uses src/hash.c).
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define DO_NOT_WANT_COMPATIBILITY
#include <cdio/cdio.h>
#include <cdio/logging.h>
#include <cdio/iso9660.h>
#include <cdio/udf.h>
#include "libfat.h"

/* The Linux port has no GUI, so the upstream "ui.h"/"vhd.h"/"missing.h"...
 * includes are gone and the few missing bits are provided here. */
#define uuprintf(...) duprintf(__VA_ARGS__)
#define _STATIC_ASSERT(expr) _Static_assert(expr, #expr)
#ifndef FILES_DIR
#define FILES_DIR APPLICATION_NAME
#endif
#ifndef ISO_BUFFER_SIZE
#define ISO_BUFFER_SIZE (64 * KB)
#endif
#ifndef isspaceU
#define isspaceU(c) isspace((unsigned char)(c))
#endif

// How often should we update the progress bar, as updating the
// progress bar too frequently will bring extraction to a crawl
_Static_assert(256 * KB >= ISO_BLOCKSIZE, "Can't set PROGRESS_THRESHOLD");
#define PROGRESS_THRESHOLD        ((256 * KB) / ISO_BLOCKSIZE)

// Needed for UDF symbolic link testing
// NB: <sys/stat.h> already provides S_IFLNK/S_ISLNK on Linux
#ifndef S_IFLNK
#define S_IFLNK                   0xA000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)                (((m) & S_IFMT) == S_IFLNK)
#endif

// Set the iso_open_ext() extension mask according to our global options
#define ISO_EXTENSION_MASK        (ISO_EXTENSION_ALL & (enable_joliet ? ISO_EXTENSION_ALL : ~ISO_EXTENSION_JOLIET) & \
                                  (enable_rockridge ? ISO_EXTENSION_ALL : ~ISO_EXTENSION_ROCK_RIDGE))

// Is an MBR partition type for a FAT12/FAT16/FAT32 partition?
#define IS_FAT_TYPE(x)            ((x) == 0x01 || (x) == 0x04 || (x) == 0x06 || (x) == 0x0b || (x) == 0x0c || (x) == 0x0e)

// Needed for UDF ISO access
CdIo_t* cdio_open (const char* psz_source, driver_id_t driver_id) {return NULL;}
void cdio_destroy (CdIo_t* p_cdio) {}

typedef struct {
	BOOLEAN is_cfg;
	BOOLEAN is_conf;
	BOOLEAN is_syslinux_cfg;
	BOOLEAN is_grub_cfg;
	BOOLEAN is_menu_cfg;
	BOOLEAN is_old_c32[NB_OLD_C32];
} EXTRACT_PROPS;

// NB: img_report, fd_md5sum, iso_blocking_status, the block counters, the
// enable_* options, has_ldlinux_c32 and modified_files are defined in
// linux/src/globals.c (they are shared with the rest of the port) and must NOT
// be redefined here like upstream does.
extern uint64_t md5sum_totalbytes;
extern BOOL preserve_timestamps, enable_ntfs_compression, validate_md5sum;
extern HANDLE format_thread;
extern StrArray modified_files;
// Upstream defines this one in rufus.c; it is only used by this module
BOOL use_own_c32[NB_OLD_C32] = { 0 };
#define ISO_BLOCKING(x) do {x; iso_blocking_status++; } while(0)
static const char* psz_extract_dir;
static const char* bootmgr_name = "bootmgr";
const char* bootmgr_efi_name = "bootmgr.efi";
static const char* grldr_name = "grldr";
static const char* ldlinux_name = "ldlinux.sys";
static const char* ldlinux_c32 = "ldlinux.c32";
const char* md5sum_name[2] = { "md5sum.txt", "MD5SUMS" };
static const char* casper_dirname = "/casper";
static const char* proxmox_dirname = "/proxmox";
const char* efi_dirname = "/efi/boot";
const char* efi_bootname[3] = { "boot", "grub", "mm" };
const char* efi_archname[ARCH_MAX] = { "", "ia32", "x64", "arm", "aa64", "ia64", "riscv64", "loongarch64", "ebc" };
static const char* sources_str = "/sources";
static const char* wininst_name[] = { "install.wim", "install.esd", "install.swm" };
_STATIC_ASSERT(ARRAYSIZE(wininst_name) < 4);	// Must fit as 4 bit position flag
// We only support GRUB/BIOS (x86) that uses a standard config dir (/boot/grub/i386-pc/)
// If the disc was mastered properly, GRUB/EFI will take care of itself
static const char* grub_dirname[] = { "/boot/grub/i386-pc", "/boot/grub2/i386-pc" };
static const char* grub_cfg[] = { "grub.cfg", "loopback.cfg" };
static const char* menu_cfg = "menu.cfg";
// NB: Do not alter the order of the array below without validating hardcoded indexes in check_iso_props
static const char* syslinux_cfg[] = { "isolinux.cfg", "syslinux.cfg", "extlinux.conf", "txt.cfg", "live.cfg" };
static const char* isolinux_bin[] = { "isolinux.bin", "boot.bin" };
static const char* pe_dirname[] = { "/i386", "/amd64", "/minint" };
static const char* pe_file[] = { "ntdetect.com", "setupldr.bin", "txtsetup.sif" };
static const char* reactos_name[] = { "setupldr.sys", "freeldr.sys" };
static const char* kolibri_name = "kolibri.img";
static const char* autorun_name = "autorun.inf";
static const char* manjaro_marker = ".miso";
static const char* pop_os_name = "pop-os";
static const char* stupid_antivirus = "  NOTE: This is usually caused by a poorly designed security solution. "
	"See https://bit.ly/40qDtyF.\r\n  This file will be skipped for now, but you should really "
	"look into using a *SMARTER* antivirus solution.";
const char* old_c32_name[NB_OLD_C32] = OLD_C32_NAMES;
static const int64_t old_c32_threshold[NB_OLD_C32] = OLD_C32_THRESHOLD;
static uint8_t joliet_level = 0;
static uint32_t md5sum_size = 0;
static BOOL scan_only = FALSE;
static StrArray config_path, isolinux_path, grub_filesystems;
static char symlinked_syslinux[MAX_PATH], *md5sum_data = NULL, *md5sum_pos = NULL;

/* ------------------------------------------------------------------ */
/* Helpers ported from upstream source files that the Linux port does  */
/* not otherwise provide (parser.c, syslinux.c, msapi_utf8.h...)       */
/* ------------------------------------------------------------------ */

// Return a pointer to the ISO-relative part of a path, i.e. the path with the
// extraction directory prefix removed. The leading '/' is preserved, so that
// e.g. "/mnt/usb/boot/isolinux/syslnx32.cfg" gives "/boot/isolinux/syslnx32.cfg".
// NB: This replaces upstream's use of &path[2], which skips the Windows drive part.
static const char* iso_relative_part(const char* path)
{
	static const char* nullstr = "";
	size_t root_len;

	if (path == NULL)
		return nullstr;
	if (psz_extract_dir != NULL) {
		root_len = safe_strlen(psz_extract_dir);
		// Ignore any trailing separators in the extraction directory
		while ((root_len > 0) && (psz_extract_dir[root_len - 1] == '/'))
			root_len--;
		if ((root_len != 0) && (strncmp(path, psz_extract_dir, root_len) == 0))
			return &path[root_len];
	}
	return path;
}

// Path relative to the extraction root, without leading separator (used by
// the WIM splitting progress callback in vhd.c for md5sum.txt entries)
const char* iso_target_relative_path(const char* path)
{
	const char* rel = iso_relative_part(path);
	while (rel[0] == '/')
		rel++;
	return rel;
}

// Same as iso_relative_part(), but returns an allocated copy of the relative
// path without the leading '/', as expected for the md5sum.txt entries
static char* iso_relative_path(const char* path)
{
	const char* rel = iso_relative_part(path);

	if (rel[0] == '/')
		rel++;
	return safe_strdup(rel);
}

// Returns TRUE if the path is a root level 'autorun.inf' file
// (replaces upstream's safe_strcmp(&psz_sanpath[3], autorun_name))
static BOOL is_autorun_file(const char* path)
{
	const char* rel = iso_relative_part(path);

	return ((rel[0] == '/') && (safe_strcmp(&rel[1], autorun_name) == 0));
}

/*
 * Replace all 'c' characters in string 'src' with the substring 'rep'.
 * The returned string is allocated and must be freed by the caller.
 * NB: Port of parser.c replace_char().
 */
static char* iso_replace_char(const char* src, const char c, const char* rep)
{
	size_t i, j, k, count = 0, str_len = safe_strlen(src), rep_len = safe_strlen(rep);
	char* res;

	if ((src == NULL) || (rep == NULL))
		return NULL;
	for (i = 0; i < str_len; i++) {
		if (src[i] == c)
			count++;
	}
	res = (char*)malloc(str_len + count * rep_len + 1);
	if (res == NULL)
		return NULL;
	for (i = 0, j = 0; i < str_len; i++) {
		if (src[i] == c) {
			for (k = 0; k < rep_len; k++)
				res[j++] = rep[k];
		} else {
			res[j++] = src[i];
		}
	}
	res[j] = 0;
	return res;
}

/*
 * Sanitize an ISO volume label or GRUB version, so that we can use it for bootloader lookup.
 * Note that this call modifies the string passed as parameter.
 * NB: Port of parser.c sanitize_label().
 */
static int iso_sanitize_label(char* label)
{
	// Notice: Do not add "-beta" to this list as we have existing GRUB lookups for
	// "grub-2.02-beta2" and stuff...
	static const char* remove[] = { "-i386", "-i686", "-amd64", "-x86-64", ".x86-64",
		"-x64", "-armhf", "-arm64", "-aarch64", "-32-bit", "-64-bit", "-32bit", "-64bit",
		"-intel", "-cd", "-dvd", "-standard", "-live", "-install", "-server", "-net",
		"-desktop", "-lts", "-studio", "-baseos", "-kde", "-xfce", "-lxde", "-gnome",
		"-mate", "-unstable", "-debug", "-release", "-final", "-stream", "-cinnamon",
		"-cinn", "-leap", "-tumbleweed", "-budgie", "-ws", "-iot", "-ostree", ".iso"
	};
	size_t i, len;
	char* s;

	len = strlen(label);
	for (i = 0; i < len; i++) {
		char c = label[i];
		// Convert to lowercase
		if (c >= 'A' && c <= 'Z')
			c += 0x20;
		// Convert non alphanum (except '.') to dash
		if ((c < '0' && c != '.') || (c > '9' && c < 'a') || (c > 'z'))
			c = '-';
		label[i] = c;
	}

	// Remove all leading '-'
	for (i = 0; i < len && label[i] == '-'; i++);
	if (i != 0)
		memmove(label, &label[i], len - i + 1);
	len = strlen(label);
	if (len <= 1)
		return -1;

	// Remove all trailing '-'
	for (i = len - 1; i > 0 && label[i] == '-'; i--)
		label[i] = 0;
	len = strlen(label);
	if (len <= 1)
		return -1;

	// Remove all duplicate '-' (non-optimized!)
	for (i = 0; len >= 2 && i < len - 2; i++) {
		if (label[i] == '-' && label[i + 1] == '-') {
			memmove(&label[i + 1], &label[i + 2], len - i - 1);
			len--;
			i--;
		}
	}

	// Remove specific substrings
	for (i = 0; i < ARRAYSIZE(remove); i++) {
		s = strstr(label, remove[i]);
		if (s != NULL)
			memmove(s, &s[strlen(remove[i])], strlen(&s[strlen(remove[i])]) + 1);
	}

	return 0;
}

/*
 * Return the Syslinux version from an isolinux.bin/ldlinux.sys buffer.
 * NB: Port of syslinux.c GetSyslinuxVersion(), which the Linux port does not
 * provide (rfus.h only declares a differently prototyped GetSyslinuxVersion()).
 */
static uint16_t iso_parse_isolinux_version(uint8_t* buf, size_t buf_size, char** ext)
{
	size_t i, j, k;
	char *p = NULL;
	unsigned long version_ul[2];
	uint16_t version = 0;
	const char LINUX[] = { 'L', 'I', 'N', 'U', 'X', ' ' };
	static char* nullstr = "";
	char unauthorized[] = { '<', '>', ':', '|', '*', '?', '\\', '/' };

	*ext = nullstr;
	if (buf_size < 256)
		return 0;

	// Start at 64 to avoid the short incomplete version at the beginning of ldlinux.sys
	for (i = 64; i < buf_size - 64; i++) {
		if (memcmp(&buf[i], LINUX, sizeof(LINUX)) == 0) {
			// Check for ISO or SYS prefix
			if (!(((buf[i - 3] == 'I') && (buf[i - 2] == 'S') && (buf[i - 1] == 'O'))
				|| ((buf[i - 3] == 'S') && (buf[i - 2] == 'Y') && (buf[i - 1] == 'S'))))
				continue;
			i += sizeof(LINUX);
			version_ul[0] = strtoul((char*)&buf[i], &p, 10);
			// Our buffer is either from our internal legit syslinux (i.e. with a NUL terminated
			// version string) or from a buffer that has been NUL-terminated through read_file(),
			// so the string we work with in p is always NUL terminated at this stage.
			assert(version_ul[0] < 256);
			if (version_ul[0] < 256) {
				version_ul[1] = strtoul(&p[1], &p, 10);
				assert(version_ul[1] < 256);
				if (version_ul[1] < 256)
					version = (uint16_t)((version_ul[0] << 8) + version_ul[1]);
			}
			if (version == 0)
				continue;
			// Ensure that our extra version string starts with a slash
			*p = '/';
			// Remove the x.yz- duplicate if present
			for (j = 0; (buf[i + j] == p[1 + j]) && (buf[i + j] != ' '); j++);
			if (p[j + 1] == '-')
				j++;
			if (j >= 4) {
				p[j] = '/';
				p = &p[j];
			}
			for (j = safe_strlen(p) - 1; j > 0; j--) {
				// Arch Linux affixes a star for their version - who knows what else is out there...
				if ((p[j] == ' ') || (p[j] == '*'))
					p[j] = 0;
				else
					break;
			}
			// Sanitize the string
			for (j = 1; j < safe_strlen(p); j++) {
				// Some people are bound to have invalid chars in their date strings
				for (k = 0; k < sizeof(unauthorized); k++) {
					if (p[j] == unauthorized[k])
						p[j] = '_';
				}
			}
			// If all we have is a slash, return the empty string for the extra version
			*ext = (p[1] == 0) ? nullstr : p;
			return version;
		}
	}
	return 0;
}

// Find a substring in a string, but do not search beyond 'len' characters
static char* iso_strnstr(const char* str, const char* substr, size_t len)
{
	const char* p;
	size_t substr_len = strlen(substr);

	if ((str == NULL) || (substr_len == 0))
		return NULL;
	if (len > strlen(str))
		len = strlen(str);
	if (len < substr_len)
		return NULL;
	for (p = str; (size_t)(p - str) <= len - substr_len; p++) {
		if ((p[0] == substr[0]) && (memcmp(p, substr, substr_len) == 0))
			return (char*)p;
	}
	return NULL;
}

// Append a fragment to a dynamically grown string
static BOOL iso_str_append(char** str, size_t* str_len, size_t* capacity, const char* append, size_t append_len)
{
	size_t new_len = *str_len + append_len;

	if (new_len + 1 > *capacity) {
		size_t new_capacity = (*capacity == 0) ? 256 : *capacity;
		char* new_str;
		while (new_capacity < new_len + 1)
			new_capacity *= 2;
		new_str = (char*)realloc(*str, new_capacity);
		if (new_str == NULL)
			return FALSE;
		*str = new_str;
		*capacity = new_capacity;
	}
	memcpy(&(*str)[*str_len], append, append_len);
	*str_len = new_len;
	(*str)[*str_len] = 0;
	return TRUE;
}

// Convert a UTF-16LE buffer (without BOM) to an allocated UTF-8 string
static char* iso_utf16le_to_utf8(const uint8_t* buf, size_t buf_size)
{
	size_t i, out_len = 0;
	char* out = (char*)calloc(buf_size * 3 + 1, 1);

	if (out == NULL)
		return NULL;
	for (i = 0; i + 1 < buf_size; i += 2) {
		uint32_t wc = buf[i] | ((uint32_t)buf[i + 1] << 8);
		if ((wc >= 0xd800) && (wc <= 0xdbff) && (i + 3 < buf_size)) {
			uint32_t wc2 = buf[i + 2] | ((uint32_t)buf[i + 3] << 8);
			if ((wc2 >= 0xdc00) && (wc2 <= 0xdfff)) {
				wc = 0x10000 + ((wc - 0xd800) << 10) + (wc2 - 0xdc00);
				i += 2;
			}
		}
		if (wc < 0x80) {
			out[out_len++] = (char)wc;
		} else if (wc < 0x800) {
			out[out_len++] = (char)(0xc0 | (wc >> 6));
			out[out_len++] = (char)(0x80 | (wc & 0x3f));
		} else if (wc < 0x10000) {
			out[out_len++] = (char)(0xe0 | (wc >> 12));
			out[out_len++] = (char)(0x80 | ((wc >> 6) & 0x3f));
			out[out_len++] = (char)(0x80 | (wc & 0x3f));
		} else {
			out[out_len++] = (char)(0xf0 | (wc >> 18));
			out[out_len++] = (char)(0x80 | ((wc >> 12) & 0x3f));
			out[out_len++] = (char)(0x80 | ((wc >> 6) & 0x3f));
			out[out_len++] = (char)(0x80 | (wc & 0x3f));
		}
	}
	return out;
}

// Convert an UTF-8 string to an allocated UTF-16LE buffer prefixed with a BOM
static uint8_t* iso_utf8_to_utf16le(const char* str, size_t* out_size)
{
	size_t i, len = safe_strlen(str), unit_count = 1;
	uint8_t* out;

	*out_size = 0;
	// Worst case: 1 UTF-16 unit for each byte + BOM
	out = (uint8_t*)calloc(len * 2 + 4, 1);
	if (out == NULL)
		return NULL;
	// UTF-16LE BOM
	out[0] = 0xff;
	out[1] = 0xfe;
	for (i = 0; i < len; ) {
		uint32_t wc;
		uint8_t c = (uint8_t)str[i];
		if (c < 0x80) {
			wc = c;
			i += 1;
		} else if (((c & 0xe0) == 0xc0) && (i + 1 < len)) {
			wc = ((uint32_t)(c & 0x1f) << 6) | ((uint8_t)str[i + 1] & 0x3f);
			i += 2;
		} else if (((c & 0xf0) == 0xe0) && (i + 2 < len)) {
			wc = ((uint32_t)(c & 0x0f) << 12) | (((uint8_t)str[i + 1] & 0x3f) << 6) | ((uint8_t)str[i + 2] & 0x3f);
			i += 3;
		} else if (((c & 0xf8) == 0xf0) && (i + 3 < len)) {
			wc = ((uint32_t)(c & 0x07) << 18) | (((uint8_t)str[i + 1] & 0x3f) << 12) |
				(((uint8_t)str[i + 2] & 0x3f) << 6) | ((uint8_t)str[i + 3] & 0x3f);
			i += 4;
		} else {
			// Invalid sequence => output the replacement character
			wc = 0xfffd;
			i += 1;
		}
		if (wc < 0x10000) {
			out[2 * unit_count] = (uint8_t)wc;
			out[2 * unit_count + 1] = (uint8_t)(wc >> 8);
			unit_count++;
		} else {
			wc -= 0x10000;
			out[2 * unit_count] = (uint8_t)(0xd800 + (wc >> 10));
			out[2 * unit_count + 1] = (uint8_t)((0xd800 + (wc >> 10)) >> 8);
			unit_count++;
			out[2 * unit_count] = (uint8_t)(0xdc00 + (wc & 0x3ff));
			out[2 * unit_count + 1] = (uint8_t)((0xdc00 + (wc & 0x3ff)) >> 8);
			unit_count++;
		}
	}
	*out_size = 2 * unit_count;
	return out;
}

/*
 * Search for a specific 'src' substring data for all occurrences of 'token', and replace
 * it with 'rep'. File can be ANSI or UNICODE and is overwritten. Parameters are UTF-8.
 * The parsed line is of the form: [ ]token[ ]data
 * Returns a pointer to rep if replacement occurred, NULL otherwise
 * NB: Linux port of parser.c replace_in_token_data(). Since POSIX has no
 * "ccs=UNICODE" text mode, we decode UTF-16LE files (which upstream reads as
 * wide characters) into UTF-8, patch the lines and re-encode them on write.
 * Also, as POSIX has no text mode either, line endings are left alone unless
 * dos2unix is set, in which case all CRs are removed (upstream relies on the
 * Windows text mode conversion and its CR removal pass for that).
 */
#define MAX_OCCURRENCES 4
char* replace_in_token_data(const char* filename, const char* token, const char* src, const char* rep, BOOL dos2unix)
{
	char *data = NULL, *out = NULL, *ret = NULL, *line, *nl;
	size_t i, j, size, out_len = 0, out_cap = 0, line_len, token_len, src_len;
	int mode = 0;

	if ((filename == NULL) || (token == NULL) || (src == NULL) || (rep == NULL))
		return NULL;
	if ((filename[0] == 0) || (token[0] == 0) || (src[0] == 0))
		return NULL;
	if (strcmp(src, rep) == 0)	// No need for processing is source is same as replacement
		return NULL;

	size = read_file(filename, (uint8_t**)&data);
	if (size == 0)
		return NULL;

	// Check the input file's encoding from its BOM
	if ((size >= 2) && ((uint8_t)data[0] == 0xff) && ((uint8_t)data[1] == 0xfe)) {
		mode = 2;	// UTF-16 (LE)
		nl = iso_utf16le_to_utf8((uint8_t*)&data[2], size - 2);
		safe_free(data);
		data = nl;
		if (data == NULL)
			return NULL;
	} else if ((size >= 3) && ((uint8_t)data[0] == 0xef) && ((uint8_t)data[1] == 0xbb) && ((uint8_t)data[2] == 0xbf)) {
		mode = 1;	// UTF-8 (with BOM)
		memmove(data, &data[3], size - 3 + 1);
		size -= 3;
	}

	// Process individual lines
	token_len = strlen(token);
	src_len = strlen(src);
	nl = data;
	while ((nl != NULL) && (*nl != 0)) {
		line = nl;
		nl = strchr(line, '\n');
		line_len = (nl != NULL) ? (size_t)(nl - line + 1) : strlen(line);
		if (nl != NULL)
			nl++;	// Skip the newline for the next line

		// Skip leading spaces
		i = strspn(line, " \t");

		// Our token should begin a line
		if ((i + token_len > line_len) || (strncasecmp(&line[i], token, token_len) != 0)) {
			if (!iso_str_append(&out, &out_len, &out_cap, line, line_len))
				goto out;
			continue;
		}

		// Token was found, move past token
		i += token_len;

		// Skip whitespaces after token (while making sure there's at least one)
		j = strspn(&line[i], " \t");
		if (j == 0) {
			if (!iso_str_append(&out, &out_len, &out_cap, line, line_len))
				goto out;
			continue;
		}
		i += j;

		// Locate the fragments with the replaceable string
		{
			size_t p[MAX_OCCURRENCES + 1], next[MAX_OCCURRENCES + 1], nb_rep = 0, fpos = i;
			char* found;

			while ((nb_rep < MAX_OCCURRENCES) &&
				((found = iso_strnstr(&line[fpos], src, line_len - fpos)) != NULL)) {
				p[nb_rep] = (size_t)(found - line);
				fpos = p[nb_rep] + src_len;
				next[nb_rep] = fpos;
				nb_rep++;
			}

			// No replaceable string found => output as is
			if (nb_rep == 0) {
				if (!iso_str_append(&out, &out_len, &out_cap, line, line_len))
					goto out;
				continue;
			}

			// Output all the truncated fragments + the replaced strings
			fpos = 0;
			for (j = 0; j < nb_rep; j++) {
				if (!iso_str_append(&out, &out_len, &out_cap, &line[fpos], p[j] - fpos) ||
					!iso_str_append(&out, &out_len, &out_cap, rep, strlen(rep)))
					goto out;
				fpos = next[j];
			}

			// Output the last fragment
			if (!iso_str_append(&out, &out_len, &out_cap, &line[fpos], line_len - fpos))
				goto out;
			ret = (char*)rep;
		}
	}

	// If a replacement occurred, overwrite the original file
	if (ret != NULL) {
		if (dos2unix) {
			// Remove all CRs, like upstream does when copying its temporary file back
			for (i = 0, j = 0; i < out_len; i++) {
				if (out[i] != '\r')
					out[j++] = out[i];
			}
			out_len = j;
		}
		if (mode == 2) {
			uint8_t* enc;
			size_t enc_size;
			enc = iso_utf8_to_utf16le(out, &enc_size);
			if ((enc == NULL) || (write_file(filename, enc, (uint32_t)enc_size) != (uint32_t)enc_size)) {
				uprintf("Could not write '%s' - original file has been left unmodified.\n", filename);
				ret = NULL;
			}
			safe_free(enc);
		} else if (mode == 1) {
			// Re-add the UTF-8 BOM
			const char bom[] = { (char)0xef, (char)0xbb, (char)0xbf };
			char* buf = NULL;
			size_t buf_len = 0, buf_cap = 0;
			if ((!iso_str_append(&buf, &buf_len, &buf_cap, bom, sizeof(bom))) ||
				(!iso_str_append(&buf, &buf_len, &buf_cap, out, out_len)) ||
				(write_file(filename, (const uint8_t*)buf, (uint32_t)buf_len) != (uint32_t)buf_len)) {
				uprintf("Could not write '%s' - original file has been left unmodified.\n", filename);
				ret = NULL;
			}
			safe_free(buf);
		} else {
			if (write_file(filename, (const uint8_t*)out, (uint32_t)out_len) != (uint32_t)out_len) {
				uprintf("Could not write '%s' - original file has been left unmodified.\n", filename);
				ret = NULL;
			}
		}
	}

out:
	safe_free(data);
	safe_free(out);
	return ret;
}

// Ensure filenames do not contain invalid FAT32 or NTFS characters
static __inline char* sanitize_filename(char* filename, BOOL* is_identical)
{
	size_t i, j;
	char* ret = NULL;
	char unauthorized[] = { '*', '?', '<', '>', ':', '|' };

	*is_identical = TRUE;
	ret = safe_strdup(filename);
	if (ret == NULL) {
		uprintf("Could not allocate string for sanitized path");
		return NULL;
	}

	// NB: Windows must start after the drive part (D:\...) so as not to
	// eliminate the first column. On Linux there is no drive part, so we
	// start at the first character instead.
	for (i = 1; i<safe_strlen(ret); i++) {
		for (j = 0; j<sizeof(unauthorized); j++) {
			if (ret[i] == unauthorized[j]) {
				ret[i] = '_';
				*is_identical = FALSE;
			}
		}
	}
	return ret;
}

static void log_handler (cdio_log_level_t level, const char *message)
{
	uprintf("libcdio: %s", message);
}

// Returns TRUE if a path appears in md5sum.txt
static BOOL is_in_md5sum(char* path)
{
	BOOL found = FALSE;
	char c, * p, * pos = md5sum_pos, * nul_pos, * search, * rel_path;
	size_t search_len;

	// If we are creating the md5sum file from scratch, every file is in it.
	if (fd_md5sum != NULL)
		return TRUE;

	// If we don't have an existing file at this stage, then no file is in it.
	if (md5sum_size == 0 || md5sum_data == NULL)
		return FALSE;

	// NB: Unlike Windows, we have no "X:/xyz" drive letter path: the ISO
	// relative path is what md5sum.txt entries are relative to.
	rel_path = iso_relative_path(path);
	if (rel_path == NULL)
		return FALSE;

	// Modify the path to have " ./xyz"
	search_len = strlen(rel_path) + 3;
	search = (char*)calloc(search_len + 1, 1);
	if (search == NULL) {
		safe_free(rel_path);
		return FALSE;
	}
	snprintf(search, search_len + 1, " ./%s", rel_path);
	safe_free(rel_path);

	// Search for the string in the remainder of the md5sum.txt
	// NB: md5sum_data is always NUL terminated.
	p = strstr(pos, search);
	// Cater for the case where we matched a partial string and look for the full one
	while (p != NULL && p[strlen(search)] != '\n' && p[strlen(search)] != '\r' && p[strlen(search)] != '\0') {
		pos = p + strlen(search);
		p = strstr(pos, search);
	}
	found = (p != NULL);
	// If not found in remainder and we have a remainder, loop to search from beginning
	if (!found && pos != md5sum_data) {
		nul_pos = pos;
		c = *nul_pos;
		*nul_pos = 0;
		p = strstr(md5sum_data, search);
		while (p != NULL && p[strlen(search)] != '\n' && p[strlen(search)] != '\r' && p[strlen(search)] != '\0') {
			pos = p + strlen(search);
			p = strstr(pos, search);
		}
		*nul_pos = c;
		found = (p != NULL);
	}

	if (found)
		md5sum_pos = p + strlen(search);
	safe_free(search);
	return found;
}

static void _print_extracted_file(char* psz_fullpath, uint64_t file_length, BOOL split)
{
	size_t nul_pos;

	if (psz_fullpath == NULL)
		return;
	// NB: Linux displays POSIX separators, so unlike upstream there is no
	// to_windows_path() conversion here; just append the size to the path.
	nul_pos = strlen(psz_fullpath);
	safe_sprintf(&psz_fullpath[nul_pos], 24, " (%s)", SizeToHumanReadable(file_length, TRUE, FALSE));
	uprintf(split ? "Splitting: %s" : "Extracting: %s", psz_fullpath);
	safe_sprintf(&psz_fullpath[nul_pos], 24, " (%s)", SizeToHumanReadable(file_length, FALSE, FALSE));
	/* Upstream shows this in the status bar: there is no GUI here, so it is
	 * rendered in the progress area (and does not add a second line) */
	UpdateProgressStatus(OP_FILE_COPY, psz_fullpath);
	// Remove the appended size for extraction
	psz_fullpath[nul_pos] = 0;
	// Update md5sum_totalbytes as needed
	if (is_in_md5sum(psz_fullpath))
		md5sum_totalbytes += file_length;
}
#define print_extracted_file(p, l) _print_extracted_file(p, l, FALSE)
#define print_split_file(p, l) _print_extracted_file(p, l, TRUE)

/*
 * Scan and set ISO properties
 * Returns true if the the current file does not need to be processed further
 */
static BOOL check_iso_props(const char* psz_dirname, int64_t file_length, const char* psz_basename,
	const char* psz_fullpath, EXTRACT_PROPS *props)
{
	size_t i, j, k, len;
	char bootloader_name[32];

	// Check for an isolinux/syslinux config file anywhere
	memset(props, 0, sizeof(EXTRACT_PROPS));
	for (i = 0; i < ARRAYSIZE(syslinux_cfg); i++) {
		if (safe_stricmp(psz_basename, syslinux_cfg[i]) == 0) {
			props->is_cfg = TRUE;	// Required for "extlinux.conf"
			props->is_syslinux_cfg = TRUE;
			// Maintain a list of all the isolinux/syslinux config files identified so far
			if ((scan_only) && (i < 3))
				StrArrayAdd(&config_path, psz_fullpath, TRUE);
			if ((scan_only) && (i == 1) && (safe_stricmp(psz_dirname, efi_dirname) == 0))
				img_report.has_efi_syslinux = TRUE;
		}
	}

	// Check for archiso loader/entries/*.conf files
	if (safe_stricmp(psz_dirname, "/loader/entries") == 0) {
		len = safe_strlen(psz_basename);
		props->is_conf = ((len > 4) && (stricmp(&psz_basename[len - 5], ".conf") == 0));
	}

	// Check for an old incompatible c32 file anywhere
	for (i = 0; i < NB_OLD_C32; i++) {
		if ((safe_stricmp(psz_basename, old_c32_name[i]) == 0) && (file_length <= old_c32_threshold[i]))
			props->is_old_c32[i] = TRUE;
	}

	if (!scan_only) {	// Write-time checks
		// Check for config files that may need patching
		len = safe_strlen(psz_basename);
		if ((len >= 4) && safe_stricmp(&psz_basename[len - 4], ".cfg") == 0) {
			props->is_cfg = TRUE;
			for (i = 0; i < ARRAYSIZE(grub_cfg); i++) {
				if (safe_stricmp(psz_basename, grub_cfg[i]) == 0)
					props->is_grub_cfg = TRUE;
			}
			if (safe_stricmp(psz_basename, menu_cfg) == 0) {
				props->is_menu_cfg = TRUE;
			}
		}

		// In case there's an ldlinux.sys on the ISO, prevent it from overwriting ours
		if ((psz_dirname != NULL) && (psz_dirname[0] == 0) && (safe_stricmp(psz_basename, ldlinux_name) == 0)) {
			uprintf("Skipping '%s' file from ISO image", psz_basename);
			return TRUE;
		}

		// Split a >4GB install.wim if the target filesystem is FAT
		if (file_length >= 4 * GB && psz_dirname != NULL && IS_FAT(fs_type) && img_report.has_4GB_file == 0x11) {
			if (safe_stricmp(&psz_dirname[max(0, ((int)safe_strlen(psz_dirname)) -
				((int)strlen(sources_str)))], sources_str) == 0) {
				char wim_path[4 * MAX_PATH];
				for (i = 0; i < ARRAYSIZE(wininst_name) - 1; i++) {
					if (safe_stricmp(psz_basename, wininst_name[i]) == 0 && file_length >= 4 * GB) {
						print_split_file((char*)psz_fullpath, file_length);
						char* dst = safe_strdup(psz_fullpath);
						dst[strlen(dst) - 3] = 's';
						dst[strlen(dst) - 2] = 'w';
						dst[strlen(dst) - 1] = 'm';
						assert(safe_strlen(image_path) + safe_strlen(psz_dirname) + safe_strlen(psz_basename) + 2 < ARRAYSIZE(wim_path));
						static_sprintf(wim_path, "%s|%s/%s", image_path, psz_dirname, psz_basename);
						WimSplitFile(wim_path, dst);
						free(dst);
						return TRUE;
					}
				}
			}
		}
	} else {	// Scan-time checks
		// Check for GRUB artifacts
		for (i = 0; i < ARRAYSIZE(grub_dirname); i++) {
			if (safe_stricmp(psz_dirname, grub_dirname[i]) == 0)
				img_report.has_grub2 = (uint8_t)i + 1;
		}

		// Check for a syslinux v5.0+ file anywhere
		if (safe_stricmp(psz_basename, ldlinux_c32) == 0) {
			has_ldlinux_c32 = TRUE;
		}

		// Check for a '/casper#####' directory (non-empty)
		if (safe_strnicmp(psz_dirname, casper_dirname, strlen(casper_dirname)) == 0) {
			img_report.uses_casper = TRUE;
			if (safe_strstr(psz_dirname, pop_os_name) != NULL)
				img_report.disable_iso = TRUE;
		}

		// Check for a '/proxmox' directory
		if (safe_stricmp(psz_dirname, proxmox_dirname) == 0) {
			img_report.disable_iso = TRUE;
		}

		// Check for various files and directories in root (psz_dirname = "")
		if ((psz_dirname != NULL) && (psz_dirname[0] == 0)) {
			if (safe_stricmp(psz_basename, bootmgr_name) == 0) {
				img_report.has_bootmgr = TRUE;
			}
			if (safe_stricmp(psz_basename, bootmgr_efi_name) == 0) {
				// We may extract the bootloaders for revocation validation later but
				// to do so, since we're working with case sensitive file systems, we
				// must store all found UEFI bootloader paths with the right case.
				for (j = 0; j < ARRAYSIZE(img_report.efi_boot_entry); j++) {
					if (img_report.efi_boot_entry[j].path[0] == 0) {
						img_report.efi_boot_entry[j].type = EBT_BOOTMGR;
						static_strcpy(img_report.efi_boot_entry[j].path, psz_fullpath);
						break;
					}
				}
				img_report.has_efi |= 1;
				img_report.has_bootmgr_efi = TRUE;
			}
			if (safe_stricmp(psz_basename, grldr_name) == 0) {
				img_report.has_grub4dos = TRUE;
			}
			if (safe_stricmp(psz_basename, kolibri_name) == 0) {
				img_report.has_kolibrios = TRUE;
			}
			if (safe_stricmp(psz_basename, manjaro_marker) == 0) {
				img_report.disable_iso = TRUE;
			}
			for (i = 0; i < ARRAYSIZE(md5sum_name); i++) {
				if (safe_stricmp(psz_basename, md5sum_name[i]) == 0)
					img_report.has_md5sum = (uint8_t)(i + 1);
			}
		}

		// Check for ReactOS presence anywhere
		if (img_report.reactos_path[0] == 0) {
			for (i = 0; i < ARRAYSIZE(reactos_name); i++)
				if (safe_stricmp(psz_basename, reactos_name[i]) == 0)
					static_strcpy(img_report.reactos_path, psz_fullpath);
		}

		// Check for the first 'efi*.img' we can find (that hopefully contains EFI boot files)
		if (!HAS_EFI_IMG(img_report) && (safe_strlen(psz_basename) >= 7) &&
			(safe_strnicmp(psz_basename, "efi", 3) == 0) &&
			(safe_stricmp(&psz_basename[strlen(psz_basename) - 4], ".img") == 0))
			static_strcpy(img_report.efi_img_path, psz_fullpath);

		// Special case for Lenovo UEFI firmware update ISOs, that use emulated El-Torito HDD images
		if (!HAS_EFI_IMG(img_report) && stricmp(psz_fullpath, "/[BOOT]/0-Boot-HardDisk.img") == 0)
			static_strcpy(img_report.efi_img_path, psz_fullpath);

		// Check for the EFI boot entries. Note that because of Bazzite maintainers' disregard for end users
		// (evidenced in https://github.com/ublue-os/bazzite/issues/4374) and Fedora's disregards for standards
		// (evidenced in pushing for '/efi/fedora/' to store bootloaders, instead of sticking to '/efi/boot/')
		// we check for anything starting with '/efi/' instead of just '/efi/boot/').
		if (safe_strnicmp(psz_dirname, "/efi/", 5) == 0) {
			for (k = 0; k < ARRAYSIZE(efi_bootname); k++) {
				for (i = 0; i < ARRAYSIZE(efi_archname); i++) {
					static_sprintf(bootloader_name, "%s%s.efi", efi_bootname[k], efi_archname[i]);
					if (safe_stricmp(psz_basename, bootloader_name) == 0) {
						if (k == 0)
							img_report.has_efi |= (2 << i);	// start at 2 since "bootmgr.efi" is bit 0
						for (j = 0; j < ARRAYSIZE(img_report.efi_boot_entry); j++) {
							if (img_report.efi_boot_entry[j].path[0] == 0) {
								img_report.efi_boot_entry[j].type = (uint8_t)k;
								static_strcpy(img_report.efi_boot_entry[j].path, psz_fullpath);
								break;
							}
						}
					}
				}
			}
			// Linux Mint Edge 21.2/Mint 21.3 have an invalid /EFI/boot/bootx64.efi
			// because it's a symbolic link to a file that does not exist on the media.
			// This is originally due to a Debian bug that was fixed in:
			// https://salsa.debian.org/live-team/live-build/-/commit/5bff71fea2dd54adcd6c428d3f1981734079a2f7
			// Because of this, if we detect a small bootx64.efi file, we assert that it's a
			// broken link and try to extract a "good" version from the El-Torito image.
			if ((safe_stricmp(psz_basename, "bootx64.efi") == 0) && (file_length < 256)) {
				img_report.has_efi |= 0x4000;
				static_strcpy(img_report.efi_img_path, "[BOOT]/1-Boot-NoEmul.img");
			}
		}

		if (psz_dirname != NULL) {
			if (safe_stricmp(&psz_dirname[max(0, ((int)safe_strlen(psz_dirname)) -
				((int)strlen(sources_str)))], sources_str) == 0) {
				// Check for "install.###" in "###/sources/"
				for (i = 0; i < ARRAYSIZE(wininst_name); i++) {
					if (safe_stricmp(psz_basename, wininst_name[i]) == 0) {
						if (img_report.wininst_index < MAX_WININST) {
							// NB: Unlike upstream, we store the ISO-relative POSIX
							// path (no "?:" prefix), as expected by the ported code
							char* rel_path = iso_relative_path(psz_fullpath);
							if (rel_path != NULL) {
								static_strcpy(img_report.wininst_path[img_report.wininst_index], rel_path);
								safe_free(rel_path);
								img_report.wininst_index++;
								if (file_length >= 4 * GB)
									img_report.has_4GB_file |= (0x10 << i);
							}
						}
					}
				}
			}
		}

		// Check for "\sources\\$OEM$\\$$\\Panther\\unattend.xml"
		if ((safe_stricmp(psz_dirname, "/sources/$OEM$/$$/Panther") == 0) &&
			(safe_stricmp(psz_basename, "unattend.xml") == 0))
			img_report.has_panther_unattend = TRUE;

		// Check for PE (XP) specific files in "/i386", "/amd64" or "/minint"
		for (i = 0; i < ARRAYSIZE(pe_dirname); i++)
			if (safe_stricmp(psz_dirname, pe_dirname[i]) == 0)
				for (j=0; j<ARRAYSIZE(pe_file); j++)
					if (safe_stricmp(psz_basename, pe_file[j]) == 0)
						img_report.winpe |= (1<<j)<<(ARRAYSIZE(pe_dirname)*i);

		for (i = 0; i < ARRAYSIZE(isolinux_bin); i++) {
			if (safe_stricmp(psz_basename, isolinux_bin[i]) == 0) {
				// Maintain a list of all the isolinux.bin files found
				StrArrayAdd(&isolinux_path, psz_fullpath, TRUE);
			}
		}

		for (i = 0; i < NB_OLD_C32; i++) {
			if (props->is_old_c32[i])
				img_report.has_old_c32[i] = TRUE;
		}
		if (file_length >= 4 * GB && (img_report.has_4GB_file & 0x0f) != 0x0f)
			img_report.has_4GB_file++;
		// Compute projected size needed (NB: ISO_BLOCKSIZE = UDF_BLOCKSIZE)
		if (file_length != 0)
			total_blocks += (file_length + (ISO_BLOCKSIZE - 1)) / ISO_BLOCKSIZE;
		return TRUE;
	}
	return FALSE;
}

// Apply various workarounds to Linux config files
static void fix_config(const char* psz_fullpath, const char* psz_path, const char* psz_basename, EXTRACT_PROPS* props)
{
	BOOL modified = FALSE, patched;
	size_t nul_pos;
	char *iso_label = NULL, *usb_label = NULL, *src, *dst;

	src = safe_strdup(psz_fullpath);
	if (src == NULL)
		return;
	nul_pos = strlen(src);

	// Add persistence to the kernel options
	if ((boot_type == BT_IMAGE) && HAS_PERSISTENCE(img_report) && persistence_size) {
		if ((props->is_grub_cfg) || (props->is_menu_cfg) || (props->is_syslinux_cfg)) {
			if (replace_in_token_data(src, props->is_grub_cfg ? "linux" : "append",
				"file=/cdrom/preseed", "persistent file=/cdrom/preseed", TRUE) != NULL) {
				// Ubuntu & derivatives are assumed to use 'file=/cdrom/preseed/...'
				// or 'layerfs-path=minimal.standard.live.squashfs' (see below)
				// somewhere in their kernel options and use 'persistent' as keyword.
				uprintf("  Added 'persistent' kernel option");
				modified = TRUE;
				// Also remove Ubuntu's "maybe-ubiquity" to avoid splash screen (GRUB only)
				if ((props->is_grub_cfg) && replace_in_token_data(src, "linux",
					"maybe-ubiquity", "", TRUE))
					uprintf("  Removed 'maybe-ubiquity' kernel option");
			} else if (replace_in_token_data(src, props->is_grub_cfg ? "linux" : "append",
				"boot=casper", "boot=casper persistent", TRUE) != NULL) {
				// Linux Mint uses "boot=casper". Oh and we want this replacement to happen BEFORE
				// the "linux /casper/vmlinuz" one, because Mint (Why is it ALWAYS them?) also use
				// "linux /casper/vmlinuz" and "kernel /casper/vmlinuz" in their config, and even
				// do so in a SUPER INCONSISTENT manner in their Syslinux' live.cfg, so we want to
				// make sure we don't have to do extra work to fix their inconsistency.
				uprintf("  Added 'persistent' kernel option");
				modified = TRUE;
			} else if (replace_in_token_data(src, "linux", "/casper/vmlinuz",
				"/casper/vmlinuz persistent", TRUE) != NULL) {
				// Ubuntu 23.04 and 24.04 use GRUB only with the above and don't use "maybe-ubiquity"
				uprintf("  Added 'persistent' kernel option");
				modified = TRUE;
			} else if (replace_in_token_data(src, "kernel", "/casper/vmlinuz",
				"/casper/vmlinuz persistent", TRUE) != NULL) {
				// Some people might use "kernel" in their Syslinux config instead of "linux"
				uprintf("  Added 'persistent' kernel option");
				modified = TRUE;
			} else if (replace_in_token_data(src, props->is_grub_cfg ? "linux" : "append",
				"boot=live", "boot=live persistence", TRUE) != NULL) {
				// Debian & derivatives are assumed to use 'boot=live' in
				// their kernel options and use 'persistence' as keyword.
				uprintf("  Added 'persistence' kernel option");
				modified = TRUE;
			}
			// Other distros can go to hell. Seriously, just check all partitions for
			// an ext volume with the right label and use persistence *THEN*. I mean,
			// why on earth do you need a bloody *NONSTANDARD* kernel option and/or a
			// "persistence.conf" file. This is SO INCREDIBLY RETARDED that it makes
			// Windows look smart in comparison. Great job there, Linux people!
		}
	}

	// Workaround for config files requiring an ISO label for kernel append that may be
	// different from our USB label. Oh, and these labels must have spaces converted to \x20.
	if ((props->is_cfg) || (props->is_conf)) {
		// Older versions of GRUB EFI used "linuxefi", newer just use "linux".
		// Also, in their great wisdom, the openSUSE maintainers added a 'set linux=linux'
		// line to their grub.cfg, which means that their kernel option cfg_token is no longer
		//'linux' but '$linux'... and we have to add a workaround for that.
		// Then, newer Arch and derivatives added an extra "search --label ..." command
		// in their GRUB conf, which we need to cater for in supplement of the kernel line.
		// Then Artix called in and decided they would use a "for kopt ..." loop.
		// Finally, we're just shoving the known isolinux/syslinux tokens in there to process
		// all config files equally.
		static const char* cfg_token[] = { "options", "append", "linux", "linuxefi", "$linux", "search", "for"};
		iso_label = iso_replace_char(img_report.label, ' ', "\\x20");
		usb_label = iso_replace_char(img_report.usb_label, ' ', "\\x20");
		if ((iso_label != NULL) && (usb_label != NULL)) {
			patched = FALSE;
			for (int i = 0; i < ARRAYSIZE(cfg_token); i++) {
				if (replace_in_token_data(src, cfg_token[i], iso_label, usb_label, TRUE) != NULL) {
					modified = TRUE;
					patched = TRUE;
				}
			}
			if (patched)
				uprintf("  Patched %s: '%s' ➔ '%s'", src, iso_label, usb_label);
			// Since version 8.2, and https://github.com/rhinstaller/anaconda/commit/a7661019546ec1d8b0935f9cb0f151015f2e1d95,
			// Red Hat derivatives have changed their CD-ROM detection policy which leads to the installation source
			// not being found. So we need to use 'inst.repo' instead of 'inst.stage2' in the kernel options.
			// *EXCEPT* this should not be done for netinst media such as Fedora 37 netinstall and trying to differentiate
			// netinst from regular is a pain. So, because I don't have all day to fix the mess that Red-Hat created when
			// they introduced a kernel option to decide where the source packages should be picked from we're just going
			// to *hope* that users didn't rename their ISOs and check whether it contains 'netinst' or not. Oh well...
			patched = FALSE;
			if (img_report.rh8_derivative && (strstr(image_path, "netinst") == NULL)) {
				for (int i = 0; i < ARRAYSIZE(cfg_token); i++) {
					if (replace_in_token_data(src, cfg_token[i], "inst.stage2", "inst.repo", TRUE) != NULL) {
						modified = TRUE;
						patched = TRUE;
					}
				}
				if (patched)
					uprintf("  Patched %s: '%s' ➔ '%s'", src, "inst.stage2", "inst.repo");
			}
		}
		safe_free(iso_label);
		safe_free(usb_label);
	}

	// Fix dual BIOS + EFI support for tails and other ISOs
	if ( (props->is_syslinux_cfg) && (safe_stricmp(psz_path, efi_dirname) == 0) &&
		 (safe_stricmp(psz_basename, syslinux_cfg[0]) == 0) &&
		 (!img_report.has_efi_syslinux) && (dst = safe_strdup(src)) ) {
		dst[nul_pos-12] = 's'; dst[nul_pos-11] = 'y'; dst[nul_pos-10] = 's';
		CopyFileA(src, dst, TRUE);
		uprintf("Duplicated %s to %s", src, dst);
		free(dst);
	}

	// Workaround for FreeNAS
	if (props->is_grub_cfg) {
		iso_label = malloc(MAX_PATH);
		usb_label = malloc(MAX_PATH);
		if ((iso_label != NULL) && (usb_label != NULL)) {
			safe_sprintf(iso_label, MAX_PATH, "cd9660:/dev/iso9660/%s", img_report.label);
			safe_sprintf(usb_label, MAX_PATH, "msdosfs:/dev/msdosfs/%s", img_report.usb_label);
			if (replace_in_token_data(src, "set", iso_label, usb_label, TRUE) != NULL) {
				uprintf("  Patched %s: '%s' ➔ '%s'", src, iso_label, usb_label);
				modified = TRUE;
			}
		}
		safe_free(iso_label);
		safe_free(usb_label);
	}

	if (modified)
		StrArrayAdd(&modified_files, psz_fullpath, TRUE);

	free(src);
}

// Convert from time_t to FILETIME
// Uses 3 static entries so that we can convert 3 concurrent values at the same time
static LPFILETIME __inline to_filetime(time_t t)
{
	static int i = 0;
	static FILETIME ft[3], *r;
	LONGLONG ll = (t * 10000000LL) + 116444736000000000LL;

	r = &ft[i];
	r->dwLowDateTime = (DWORD)ll;
	r->dwHighDateTime = (DWORD)(ll >> 32);
	i = (i + 1) % ARRAYSIZE(ft);
	return r;
}

/* ------------------------------------------------------------------ */
/* MD5 (needed to validate or create md5sum.txt files)                 */
/* ------------------------------------------------------------------ */
// TODO(hash): Upstream gets this from src/hash.c (which the Linux port does
// not have yet); the four MD5 routines below are copied verbatim from it.

#define ISO_MD5_BLOCKSIZE 64
#define ISO_MD5_HASHSIZE  16

// Context for the MD5 hash, using the same layout as upstream's HASH_CONTEXT
// (which has to accommodate every algorithm, hence the oversized buf/state)
typedef struct {
	uint8_t buf[ISO_MD5_BLOCKSIZE];
	uint64_t state[8];
	uint64_t bytecount;
} ISO_MD5_CONTEXT;

#ifdef BIG_ENDIAN_HOST
#define iso_md5_read_le32(p) (__builtin_bswap32(*(const uint32_t*)(const void*)(p)))
#define iso_md5_write_le32(p, v) do { *(uint32_t*)(void*)(p) = __builtin_bswap32(v); } while (0)
#endif

static void iso_md5_init(ISO_MD5_CONTEXT *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
}

static void iso_md5_transform(ISO_MD5_CONTEXT *ctx, const uint8_t *data)
{
	uint32_t a, b, c, d, x[16];

	a = (uint32_t)ctx->state[0];
	b = (uint32_t)ctx->state[1];
	c = (uint32_t)ctx->state[2];
	d = (uint32_t)ctx->state[3];

#ifdef BIG_ENDIAN_HOST
	{
		uint32_t k;
		for (k = 0; k < 16; k += 4) {
			const uint8_t *p2 = data + k * 4;
			x[k] = iso_md5_read_le32(p2);
			x[k + 1] = iso_md5_read_le32(p2 + 4);
			x[k + 2] = iso_md5_read_le32(p2 + 8);
			x[k + 3] = iso_md5_read_le32(p2 + 12);
		}
	}
#else
	memcpy(x, data, sizeof(x));
#endif

#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

#define MD5STEP(f, w, x, y, z, data, s) do { \
	( w += f(x, y, z) + data,  w = w<<s | w>>(32-s),  w += x ); } while(0)

	MD5STEP(F1, a, b, c, d, x[0] + 0xd76aa478, 7);
	MD5STEP(F1, d, a, b, c, x[1] + 0xe8c7b756, 12);
	MD5STEP(F1, c, d, a, b, x[2] + 0x242070db, 17);
	MD5STEP(F1, b, c, d, a, x[3] + 0xc1bdceee, 22);
	MD5STEP(F1, a, b, c, d, x[4] + 0xf57c0faf, 7);
	MD5STEP(F1, d, a, b, c, x[5] + 0x4787c62a, 12);
	MD5STEP(F1, c, d, a, b, x[6] + 0xa8304613, 17);
	MD5STEP(F1, b, c, d, a, x[7] + 0xfd469501, 22);
	MD5STEP(F1, a, b, c, d, x[8] + 0x698098d8, 7);
	MD5STEP(F1, d, a, b, c, x[9] + 0x8b44f7af, 12);
	MD5STEP(F1, c, d, a, b, x[10] + 0xffff5bb1, 17);
	MD5STEP(F1, b, c, d, a, x[11] + 0x895cd7be, 22);
	MD5STEP(F1, a, b, c, d, x[12] + 0x6b901122, 7);
	MD5STEP(F1, d, a, b, c, x[13] + 0xfd987193, 12);
	MD5STEP(F1, c, d, a, b, x[14] + 0xa679438e, 17);
	MD5STEP(F1, b, c, d, a, x[15] + 0x49b40821, 22);

	MD5STEP(F2, a, b, c, d, x[1] + 0xf61e2562, 5);
	MD5STEP(F2, d, a, b, c, x[6] + 0xc040b340, 9);
	MD5STEP(F2, c, d, a, b, x[11] + 0x265e5a51, 14);
	MD5STEP(F2, b, c, d, a, x[0] + 0xe9b6c7aa, 20);
	MD5STEP(F2, a, b, c, d, x[5] + 0xd62f105d, 5);
	MD5STEP(F2, d, a, b, c, x[10] + 0x02441453, 9);
	MD5STEP(F2, c, d, a, b, x[15] + 0xd8a1e681, 14);
	MD5STEP(F2, b, c, d, a, x[4] + 0xe7d3fbc8, 20);
	MD5STEP(F2, a, b, c, d, x[9] + 0x21e1cde6, 5);
	MD5STEP(F2, d, a, b, c, x[14] + 0xc33707d6, 9);
	MD5STEP(F2, c, d, a, b, x[3] + 0xf4d50d87, 14);
	MD5STEP(F2, b, c, d, a, x[8] + 0x455a14ed, 20);
	MD5STEP(F2, a, b, c, d, x[13] + 0xa9e3e905, 5);
	MD5STEP(F2, d, a, b, c, x[2] + 0xfcefa3f8, 9);
	MD5STEP(F2, c, d, a, b, x[7] + 0x676f02d9, 14);
	MD5STEP(F2, b, c, d, a, x[12] + 0x8d2a4c8a, 20);

	MD5STEP(F3, a, b, c, d, x[5] + 0xfffa3942, 4);
	MD5STEP(F3, d, a, b, c, x[8] + 0x8771f681, 11);
	MD5STEP(F3, c, d, a, b, x[11] + 0x6d9d6122, 16);
	MD5STEP(F3, b, c, d, a, x[14] + 0xfde5380c, 23);
	MD5STEP(F3, a, b, c, d, x[1] + 0xa4beea44, 4);
	MD5STEP(F3, d, a, b, c, x[4] + 0x4bdecfa9, 11);
	MD5STEP(F3, c, d, a, b, x[7] + 0xf6bb4b60, 16);
	MD5STEP(F3, b, c, d, a, x[10] + 0xbebfbc70, 23);
	MD5STEP(F3, a, b, c, d, x[13] + 0x289b7ec6, 4);
	MD5STEP(F3, d, a, b, c, x[0] + 0xeaa127fa, 11);
	MD5STEP(F3, c, d, a, b, x[3] + 0xd4ef3085, 16);
	MD5STEP(F3, b, c, d, a, x[6] + 0x04881d05, 23);
	MD5STEP(F3, a, b, c, d, x[9] + 0xd9d4d039, 4);
	MD5STEP(F3, d, a, b, c, x[12] + 0xe6db99e5, 11);
	MD5STEP(F3, c, d, a, b, x[15] + 0x1fa27cf8, 16);
	MD5STEP(F3, b, c, d, a, x[2] + 0xc4ac5665, 23);

	MD5STEP(F4, a, b, c, d, x[0] + 0xf4292244, 6);
	MD5STEP(F4, d, a, b, c, x[7] + 0x432aff97, 10);
	MD5STEP(F4, c, d, a, b, x[14] + 0xab9423a7, 15);
	MD5STEP(F4, b, c, d, a, x[5] + 0xfc93a039, 21);
	MD5STEP(F4, a, b, c, d, x[12] + 0x655b59c3, 6);
	MD5STEP(F4, d, a, b, c, x[3] + 0x8f0ccc92, 10);
	MD5STEP(F4, c, d, a, b, x[10] + 0xffeff47d, 15);
	MD5STEP(F4, b, c, d, a, x[1] + 0x85845dd1, 21);
	MD5STEP(F4, a, b, c, d, x[8] + 0x6fa87e4f, 6);
	MD5STEP(F4, d, a, b, c, x[15] + 0xfe2ce6e0, 10);
	MD5STEP(F4, c, d, a, b, x[6] + 0xa3014314, 15);
	MD5STEP(F4, b, c, d, a, x[13] + 0x4e0811a1, 21);
	MD5STEP(F4, a, b, c, d, x[4] + 0xf7537e82, 6);
	MD5STEP(F4, d, a, b, c, x[11] + 0xbd3af235, 10);
	MD5STEP(F4, c, d, a, b, x[2] + 0x2ad7d2bb, 15);
	MD5STEP(F4, b, c, d, a, x[9] + 0xeb86d391, 21);

#undef F1
#undef F2
#undef F3
#undef F4

	/* Update chaining vars */
	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
}

static void iso_md5_write(ISO_MD5_CONTEXT *ctx, const uint8_t *buf, size_t len)
{
	size_t num = ctx->bytecount & (ISO_MD5_BLOCKSIZE - 1);

	/* Update bytecount */
	ctx->bytecount += len;

	/* Handle any leading odd-sized chunks */
	if (num) {
		uint8_t *p = ctx->buf + num;

		num = ISO_MD5_BLOCKSIZE - num;
		if (len < num) {
			memcpy(p, buf, num);
			return;
		}
		memcpy(p, buf, num);
		iso_md5_transform(ctx, ctx->buf);
		buf += num;
		len -= num;
	}

	/* Process data in blocksize chunks */
	while (len >= ISO_MD5_BLOCKSIZE) {
		iso_md5_transform(ctx, buf);
		buf += ISO_MD5_BLOCKSIZE;
		len -= ISO_MD5_BLOCKSIZE;
	}

	/* Handle any remaining bytes of data. */
	memcpy(ctx->buf, buf, len);
}

static void iso_md5_final(ISO_MD5_CONTEXT *ctx)
{
	size_t count = ((size_t)ctx->bytecount) & (ISO_MD5_BLOCKSIZE - 1);
	uint64_t bitcount = ctx->bytecount << 3;
	uint8_t *p;

	/* Set the first char of padding to 0x80.
	 * This is safe since there is always at least one byte free
	 */
	p = ctx->buf + count;
	*p++ = 0x80;

	/* Bytes of padding needed to make blocksize */
	count = (ISO_MD5_BLOCKSIZE - 1) - count;

	/* Pad out to 56 mod 64 */
	if (count < 8) {
		/* Two lots of padding: Pad the first block to blocksize */
		memset(p, 0, count);
		iso_md5_transform(ctx, ctx->buf);

		/* Now fill the next block */
		memset(ctx->buf, 0, ISO_MD5_BLOCKSIZE - 8);
	} else {
		/* Pad block to blocksize */
		memset(p, 0, count - 8);
	}

	/* append the 64 bit count (little endian) */
	ctx->buf[ISO_MD5_BLOCKSIZE - 8] = (uint8_t) bitcount;
	ctx->buf[ISO_MD5_BLOCKSIZE - 7] = (uint8_t) (bitcount >> 8);
	ctx->buf[ISO_MD5_BLOCKSIZE - 6] = (uint8_t) (bitcount >> 16);
	ctx->buf[ISO_MD5_BLOCKSIZE - 5] = (uint8_t) (bitcount >> 24);
	ctx->buf[ISO_MD5_BLOCKSIZE - 4] = (uint8_t) (bitcount >> 32);
	ctx->buf[ISO_MD5_BLOCKSIZE - 3] = (uint8_t) (bitcount >> 40);
	ctx->buf[ISO_MD5_BLOCKSIZE - 2] = (uint8_t) (bitcount >> 48);
	ctx->buf[ISO_MD5_BLOCKSIZE - 1] = (uint8_t) (bitcount >> 56);

	iso_md5_transform(ctx, ctx->buf);

	p = ctx->buf;
#ifdef BIG_ENDIAN_HOST
#define X(a) do { iso_md5_write_le32(p, (uint32_t)ctx->state[a]); p += 4; } while(0);
#else
#define X(a) do { *(uint32_t*)p = (uint32_t)ctx->state[a]; p += 4; } while(0)
#endif
	X(0);
	X(1);
	X(2);
	X(3);
#undef X
}


// TODO(no preallocation): Upstream uses NtCreateFile() with an allocation size
// to preallocate the extracted files; POSIX has fallocate() but we don't need
// it yet, so we just create the file.
static HANDLE create_preallocated_file(const char* lpFileName, DWORD dwDesiredAccess,
	DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
	DWORD dwFlagsAndAttributes, LONGLONG fileSize)
{
	IGNORE_RETVAL(fileSize);
	return CreateFileU(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
		dwCreationDisposition, dwFlagsAndAttributes, NULL);
}

// This version of _mkdirU creates all needed directories along the way
// NB: Port of msapi_utf8.h _mkdirExU(), starting at index 1 so that we don't
// try to create the root directory of absolute paths.
static int mkdir_ex(const char* dirname)
{
	int ret = -1;
	size_t i, len;
	char* path = safe_strdup(dirname);
	char separator;

	if (path == NULL)
		return -1;
	// Remove trailing separators
	len = strlen(path);
	while ((len > 0) && ((path[len - 1] == '\\') || (path[len - 1] == '/')))
		path[--len] = 0;
	// Normalize separators
	for (i = 0; i < len; i++)
		if (path[i] == '\\')
			path[i] = '/';
	// Create all the directories along the way
	for (i = 1; i <= len; i++) {
		if ((i < len) && (path[i] != '/'))
			continue;
		separator = path[i];
		path[i] = 0;
		if ((mkdir(path, 0777) != 0) && (errno != EEXIST) && (errno != EACCES)) {
			path[i] = separator;
			goto out;
		}
		path[i] = separator;
	}
	ret = 0;
out:
	free(path);
	return ret;
}

// Convert a FILETIME to a POSIX timespec (used to restore directory timestamps)
static struct timespec filetime_to_timespec(LPFILETIME ft)
{
	struct timespec ts = { 0, 0 };
	LONGLONG ll;

	if (ft == NULL)
		return ts;
	ll = (((LONGLONG)ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
	ll -= 116444736000000000LL;	// 1601-01-01 to 1970-01-01, in 100 ns units
	if (ll < 0)	// Dates before the epoch cannot be represented: clamp to it
		return ts;
	ts.tv_sec = (time_t)(ll / 10000000LL);
	ts.tv_nsec = (long)((ll % 10000000LL) * 100);
	return ts;
}

// Helper function to restore the timestamp on a directory
// NB: POSIX has no concept of a creation time, so unlike upstream, which uses
// SetFileTime() on a backup-semantics directory handle, only the access and
// modification times can be restored (through utimensat()).
static void __inline set_directory_timestamp(char* path, LPFILETIME creation, LPFILETIME last_access, LPFILETIME modify)
{
	struct timespec times[2];

	times[0] = filetime_to_timespec(last_access);
	times[1] = filetime_to_timespec(modify);
	if (utimensat(AT_FDCWD, path, times, 0) != 0)
		uprintf("  Could not set timestamp for directory '%s': %s", path, strerror(errno));
	IGNORE_RETVAL(creation);
}

// Returns 0 on success, nonzero on error
static int udf_extract_files(udf_t *p_udf, udf_dirent_t *p_udf_dirent, const char *psz_path)
{
	HANDLE file_handle = NULL;
	DWORD buf_size, wr_size, err;
	EXTRACT_PROPS props;
	ISO_MD5_CONTEXT ctx;
	BOOL r, is_identical;
	int length;
	size_t i, j, nb;
	char tmp[128], *psz_fullpath = NULL, *psz_sanpath = NULL, *rel_path = NULL;
	const char* psz_basename;
	udf_dirent_t *p_udf_dirent2;
	_Static_assert(ISO_BUFFER_SIZE % UDF_BLOCKSIZE == 0,
		"ISO_BUFFER_SIZE is not a multiple of UDF_BLOCKSIZE");
	uint8_t* buf = malloc(ISO_BUFFER_SIZE);
	int64_t read, file_length;

	if ((p_udf_dirent == NULL) || (psz_path == NULL) || (buf == NULL)) {
		safe_free(buf);
		return 1;
	}

	if (psz_path[0] == 0)
		UpdateProgressWithInfoInit(NULL, TRUE);
	while ((p_udf_dirent = udf_readdir(p_udf_dirent)) != NULL) {
		if (ErrorStatus) goto out;
		psz_basename = udf_get_filename(p_udf_dirent);
		if (strlen(psz_basename) == 0)
			continue;
		length = (int)(3 + strlen(psz_path) + strlen(psz_basename) + strlen(psz_extract_dir) + 24);
		psz_fullpath = (char*)calloc(sizeof(char), length);
		if (psz_fullpath == NULL) {
			uprintf("Error allocating file name");
			goto out;
		}
		// NB: MSVC's _snprintf_s(..., _TRUNCATE, ...) is not available on
		// Linux: use snprintf() and bail out if the output was truncated
		if (snprintf(psz_fullpath, (size_t)length, "%s%s/%s", psz_extract_dir, psz_path, psz_basename) >= length)
			goto out;
		if (S_ISLNK(udf_get_posix_filemode(p_udf_dirent)))
			img_report.has_symlinks = SYMLINKS_UDF;
		if (udf_is_dir(p_udf_dirent)) {
			if (!scan_only) {
				psz_sanpath = sanitize_filename(psz_fullpath, &is_identical);
				IGNORE_RETVAL(_mkdirU(psz_sanpath));
				if (preserve_timestamps) {
					set_directory_timestamp(psz_sanpath, to_filetime(udf_get_attribute_time(p_udf_dirent)),
						to_filetime(udf_get_access_time(p_udf_dirent)), to_filetime(udf_get_modification_time(p_udf_dirent)));
				}
				safe_free(psz_sanpath);
			}
			p_udf_dirent2 = udf_opendir(p_udf_dirent);
			if (p_udf_dirent2 != NULL) {
				if (udf_extract_files(p_udf, p_udf_dirent2, &psz_fullpath[strlen(psz_extract_dir)]))
					goto out;
			}
		} else {
			file_length = udf_get_file_length(p_udf_dirent);
			if (check_iso_props(psz_path, file_length, psz_basename, psz_fullpath, &props)) {
				safe_free(psz_fullpath);
				continue;
			}
			print_extracted_file(psz_fullpath, file_length);
			for (i = 0; i < NB_OLD_C32; i++) {
				if (props.is_old_c32[i] && use_own_c32[i]) {
					static_sprintf(tmp, "%s/syslinux-%s/%s", FILES_DIR, embedded_sl_version_str[0], old_c32_name[i]);
					if (CopyFileU(tmp, psz_fullpath, FALSE)) {
						uprintf("  Replaced with local version %s", IsFileInDB(tmp)?"✓":"✗");
						break;
					}
					uprintf("  Could not replace file: %s", WindowsErrorString());
				}
			}
			if (i < NB_OLD_C32)
				continue;
			psz_sanpath = sanitize_filename(psz_fullpath, &is_identical);
			if (!is_identical)
				uprintf("  File name sanitized to '%s'", psz_sanpath);
			file_handle = create_preallocated_file(psz_sanpath, GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, file_length);
			if (file_handle == INVALID_HANDLE_VALUE) {
				err = GetLastError();
				uprintf("  Unable to create file: %s", WindowsErrorString());
				if (((err == ERROR_ACCESS_DENIED) || (err == ERROR_INVALID_HANDLE)) &&
					(is_autorun_file(psz_sanpath)))
					uprintf(stupid_antivirus);
				else
					goto out;
			} else {
				if (fd_md5sum != NULL)
					iso_md5_init(&ctx);
				while (file_length > 0) {
					if (ErrorStatus)
						goto out;
					nb = (size_t)MIN(ISO_BUFFER_SIZE / UDF_BLOCKSIZE, (file_length + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE);
					read = udf_read_block(p_udf_dirent, buf, nb);
					if (read < 0) {
						uprintf("  Error reading UDF file %s", &psz_fullpath[strlen(psz_extract_dir)]);
						goto out;
					}
					buf_size = (DWORD)MIN(file_length, read);
					if (fd_md5sum != NULL)
						iso_md5_write(&ctx, buf, buf_size);
					ISO_BLOCKING(r = WriteFileWithRetry(file_handle, buf, buf_size, &wr_size, WRITE_RETRIES));
					if (!r || (wr_size != buf_size)) {
						if (r)
							SetLastError(ERROR_WRITE_FAULT);
						uprintf("  Error writing file: %s", WindowsErrorString());
						goto out;
					}
					file_length -= wr_size;
					nb_blocks += nb;
					if (nb_blocks - last_nb_blocks >= PROGRESS_THRESHOLD) {
						UpdateProgressWithInfo(OP_FILE_COPY, MSG_231, nb_blocks, total_blocks);
						last_nb_blocks = nb_blocks;
					}
				}
				if (fd_md5sum != NULL) {
					iso_md5_final(&ctx);
					for (j = 0; j < ISO_MD5_HASHSIZE; j++)
						fprintf(fd_md5sum, "%02x", ctx.buf[j]);
					// NB: Unlike upstream (which strips the Windows drive part
					// with &psz_fullpath[3]), we print the ISO-relative path
					rel_path = iso_relative_path(psz_fullpath);
					fprintf(fd_md5sum, "  ./%s\n", (rel_path != NULL) ? rel_path : psz_fullpath);
					safe_free(rel_path);
				}
			}
			if ((preserve_timestamps) && (!SetFileTime(file_handle, to_filetime(udf_get_attribute_time(p_udf_dirent)),
				to_filetime(udf_get_access_time(p_udf_dirent)), to_filetime(udf_get_modification_time(p_udf_dirent)))))
				uprintf("  Could not set timestamp: %s", WindowsErrorString());

			// If you have a fast USB 3.0 device, the default Windows buffering does an
			// excellent job at compensating for our small blocks read/writes to max out the
			// device's bandwidth.
			// The drawback however is with cancellation. With a large file, CloseHandle()
			// may take forever to complete and is not interruptible. We try to detect this.
			ISO_BLOCKING(safe_closehandle(file_handle));
			if (props.is_cfg || props.is_conf)
				fix_config(psz_sanpath, psz_path, psz_basename, &props);
			safe_free(psz_sanpath);
		}
		safe_free(psz_fullpath);
	}
	safe_free(buf);
	return 0;

out:
	if (GetLastError() != ERROR_SUCCESS)
		ErrorStatus = RUFUS_ERROR(GetLastError());
	udf_dirent_free(p_udf_dirent);
	ISO_BLOCKING(safe_closehandle(file_handle));
	safe_free(psz_sanpath);
	safe_free(psz_fullpath);
	safe_free(buf);
	return 1;
}

// Returns 0 on success, >0 on error, <0 to ignore current dir
static int iso_extract_files(iso9660_t* p_iso, const char *psz_path)
{
	HANDLE file_handle = NULL;
	DWORD buf_size, wr_size, err;
	EXTRACT_PROPS props;
	ISO_MD5_CONTEXT ctx;
	BOOL is_symlink, is_identical, create_file, free_p_statbuf = FALSE;
	int length, r = 1;
	char psz_fullpath[MAX_PATH], *psz_basename = NULL, *psz_sanpath = NULL, *rel_path = NULL;
	char tmp[128], target_path[256], *last_slash;
	const char *psz_iso_name = &psz_fullpath[strlen(psz_extract_dir)];
	_Static_assert(ISO_BUFFER_SIZE % ISO_BLOCKSIZE == 0,
		"ISO_BUFFER_SIZE is not a multiple of ISO_BLOCKSIZE");
	uint8_t* buf = malloc(ISO_BUFFER_SIZE);
	CdioListNode_t* p_entnode;
	iso9660_stat_t *p_statbuf;
	CdioISO9660FileList_t* p_entlist = NULL;
	size_t i, j, nb;
	lsn_t lsn;
	int64_t file_length;

	if ((p_iso == NULL) || (psz_path == NULL) || (buf == NULL)) {
		safe_free(buf);
		return 1;
	}

	// NB: MSVC's _snprintf_s(..., _TRUNCATE, ...) is not available on Linux:
	// use snprintf() and bail out if the output was truncated
	length = snprintf(psz_fullpath, sizeof(psz_fullpath), "%s%s/", psz_extract_dir, psz_path);
	if ((length < 0) || ((size_t)length >= sizeof(psz_fullpath)))
		goto out;
	psz_basename = &psz_fullpath[length];

	p_entlist = iso9660_ifs_readdir(p_iso, psz_path);
	if (!p_entlist) {
		uprintf("Could not access directory %s", psz_path);
		goto out;
	}

	if (psz_path[0] == 0)
		UpdateProgressWithInfoInit(NULL, TRUE);
	_CDIO_LIST_FOREACH(p_entnode, p_entlist) {
		if (ErrorStatus) goto out;
		p_statbuf = (iso9660_stat_t*) _cdio_list_node_data(p_entnode);
		free_p_statbuf = FALSE;
		if (scan_only && (p_statbuf->rr.b3_rock == yep) && enable_rockridge) {
			if (p_statbuf->rr.u_su_fields & ISO_ROCK_SUF_PL) {
				if (!img_report.has_deep_directories)
					uprintf("  Note: The selected ISO uses Rock Ridge 'deep directories'.\r\n"
						"  Because of this, it may take a very long time to scan or extract...");
				img_report.has_deep_directories = TRUE;
				// Due to the nature of the parsing of Rock Ridge deep directories
				// which requires performing a *very costly* search of the whole
				// ISO9660 file system to find the matching LSN, ISOs with loads of
				// deep directory entries (e.g. OPNsense) are very slow to parse...
				// To speed up the scan process, and since we expect deep directory
				// entries to appear below anything we care for, we cut things
				// short by telling the parent not to bother any further once we
				// find that we are dealing with a deep directory.
				r = -1;
				// Add at least one extra block, since we're skipping content.
				total_blocks++;
				goto out;
			}
		}
		// Eliminate . and .. entries
		if ( (strcmp(p_statbuf->filename, ".") == 0)
			|| (strcmp(p_statbuf->filename, "..") == 0) )
			continue;
		// Rock Ridge requires an exception
		is_symlink = FALSE;
		if ((p_statbuf->rr.b3_rock == yep) && enable_rockridge) {
			safe_strcpy(psz_basename, sizeof(psz_fullpath) - length - 1, p_statbuf->filename);
			if (safe_strlen(p_statbuf->filename) > 64)
				img_report.has_long_filename = TRUE;
			is_symlink = (p_statbuf->rr.psz_symlink != NULL);
			if (is_symlink)
				img_report.has_symlinks = SYMLINKS_RR;
		} else {
			iso9660_name_translate_ext(p_statbuf->filename, psz_basename, joliet_level);
		}
		if (p_statbuf->type == _STAT_DIR) {
			if (!scan_only) {
				psz_sanpath = sanitize_filename(psz_fullpath, &is_identical);
				IGNORE_RETVAL(_mkdirU(psz_sanpath));
				if (preserve_timestamps) {
					LPFILETIME ft = to_filetime(mktime(&p_statbuf->tm));
					set_directory_timestamp(psz_sanpath, ft, ft, ft);
				}
				safe_free(psz_sanpath);
			}
			r = iso_extract_files(p_iso, psz_iso_name);
			if (r > 0)
				goto out;
			if (r < 0)	// Stop processing current dir
				break;
		} else {
			file_length = p_statbuf->total_size;
			if (check_iso_props(psz_path, file_length, psz_basename, psz_fullpath, &props)) {
				if (is_symlink && (file_length == 0)) {
					// Add symlink duplicated files to total_size at scantime
					if ((strcmp(psz_path, "/firmware") == 0)) {
						static_sprintf(target_path, "%s/%s", psz_path, p_statbuf->rr.psz_symlink);
						iso9660_stat_t* p_statbuf2 = iso9660_ifs_stat_translate(p_iso, target_path);
						if (p_statbuf2 != NULL) {
							extra_blocks += (p_statbuf2->total_size + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;
							iso9660_stat_free(p_statbuf2);
						}
					} else if ((strcmp(p_statbuf->filename, "live") == 0) &&
						(strcmp(p_statbuf->rr.psz_symlink, "casper") == 0)) {
						// Mint LMDE requires working symbolic links and therefore requires the use of NTFS
						img_report.needs_ntfs = TRUE;
					}
				}
				continue;
			}
			if (!is_symlink)
				print_extracted_file(psz_fullpath, file_length);
			for (i = 0; i < NB_OLD_C32; i++) {
				if (props.is_old_c32[i] && use_own_c32[i]) {
					static_sprintf(tmp, "%s/syslinux-%s/%s", FILES_DIR, embedded_sl_version_str[0], old_c32_name[i]);
					if (CopyFileU(tmp, psz_fullpath, FALSE)) {
						uprintf("  Replaced with local version %s", IsFileInDB(tmp)?"✓":"✗");
						break;
					}
					uprintf("  Could not replace file: %s", WindowsErrorString());
				}
			}
			if (i < NB_OLD_C32)
				continue;
			psz_sanpath = sanitize_filename(psz_fullpath, &is_identical);
			if (!is_identical)
				uprintf("  File name sanitized to '%s'", psz_sanpath);
			create_file = TRUE;
			if (is_symlink) {
				if (fs_type == FS_NTFS) {
					// Replicate symlinks if NTFS is being used
					static_sprintf(target_path, "%s/%s", psz_path, p_statbuf->rr.psz_symlink);
					iso9660_stat_t* p_statbuf2 = iso9660_ifs_stat_translate(p_iso, target_path);
					if (p_statbuf2 != NULL) {
						// NB: Linux only uses POSIX separators, so unlike
						// upstream there is no path conversion here
						uprintf("Symlinking: %s%s ➔ %s", psz_fullpath,
							(p_statbuf2->type == _STAT_DIR) ? "/" : "", p_statbuf->rr.psz_symlink);
						if (!CreateSymbolicLinkU(psz_fullpath, p_statbuf->rr.psz_symlink,
							(p_statbuf2->type == _STAT_DIR) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0))
							uprintf("  Could not create symlink: %s", WindowsErrorString());
						iso9660_stat_free(p_statbuf2);
						create_file = FALSE;
					}
				} else if (file_length == 0) {
					if ((safe_stricmp(p_statbuf->filename, "syslinux") == 0) &&
						// Special handling for ISOs that have a syslinux → isolinux symbolic link (e.g. Knoppix)
						(safe_stricmp(p_statbuf->rr.psz_symlink, "isolinux") == 0)) {
						static_strcpy(symlinked_syslinux, psz_fullpath);
						print_extracted_file(psz_fullpath, file_length);
						uprintf("  Found Rock Ridge symbolic link to '%s'", p_statbuf->rr.psz_symlink);
					} else if (strcmp(psz_path, "/firmware") == 0) {
						// Special handling for ISOs that use symlinks for /firmware/ (e.g. Debian non-free)
						// TODO: Do we want to do this for all file symlinks?
						static_sprintf(target_path, "%s/%s", psz_path, p_statbuf->rr.psz_symlink);
						p_statbuf = iso9660_ifs_stat_translate(p_iso, target_path);
						if (p_statbuf != NULL) {
							// The original p_statbuf will be freed automatically, but not
							// the new one so we need to force an explicit free.
							free_p_statbuf = TRUE;
							file_length = p_statbuf->total_size;
							print_extracted_file(psz_fullpath, file_length);
							uprintf("  Duplicated from '%s'", target_path);
						} else {
							uprintf("Could not resolve Rock Ridge Symlink - ABORTING!");
							goto out;
						}
					} else {
						print_extracted_file(psz_fullpath, safe_strlen(p_statbuf->rr.psz_symlink));
						uprintf("  Ignoring Rock Ridge symbolic link to '%s'", p_statbuf->rr.psz_symlink);
					}
				} else {
					uuprintf("Unexpected symlink length: %d", file_length);
					create_file = FALSE;
				}
			}
			if (create_file) {
				file_handle = create_preallocated_file(psz_sanpath, GENERIC_READ | GENERIC_WRITE,
					FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, file_length);
				// NB: io.c maps a missing parent directory to ERROR_FILE_NOT_FOUND
				// (POSIX has no such distinction), so check for both
				if (file_handle == INVALID_HANDLE_VALUE && (GetLastError() == ERROR_PATH_NOT_FOUND ||
					GetLastError() == ERROR_FILE_NOT_FOUND)) {
					// Some folks (umbrelos) managed to master their ISOs in a manner where some
					// directories don't exist (or don't have _STAT_DIR) but still have files,
					// in which case our approach, that expects a sane layout with directories
					// properly declared before the files they contain, breaks. Therefore:
					last_slash = strrchr(psz_sanpath, '/');
					if (last_slash != NULL) {
						*last_slash = '\0';
						uprintf("WARNING: Directory '%s/' was improperly mastered on the source image!", iso_relative_part(psz_sanpath));
						mkdir_ex(psz_sanpath);
						*last_slash = '/';
						file_handle = create_preallocated_file(psz_sanpath, GENERIC_READ | GENERIC_WRITE,
							FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, file_length);
					}
				}
				if (file_handle == INVALID_HANDLE_VALUE) {
					err = GetLastError();
					uprintf("  Unable to create file: %s", WindowsErrorString());
					if (((err == ERROR_ACCESS_DENIED) || (err == ERROR_INVALID_HANDLE)) &&
						(is_autorun_file(psz_sanpath)))
						uprintf(stupid_antivirus);
					else
						goto out;
				} else if (is_symlink) {
					// Create a text file that contains the target link
					ISO_BLOCKING(r = WriteFileWithRetry(file_handle, p_statbuf->rr.psz_symlink,
						(DWORD)safe_strlen(p_statbuf->rr.psz_symlink), &wr_size, WRITE_RETRIES));
					if (!r) {
						uprintf("  Error writing file: %s", WindowsErrorString());
						goto out;
					}
				} else {
					if (fd_md5sum != NULL)
						iso_md5_init(&ctx);
					for (i = 0; file_length > 0; i += nb) {
						if (ErrorStatus)
							goto out;
						lsn = p_statbuf->lsn + (lsn_t)i;
						nb = (size_t)MIN(ISO_BUFFER_SIZE / ISO_BLOCKSIZE, (file_length + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE);
						if (iso9660_iso_seek_read(p_iso, buf, lsn, (long)nb) != (nb * ISO_BLOCKSIZE)) {
							uprintf("  Error reading ISO9660 file %s at LSN %lu",
								psz_iso_name, (long unsigned int)lsn);
							goto out;
						}
						buf_size = (DWORD)MIN(file_length, ISO_BUFFER_SIZE);
						if (fd_md5sum != NULL)
							iso_md5_write(&ctx, buf, buf_size);
						ISO_BLOCKING(r = WriteFileWithRetry(file_handle, buf, buf_size, &wr_size, WRITE_RETRIES));
						if (!r || wr_size != buf_size) {
							if (r)
								SetLastError(ERROR_WRITE_FAULT);
							uprintf("  Error writing file: %s", WindowsErrorString());
							goto out;
						}
						file_length -= wr_size;
						nb_blocks += nb;
						if (nb_blocks - last_nb_blocks >= PROGRESS_THRESHOLD) {
							UpdateProgressWithInfo(OP_FILE_COPY, MSG_231, nb_blocks, total_blocks +
								((fs_type != FS_NTFS) ? extra_blocks : 0));
							last_nb_blocks = nb_blocks;
						}
					}
					if (fd_md5sum != NULL) {
						iso_md5_final(&ctx);
						for (j = 0; j < ISO_MD5_HASHSIZE; j++)
							fprintf(fd_md5sum, "%02x", ctx.buf[j]);
						// NB: Unlike upstream (which strips the Windows drive part
						// with &psz_fullpath[3]), we print the ISO-relative path
						rel_path = iso_relative_path(psz_fullpath);
						fprintf(fd_md5sum, "  ./%s\n", (rel_path != NULL) ? rel_path : psz_fullpath);
						safe_free(rel_path);
					}
				}
				if (preserve_timestamps) {
					LPFILETIME ft = to_filetime(mktime(&p_statbuf->tm));
					if (!SetFileTime(file_handle, ft, ft, ft))
						uprintf("  Could not set timestamp: %s", WindowsErrorString());
				}
			}
			if (free_p_statbuf)
				iso9660_stat_free(p_statbuf);
			ISO_BLOCKING(safe_closehandle(file_handle));
			if (props.is_cfg || props.is_conf)
				fix_config(psz_sanpath, psz_path, psz_basename, &props);
			safe_free(psz_sanpath);
		}
	}
	r = 0;

out:
	if (r != 0 && GetLastError() != ERROR_SUCCESS)
		ErrorStatus = RUFUS_ERROR(GetLastError());
	ISO_BLOCKING(safe_closehandle(file_handle));
	if (p_entlist != NULL)
		iso9660_filelist_free(p_entlist);
	safe_free(psz_sanpath);
	safe_free(buf);
	return r;
}

void GetGrubVersion(char* buf, size_t buf_size, const char* source)
{
	// In typical "I'll make my own Open Source... with blackjack and hookers!" fashion,
	// IBM/Red-Hat/Fedora took it upon themselves to "fix" the double space typo from the
	// GRUB version string. But of course, just like their introduction of GRUB calls like
	// 'grub_debug_is_enabled', they didn't want to bother upstreaming their changes...
	// On the other hand, boy do they want to leech of FSF/GNU developed software, while
	// not having it mention GNU anywhere. See:
	// https://src.fedoraproject.org/rpms/grub2/blob/rawhide/f/0024-Don-t-say-GNU-Linux-in-generated-menus.patch
	const char* grub_version_str[] = { "GRUB  version %s", "GRUB version %s" };
	const char* grub_debug_is_enabled_str = "grub_debug_is_enabled";
	const size_t max_string_size = 32;	// The strings above *MUST* be no longer than this value
	char grub_version[192] = { 0 };
	size_t i, j;
	BOOL has_grub_debug_is_enabled = FALSE;

	// Make sure we don't overflow our buffer
	if (buf_size > max_string_size) {
		for (i = 0; i < buf_size - max_string_size; i++) {
			for (j = 0; j < ARRAYSIZE(grub_version_str); j++) {
				if (memcmp(&buf[i], grub_version_str[j], strlen(grub_version_str[j])) == 0) {
					// For CentOS, who decided to add a '\n' after "GRUB  version %s"
					if (buf[i + strlen(grub_version_str[j]) + 1] == '\0')
						i++;
					static_strcpy(grub_version, &buf[i + strlen(grub_version_str[j]) + 1]);
				}
			}
			if (memcmp(&buf[i], grub_debug_is_enabled_str, strlen(grub_debug_is_enabled_str)) == 0)
				has_grub_debug_is_enabled = TRUE;
		}
	}

	uprintf("  Detected GRUB version: %s (from '%s')", grub_version, source);

	if (img_report.grub2_version[0] != 0)
		return;

	static_strcpy(img_report.grub2_version, grub_version);

	// <Shakes fist angrily> "KASPERSKYYYYYY!!!..." (https://github.com/pbatard/rufus/issues/467)
	// But seriously, these guys should know better than "security" through obscurity...
	if (img_report.grub2_version[0] == '0')
		img_report.grub2_version[0] = 0;

	// For some obscure reason, openSUSE have decided that their Live images should
	// use /boot/grub2/ as their prefix directory instead of the standard /boot/grub/
	// This creates a MAJOR issue because the prefix directory is hardcoded in
	// 'core.img', and Rufus must install a 'core.img', that is not provided by the
	// ISO, for the USB to boot (since even trying to pick the one from ISOHybrid
	// does usually not guarantees the presence of the FAT driver which is mandatory
	// for ISO boot).
	// Therefore, when *someone* uses a nonstandard GRUB prefix directory, our base
	// 'core.img' can't work with their image, since it isn't able to load modules
	// like 'normal.mod', that are required to access the configuration files. Oh and
	// you can forget about direct editing the prefix string inside 'core.img' since
	// GRUB are forcing LZMA compression for BIOS payloads. And it gets even better,
	// because even if you're trying to be smart and use GRUB's earlyconfig features
	// to do something like:
	//   if [ -e /boot/grub2/i386-pc/normal.mod ]; then set prefix = ...
	// you still must embed 'configfile.mod' and 'normal.mod' in 'core.img' in order
	// to do that, which ends up tripling the file size...
	// Also, as mentioned above, Fedora, Ubuntu and others have started applying
	// *BREAKING* patches willy-nilly, without bothering to alter the GRUB version
	// string. And it gets worse with 2.06 since there are patches we can't detect
	// that will produce "452: out of range pointer" whether they are applied OR NOT
	// (meaning that if you use a patched GRUB 2.06 with unpatched GRUB 2.06 modules
	// you will get the error, and if you use unpatched with patched modules, you
	// will also get the error).
	// Soooo, since the universe, and project maintainers who do not REALISE that
	// NOT RELEASING IN A TIMELY MANNER *DOES* HAVE VERY NEGATIVE CONSEQUENCES FOR
	// END USERS, are conspiring against us, and since we already have a facility
	// for it, we'll use it to dowload the relevant 'core.img' by appending a missing
	// version suffix as needed. Especially, if GRUB only identifies itself as '2.06'
	// we'll append a sanitized version of the ISO label to try to differentiate
	// between GRUB 2.06 incompatible versions...
	if (img_report.grub2_version[0] != 0) {
		// Make sure we append '-nonstandard' and '-gdie' before the sanitized label.
		BOOL append_label = (safe_strcmp(img_report.grub2_version, "2.06") == 0);
		// Must be in the same order as we have on the server
		if ((img_report.has_grub2 & 0x7f) > 1)
			safe_strcat(img_report.grub2_version, sizeof(img_report.grub2_version), "-nonstandard");
		if (has_grub_debug_is_enabled)
			safe_strcat(img_report.grub2_version, sizeof(img_report.grub2_version), "-gdie");
		if (append_label) {
			safe_strcat(img_report.grub2_version, sizeof(img_report.grub2_version), "-");
			safe_strcat(img_report.grub2_version, sizeof(img_report.grub2_version), img_report.label);
		}
		iso_sanitize_label(img_report.grub2_version);
	}
}

void GetGrubFs(char* buf, size_t buf_size)
{
	const char* grub_fshelp_str = "fshelp";
	const size_t max_string_size = 32;
	size_t i;

	if (buf_size > max_string_size) {
		for (i = 0; i < buf_size - max_string_size; i++) {
			if (memcmp(&buf[i], grub_fshelp_str, strlen(grub_fshelp_str) + 1) == 0) {
				if (buf[i + strlen(grub_fshelp_str) + 1] != 0 && strlen(&buf[i + strlen(grub_fshelp_str) + 1]) < 12) {
					StrArrayAddUnique(&grub_filesystems, &buf[i + strlen(grub_fshelp_str) + 1], TRUE);
				}
			}
		}
	}
}

void GetEfiBootInfo(char* buf, size_t buf_size, const char* source)
{
	// Data to help us identify the EFI bootloader type
	const struct {
		const char* label;
		const char* search_string;
	} boot_info[] = {
		{ "Shim", "UEFI SHIM\n$Version: "},
		// NB: There's also an ID=systemd-boot\nVERSION="x.y.z" footer
		// in the Arch systemd-boot EFI binary, but I'm not sure if we
		// can count on this metadata footer to always be present...
		{ "systemd-boot", "#### LoaderInfo: systemd-boot " },
	};
	const size_t max_string_size = 64;
	size_t i, j, k;

	if (buf_size > max_string_size) {
		for (i = 0; i < buf_size - max_string_size; i++) {
			for (j = 0; j < ARRAYSIZE(boot_info); j++) {
				if (memcmp(&buf[i], boot_info[j].search_string, strlen(boot_info[j].search_string)) == 0) {
					i += strlen(boot_info[j].search_string);
					for (k = 0; k < 32 && i + k < buf_size - 1 && !isspace(buf[i + k]); k++);
					buf[i + k] = '\0';
					uprintf("  Detected %s version: %s (from '%s')", boot_info[j].label, &buf[i], source);
					return;
				}
			}
		}
	}
}

BOOL ExtractISO(const char* src_iso, const char* dest_dir, BOOL scan)
{
	const char* basedir[] = { "i386", "amd64", "minint" };
	int k, r = 1;
	char *tmp, *ext, *spacing = "  ";
	char path[MAX_PATH], path2[16];
	uint8_t* buf = NULL;
	uint16_t sl_version;
	size_t i, j, size, sl_index = 0;
	FILE* fd;
	iso9660_t* p_iso = NULL;
	iso9660_pvd_t pvd;
	udf_t* p_udf = NULL;
	udf_dirent_t* p_udf_root;
	iso_extension_mask_t iso_extension_mask = ISO_EXTENSION_ALL;

	if ((!enable_iso) || (src_iso == NULL) || (dest_dir == NULL))
		return FALSE;

	scan_only = scan;
	if (!scan_only)
		spacing = "";
	cdio_log_set_handler((scan_only && !enable_debug) ? NULL : log_handler);
	psz_extract_dir = dest_dir;
	// Change progress style to marquee for scanning
	if (scan_only) {
		uprintf("ISO analysis:");
		// NB: No GUI on Linux, so the marquee progress bar init is dropped
		total_blocks = 0;
		extra_blocks = 0;
		has_ldlinux_c32 = FALSE;
		// String array of all isolinux/syslinux locations
		StrArrayCreate(&config_path, 8);
		StrArrayCreate(&isolinux_path, 8);
		StrArrayCreate(&grub_filesystems, 8);
		PrintInfo(0, MSG_202);
	} else {
		uprintf("Extracting files...");
		// PORT: downloaded files live under the per-user data directory
		IGNORE_RETVAL(_chdirU(app_data_dir));
		if (total_blocks == 0) {
			uprintf("Error: ISO has not been properly scanned.");
			ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_SCAN));
			goto out;
		}
		nb_blocks = 0;
		last_nb_blocks = 0;
		iso_blocking_status = 0;
		symlinked_syslinux[0] = 0;
		StrArrayClear(&modified_files);
		if (validate_md5sum) {
			md5sum_totalbytes = 0;
			// If there isn't an already existing md5sum.txt create one
			if (img_report.has_md5sum != 1) {
				static_sprintf(path, "%s/%s", dest_dir, md5sum_name[0]);
				fd_md5sum = fopen(path, "wb");
				if (fd_md5sum == NULL)
					uprintf("WARNING: Could not create '%s'", md5sum_name[0]);
			} else {
				md5sum_size = ReadISOFileToBuffer(src_iso, md5sum_name[0], (uint8_t**)&md5sum_data);
				md5sum_pos = md5sum_data;
			}
		}
	}

	// First try to open as UDF - fallback to ISO if it failed
	p_udf = udf_open(src_iso);
	if (p_udf == NULL)
		goto try_iso;
	uprintf("%sImage is a UDF image", spacing);

	p_udf_root = udf_get_root(p_udf, true, 0);
	if (p_udf_root == NULL) {
		uprintf("%sCould not locate UDF root directory", spacing);
		goto try_iso;
	}
	if (scan_only) {
		if (udf_get_logical_volume_id(p_udf, img_report.label, sizeof(img_report.label)) <= 0)
			img_report.label[0] = 0;
		// Open the UDF as ISO so that we can perform size checks
		p_iso = iso9660_open(src_iso);
	}
	r = udf_extract_files(p_udf, p_udf_root, "");
	goto out;

try_iso:
	// Perform our first scan with Joliet disabled (if Rock Ridge is enabled), so that we can find if
	// there exists a Rock Ridge file with a name > 64 chars or if there are symlinks. If that is the
	// case then we also disable Joliet during the extract phase.
	if ((!enable_joliet) || (enable_rockridge && (scan_only || img_report.has_long_filename ||
		(img_report.has_symlinks == SYMLINKS_RR)))) {
		iso_extension_mask &= ~ISO_EXTENSION_JOLIET;
	}
	if (!enable_rockridge) {
		iso_extension_mask &= ~ISO_EXTENSION_ROCK_RIDGE;
	}

	p_iso = iso9660_open_ext(src_iso, iso_extension_mask);
	if (p_iso == NULL) {
		uprintf("%s'%s' doesn't look like an ISO image", spacing, src_iso);
		r = 1;
		goto out;
	}
	uprintf("%sImage is an ISO9660 image", spacing);
	joliet_level = iso9660_ifs_get_joliet_level(p_iso);
	if (scan_only) {
		if (iso9660_ifs_get_volume_id(p_iso, &tmp)) {
			static_strcpy(img_report.label, tmp);
			safe_free(tmp);
		} else
			img_report.label[0] = 0;
	} else {
		if (iso_extension_mask & (ISO_EXTENSION_JOLIET|ISO_EXTENSION_ROCK_RIDGE))
			uprintf("%sThis image will be extracted using %s extensions (if present)", spacing,
				(iso_extension_mask & ISO_EXTENSION_JOLIET)?"Joliet":"Rock Ridge");
		else
			uprintf("%sThis image will not be extracted using any ISO extensions", spacing);
	}
	r = iso_extract_files(p_iso, "");

out:
	iso_blocking_status = -1;
	if (scan_only) {
		const char* fs_name[] = { "fat", "exfat", "ntfs" };
		struct stat stat;
		char fses[256] = { 0 };
		// Find if there is a mismatch between the ISO size, as reported by the PVD, and the actual file size
		if ((iso9660_ifs_read_pvd(p_iso, &pvd)) && (_stat64U(src_iso, &stat) == 0))
			img_report.mismatch_size = (int64_t)(iso9660_get_pvd_space_size(&pvd)) * ISO_BLOCKSIZE - stat.st_size;
		// Remove trailing spaces from the label
		for (k = (int)safe_strlen(img_report.label) - 1; ((k > 0) && (isspaceU(img_report.label[k]))); k--)
			img_report.label[k] = 0;
		// We use the fact that UDF_BLOCKSIZE and ISO_BLOCKSIZE are the same here
		// Also, we add 1% extra requirement, on account that we most likely use a 4k or higher cluster size
		// whereas ISO_BLOCKSIZE is 2k, which means we'll need extra spaces if there are many small files.
		img_report.projected_size = (uint64_t)((double)total_blocks * ISO_BLOCKSIZE * 1.01f);
		// We will link the existing isolinux.cfg from a syslinux.cfg we create
		// If multiple config files exist, choose the one with the shortest path
		// (so that a '/syslinux.cfg' is preferred over a '/isolinux/isolinux.cfg')
		if (!IsStrArrayEmpty(config_path)) {
			// Set the img_report.cfg_path string to maximum length, so that we don't have to
			// do a special case for StrArray entry 0.
			memset(img_report.cfg_path, '_', sizeof(img_report.cfg_path) - 1);
			img_report.cfg_path[sizeof(img_report.cfg_path) - 1] = 0;
			for (i = 0; i < config_path.Index; i++) {
				// OpenSuse based Live image have a /syslinux.cfg that doesn't work, so we enforce
				// the use of the one in '/boot/[i386|x86_64]/loader/isolinux.cfg' if present.
				// Note that, because the openSuse live script are not designed to handle anything but
				// an ISO9660 filesystem for the live device, this still won't allow for proper boot.
				// See https://github.com/openSUSE/kiwi/issues/354
				if ( (_stricmp(config_path.String[i], "/boot/i386/loader/isolinux.cfg") == 0) ||
					 (_stricmp(config_path.String[i], "/boot/x86_64/loader/isolinux.cfg") == 0)) {
					static_strcpy(img_report.cfg_path, config_path.String[i]);
					img_report.needs_syslinux_overwrite = TRUE;
					break;
				}
				// Tails uses an '/EFI/BOOT/isolinux.cfg' along with a '/isolinux/isolinux.cfg'
				// which are the exact same length. However, only the /isolinux one will work,
				// so for now, at equal length, always pick the latest.
				// We may have to revisit this and prefer a path that contains '/isolinux' if
				// this hack is not enough for other images.
				if (safe_strlen(img_report.cfg_path) >= safe_strlen(config_path.String[i]))
					static_strcpy(img_report.cfg_path, config_path.String[i]);
			}
			uprintf("  Will use '%s' for Syslinux", img_report.cfg_path);
			// Extract all of the isolinux.bin files we found to identify their versions
			for (i = 0; i < isolinux_path.Index; i++) {
				size = (size_t)ReadISOFileToBuffer(src_iso, isolinux_path.String[i], &buf);
				if (size == 0) {
					uprintf("  Could not access %s", isolinux_path.String[i]);
				} else {
					sl_version = iso_parse_isolinux_version(buf, size, &ext);
					if (img_report.sl_version == 0) {
						static_strcpy(img_report.sl_version_ext, ext);
						img_report.sl_version = sl_version;
						sl_index = i;
					} else if ((img_report.sl_version != sl_version) || (safe_strcmp(img_report.sl_version_ext, ext) != 0)) {
						uprintf("  Found conflicting Isolinux versions:\r\n  '%s' (%d.%02d%s) vs '%s' (%d.%02d%s)",
							isolinux_path.String[sl_index], SL_MAJOR(img_report.sl_version), SL_MINOR(img_report.sl_version),
							img_report.sl_version_ext, isolinux_path.String[i], SL_MAJOR(sl_version), SL_MINOR(sl_version), ext);
						// Workaround for Antergos and other ISOs, that have multiple Syslinux versions.
						// Where possible, prefer to the one that resides in the same directory as the config file.
						for (j=safe_strlen(img_report.cfg_path); (j > 0) && (img_report.cfg_path[j] != '/'); j--);
						if (safe_strnicmp(img_report.cfg_path, isolinux_path.String[i], j) == 0) {
							static_strcpy(img_report.sl_version_ext, ext);
							img_report.sl_version = sl_version;
							sl_index = i;
						}
					}
					safe_free(buf);
				}
			}
			if (img_report.sl_version != 0) {
				static_sprintf(img_report.sl_version_str, "%d.%02d",
					SL_MAJOR(img_report.sl_version), SL_MINOR(img_report.sl_version));
				uprintf("  Detected Syslinux version: %s%s (from '%s')",
					img_report.sl_version_str, img_report.sl_version_ext, isolinux_path.String[sl_index]);
				if ( (has_ldlinux_c32 && (SL_MAJOR(img_report.sl_version) < 5))
				  || (!has_ldlinux_c32 && (SL_MAJOR(img_report.sl_version) >= 5)) )
					uprintf("  Warning: Conflict between Isolinux version and the presence of ldlinux.c32...");
			} else {
				// Couldn't find a version from isolinux.bin. Force set to the versions we embed
				img_report.sl_version = embedded_sl_version[has_ldlinux_c32?1:0];
				static_sprintf(img_report.sl_version_str, "%d.%02d",
					SL_MAJOR(img_report.sl_version), SL_MINOR(img_report.sl_version));
				uprintf("  Warning: Could not detect Isolinux version - Forcing to %s (embedded)",
					img_report.sl_version_str);
			}
		}
		if (!IS_EFI_BOOTABLE(img_report) && HAS_EFI_IMG(img_report) && HasEfiImgBootLoaders(p_iso)) {
			img_report.has_efi = 0x8000;
		}
		if (HAS_WINPE(img_report)) {
			// In case we have a WinPE 1.x based iso, we extract and parse txtsetup.sif
			// during scan, to see if /minint was provided for OsLoadOptions, as it decides
			// whether we should use 0x80 or 0x81 as the disk ID in the MBR
			static_sprintf(path, "/%s/txtsetup.sif",
				basedir[((img_report.winpe & WINPE_I386) == WINPE_I386) ? 0 : ((img_report.winpe & WINPE_AMD64) == WINPE_AMD64 ? 1 : 2)]);
			size = (size_t)ReadISOFileToBuffer(src_iso, path, &buf);
			tmp = get_token_data_buffer("OsLoadOptions", 1, buf, size);
			if (tmp != NULL) {
				for (i = 0; i < strlen(tmp); i++)
					tmp[i] = (char)tolower(tmp[i]);
				uprintf("  Checking txtsetup.sif:\r\n  OsLoadOptions = %s", tmp);
				img_report.uses_minint = (strstr(tmp, "/minint") != NULL);
				safe_free(tmp);
			}
			safe_free(buf);
		}
		if (HAS_WININST(img_report)) {
			char wim_path[4 * MAX_PATH];
			assert(safe_strlen(image_path) + safe_strlen(img_report.wininst_path[0]) + 2 < ARRAYSIZE(wim_path));
			static_sprintf(wim_path, "%s|%s", image_path, img_report.wininst_path[0]);
			img_report.wininst_version = GetWimVersion(wim_path);
		}
		if (img_report.has_grub2) {
			char grub_path[128];
			static_sprintf(grub_path, "/%s/normal.mod", &grub_dirname[img_report.has_grub2 - 1][1]);
			// In case we have a GRUB2 based iso, we extract boot/grub/i386-pc/normal.mod to parse its version
			img_report.grub2_version[0] = 0;
			size = (size_t)ReadISOFileToBuffer(src_iso, grub_path, &buf);
			if (size == 0)
				uprintf("  Could not read Grub version from '%s'", grub_path);
			else
				GetGrubVersion(buf, size, grub_path);
			safe_free(buf);
			if (img_report.grub2_version[0] == 0) {
				uprintf("  Could not detect Grub version");
				img_report.has_grub2 &= 0x80;
			}
		}
		for (j = 0; j < ARRAYSIZE(img_report.efi_boot_entry); j++) {
			if (!img_report.efi_boot_entry[j].path[0])
				continue;
			if (img_report.efi_boot_entry[j].type == EBT_GRUB) {
				size = (size_t)ReadISOFileToBuffer(src_iso, img_report.efi_boot_entry[j].path, &buf);
				if (size == 0) {
					uprintf("  Could not read Grub version from '%s'", img_report.efi_boot_entry[j].path);
				} else {
					img_report.has_grub2 |= 0x80;
					GetGrubVersion(buf, size, img_report.efi_boot_entry[j].path);
					GetGrubFs(buf, size);
				}
				safe_free(buf);
			} else if (img_report.efi_boot_entry[j].type == EBT_MAIN) {
				size = (size_t)ReadISOFileToBuffer(src_iso, img_report.efi_boot_entry[j].path, &buf);
				if (size == 0) {
					uprintf("  Could not parse '%s'", img_report.efi_boot_entry[j].path);
				} else {
					GetEfiBootInfo(buf, size, img_report.efi_boot_entry[j].path);
				}
				safe_free(buf);
			}
		}
		for (i = 0; i < (int)grub_filesystems.Index; i++) {
			if (i != 0)
				static_strcat(fses, ", ");
			static_strcat(fses, grub_filesystems.String[i]);
			for (j = 0; j < ARRAYSIZE(fs_name); j++)
				if (stricmp(grub_filesystems.String[i], fs_name[j]) == 0)
					img_report.has_grub2_fs |= (1 << j);
		}
		if (*fses)
			uprintf("  Supported GRUB filesystems: %s", fses);

		StrArrayDestroy(&config_path);
		StrArrayDestroy(&isolinux_path);
		StrArrayDestroy(&grub_filesystems);
	} else {
		// Solus and other ISOs only provide EFI boot files in a FAT efi.img
		// Also work around ISOs that have a borked symbolic link for bootx64.efi.
		// See https://github.com/linuxmint/linuxmint/issues/622.
		if (img_report.has_efi & 0xc000) {
			if (img_report.has_efi & 0x4000) {
				uprintf("Broken UEFI bootloader detected - Applying workaround:");
				static_sprintf(path, "%s/EFI/boot/bootx64.efi", dest_dir);
				DeleteFileU(path);
			}
			DumpFatDir(p_iso, dest_dir, 0);
		}
		if (HAS_SYSLINUX(img_report)) {
			static_sprintf(path, "%s/syslinux.cfg", dest_dir);
			// Create a /syslinux.cfg (if none exists) that points to the existing isolinux cfg
			fd = fopen(path, "r");
			if (fd != NULL && img_report.needs_syslinux_overwrite) {
				fclose(fd);
				fd = NULL;
				static_sprintf(path2, "%s/syslinux.org", dest_dir);
				uprintf("Renaming: %s ➔ %s", path, path2);
				IGNORE_RETVAL(rename(path, path2));
			}
			if (fd == NULL) {
				fd = fopen(path, "w");	// No "/syslinux.cfg" => create a new one
				if (fd == NULL) {
					uprintf("Unable to create %s - booting from USB will not work", path);
					r = 1;
				} else {
					fprintf(fd, "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG %s\n", img_report.cfg_path);
					for (i = safe_strlen(img_report.cfg_path); (i > 0) && (img_report.cfg_path[i] != '/'); i--);
					if (i > 0) {
						img_report.cfg_path[i] = 0;
						fprintf(fd, "  APPEND %s/\n", img_report.cfg_path);
						img_report.cfg_path[i] = '/';
					}
					uprintf("Created: %s → %s", path, img_report.cfg_path);
				}
			}
			if (fd != NULL)
				fclose(fd);
			// Workaround needed for Knoppix that has a /boot/syslinux that links to /boot/isolinux/
			// with EFI Syslinux trying to read /boot/syslinux/syslnx[32|64].cfg as the config file.
			if (symlinked_syslinux[0] != 0) {
				static const char* efi_cfg_name[] = { "syslnx32.cfg", "syslnx64.cfg" };
				size_t len = strlen(symlinked_syslinux);
				char isolinux_dir[MAX_PATH];
				static_strcpy(isolinux_dir, symlinked_syslinux);
				assert(len > 8);
				// ".../syslinux" -> ".../isolinux"
				isolinux_dir[len - 8] = 'i';
				isolinux_dir[len - 7] = 's';
				isolinux_dir[len - 6] = 'o';
				// Delete the empty syslinux symbolic link remnant and replace it with a syslinux/ dir
				DeleteFileU(symlinked_syslinux);
				CreateDirectoryU(symlinked_syslinux, NULL);
				// Now add the relevant config files that link back to the ones in isolinux/
				for (i = 0; i < ARRAYSIZE(efi_cfg_name); i++) {
					static_sprintf(path, "%s/%s", isolinux_dir, efi_cfg_name[i]);
					if (!PathFileExistsU(path))
						continue;
					static_sprintf(path, "%s/%s", symlinked_syslinux, efi_cfg_name[i]);
					fd = fopen(path, "w");
					if (fd == NULL) {
						uprintf("Unable to create %s - booting from USB may not work", path);
						r = 1;
						continue;
					}
					static_sprintf(path, "%s/%s", isolinux_dir, efi_cfg_name[i]);
					// NB: Unlike upstream (which uses &path[2] to skip the Windows
					// drive part), we print the ISO-relative config paths
					fprintf(fd, "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG %s\n  APPEND %s\n",
						iso_relative_part(path), iso_relative_part(isolinux_dir));
					fclose(fd);
					uprintf("Created: %s/%s → %s", symlinked_syslinux, efi_cfg_name[i], iso_relative_part(path));
				}
			}
		} else if (HAS_BOOTMGR(img_report) && enable_ntfs_compression) {
			// bootmgr might need to be uncompressed: https://github.com/pbatard/rufus/issues/1381
			// TODO(ntfs): 'compact /u' is a Windows only tool and the port has no
			// NTFS uncompress implementation yet, so just warn about it
			uprintf("WARNING: Cannot uncompress the bootmgr files ('compact /u' is not available on Linux)");
		}
		// Exception for Slax Syslinux UEFI bootloaders...
		// ...that don't appear to work anyway as of slax-64bit-slackware-15.0.3.iso
		static_sprintf(path, "%s/slax/boot/EFI", dest_dir);
		if (PathFileExistsU(path)) {
			char dst_path[16];
			static_sprintf(dst_path, "%s/EFI", dest_dir);
			if (!PathFileExistsU(dst_path)) {
				if (MoveFileU(path, dst_path))
					uprintf("Moved: %s → %s", path, dst_path);
				else
					uprintf("Could not move %s → %s: %s", path, dst_path, WindowsErrorString());
			}
		}
		if (fd_md5sum != NULL) {
			uprintf("Created: %s/%s (%s)", dest_dir, md5sum_name[0], SizeToHumanReadable(ftell(fd_md5sum), FALSE, FALSE));
			fclose(fd_md5sum);
		} else if (md5sum_data != NULL) {
			safe_free(md5sum_data);
			md5sum_size = 0;
		}
	}
	iso9660_close(p_iso);
	udf_close(p_udf);
	if ((r != 0) && (ErrorStatus == 0))
		ErrorStatus = RUFUS_ERROR(APPERR(scan_only ? ERROR_ISO_SCAN : ERROR_ISO_EXTRACT));
	return (r == 0);
}

int64_t ExtractISOFile(const char* iso, const char* iso_file, const char* dest_file, DWORD attributes)
{
	size_t i;
	ssize_t read_size;
	int64_t file_length, r = 0;
	char buf[UDF_BLOCKSIZE];
	DWORD buf_size, wr_size;
	iso9660_t* p_iso = NULL;
	udf_t* p_udf = NULL;
	udf_dirent_t *p_udf_root = NULL, *p_udf_file = NULL;
	iso9660_stat_t *p_statbuf = NULL;
	lsn_t lsn;
	HANDLE file_handle = INVALID_HANDLE_VALUE;

	file_handle = CreateFileU(dest_file, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ, NULL, CREATE_ALWAYS, attributes, NULL);
	if (file_handle == INVALID_HANDLE_VALUE) {
		uprintf("Could not create file %s: %s", dest_file, WindowsErrorString());
		goto out;
	}

	// First try to open as UDF - fallback to ISO if it failed
	p_udf = udf_open(iso);
	if (p_udf == NULL)
		goto try_iso;

	p_udf_root = udf_get_root(p_udf, true, 0);
	if (p_udf_root == NULL) {
		uprintf("Could not locate UDF root directory");
		goto out;
	}
	p_udf_file = udf_fopen(p_udf_root, iso_file);
	if (!p_udf_file) {
		uprintf("Could not locate file %s in ISO image", iso_file);
		goto out;
	}
	file_length = udf_get_file_length(p_udf_file);
	while (file_length > 0) {
		memset(buf, 0, UDF_BLOCKSIZE);
		read_size = udf_read_block(p_udf_file, buf, 1);
		if (read_size < 0) {
			uprintf("Error reading UDF file %s", iso_file);
			goto out;
		}
		buf_size = (DWORD)MIN(file_length, (int64_t)read_size);
		if (!WriteFileWithRetry(file_handle, buf, buf_size, &wr_size, WRITE_RETRIES)) {
			uprintf("Error writing file %s: %s", dest_file, WindowsErrorString());
			goto out;
		}
		file_length -= buf_size;
		r += buf_size;
	}
	goto out;

try_iso:
	// Make sure to enable extensions, else we may not match the name of the file we are looking
	// for since Rock Ridge may be needed to translate something like 'I386_PC' into 'i386-pc'...
	p_iso = iso9660_open_ext(iso, ISO_EXTENSION_MASK);
	if (p_iso == NULL) {
		uprintf("Unable to open image '%s'", iso);
		goto out;
	}

	p_statbuf = iso9660_ifs_stat_translate(p_iso, iso_file);
	if (p_statbuf == NULL) {
		uprintf("Could not get ISO-9660 file information for file %s", iso_file);
		goto out;
	}

	file_length = p_statbuf->total_size;
	for (i = 0; file_length > 0; i++) {
		memset(buf, 0, ISO_BLOCKSIZE);
		lsn = p_statbuf->lsn + (lsn_t)i;
		if (iso9660_iso_seek_read(p_iso, buf, lsn, 1) != ISO_BLOCKSIZE) {
			uprintf("Error reading ISO9660 file %s at LSN %lu", iso_file, (long unsigned int)lsn);
			goto out;
		}
		buf_size = (DWORD)MIN(file_length, ISO_BLOCKSIZE);
		if (!WriteFileWithRetry(file_handle, buf, buf_size, &wr_size, WRITE_RETRIES)) {
			uprintf("Error writing file %s: %s", dest_file, WindowsErrorString());
			goto out;
		}
		file_length -= buf_size;
		r += buf_size;
	}

out:
	safe_closehandle(file_handle);
	if (r == 0)
		DeleteFileU(dest_file);
	iso9660_stat_free(p_statbuf);
	udf_dirent_free(p_udf_root);
	udf_dirent_free(p_udf_file);
	iso9660_close(p_iso);
	udf_close(p_udf);
	return r;
}

/*
 * Extract a file to a buffer. Buffer must be freed by the caller.
 */
uint32_t ReadISOFileToBuffer(const char* iso, const char* iso_file, uint8_t** buf)
{
	ssize_t read_size;
	int64_t file_length;
	uint32_t ret = 0, nblocks;
	iso9660_t* p_iso = NULL;
	udf_t* p_udf = NULL;
	udf_dirent_t *p_udf_root = NULL, *p_udf_file = NULL;
	iso9660_stat_t* p_statbuf = NULL;

	*buf = NULL;
	cdio_loglevel_default = CDIO_LOG_WARN;

	// First try to open as UDF - fallback to ISO if it failed
	p_udf = udf_open(iso);
	if (p_udf == NULL)
		goto try_iso;
	p_udf_root = udf_get_root(p_udf, true, 0);
	if (p_udf_root == NULL) {
		uprintf("Could not locate UDF root directory");
		goto out;
	}
	p_udf_file = udf_fopen(p_udf_root, iso_file);
	if (!p_udf_file) {
		uprintf("Could not locate file %s in ISO image", iso_file);
		goto out;
	}
	file_length = udf_get_file_length(p_udf_file);
	if (file_length > 1 * GB) {
		uprintf("Only files smaller than 1 GB are supported");
		goto out;
	}
	nblocks = (uint32_t)((file_length + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE);
	*buf = malloc(nblocks * UDF_BLOCKSIZE + 1);
	if (*buf == NULL) {
		uprintf("Could not allocate buffer for file %s", iso_file);
		goto out;
	}
	read_size = udf_read_block(p_udf_file, *buf, nblocks);
	if (read_size < 0 || read_size != file_length) {
		uprintf("Error reading UDF file %s", iso_file);
		goto out;
	}
	ret = (uint32_t)file_length;
	(*buf)[ret] = 0;
	goto out;

try_iso:
	// Make sure to enable extensions, else we may not match the name of the file we are looking
	// for since Rock Ridge may be needed to translate something like 'I386_PC' into 'i386-pc'...
	p_iso = iso9660_open_ext(iso, ISO_EXTENSION_MASK);
	if (p_iso == NULL) {
		uprintf("Unable to open image '%s'", iso);
		goto out;
	}
	p_statbuf = iso9660_ifs_stat_translate(p_iso, iso_file);
	if (p_statbuf == NULL) {
		uprintf("Could not get ISO-9660 file information for file %s", iso_file);
		goto out;
	}
	file_length = p_statbuf->total_size;
	if (file_length > 1 * GB) {
		uprintf("Only files smaller than 1 GB are supported");
		goto out;
	}
	// coverity[cast_overflow]
	nblocks = (uint32_t)((file_length + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE);
	*buf = malloc(nblocks * ISO_BLOCKSIZE + 1);
	if (*buf == NULL) {
		uprintf("Could not allocate buffer for file %s", iso_file);
		goto out;
	}
	if (iso9660_iso_seek_read(p_iso, *buf, p_statbuf->lsn, nblocks) != nblocks * ISO_BLOCKSIZE) {
		uprintf("Error reading ISO file %s", iso_file);
		goto out;
	}
	ret = (uint32_t)file_length;
	(*buf)[ret] = 0;

out:
	iso9660_stat_free(p_statbuf);
	udf_dirent_free(p_udf_root);
	udf_dirent_free(p_udf_file);
	iso9660_close(p_iso);
	udf_close(p_udf);
	cdio_loglevel_default = enable_debug ? CDIO_LOG_INFO : CDIO_LOG_WARN;
	if (ret == 0)
		safe_free(*buf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* libfat based access to FAT images embedded on an ISO                */
/* ------------------------------------------------------------------ */
// PORT: upstream code, over the vendored libfat (compiled for Syslinux)
#define ISO_NB_BLOCKS 16
typedef struct {
	iso9660_t*      p_iso;
	lsn_t           lsn;
	libfat_sector_t sec_start;
	// Use a multi block buffer, to improve sector reads
	uint8_t         buf[ISO_BLOCKSIZE * ISO_NB_BLOCKS];
} iso9660_readfat_private;

/*
 * Read sectors from a FAT img file residing on an ISO-9660 filesystem.
 * NB: This assumes that the img file sectors are contiguous on the ISO.
  */
int iso9660_readfat(intptr_t pp, void *buf, size_t secsize, libfat_sector_t sec)
{
	iso9660_readfat_private* p_private = (iso9660_readfat_private*)pp;

	if (sizeof(p_private->buf) % secsize != 0) {
		uprintf("iso9660_readfat: Sector size %zu is not a divisor of %zu", secsize, sizeof(p_private->buf));
		return 0;
	}

	if ((sec < p_private->sec_start) || (sec >= p_private->sec_start + sizeof(p_private->buf) / secsize)) {
		// Sector being queried is not in our multi block buffer -> Update it
		p_private->sec_start = (((sec * secsize) / ISO_BLOCKSIZE) * ISO_BLOCKSIZE) / secsize;
		if (iso9660_iso_seek_read(p_private->p_iso, p_private->buf,
			p_private->lsn + (lsn_t)((p_private->sec_start * secsize) / ISO_BLOCKSIZE), ISO_NB_BLOCKS)
			!= ISO_NB_BLOCKS * ISO_BLOCKSIZE) {
			uprintf("Error reading ISO-9660 file %s at LSN %lu", img_report.efi_img_path,
				(long unsigned int)(p_private->lsn + (p_private->sec_start * secsize) / ISO_BLOCKSIZE));
			return 0;
		}
	}
	memcpy(buf, &p_private->buf[(sec - p_private->sec_start)*secsize], secsize);
	return (int)secsize;
}

/*
 * Returns TRUE if an EFI bootloader exists in the img.
 */
BOOL HasEfiImgBootLoaders(void* iso)
{
	BOOL ret = FALSE;
	iso9660_t* p_iso = (iso9660_t*)iso;
	iso9660_stat_t* p_statbuf = NULL;
	iso9660_readfat_private* p_private = NULL;
	int32_t dc, c;
	struct libfat_filesystem *lf_fs = NULL;
	struct libfat_direntry direntry;
	char bootloader_name[16];
	int i;

	if ((p_iso == NULL) || !HAS_EFI_IMG(img_report))
		return FALSE;

	p_statbuf = iso9660_ifs_stat_translate(p_iso, img_report.efi_img_path);
	if (p_statbuf == NULL) {
		uprintf("Could not get ISO-9660 file information for file %s", img_report.efi_img_path);
		goto out;
	}
	p_private = malloc(sizeof(iso9660_readfat_private));
	if (p_private == NULL)
		goto out;
	p_private->p_iso = p_iso;
	p_private->lsn = p_statbuf->lsn;
	p_private->sec_start = 0;
	// Populate our initial buffer
	if (iso9660_iso_seek_read(p_private->p_iso, p_private->buf, p_private->lsn, ISO_NB_BLOCKS) != ISO_NB_BLOCKS * ISO_BLOCKSIZE) {
		uprintf("Error reading ISO-9660 file %s at LSN %lu", img_report.efi_img_path, (long unsigned int)p_private->lsn);
		goto out;
	}
	// Try to skip to first FAT partition, if working with an MBR partitioned image
	if (p_private->buf[0x1fe] == 0x55 && p_private->buf[0x1ff] == 0xaa &&
		p_private->buf[0x1be] == 0x80 && IS_FAT_TYPE(p_private->buf[0x1c2])) {
		uint32_t lba = *((uint32_t*)&p_private->buf[0x1c6]);
		if (lba % 4 != 0) {
			uprintf("Error: First MBR partition doesn't map to ISO-9660 sector");
			goto out;
		}
		p_private->lsn += lba / 4;
		if (iso9660_iso_seek_read(p_private->p_iso, p_private->buf, p_private->lsn, ISO_NB_BLOCKS) != ISO_NB_BLOCKS * ISO_BLOCKSIZE) {
			uprintf("Error reading ISO-9660 file %s at LSN %lu", img_report.efi_img_path, (long unsigned int)p_private->lsn);
			goto out;
		}
	}
	lf_fs = libfat_open(iso9660_readfat, (intptr_t)p_private);
	if (lf_fs == NULL) {
		uprintf("FAT access error");
		goto out;
	}

	// Navigate to /EFI/BOOT
	if (libfat_searchdir(lf_fs, 0, "EFI        ", &direntry) < 0)
		goto out;
	dc = direntry.entry[26] + (direntry.entry[27] << 8);
	if (libfat_searchdir(lf_fs, dc, "BOOT       ", &direntry) < 0)
		goto out;
	dc = direntry.entry[26] + (direntry.entry[27] << 8);

	for (i = 1; i < ARRAYSIZE(efi_archname); i++) {
		// We consider it unlikely that any bootri#####.efi or bootlo#####.efi files
		// in the /efi/boot/ subdirectory will be anything but 'bootriscv64.efi' and
		// 'bootloongarch64.efi', so we'll use the ~1 LFN shortened names for them.
		static_sprintf(bootloader_name, "BOOT%c%c%c%cEFI", efi_archname[i][0], efi_archname[i][1],
			strlen(efi_archname[i]) > 4 ? '~' : efi_archname[i][2],
			strlen(efi_archname[i]) > 4 ? '1' : (strlen(efi_archname[i]) > 3 ? efi_archname[i][3] : ' '));
		safe_strtoupper(bootloader_name);
		c = libfat_searchdir(lf_fs, dc, bootloader_name, &direntry);
		if (c > 0) {
			if (!ret)
				uprintf("  Detected EFI bootloader(s) (from '%s'):", img_report.efi_img_path);
			uprintf("  ● 'boot%s.efi'", efi_archname[i]);
			ret = TRUE;
		}
	}

out:
	if (lf_fs != NULL)
		libfat_close(lf_fs);
	iso9660_stat_free(p_statbuf);
	safe_free(p_private);
	return ret;
}

BOOL DumpFatDir(void* iso, const char* path, int32_t cluster)
{
	// We don't have concurrent calls to this function, so a static lf_fs is fine
	static struct libfat_filesystem *lf_fs = NULL;
	void* buf;
	char *target = NULL, *name = NULL;
	BOOL ret = FALSE;
	HANDLE handle = NULL;
	DWORD size, written;
	libfat_diritem_t diritem = { 0 };
	libfat_dirpos_t dirpos = { cluster, -1, 0 };
	libfat_sector_t s;
	iso9660_t* p_iso = (iso9660_t*)iso;
	iso9660_stat_t* p_statbuf = NULL;
	iso9660_readfat_private* p_private = NULL;

	if (path == NULL)
		return -1;

	if (cluster == 0) {
		// Root dir => Perform init stuff
		if (iso == NULL || image_path[0] == 0)
			return FALSE;
		p_statbuf = iso9660_ifs_stat_translate(p_iso, img_report.efi_img_path);
		if (p_statbuf == NULL) {
			uprintf("Could not get ISO-9660 file information for file %s", img_report.efi_img_path);
			goto out;
		}
		p_private = malloc(sizeof(iso9660_readfat_private));
		if (p_private == NULL)
			goto out;
		p_private->p_iso = p_iso;
		p_private->lsn = p_statbuf->lsn;
		p_private->sec_start = 0;
		// Populate our initial buffer
		if (iso9660_iso_seek_read(p_private->p_iso, p_private->buf, p_private->lsn, ISO_NB_BLOCKS) != ISO_NB_BLOCKS * ISO_BLOCKSIZE) {
			uprintf("Error reading ISO-9660 file %s at LSN %lu", img_report.efi_img_path, (long unsigned int)p_private->lsn);
			goto out;
		}
		// Try to skip to first FAT partition, if working with an MBR partitioned image
		if (p_private->buf[0x1fe] == 0x55 && p_private->buf[0x1ff] == 0xaa &&
			p_private->buf[0x1be] == 0x80 && IS_FAT_TYPE(p_private->buf[0x1c2])) {
			p_private->lsn += *((uint32_t*)&p_private->buf[0x1c6]) / 4;
			if (iso9660_iso_seek_read(p_private->p_iso, p_private->buf, p_private->lsn, ISO_NB_BLOCKS) != ISO_NB_BLOCKS * ISO_BLOCKSIZE) {
				uprintf("Error reading ISO-9660 file %s at LSN %lu", img_report.efi_img_path, (long unsigned int)p_private->lsn);
				goto out;
			}
		}
		lf_fs = libfat_open(iso9660_readfat, (intptr_t)p_private);
		if (lf_fs == NULL) {
			uprintf("FAT access error");
			goto out;
		}
	}

	do {
		dirpos.cluster = libfat_dumpdir(lf_fs, &dirpos, &diritem);
		if (dirpos.cluster >= 0) {
			name = wchar_to_utf8(diritem.name);
			target = malloc(strlen(path) + safe_strlen(name) + 2);
			if ((name == NULL) || (target == NULL)) {
				uprintf("Could not allocate buffer");
				goto out;
			}
			strcpy(target, path);
			strcat(target, "/");	// PORT: POSIX separator
			strcat(target, name);
			if (diritem.attributes & 0x10) {
				// Directory => Create directory
				if (!CreateDirectoryU(target, 0) && (GetLastError() != ERROR_ALREADY_EXISTS)) {
					uprintf("Could not create directory '%s': %s", target, WindowsErrorString());
					continue;
				}
				if (!DumpFatDir(p_iso, target, dirpos.cluster))
					goto out;
			} else if (!PathFileExistsU(target)) {
				// Need to figure out if it's a .conf file (Damn you Solus!!)
				EXTRACT_PROPS props = { 0 };
				size_t len = strlen(name);
				props.is_conf = ((len > 4) && (strcasecmp(&name[len - 5], ".conf") == 0));
				uprintf("Extracting: %s (from '%s', %s)", target, img_report.efi_img_path,
					SizeToHumanReadable(diritem.size, FALSE, FALSE));
				handle = CreateFileU(target, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
					NULL, CREATE_ALWAYS, diritem.attributes, NULL);
				if (handle == INVALID_HANDLE_VALUE) {
					uprintf("Could not create '%s': %s", target, WindowsErrorString());
					continue;
				}

				written = 0;
				s = libfat_clustertosector(lf_fs, dirpos.cluster);
				while ((s != 0) && (s < 0xFFFFFFFFULL) && (written < diritem.size)) {
					buf = libfat_get_sector(lf_fs, s);
					if (buf == NULL)
						ErrorStatus = RUFUS_ERROR(ERROR_SECTOR_NOT_FOUND);
					if (IS_ERROR(ErrorStatus))
						goto out;
					size = MIN(LIBFAT_SECTOR_SIZE, diritem.size - written);
					if (!WriteFileWithRetry(handle, buf, size, &size, WRITE_RETRIES) ||
						(size != MIN(LIBFAT_SECTOR_SIZE, diritem.size - written))) {
						uprintf("Could not write '%s': %s", target, WindowsErrorString());
						break;
					}
					written += size;
					s = libfat_nextsector(lf_fs, s);
					// Trust me, you *REALLY* want to invoke libfat_flush() here
					libfat_flush(lf_fs);
				}
				safe_closehandle(handle);
				if (props.is_conf)
					fix_config(target, NULL, NULL, &props);
			}
			safe_free(target);
			safe_free(name);
		}
	// coverity[tainted_data]
	} while (dirpos.cluster >= 0);
	ret = TRUE;

out:
	if (cluster == 0) {
		if (lf_fs != NULL) {
			libfat_close(lf_fs);
			lf_fs = NULL;
		}
		iso9660_stat_free(p_statbuf);
		safe_free(p_private);
	}
	safe_closehandle(handle);
	safe_free(name);
	safe_free(target);
	return ret;
}

static DWORD WINAPI OpticalDiscSaveImageThread(void* param)
{
	// TODO(vhd): Implement dumping an optical disc to an ISO image
	IGNORE_RETVAL(param);
	uprintf("Saving an optical disc to an ISO image is not implemented yet");
	ErrorStatus = RUFUS_ERROR(ERROR_CALL_NOT_IMPLEMENTED);
	return ERROR_CALL_NOT_IMPLEMENTED;
}

void OpticalDiscSaveImage(void)
{
	// NB: Referencing the stub thread also prevents an unused function warning
	IGNORE_RETVAL(OpticalDiscSaveImageThread);
	// TODO(vhd): Implement dumping an optical disc to an ISO image
	uprintf("Saving an optical disc to an ISO image is not implemented yet");
	ErrorStatus = RUFUS_ERROR(ERROR_CALL_NOT_IMPLEMENTED);
}

// Create an ISO image from the currently selected drive (upstream uses oscdimg.exe)
DWORD WINAPI IsoSaveImageThread(void* param)
{
	// TODO(vhd): Implement creating an ISO image from a drive
	IGNORE_RETVAL(param);
	uprintf("Creating an ISO image from a drive is not implemented yet");
	ErrorStatus = RUFUS_ERROR(ERROR_CALL_NOT_IMPLEMENTED);
	return ERROR_CALL_NOT_IMPLEMENTED;
}

BOOL IsoSaveImage(IMG_SAVE* img_save)
{
	// TODO(vhd): Implement creating an ISO image from a drive
	IGNORE_RETVAL(img_save);
	uprintf("Creating an ISO image from a drive is not implemented yet");
	ErrorStatus = RUFUS_ERROR(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}
