/*
 * Rufus: The Reliable USB Formatting Utility
 * Extract icon from executable and set autorun.inf
 * Copyright © 2012-2023 Pete Batard <pete@akeo.ie>
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
 * Upstream extracts the application icon from the Rufus executable's
 * resources (ExtractAppIcon()); the port copies the same icon from
 * res/rufus.ico, which is what the executable's icon is built from.
 * autorun.inf is written as UTF-16LE with a BOM and CRLF line endings,
 * which is what MSVC's "w, ccs=UTF-16LE" text mode produces upstream.
 */

#include "rfus.h"

#include <stdio.h>
#include <string.h>

BOOL extended_label = TRUE;     /* IDC_EXTENDED_LABEL (checked by default) */

static void fputs_utf16le(FILE* fd, const char* str)
{
    wchar_t* w = utf8_to_wchar(str);
    size_t i;

    if (w == NULL)
        return;
    for (i = 0; w[i] != 0; i++) {
        uint32_t c = (uint32_t)w[i];
        if (c == '\n') {
            fputc('\r', fd);
            fputc(0, fd);
        }
        if (c >= 0x10000) {
            uint16_t hi = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
            uint16_t lo = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
            fputc(hi & 0xFF, fd); fputc(hi >> 8, fd);
            fputc(lo & 0xFF, fd); fputc(lo >> 8, fd);
        } else {
            fputc(c & 0xFF, fd);
            fputc(c >> 8, fd);
        }
    }
    free(w);
}

/* Copy the application icon to 'path' */
static BOOL ExtractAppIcon(const char* path, BOOL bSilent)
{
    char src[MAX_PATH * 2];

    if (!GetResourceFilePath("res/rufus.ico", src, sizeof(src)) || !CopyFileU(src, path, FALSE)) {
        if (!bSilent)
            uprintf("Could not create '%s'", path);
        return FALSE;
    }
    if (!bSilent)
        uprintf("Created: %s", path);
    return TRUE;
}

/*
 * Create an autorun.inf, if none exists
 * We use this to set the icon as well as labels that are longer than 11/32 chars or,
 * in the case of FAT, contain non-English characters
 */
BOOL SetAutorun(const char* path)
{
    FILE* fd;
    char filename[MAX_PATH], line[256];

    static_sprintf(filename, "%s/autorun.inf", path);
    fd = fopen(filename, "r");  /* If there's an existing autorun, don't overwrite */
    if (fd != NULL) {
        uprintf("%s already exists - keeping it", filename);
        fclose(fd);
        return FALSE;
    }
    /* No "/autorun.inf" => create a new one in UTF-16 LE mode */
    fd = fopen(filename, "wb");
    if (fd == NULL) {
        uprintf("Unable to create %s", filename);
        return FALSE;
    }
    fputc(0xFF, fd);
    fputc(0xFE, fd);
    static_sprintf(line, "; Created by Rufus %d.%d.%d\n; " APPLICATION_URL "\n",
        RUFUS_VERSION_MAJOR, RUFUS_VERSION_MINOR, RUFUS_VERSION_BUILD);
    fputs_utf16le(fd, line);
    static_sprintf(line, "[autorun]\nicon  = autorun.ico\nlabel = %s\n", label);
    fputs_utf16le(fd, line);
    fclose(fd);
    uprintf("Created: %s", filename);

    /* .inf -> .ico */
    filename[strlen(filename) - 1] = 'o';
    filename[strlen(filename) - 2] = 'c';
    return ExtractAppIcon(filename, FALSE);
}
