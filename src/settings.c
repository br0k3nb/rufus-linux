/*
 * Rufus Linux port: settings and simple INI parsing.
 *
 * Native replacement for upstream parser.c + settings.h: the same
 * token-based INI access (used by ported code for autorun/config parsing)
 * and the same ReadSetting and WriteSetting API, backed by
 * $XDG_CONFIG_HOME/rufus/rufus.ini on Linux.
 *
 * Both UTF-8 and UTF-16LE INI files are read; writes are UTF-8.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "rfus.h"


#include <ftw.h>
#include <pwd.h>
#include <unistd.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

char* ini_file = NULL;

#define MAX_INI_LINE    4096

/* ------------------------------------------------------------------ */
/* Token helpers (upstream parser.c semantics)                         */
/* ------------------------------------------------------------------ */

/* Case-insensitive: does 'line' contain "token=..." ? Returns pointer to
 * the value (after '='), or NULL. Handles quoted values. */
static const char* get_token_data_line(const char* token, char* line)
{
    size_t token_len = strlen(token);
    char* p = line;

    /* Skip leading spaces */
    while (*p == ' ' || *p == '\t')
        p++;
    /* Ignore comments and section headers */
    if (*p == ';' || *p == '#' || *p == '[')
        return NULL;
    if (strncasecmp(p, token, token_len) != 0)
        return NULL;
    p += token_len;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '=')
        return NULL;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    /* Strip trailing CR/LF and quotes */
    {
        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r'))
            p[--len] = 0;
        while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t'))
            p[--len] = 0;
        if (len >= 2 && p[0] == '"' && p[len-1] == '"') {
            p[len-1] = 0;
            p++;
        }
    }
    return p;
}

/* Read a whole file, converting UTF-16LE to UTF-8 when a BOM is found.
 * Returns a malloc'd NUL-terminated buffer or NULL. */
static char* read_text_file(const char* filename, size_t* out_len)
{
    FILE* f = fopen(filename, "rb");
    uint8_t* raw = NULL;
    size_t size;
    char* utf8 = NULL;

    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    raw = (uint8_t*)malloc(size + 2);
    if (raw == NULL) {
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(raw, 1, size, f) != size) {
        free(raw);
        fclose(f);
        return NULL;
    }
    fclose(f);
    raw[size] = 0;
    raw[size + 1] = 0;

    if (size >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        /* UTF-16LE to UTF-8 */
        size_t i, n = (size - 2) / 2;
        utf8 = (char*)malloc(n * 4 + 1);
        if (utf8 != NULL) {
            size_t o = 0;
            for (i = 0; i < n; i++) {
                uint32_t cp = raw[2 + i * 2] | (raw[3 + i * 2] << 8);
                if (cp == '\r')
                    continue;
                if (cp == '\n') { utf8[o++] = '\n'; continue; }
                if (cp < 0x80) {
                    utf8[o++] = (char)cp;
                } else if (cp < 0x800) {
                    utf8[o++] = (char)(0xC0 | (cp >> 6));
                    utf8[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    utf8[o++] = (char)(0xE0 | (cp >> 12));
                    utf8[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    utf8[o++] = (char)(0x80 | (cp & 0x3F));
                }
            }
            utf8[o] = 0;
            if (out_len != NULL)
                *out_len = o;
        }
        free(raw);
        return utf8;
    }
    if (size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        memmove(raw, raw + 3, size - 3);
        size -= 3;
    }
    if (out_len != NULL)
        *out_len = size;
    return (char*)raw;
}

char* get_token_data_file_indexed(const char* token, const char* filename, int index)
{
    char* content;
    char* line, *next;
    int i = 0;

    if (filename == NULL || token == NULL || filename[0] == 0 || token[0] == 0)
        return NULL;
    content = read_text_file(filename, NULL);
    if (content == NULL)
        return NULL;

    line = content;
    while (line != NULL && *line != 0) {
        const char* value;
        next = strchr(line, '\n');
        if (next != NULL)
            *next++ = 0;
        value = get_token_data_line(token, line);
        if (value != NULL && ++i == index) {
            char* ret = strdup(value);
            free(content);
            return ret;
        }
        line = next;
    }
    free(content);
    return NULL;
}

char* set_token_data_file(const char* token, const char* data, const char* filename)
{
    char* content = NULL;
    size_t content_len = 0;
    FILE* f;
    char* line, *next;
    BOOL found = FALSE;
    char* ret = NULL;

    if (filename == NULL || token == NULL || data == NULL)
        return NULL;
    if (filename[0] == 0 || token[0] == 0 || data[0] == 0)
        return NULL;

    content = read_text_file(filename, &content_len);
    if (content == NULL) {
        content = strdup("");
        if (content == NULL)
            return NULL;
    }

    f = fopen(filename, "wb");
    if (f == NULL) {
        free(content);
        return NULL;
    }

    line = content;
    while (line != NULL && *line != 0) {
        next = strchr(line, '\n');
        if (next != NULL)
            *next++ = 0;
        if (!found && get_token_data_line(token, line) != NULL) {
            fprintf(f, "%s=%s\n", token, data);
            found = TRUE;
        } else {
            fprintf(f, "%s\n", line);
        }
        line = next;
    }
    if (!found)
        fprintf(f, "%s=%s\n", token, data);
    fclose(f);

    if (data != NULL)
        ret = strdup(data);
    free(content);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Settings API                                                        */
/* ------------------------------------------------------------------ */
int64_t ReadSetting64(const char* key)
{
    char* str;
    int64_t val = 0;
    if (ini_file == NULL)
        return 0;
    str = get_token_data_file_indexed(key, ini_file, 1);
    if (str != NULL) {
        val = (int64_t)strtoll(str, NULL, 0);
        free(str);
    }
    return val;
}

int32_t ReadSetting32(const char* key)
{
    return (int32_t)ReadSetting64(key);
}

BOOL WriteSetting64(const char* key, int64_t val)
{
    char str[32];
    if (ini_file == NULL)
        return FALSE;
    snprintf(str, sizeof(str), "%lld", (long long)val);
    return (set_token_data_file(key, str, ini_file) != NULL);
}

BOOL WriteSetting32(const char* key, int32_t val)
{
    return WriteSetting64(key, val);
}

BOOL WriteSettingBool(const char* key, BOOL val)
{
    return WriteSetting32(key, (val) ? 1 : 0);
}

char* ReadSettingStr(const char* key)
{
    static char str[512];
    char* val;
    str[0] = 0;
    if (ini_file == NULL)
        return str;
    val = get_token_data_file_indexed(key, ini_file, 1);
    if (val != NULL) {
        snprintf(str, sizeof(str), "%s", val);
        free(val);
    }
    return str;
}

BOOL WriteSettingStr(const char* key, const char* val)
{
    if (ini_file == NULL)
        return FALSE;
    return (set_token_data_file(key, val, ini_file) != NULL);
}

/* ------------------------------------------------------------------ */
/* Paths and initialization                                            */
/* ------------------------------------------------------------------ */
/* Upstream uses %LOCALAPPDATA% for downloaded files; the XDG equivalent is
 * $XDG_DATA_HOME (~/.local/share). When running through sudo, the invoking
 * user's directory is used, so that downloads are shared with non-root runs. */
/* When running as root on behalf of a user, the files we download into the
 * user's data directory are given back to that user on exit */
static uid_t data_owner = (uid_t)-1;
static gid_t data_group = (gid_t)-1;

static int chown_entry(const char* path, const struct stat* sb, int flag, struct FTW* ftwbuf)
{
    (void)sb; (void)flag; (void)ftwbuf;
    if (lchown(path, data_owner, data_group) != 0)
        return 0;
    return 0;
}

static void restore_data_owner(void)
{
    if (data_owner != (uid_t)-1)
        nftw(app_data_dir, chown_entry, 16, FTW_PHYS);
}

void init_app_data_dir(void)
{
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    const char* sudo_user = getenv("SUDO_USER");
    const char* pkexec_uid = getenv("PKEXEC_UID");
    char home_buf[MAX_PATH];

    /* pkexec (used by the GUI for its worker) sets PKEXEC_UID */
    if ((geteuid() == 0) && (pkexec_uid != NULL) && (atoi(pkexec_uid) != 0)) {
        struct passwd* pw = getpwuid((uid_t)atoi(pkexec_uid));
        if (pw != NULL && pw->pw_dir != NULL) {
            snprintf(home_buf, sizeof(home_buf), "%s", pw->pw_dir);
            home = home_buf;
            xdg = NULL;
            data_owner = pw->pw_uid;
            data_group = pw->pw_gid;
        }
    } else if ((geteuid() == 0) && (sudo_user != NULL) && (sudo_user[0] != 0) && (strcmp(sudo_user, "root") != 0)) {
        struct passwd* pw = getpwnam(sudo_user);
        if (pw != NULL) {
            data_owner = pw->pw_uid;
            data_group = pw->pw_gid;
        }
        if (pw != NULL && pw->pw_dir != NULL) {
            snprintf(home_buf, sizeof(home_buf), "%s", pw->pw_dir);
            home = home_buf;
            xdg = NULL;
        }
    }
    if (xdg != NULL && xdg[0] == '/')
        snprintf(app_data_dir, sizeof(app_data_dir), "%s/rufus", xdg);
    else if (home != NULL && home[0] != 0)
        snprintf(app_data_dir, sizeof(app_data_dir), "%s/.local/share/rufus", home);
    else
        snprintf(app_data_dir, sizeof(app_data_dir), "/tmp/rufus");
    SHCreateDirectoryExU(NULL, app_data_dir, NULL);
    if (data_owner != (uid_t)-1)
        atexit(restore_data_owner);
}

void init_settings(void)
{
    static char path[MAX_PATH];
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    char dir[MAX_PATH];

    if (xdg != NULL && xdg[0] != 0)
        snprintf(dir, sizeof(dir), "%s/rufus", xdg);
    else if (home != NULL && home[0] != 0)
        snprintf(dir, sizeof(dir), "%s/.config/rufus", home);
    else
        snprintf(dir, sizeof(dir), "/tmp/rufus");

    /* Also honor a portable rufus.ini next to the executable */
    snprintf(path, sizeof(path), "%s/rufus.ini", app_dir);
    if (app_dir[0] != 0 && PathFileExistsU(path)) {
        ini_file = path;
        return;
    }

    mkdir(dir, 0755);   /* best effort: parents already exist in practice */
    snprintf(path, sizeof(path), "%s/rufus.ini", dir);
    ini_file = path;
}
