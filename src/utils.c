/*
 * Rufus Linux port: shared utilities.
 *
 * Port of the portable parts of upstream stdio.c (size/timestamp formatting,
 * GUID conversions, error strings, file helpers) plus a few Linux-specific
 * helpers used by the new modules.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"
#include <wchar.h>
#include "localization.h"

#include <sys/time.h>
#include <unistd.h>
#include <limits.h>

#define LEFT_TO_RIGHT_MARK   "\xe2\x80\x8e"
#define RIGHT_TO_LEFT_MARK   "\xe2\x80\x8f"
#define MAX_SIZE_SUFFIXES    6       // bytes, KB, MB, GB, TB, PB

/* ------------------------------------------------------------------ */
/* Time helpers                                                        */
/* ------------------------------------------------------------------ */
int64_t GetTime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void FormatTime(char* buf, size_t len, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y.%m.%d %H:%M:%S", &tm);
}

/* realpath() into a newly allocated buffer (caller frees).
 * glibc's fortified realpath() aborts unless the destination is PATH_MAX
 * bytes, so the port never calls realpath() with a smaller buffer. */
char* realpath_dup(const char* path)
{
    char buf[PATH_MAX];
    if (path == NULL || path[0] == 0)
        return NULL;
    if (realpath(path, buf) == NULL)
        return NULL;
    return strdup(buf);
}

/* ------------------------------------------------------------------ */
/* UTF-8 <-> wchar_t (UTF-32) conversions                              */
/* ------------------------------------------------------------------ */
size_t utf8_to_wchar_no_alloc(const char* src, wchar_t* dst, size_t dst_count)
{
    size_t i = 0, o = 0;
    if (src == NULL || dst == NULL || dst_count == 0)
        return 0;
    while (src[i] != 0 && o + 1 < dst_count) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp;
        size_t extra;
        if (c < 0x80)            { cp = c;          extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { i++; continue; }  /* invalid byte: skip */
        if (i + extra >= strlen(src) + 1) {
            /* truncated sequence: decode as much as possible */
            extra = (size_t)(strlen(src) - i - 1);
        }
        for (size_t j = 0; j < extra; j++) {
            unsigned char cc = (unsigned char)src[i + 1 + j];
            if ((cc & 0xC0) != 0x80)
                break;
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += extra + 1;
        dst[o++] = (wchar_t)cp;
    }
    dst[o] = 0;
    return o + 1;
}

wchar_t* utf8_to_wchar(const char* str)
{
    wchar_t* w;
    if (str == NULL)
        return NULL;
    w = (wchar_t*)calloc(strlen(str) + 1, sizeof(wchar_t));
    if (w == NULL)
        return NULL;
    utf8_to_wchar_no_alloc(str, w, strlen(str) + 1);
    return w;
}

size_t wchar_to_utf8_no_alloc(const wchar_t* src, char* dst, size_t dst_size)
{
    size_t i = 0, o = 0;
    if (src == NULL || dst == NULL || dst_size == 0)
        return 0;
    while (src[i] != 0) {
        uint32_t cp = (uint32_t)src[i++];
        char tmp[4];
        size_t n;
        if (cp < 0x80)              { tmp[0] = (char)cp; n = 1; }
        else if (cp < 0x800)        { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
        else if (cp < 0x10000)      { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
        else                       { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
        if (o + n + 1 > dst_size)
            break;
        memcpy(dst + o, tmp, n);
        o += n;
    }
    dst[o] = 0;
    return o + 1;
}

BOOL IsElevated(void)
{
    return (geteuid() == 0);
}

const char* GetLastErrorString(void)
{
    return WindowsErrorString();
}

/* ------------------------------------------------------------------ */
/* GUID helpers (upstream stdio.c:356)                                 */
/* ------------------------------------------------------------------ */
#define MAX_GUID_STRING_LENGTH  40

char* GuidToString(const GUID* guid, BOOL bDecorated)
{
    static char guid_string[MAX_GUID_STRING_LENGTH];

    if (guid == NULL) return NULL;
    snprintf(guid_string, sizeof(guid_string),
             bDecorated ? "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}"
                        : "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
             (uint32_t)guid->Data1, guid->Data2, guid->Data3,
             guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
             guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
    return guid_string;
}

GUID StringToGuid(const char* str)
{
    GUID guid = { 0 };
    uint32_t d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7;
    if (str != NULL && sscanf(str[0] == '{' ? &str[1] : str,
        "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &d1, &d2, &d3, &b0, &b1, &b2, &b3, &b4, &b5, &b6, &b7) == 11) {
        guid.Data1 = d1;
        guid.Data2 = (uint16_t)d2;
        guid.Data3 = (uint16_t)d3;
        guid.Data4[0] = (uint8_t)b0;
        guid.Data4[1] = (uint8_t)b1;
        guid.Data4[2] = (uint8_t)b2;
        guid.Data4[3] = (uint8_t)b3;
        guid.Data4[4] = (uint8_t)b4;
        guid.Data4[5] = (uint8_t)b5;
        guid.Data4[6] = (uint8_t)b6;
        guid.Data4[7] = (uint8_t)b7;
    }
    return guid;
}

BOOL GetGuidFromString(const char* str, GUID* guid)
{
    GUID parsed;
    if (str == NULL || guid == NULL)
        return FALSE;
    parsed = StringToGuid(str);
    if (parsed.Data1 == 0 && parsed.Data2 == 0 && parsed.Data3 == 0)
        return FALSE;
    *guid = parsed;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Size / timestamp formatting (upstream stdio.c:403)                  */
/* ------------------------------------------------------------------ */
static __inline uint16_t upo2(uint16_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    return ++v;
}

/* Fetch the localized unit suffix using the requested locale mode */
static const char* size_suffix(int suffix, BOOL copy_to_log)
{
    static char unit[64];
    BOOL saved = en_msg_mode;
    if (copy_to_log)
        en_msg_mode = TRUE;
    snprintf(unit, sizeof(unit), "%s", lmprintf(MSG_020 + suffix));
    en_msg_mode = saved;
    return unit;
}

char* SizeToHumanReadable(uint64_t size, BOOL copy_to_log, BOOL fake_units)
{
    int suffix;
    static char str_size[40];
    const char* dir = ((right_to_left_mode) && (!copy_to_log)) ? LEFT_TO_RIGHT_MARK : "";
    double hr_size = (double)size;
    double t;
    uint16_t i_size;
    const double divider = fake_units ? 1000.0 : 1024.0;

    for (suffix = 0; suffix < MAX_SIZE_SUFFIXES - 1; suffix++) {
        if (hr_size < divider)
            break;
        hr_size /= divider;
    }
    if (suffix == 0) {
        snprintf(str_size, sizeof(str_size), "%s%d%s %s",
                 dir, (int)hr_size, dir, size_suffix(0, copy_to_log));
    } else if (fake_units) {
        if (hr_size < 8) {
            snprintf(str_size, sizeof(str_size),
                     (fabs((hr_size * 10.0) - (floor(hr_size + 0.5) * 10.0)) < 0.5) ?
                     "%0.0f%s" : "%0.1f%s",
                     hr_size, size_suffix(suffix, copy_to_log));
        } else {
            t = (double)upo2((uint16_t)hr_size);
            i_size = (uint16_t)((fabs(1.0f - (hr_size / t)) < 0.05f) ? t : hr_size);
            snprintf(str_size, sizeof(str_size), "%s%d%s %s",
                     dir, i_size, dir, size_suffix(suffix, copy_to_log));
        }
    } else {
        snprintf(str_size, sizeof(str_size),
                 (hr_size * 10.0 - (floor(hr_size) * 10.0)) < 0.5 ?
                 "%s%0.0f%s %s" : "%s%0.1f%s %s",
                 dir, hr_size, dir, size_suffix(suffix, copy_to_log));
    }
    return str_size;
}

// Convert a YYYYMMDDHHMMSS UTC timestamp to a more human readable version
char* TimestampToHumanReadable(uint64_t ts)
{
    uint64_t rem = ts, divisor = 10000000000ULL;
    uint16_t data[6];
    int i;
    static char str[64];

    for (i = 0; i < 6; i++) {
        data[i] = (uint16_t)((divisor == 0) ? rem : (rem / divisor));
        rem %= divisor;
        divisor /= 100ULL;
    }
    snprintf(str, sizeof(str), "%04d.%02d.%02d %02d:%02d:%02d (UTC)",
             data[0], data[1], data[2], data[3], data[4], data[5]);
    return str;
}

double FtoU64_impl(double f)
{
    (void)f;
    return 0.0;
}

uint64_t FtoU64(double f)
{
    return (uint64_t)f;
}

/* ------------------------------------------------------------------ */
/* Error strings (upstream stdio.c:455)                                */
/* ------------------------------------------------------------------ */
const char* _StrError(DWORD error_code)
{
    if ((!IS_ERROR(error_code)) || (SCODE_CODE(error_code) == ERROR_SUCCESS)) {
        return lmprintf(MSG_050);
    }
    if (SCODE_FACILITY(error_code) != FACILITY_STORAGE) {
        SetLastError(error_code);
        return WindowsErrorString();
    }
    switch (SCODE_CODE(error_code)) {
    case ERROR_GEN_FAILURE:
        return lmprintf(MSG_051);
    case ERROR_INCOMPATIBLE_FS:
        return lmprintf(MSG_052);
    case ERROR_ACCESS_DENIED:
        return lmprintf(MSG_053);
    case ERROR_WRITE_PROTECT:
        return lmprintf(MSG_054);
    case ERROR_DEVICE_IN_USE:
        return lmprintf(MSG_055);
    case ERROR_CANT_QUICK_FORMAT:
        return lmprintf(MSG_056);
    case ERROR_LABEL_TOO_LONG:
        return lmprintf(MSG_057);
    case ERROR_INVALID_HANDLE:
        return lmprintf(MSG_058);
    case ERROR_INVALID_CLUSTER_SIZE:
        return lmprintf(MSG_059);
    case ERROR_INVALID_VOLUME_SIZE:
        return lmprintf(MSG_060);
    case ERROR_NO_MEDIA_IN_DRIVE:
        return lmprintf(MSG_061);
    case ERROR_NOT_SUPPORTED:
        return lmprintf(MSG_062);
    case ERROR_NOT_ENOUGH_MEMORY:
        return lmprintf(MSG_063);
    case ERROR_READ_FAULT:
        return lmprintf(MSG_064);
    case ERROR_WRITE_FAULT:
        return lmprintf(MSG_065);
    case ERROR_INSTALL_FAILURE:
        return lmprintf(MSG_066);
    case ERROR_OPEN_FAILED:
        return lmprintf(MSG_067);
    case ERROR_PARTITION_FAILURE:
        return lmprintf(MSG_068);
    case ERROR_CANNOT_COPY:
        return lmprintf(MSG_069);
    case ERROR_CANCELLED:
        return lmprintf(MSG_070);
    case ERROR_CANT_START_THREAD:
        return lmprintf(MSG_071);
    case ERROR_BADBLOCKS_FAILURE:
        return lmprintf(MSG_072);
    case ERROR_ISO_SCAN:
        return lmprintf(MSG_073);
    case ERROR_ISO_EXTRACT:
        return lmprintf(MSG_074);
    case ERROR_CANT_REMOUNT_VOLUME:
        return lmprintf(MSG_075);
    case ERROR_CANT_PATCH:
        return lmprintf(MSG_076);
    case ERROR_CANT_ASSIGN_LETTER:
        return lmprintf(MSG_077);
    case ERROR_CANT_MOUNT_VOLUME:
        return lmprintf(MSG_078);
    case ERROR_NOT_READY:
        return lmprintf(MSG_079);
    case ERROR_BAD_SIGNATURE:
        return lmprintf(MSG_172);
    case ERROR_CANT_DOWNLOAD:
        return lmprintf(MSG_242);
    default:
        SetLastError(error_code);
        return WindowsErrorString();
    }
}

const char* StrError(DWORD error_code, BOOL use_default_locale)
{
    const char* ret;
    if (use_default_locale)
        toggle_default_locale();
    ret = _StrError(error_code);
    if (use_default_locale)
        toggle_default_locale();
    return ret;
}

/* ------------------------------------------------------------------ */
/* File helpers (upstream stdio.c:159)                                 */
/* ------------------------------------------------------------------ */
uint32_t read_file(const char* path, uint8_t** buf)
{
    FILE* fd = fopen(path, "rb");
    uint32_t size;

    if (fd == NULL) {
        uprintf("Error: Can't open file '%s'", path);
        return 0;
    }

    fseek(fd, 0L, SEEK_END);
    size = (uint32_t)ftell(fd);
    fseek(fd, 0L, SEEK_SET);

    // +1 so we can add an extra NUL
    *buf = (uint8_t*)malloc(size + 1);
    if (*buf == NULL) {
        uprintf("Error: Can't allocate %d bytes buffer for file '%s'", size, path);
        size = 0;
        goto out;
    }
    if (fread(*buf, 1, size, fd) != size) {
        uprintf("Error: Can't read '%s'", path);
        size = 0;
    }
    // Always NUL terminate the file
    (*buf)[size] = 0;

out:
    fclose(fd);
    if (size == 0 && *buf != NULL) {
        free(*buf);
        *buf = NULL;
    }
    return size;
}

uint32_t write_file(const char* path, const uint8_t* buf, const uint32_t size)
{
    uint32_t written;
    FILE* fd = fopen(path, "wb");
    if (fd == NULL) {
        uprintf("Error: Can't create '%s'", path);
        return 0;
    }
    written = (uint32_t)fwrite(buf, 1, size, fd);
    if (written != size)
        uprintf("Error: Can't write '%s'", path);
    fclose(fd);
    return written;
}

/* ------------------------------------------------------------------ */
/* Debug dumps                                                         */
/* ------------------------------------------------------------------ */
char* _printbits(size_t const size, void const* const ptr, int leading_zeroes)
{
    static char str[256];
    unsigned char* b = (unsigned char*)ptr;
    unsigned char byte;
    int i, j, k = 0;
    static int size_lookup[8] = { 8, 9, 10, 11, 12, 14, 16, 32 };

    if ((size == 0) || (size > 8))
        return NULL;
    for (i = (int)size - 1; i >= 0; i--) {
        for (j = 7; j >= 0; j--) {
            if ((leading_zeroes) || ((i == (int)size - 1) && (j == size_lookup[size - 1] - 1)))
                leading_zeroes = TRUE;
            if (leading_zeroes) {
                byte = b[i] & (1 << j);
                byte >>= j;
                str[k++] = (byte) ? '1' : '0';
            }
        }
    }
    str[k] = 0;
    return str;
}

void DumpBufferHex(void* buf, size_t size)
{
    unsigned char* buffer = (unsigned char*)buf;
    size_t i, j;

    for (i = 0; i < size; i += 16) {
        uprintf("%08zx  ", i);
        for (j = 0; j < 16; j++) {
            if (i + j < size)
                uprintf("%02x ", buffer[i + j]);
            else
                uprintf("   ");
            if (j == 7)
                uprintf(" ");
        }
        uprintf(" |");
        for (j = 0; j < 16; j++) {
            if (i + j < size)
                uprintf("%c", (buffer[i + j] >= ' ') && (buffer[i + j] <= '~') ? buffer[i + j] : '.');
        }
        uprintf("|\n");
    }
}

/* ------------------------------------------------------------------ */
/* Database checks (no DB on Linux yet: everything is "not in DB")     */
/* ------------------------------------------------------------------ */
/* Upstream smart.c IsHDD(): heuristic score of a drive being an HDD/SSD
 * (> 0) rather than a flash drive (<= 0), from its type, size, name and
 * USB VID:PID, with upstream's tables (hdd_vs_ufd.h) */
#include "hdd_vs_ufd.h"
int IsHDD(DWORD DriveIndex, uint16_t vid, uint16_t pid, const char* strid)
{
	int score = 0;
	size_t i, mlen, ilen, score_list_size = 0;
	BOOL wc;
	uint64_t drive_size;
	int8_t score_list[16];
	char str[64] = { 0 };

	// Boost the score if fixed, as these are *generally* HDDs
	if (GetDriveTypeFromIndex(DriveIndex) == DRIVE_FIXED) {
		score_list[score_list_size] = 3;
		score += score_list[score_list_size++];
	}

	// Adjust the score depending on the size
	drive_size = GetDriveSize(DriveIndex);
	if (drive_size > 800 * GB) {
		score_list[score_list_size] = 15;
		score += score_list[score_list_size++];
		if (drive_size > 1800 * GB) {
			score_list[score_list_size] = 15;
			score += score_list[score_list_size++];
		}
	} else if (drive_size < 128 * GB) {
		score_list[score_list_size] = -15;
		score += score_list[score_list_size++];
	}

	// Check the string against well known HDD identifiers
	if (strid != NULL) {
		ilen = strlen(strid);
		for (i = 0; i < ARRAYSIZE(str_score); i++) {
			mlen = strlen(str_score[i].name);
			if (mlen > ilen)
				break;
			wc = (str_score[i].name[mlen-1] == '#');
			if ( (strncasecmp(strid, str_score[i].name, mlen-((wc)?1:0)) == 0)
			  && ((!wc) || ((strid[mlen] >= '0') && (strid[mlen] <= '9'))) ) {
				score_list[score_list_size] = str_score[i].score;
				score += score_list[score_list_size++];
				break;
			}
		}
	}

	// Adjust for oddball devices
	if (strid != NULL) {
		for (i = 0; i < ARRAYSIZE(str_adjust); i++)
			if (StrStrIA(strid, str_adjust[i].name) != NULL) {
				score_list[score_list_size] = str_adjust[i].score;
				score += score_list[score_list_size++];
			}
	}

	// Check against known VIDs
	for (i = 0; i < ARRAYSIZE(vid_score); i++) {
		if (vid == vid_score[i].vid) {
			score_list[score_list_size] = vid_score[i].score;
			score += score_list[score_list_size++];
			break;
		}
	}

	// Check against known VID:PIDs
	for (i = 0; i < ARRAYSIZE(vidpid_score); i++) {
		if ((vid == vidpid_score[i].vid) && (pid == vidpid_score[i].pid)) {
			score_list[score_list_size] = vidpid_score[i].score;
			score += score_list[score_list_size++];
			break;
		}
	}

	// Print a breakdown of the device score if requested
	if (usb_debug) {
		static_strcat(str, "Device score: ");
		for (i = 0; i < score_list_size; i++)
			safe_sprintf(&str[strlen(str)], sizeof(str) - strlen(str), "%+d", score_list[i]);
		uprintf("%s=%+d → Detected as %s", str, score, (score > 0) ? "HDD" : "UFD");
	}

	return score;
}

/* Linux has no drive letters; return an empty string */
const char* GetDriveLettersStr(void)
{
    return "";
}

/* ------------------------------------------------------------------ */
/* String arrays (port of upstream stdfn.c)                            */
/* ------------------------------------------------------------------ */
void StrArrayCreate(StrArray* arr, uint32_t initial_size)
{
    if (arr == NULL) return;
    arr->Max = initial_size; arr->Index = 0;
    arr->String = (char**)calloc(arr->Max, sizeof(char*));
    if (arr->String == NULL)
        uprintf("Could not allocate string array");
}

int32_t StrArrayAdd(StrArray* arr, const char* str, BOOL duplicate)
{
    char** old_table;
    if ((arr == NULL) || (arr->String == NULL) || (str == NULL))
        return -1;
    if (arr->Index == arr->Max) {
        arr->Max *= 2;
        old_table = arr->String;
        arr->String = (char**)realloc(arr->String, arr->Max * sizeof(char*));
        if (arr->String == NULL) {
            free(old_table);
            uprintf("Could not reallocate string array");
            return -1;
        }
    }
    arr->String[arr->Index] = (duplicate) ? safe_strdup(str) : (char*)str;
    if (arr->String[arr->Index] == NULL) {
        uprintf("Could not store string in array");
        return -1;
    }
    return (int32_t)arr->Index++;
}

int32_t StrArrayFind(StrArray* arr, const char* str)
{
    uint32_t i;
    if ((str == NULL) || (arr == NULL) || (arr->String == NULL))
        return -1;
    for (i = 0; i < arr->Index; i++) {
        if (strcmp(arr->String[i], str) == 0)
            return (int32_t)i;
    }
    return -1;
}

int32_t StrArrayAddUnique(StrArray* arr, const char* str, BOOL duplicate)
{
    int32_t i = StrArrayFind(arr, str);
    return (i < 0) ? StrArrayAdd(arr, str, duplicate) : i;
}

void StrArrayClear(StrArray* arr)
{
    uint32_t i;
    if ((arr == NULL) || (arr->String == NULL))
        return;
    for (i = 0; i < arr->Index; i++)
        safe_free(arr->String[i]);
    arr->Index = 0;
}

void StrArrayDestroy(StrArray* arr)
{
    StrArrayClear(arr);
    if (arr != NULL)
        safe_free(arr->String);
}

/* ------------------------------------------------------------------ */
/* Simple token parsing (port of upstream parser.c get_token_data_*)   */
/* ------------------------------------------------------------------ */

/* Return the data for 'token' if it starts the (UTF-8) line, else NULL */
static char* get_token_data_line(const char* token, char* line)
{
    size_t i, r;
    BOOL quoteth = FALSE, xml = FALSE;

    if ((token == NULL) || (line == NULL) || (line[0] == 0))
        return NULL;

    i = 0;
    /* Skip leading spaces and opening '<' */
    while ((line[i] == ' ') || (line[i] == '\t'))
        i++;
    if (line[i] == '<')
        i++;
    while ((line[i] == ' ') || (line[i] == '\t'))
        i++;

    /* Our token should begin a line */
    if (strncasecmp(&line[i], token, strlen(token)) != 0)
        return NULL;

    i += strlen(token);
    while ((line[i] == ' ') || (line[i] == '\t'))
        i++;

    /* Check for '=' or '>' sign */
    if (line[i] == '>')
        xml = TRUE;
    else if (line[i] != '=')
        return NULL;
    i++;
    while ((line[i] == ' ') || (line[i] == '\t'))
        i++;

    if (line[i] == '"') {
        quoteth = TRUE;
        i++;
    }
    r = i;

    while ((line[i] != 0) && (((line[i] != '"') && (line[i] != '<')) ||
           ((line[i] == '"') && (!quoteth)) || ((line[i] == '<') && (!xml))))
        i++;
    line[i--] = 0;

    while ((i >= r) && ((line[i] == '\r') || (line[i] == '\n')))
        line[i--] = 0;

    return (line[r] == 0) ? NULL : &line[r];
}

/*
 * Parse a UTF-8 buffer and return the data for the 'n'th occurrence of
 * 'token'. The returned string is UTF-8 and MUST be freed by the caller.
 */
char* get_token_data_buffer(const char* token, unsigned int n, const char* buffer, size_t buffer_size)
{
    unsigned int j, curly_count, i;
    BOOL done = FALSE;
    char* ret = NULL;
    char* wbuffer = NULL, * wline;

    if ((token == NULL) || (buffer == NULL) || (buffer_size <= 4) || (buffer_size > 65536))
        return NULL;
    if (buffer[buffer_size - 1] != 0)
        return NULL;

    wbuffer = strdup(buffer);
    if (wbuffer == NULL)
        return NULL;

    for (i = 0, j = 0, done = FALSE; (j != n) && (!done); ) {
        wline = &wbuffer[i];
        for (curly_count = 0; ((curly_count > 0) || ((wbuffer[i] != '\n') && (wbuffer[i] != '\r'))) && (wbuffer[i] != 0); i++) {
            if (wbuffer[i] == '{') curly_count++;
            if (wbuffer[i] == '}') curly_count--;
        }
        if (wbuffer[i] == 0)
            done = TRUE;
        else
            wbuffer[i++] = 0;
        ret = get_token_data_line(token, wline);
        if (ret != NULL)
            j++;
    }
    if (ret != NULL) {
        /* Move the data to the start of the buffer so it can be freed */
        memmove(wbuffer, ret, strlen(ret) + 1);
        ret = wbuffer;
    } else {
        free(wbuffer);
    }
    return ret;
}

BOOL CompareGUID(const GUID* guid1, const GUID* guid2)
{
    if ((guid1 != NULL) && (guid2 != NULL))
        return (memcmp(guid1, guid2, sizeof(GUID)) == 0);
    return FALSE;
}

/* Allocating version of wchar_to_utf8_no_alloc(). UTF-16 surrogate pairs
 * (as produced by code that stores UTF-16 units in wchar_t, e.g. libfat's
 * long file names) are combined. The result must be freed by the caller. */
char* wchar_to_utf8(const wchar_t* wstr)
{
    size_t i, j, len;
    wchar_t* tmp;
    char* ret;

    if (wstr == NULL)
        return NULL;
    len = wcslen(wstr);
    tmp = calloc(len + 1, sizeof(wchar_t));
    ret = malloc(len * 4 + 1);
    if (tmp == NULL || ret == NULL) {
        free(tmp);
        free(ret);
        return NULL;
    }
    for (i = 0, j = 0; i < len; i++) {
        uint32_t c = (uint32_t)wstr[i];
        if ((c >= 0xD800) && (c < 0xDC00) && (i + 1 < len) &&
            ((uint32_t)wstr[i + 1] >= 0xDC00) && ((uint32_t)wstr[i + 1] < 0xE000)) {
            c = 0x10000 + ((c - 0xD800) << 10) + ((uint32_t)wstr[i + 1] - 0xDC00);
            i++;
        }
        tmp[j++] = (wchar_t)c;
    }
    wchar_to_utf8_no_alloc(tmp, ret, len * 4 + 1);
    free(tmp);
    return ret;
}
