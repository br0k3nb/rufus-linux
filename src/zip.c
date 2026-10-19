/*
 * Rufus: The Reliable USB Formatting Utility
 * Zip archive extraction (upstream stdio.c ExtractZip())
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

#include "rfus.h"
#include "localization.h"

char* archive_path = NULL;      /* Zip archive of extra files to copy to the target */
static uint64_t archive_size;
static unsigned long zip_cancel = 0;

static void print_extracted_file(const char* file_path, uint64_t file_length)
{
    char str[MAX_PATH];

    if (file_path == NULL)
        return;
    static_sprintf(str, "%s (%s)", file_path, SizeToHumanReadable(file_length, TRUE, FALSE));
    uprintf("Extracting: %s", str);
    PrintStatus(0, MSG_000, str);   /* MSG_000 is "%s" */
}

static void update_progress(const int64_t processed_bytes)
{
    static uint64_t total_bytes = 0;

    /* PORT: bled's cancel flag is an unsigned long (ErrorStatus is a DWORD) */
    zip_cancel = IS_ERROR(ErrorStatus) ? 1 : 0;
    if (processed_bytes < 0) {
        total_bytes = (uint64_t)-processed_bytes;
        UpdateProgressWithInfo(OP_EXTRACT_ZIP, MSG_348, 0, total_bytes);
    } else {
        UpdateProgressWithInfo(OP_EXTRACT_ZIP, MSG_348, (uint64_t)processed_bytes, total_bytes);
    }
}

/* Extract content from a zip archive onto the designated directory or drive */
BOOL ExtractZip(const char* src_zip, const char* dest_dir)
{
    int64_t extracted_bytes = 0;

    if (src_zip == NULL)
        return FALSE;
    archive_size = _filesizeU(src_zip);
    (void)archive_size;
    zip_cancel = 0;
    if (bled_init(256 * KB, NULL, NULL, NULL, update_progress, print_extracted_file, &zip_cancel) != 0)
        return FALSE;
    uprintf("● Copying files from '%s'", src_zip);
    extracted_bytes = bled_uncompress_to_dir(src_zip, dest_dir, BLED_COMPRESSION_ZIP);
    bled_exit();
    return (extracted_bytes > 0);
}
