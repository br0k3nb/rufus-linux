/*
 * Rufus Linux port: <guiddef.h> replacement.
 *
 * The upstream GPT type tables (src/gpt_types.h) use DEFINE_GUID(); this
 * shim provides the same macro on top of the port's GUID type. Include
 * with INITGUID defined in exactly one translation unit to emit the
 * definitions, elsewhere the GUIDs are declared extern.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RUFUS_LINUX_GUIDDEF_H
#define RUFUS_LINUX_GUIDDEF_H

#include "win32compat.h"

#ifdef INITGUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#else
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    extern const GUID name
#endif

/* A few macros from guiddef.h that vendored code may rely on */
#ifndef IsEqualGUID
#define IsEqualGUID(rguid1, rguid2) \
    (memcmp(rguid1, rguid2, sizeof(GUID)) == 0)
#endif
#ifndef IsEqualIID
#define IsEqualIID(riid1, riid2) IsEqualGUID(riid1, riid2)
#endif

#endif /* RUFUS_LINUX_GUIDDEF_H */
