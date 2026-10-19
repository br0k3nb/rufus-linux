/*
 * Rufus: The Reliable USB Formatting Utility
 * Update and UEFI DBX checks
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
 * Port of upstream net.c CheckForUpdatesThread()/CheckForDBXUpdates().
 * - The version files on the Rufus server describe the Windows releases
 *   (Rufus_win_<arch>_<os>.ver, signed): the port checks them the same way
 *   (same lookup order, signature validation and settings), and reports a
 *   newer upstream release, but it does not download the Windows executable.
 * - The DBX (UEFI revocation list) updates are identical to upstream: newer
 *   DBX files are downloaded to app_data_dir/FILES_DIR/dbx_<arch>.bin, where
 *   hash.c picks them up for the bootloader revocation checks.
 */

#include "rfus.h"
#include "localization.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../res/dbx/dbx_info.h"

#define DEFAULT_UPDATE_INTERVAL     (24 * 3600)

/* dbx_info.h defines its data: it can only be included once (here) */
BOOL UseLocalDbx(int arch)
{
	char reg_name[32];
	static_sprintf(reg_name, "DBXTimestamp_%s", efi_archname[arch]);
	return (uint64_t)ReadSetting64(reg_name) > dbx_info[arch - 1].timestamp;
}


static __inline uint64_t to_uint64_t(uint16_t x[3]) {
    int i;
    uint64_t ret = 0;
    for (i = 0; i < 3; i++)
        ret = (ret << 16) + x[i];
    return ret;
}

static char* replace_char_dup(const char* src, char c, const char* rep)
{
    return replace_char(src, c, rep);
}

static void CheckForDBXUpdates(int verbose)
{
    int i, r;
    char reg_name[32], timestamp_url[256], path[MAX_PATH];
    char *p, *c, *rep, *buf = NULL;
    struct tm t = { 0 };
    uint64_t size, timestamp;
    BOOL already_prompted = FALSE;

    for (i = 0; i < (int)ARRAYSIZE(dbx_info); i++) {
        /* Get the epoch of the last commit */
        timestamp = 0;
        static_strcpy(timestamp_url, dbx_info[i].url);
        p = strstr(timestamp_url, "contents/");
        if (p == NULL)
            continue;
        *p = 0;
        rep = replace_char_dup(&p[9], '/', "%2F");
        static_strcat(timestamp_url, "commits?path=");
        static_strcat(timestamp_url, rep);
        free(rep);
        static_strcat(timestamp_url, "&page=1&per_page=1");
        if (verbose)
            uprintf("Querying %s for DBX update timestamp", timestamp_url);
        size = DownloadToFileOrBuffer(timestamp_url, NULL, (BYTE**)&buf, NULL, FALSE);
        if (size == 0)
            continue;
        /* Assumes that the GitHub JSON commit dates are of the form:
         * "date":[ ]*"2025-02-24T20:20:22Z" */
        p = strstr(buf, "\"date\":");
        if (p == NULL) {
            safe_free(buf);
            continue;
        }
        c = &p[7];
        while (*c == ' ' || *c == '"')
            c++;
        p = c;
        while (*c != '"' && *c != '\0')
            c++;
        *c = 0;
        r = sscanf(p, "%d-%d-%dT%d:%d:%dZ", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec);
        safe_free(buf);
        if (r != 6)
            continue;
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        timestamp = (uint64_t)timegm(&t);
        if (verbose)
            uprintf("DBX update timestamp is %llu", (unsigned long long)timestamp);
        static_sprintf(reg_name, "DBXTimestamp_%s", efi_archname[i + 1]);
        /* Check if we have an external DBX that is newer than embedded/last downloaded */
        if (timestamp <= dbx_info[i].timestamp)
            continue;
        static_sprintf(path, "%s/%s/dbx_%s.bin", app_data_dir, FILES_DIR, efi_archname[i + 1]);
        if (PathFileExistsU(path) && timestamp <= (uint64_t)ReadSetting64(reg_name))
            continue;
        if (!already_prompted) {
            r = Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_353), lmprintf(MSG_354));
            already_prompted = TRUE;
            if (r != IDYES)
                break;
            static_sprintf(reg_name, "%s/%s", app_data_dir, FILES_DIR);
            SHCreateDirectoryExU(NULL, reg_name, NULL);
            static_sprintf(reg_name, "DBXTimestamp_%s", efi_archname[i + 1]);
        }
        if (DownloadToFileOrBuffer(dbx_info[i].url, path, NULL, NULL, FALSE) != 0) {
            WriteSetting64(reg_name, timestamp);
            uprintf("Saved %s as 'dbx_%s.bin'", dbx_info[i].url, efi_archname[i + 1]);
        } else {
            uprintf("WARNING: Failed to download %s", dbx_info[i].url);
        }
    }
}

/* Upstream's update data (parser.c parse_update()), for the fields we use */
static void parse_version(char* buf, uint16_t version[3])
{
    char *data, *token;
    int i;

    version[0] = version[1] = version[2] = 0;
    data = get_token_data_buffer("version", 1, buf, strlen(buf) + 1);
    if (data == NULL)
        return;
    for (i = 0; (i < 3) && ((token = strtok((i == 0) ? data : NULL, ".")) != NULL); i++)
        version[i] = (uint16_t)atoi(token);
    free(data);
}

/*
 * Check for updates. Unless 'force' is set, this only happens if the update
 * interval set in the settings has elapsed (as upstream). Returns TRUE if a
 * newer release was found.
 */
BOOL CheckForUpdates(BOOL force)
{
    static const char* channel[] = { "release", "beta", "test" };
    uint16_t rufus_version[3] = { RUFUS_VERSION_MAJOR, RUFUS_VERSION_MINOR, RUFUS_VERSION_BUILD };
    uint16_t version[3];
    char urlpath[128], url[256], *buf = NULL;
    uint8_t* sig = NULL;
    int64_t local_time = (int64_t)time(NULL), reg_time, update_interval;
    int i, j, k, verbose = ReadSetting32(SETTING_VERBOSE_UPDATES), verpos[4], max_channel;
    uint64_t len, sig_len;
    BOOL found_new_version = FALSE, releases_only;

    if (!force) {
        if (ReadSetting32(SETTING_UPDATE_INTERVAL) == -1) {
            if (verbose)
                uprintf("Check for updates disabled, as per settings.");
            return FALSE;
        }
        reg_time = ReadSetting64(SETTING_LAST_UPDATE);
        update_interval = (int64_t)ReadSetting32(SETTING_UPDATE_INTERVAL);
        if (update_interval == 0) {
            WriteSetting32(SETTING_UPDATE_INTERVAL, DEFAULT_UPDATE_INTERVAL);
            update_interval = DEFAULT_UPDATE_INTERVAL;
        }
        if (local_time < reg_time + update_interval) {
            if (verbose)
                uprintf("Next update check in %lld seconds.", (long long)(reg_time + update_interval - local_time));
            return FALSE;
        }
    }

    /* Perform the DBX Update check */
    PrintInfoDebug(3000, MSG_352);
    CheckForDBXUpdates(verbose);

    PrintInfoDebug(3000, MSG_243);
    releases_only = !ReadSettingBool(SETTING_INCLUDE_BETAS);
    max_channel = releases_only ? 1 : (int)ARRAYSIZE(channel) - 1;
    for (k = 0; (k < max_channel) && (!found_new_version); k++) {
        uprintf("Checking %s channel...", channel[k]);
        /* Same lookup as upstream, for an up to date Windows 10/11 x64 host:
         * Rufus_win_x64_10.0.ver, then Rufus_win_x64_10.ver, ... */
        static_sprintf(urlpath, "%s%s%s_win_%s_%lu.%lu.ver", APPLICATION_NAME, (k == 0) ? "" : "_",
            (k == 0) ? "" : channel[k], "x64", 10UL, 0UL);
        for (i = 0, j = (int)strlen(urlpath) - 5; (j > 0) && (i < (int)ARRAYSIZE(verpos)); j--) {
            if ((urlpath[j] == '.') || (urlpath[j] == '_'))
                verpos[i++] = j;
        }
        len = 0;
        for (i = 0; i < (int)ARRAYSIZE(verpos) && len == 0; i++) {
            static_sprintf(url, "%s/%s", RUFUS_URL, urlpath);
            safe_free(buf);
            len = DownloadToFileOrBuffer(url, NULL, (BYTE**)&buf, NULL, FALSE);
            if (len == 0)
                safe_strcpy(&urlpath[verpos[i]], 5, ".ver");
        }
        if (len == 0) {
            if (verbose)
                uprintf("Could not find a %s version file on server %s", channel[k], RUFUS_URL);
            continue;
        }
        WriteSetting64(SETTING_LAST_UPDATE, local_time);
        static_sprintf(url, "%s/%s.sig", RUFUS_URL, urlpath);
        sig_len = DownloadToFileOrBuffer(url, NULL, &sig, NULL, FALSE);
        if ((sig_len != RSA_SIGNATURE_SIZE) || (!ValidateOpensslSignature((BYTE*)buf, (DWORD)len, sig, (DWORD)sig_len))) {
            uprintf("FATAL: Version signature is invalid ✗");
            break;
        }
        uprintf("Version signature is valid ✓");
        parse_version(buf, version);
        uprintf("  version: %d.%d.%d (%s)", version[0], version[1], version[2], channel[k]);
        found_new_version = (to_uint64_t(version) > to_uint64_t(rufus_version));
        uprintf("N%sew %s version found%c", found_new_version ? "" : "o n", channel[k], found_new_version ? '!' : '.');
        if (found_new_version) {
            PrintInfo(3000, MSG_246);
            uprintf("Rufus %d.%d.%d is available upstream (this port is based on %d.%d.%d) - see %s",
                version[0], version[1], version[2], rufus_version[0], rufus_version[1], rufus_version[2], DOWNLOAD_URL);
        } else {
            PrintInfo(3000, MSG_247);
        }
        safe_free(sig);
    }
    safe_free(buf);
    safe_free(sig);
    return found_new_version;
}
