/*
 * Rufus: The Reliable USB Formatting Utility
 * Elementary Unicode compliant find/replace parser
 * Copyright © 2012-2025 Pete Batard <pete@akeo.ie>
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
 * The string helpers are upstream's (parser.c), ListDirectoryContent() is
 * from upstream stdio.c. insert_section_data() is reimplemented over byte
 * buffers: upstream relies on the Windows CRT's "ccs=UNICODE" text mode to
 * transparently read/write ANSI, UTF-8 and UTF-16LE files, which glibc does
 * not have. The behavior (BOM preserving, section match at the start of a
 * line, case insensitive, optional CR removal) is the same.
 */

#include "rfus.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Convert a UTF-16LE buffer (without BOM) to UTF-8, and back */
static char* utf16le_to_utf8(const uint8_t* buf, size_t len)
{
	size_t i, j = 0;
	char* out = malloc(len * 2 + 1);

	if (out == NULL)
		return NULL;
	for (i = 0; i + 1 < len; i += 2) {
		uint32_t c = buf[i] | (buf[i + 1] << 8);
		if ((c >= 0xD800) && (c < 0xDC00) && (i + 3 < len)) {
			uint32_t c2 = buf[i + 2] | (buf[i + 3] << 8);
			c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
			i += 2;
		}
		if (c < 0x80) {
			out[j++] = (char)c;
		} else if (c < 0x800) {
			out[j++] = (char)(0xC0 | (c >> 6));
			out[j++] = (char)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			out[j++] = (char)(0xE0 | (c >> 12));
			out[j++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[j++] = (char)(0x80 | (c & 0x3F));
		} else {
			out[j++] = (char)(0xF0 | (c >> 18));
			out[j++] = (char)(0x80 | ((c >> 12) & 0x3F));
			out[j++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[j++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[j] = 0;
	return out;
}

static size_t utf8_to_utf16le(const char* str, uint8_t* out)
{
	size_t i = 0, j = 0, len = strlen(str);

	while (i < len) {
		uint32_t c = (uint8_t)str[i];
		if (c < 0x80) {
			i++;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
			c = ((c & 0x1F) << 6) | (str[i + 1] & 0x3F);
			i += 2;
		} else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
			c = ((c & 0x0F) << 12) | ((str[i + 1] & 0x3F) << 6) | (str[i + 2] & 0x3F);
			i += 3;
		} else if (i + 3 < len) {
			c = ((c & 0x07) << 18) | ((str[i + 1] & 0x3F) << 12) | ((str[i + 2] & 0x3F) << 6) | (str[i + 3] & 0x3F);
			i += 4;
		} else {
			break;
		}
		if (c >= 0x10000) {
			c -= 0x10000;
			out[j++] = (uint8_t)((0xD800 + (c >> 10)) & 0xFF);
			out[j++] = (uint8_t)((0xD800 + (c >> 10)) >> 8);
			c = 0xDC00 + (c & 0x3FF);
		}
		out[j++] = (uint8_t)(c & 0xFF);
		out[j++] = (uint8_t)(c >> 8);
	}
	return j;
}

/*
 * Insert entry 'data' under section 'section' of a config file
 * Section must include the relevant delimiters (eg '[', ']') if needed
 */
char* insert_section_data(const char* filename, const char* section, const char* data, BOOL dos2unix)
{
	uint8_t* raw = NULL;
	char *text = NULL, *out = NULL, *ret = NULL, *line, *next;
	size_t out_len = 0, out_cap, section_len, data_len;
	uint32_t raw_len;
	int mode = 0;	/* 0: ANSI/UTF-8, 1: UTF-8 with BOM, 2: UTF-16LE with BOM */
	FILE* fd;

	if ((filename == NULL) || (section == NULL) || (data == NULL))
		return NULL;
	if ((filename[0] == 0) || (section[0] == 0) || (data[0] == 0))
		return NULL;

	raw_len = read_file(filename, &raw);
	if (raw_len == 0) {
		uprintf("Could not read file '%s'", filename);
		return NULL;
	}
	if ((raw_len >= 2) && (raw[0] == 0xFF) && (raw[1] == 0xFE)) {
		mode = 2;
		text = utf16le_to_utf8(&raw[2], raw_len - 2);
	} else {
		if ((raw_len >= 3) && (raw[0] == 0xEF) && (raw[1] == 0xBB) && (raw[2] == 0xBF))
			mode = 1;
		text = malloc(raw_len + 1);
		if (text != NULL) {
			memcpy(text, &raw[(mode == 1) ? 3 : 0], raw_len - ((mode == 1) ? 3 : 0));
			text[raw_len - ((mode == 1) ? 3 : 0)] = 0;
		}
	}
	if (text == NULL)
		goto out;

	section_len = strlen(section);
	data_len = strlen(data);
	out_cap = strlen(text) * 2 + data_len * 4 + 16;
	out = malloc(out_cap);
	if (out == NULL)
		goto out;

	/* Process individual lines */
	for (line = text; *line != 0; line = next) {
		size_t i = strspn(line, " \t");
		next = strchr(line, '\n');
		next = (next == NULL) ? &line[strlen(line)] : &next[1];
		if ((out_len + (next - line) + data_len + 4) >= out_cap)
			break;
		memcpy(&out[out_len], line, next - line);
		out_len += next - line;
		/* Our token should begin a line */
		if (strncasecmp(&line[i], section, section_len) != 0)
			continue;
		/* Section was found: output the new data (in Windows text mode) */
		if ((out_len > 0) && (out[out_len - 1] != '\n')) {
			out[out_len++] = '\r';
			out[out_len++] = '\n';
		}
		memcpy(&out[out_len], data, data_len);
		out_len += data_len;
		if (!dos2unix)
			out[out_len++] = '\r';
		out[out_len++] = '\n';
		ret = (char*)data;
	}
	out[out_len] = 0;

	/* If an insertion occurred, overwrite the existing file */
	if (ret != NULL) {
		size_t i, j;
		/* Remove CRs if requested */
		if (dos2unix) {
			for (i = 0, j = 0; i < out_len; i++)
				if (out[i] != '\r')
					out[j++] = out[i];
			out_len = j;
			out[out_len] = 0;
		}
		fd = fopen(filename, "wb");
		if (fd == NULL) {
			uprintf("Could not write '%s' - original file has been left unmodified", filename);
			ret = NULL;
			goto out;
		}
		if (mode == 2) {
			uint8_t* w = malloc(out_len * 2 + 2);
			if (w != NULL) {
				size_t wl;
				w[0] = 0xFF; w[1] = 0xFE;
				wl = utf8_to_utf16le(out, &w[2]);
				fwrite(w, 1, wl + 2, fd);
				free(w);
			}
		} else {
			if (mode == 1)
				fwrite("\xEF\xBB\xBF", 1, 3, fd);
			fwrite(out, 1, out_len, fd);
		}
		fclose(fd);
	}

out:
	free(raw);
	free(text);
	free(out);
	return ret;
}

/*
 * Replace all 'c' characters in string 'src' with the substring 'rep'
 * The returned string is allocated and must be freed by the caller.
 */
char* replace_char(const char* src, const char c, const char* rep)
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
 * Replace all characters from string 'str' that are present in the array of chars 'rem'
 * to the 'rep' character.
 */
void filter_chars(char* str, const char* rem, const char rep)
{
	char *p, *q;

	if (str == NULL || rem == NULL)
		return;
	for (p = str; *p != '\0'; p++) {
		for (q = (char*)rem; *q != '\0'; q++)
			if (*p == *q)
				*p = rep;
	}
}

/*
 * Remove all instances of substring 'sub' form string 'src.
 * The returned string is allocated and must be freed by the caller.
 */
char* remove_substr(const char* src, const char* sub)
{
	size_t i, j, str_len = safe_strlen(src), sub_len = safe_strlen(sub);
	char* res;

	if ((src == NULL) || (sub == NULL) || (sub_len > str_len))
		return NULL;

	res = (char*)calloc(str_len + 1, 1);
	if (res == NULL)
		return NULL;
	for (i = 0, j = 0; i <= str_len; ) {
		if (i <= str_len - sub_len && memcmp(&src[i], sub, sub_len) == 0)
			i += sub_len;
		else
			res[j++] = src[i++];
	}
	return res;
}

/*
 * List the files (and/or directories) of a directory. Directories get a
 * trailing separator. Returns NO_ERROR, ERROR_FILE_NOT_FOUND if no file
 * was found, or an error code. (Upstream stdio.c)
 */
DWORD ListDirectoryContent(StrArray* arr, char* dir, uint8_t type)
{
	DIR* d;
	struct dirent* de;
	struct stat st;
	char path[MAX_PATH + 1];
	DWORD dwError, dwResult = ERROR_FILE_NOT_FOUND;
	size_t len;

	if (arr == NULL || dir == NULL || (type & 0x03) == 0)
		return ERROR_INVALID_PARAMETER;
	d = opendir(dir);
	if (d == NULL)
		return ERROR_PATH_NOT_FOUND;
	len = strlen(dir);
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof(path), "%s%s%s", dir, (len > 0 && dir[len - 1] == '/') ? "" : "/", de->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if ((type & LIST_DIR_TYPE_RECURSIVE) == 0)
				continue;
			static_strcat(path, "/");
			if (type & LIST_DIR_TYPE_DIRECTORY)
				StrArrayAdd(arr, path, TRUE);
			dwError = ListDirectoryContent(arr, path, type);
			if (dwError != NO_ERROR && dwError != ERROR_FILE_NOT_FOUND) {
				closedir(d);
				return dwError;
			}
		} else if (type & LIST_DIR_TYPE_FILE) {
			StrArrayAdd(arr, path, TRUE);
			dwResult = NO_ERROR;
		}
	}
	closedir(d);
	return dwResult;
}
