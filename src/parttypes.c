/*
 * Rufus Linux port: MBR/GPT partition type tables.
 *
 * Compiles the vendored upstream tables (src/mbr_types.h, src/gpt_types.h)
 * unchanged, so partition type names match the Windows build exactly.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#define INITGUID
#include "mbr_types.h"
#include "gpt_types.h"

const char* GetMBRPartitionType(const uint8_t type)
{
    size_t i;
    for (i = 0; (i < ARRAYSIZE(mbr_type)) && (mbr_type[i].type != type); i++);
    return (i < ARRAYSIZE(mbr_type)) ? mbr_type[i].name : "Unknown";
}

const char* GetGPTPartitionType(const GUID* guid)
{
    const char* desc = gpt_type_desc(guid);
    return desc != NULL ? desc : GuidToString(guid, TRUE);
}
