/*
 * Rufus Linux port: resource access.
 *
 * On Windows, Rufus embeds its data blobs (ldlinux.sys, core.img, FreeDOS
 * files, ...) as RCDATA resources. On Linux the same blobs live in the
 * upstream res/ directory, mapped by resource ID through a build-time
 * generated table (tools/gen_res_map.sh, from src/rufus.rc).
 *
 * Semantics match upstream stdfn.c: GetResource() returns a pointer to the
 * resource data; when 'duplicate' is set the caller gets an allocated copy
 * whose size is min(*len, resource_size).
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "res_map.h"

#ifndef RUFUS_RES_ROOT
#define RUFUS_RES_ROOT ".."
#endif

typedef struct cached_resource {
    unsigned id;
    uint8_t* data;
    DWORD len;
    struct cached_resource* next;
} cached_resource_t;

static cached_resource_t* cache_head = NULL;

/* Resolve a path relative to the res/ root: try, in order,
 * $RUFUS_RES_ROOT (env), the compile-time source root, CWD, /usr/share. */
static BOOL resolve_path(const char* rel_path, char* out, size_t out_len)
{
    static const char* extra_roots[] = {
        "/usr/share/rufus", "/usr/local/share/rufus", "/usr/share/rufus/linux", NULL
    };
    const char* env = getenv("RUFUS_RES_ROOT");
    char candidate[MAX_PATH * 2];
    FILE* f;
    int i;

    if (env != NULL && env[0] != 0) {
        snprintf(candidate, sizeof(candidate), "%s/%s", env, rel_path);
        f = fopen(candidate, "rb");
        if (f != NULL) {
            fclose(f);
            snprintf(out, out_len, "%s", candidate);
            return TRUE;
        }
    }
    snprintf(candidate, sizeof(candidate), "%s/%s", RUFUS_RES_ROOT, rel_path);
    f = fopen(candidate, "rb");
    if (f != NULL) {
        fclose(f);
        snprintf(out, out_len, "%s", candidate);
        return TRUE;
    }
    /* Relative to the executable: <prefix>/bin/rufus -> <prefix>/share/rufus */
    if (app_dir[0] != 0) {
        snprintf(candidate, sizeof(candidate), "%s/../share/rufus/%s", app_dir, rel_path);
        f = fopen(candidate, "rb");
        if (f != NULL) {
            fclose(f);
            snprintf(out, out_len, "%s", candidate);
            return TRUE;
        }
    }
    for (i = 0; extra_roots[i] != NULL; i++) {
        snprintf(candidate, sizeof(candidate), "%s/%s", extra_roots[i], rel_path);
        f = fopen(candidate, "rb");
        if (f != NULL) {
            fclose(f);
            snprintf(out, out_len, "%s", candidate);
            return TRUE;
        }
    }
    return FALSE;
}

static cached_resource_t* lookup_cache(unsigned id)
{
    cached_resource_t* c;
    for (c = cache_head; c != NULL; c = c->next)
        if (c->id == id)
            return c;
    return NULL;
}

static cached_resource_t* load_resource(unsigned id, const char* desc)
{
    const res_map_entry_t* e = NULL;
    cached_resource_t* c;
    char path[MAX_PATH * 2];
    FILE* f;
    size_t i, size;
    uint8_t* data = NULL;

    for (i = 0; i < res_map_count; i++) {
        if (res_map[i].id == id) {
            e = &res_map[i];
            break;
        }
    }
    if (e == NULL) {
        uprintf("Could not locate resource '%s': unknown resource ID %u", desc, id);
        return NULL;
    }
    if (!resolve_path(e->path, path, sizeof(path))) {
        uprintf("Could not locate resource '%s': file '%s' not found "
                "(set RUFUS_RES_ROOT to the Rufus source root)", desc, e->path);
        return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        uprintf("Could not open resource '%s': %s", desc, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc(size + 1);      /* +1: some blobs are parsed as text */
    if (data == NULL) {
        uprintf("Could not allocate resource '%s'", desc);
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(data, 1, size, f) != size) {
        uprintf("Could not read resource '%s'", desc);
        free(data);
        fclose(f);
        return NULL;
    }
    data[size] = 0;
    fclose(f);

    c = (cached_resource_t*)calloc(1, sizeof(cached_resource_t));
    if (c == NULL) {
        free(data);
        return NULL;
    }
    c->id = id;
    c->data = data;
    c->len = (DWORD)size;
    c->next = cache_head;
    cache_head = c;
    return c;
}

uint8_t* GetResource(HMODULE module, char* name, char* type, const char* desc, DWORD* len, BOOL duplicate)
{
    cached_resource_t* c;
    unsigned id;
    uint8_t* p = NULL;

    (void)module;
    (void)type;
    id = (unsigned)(uintptr_t)name;     /* MAKEINTRESOURCEA() */
    if (id == 0 || id >= 0x10000) {
        uprintf("GetResource: named resources are not supported on Linux ('%s')", desc);
        return NULL;
    }

    c = lookup_cache(id);
    if (c == NULL)
        c = load_resource(id, desc);
    if (c == NULL)
        goto out;

    if (duplicate) {
        if (*len == 0)
            *len = c->len;
        p = (uint8_t*)calloc(*len, 1);
        if (p == NULL) {
            uprintf("Could not allocate resource '%s'", desc);
            goto out;
        }
        memcpy(p, c->data, MIN(c->len, *len));
        if (c->len > *len)
            uprintf("WARNING: Resource '%s' was truncated by %d bytes!", desc, c->len - *len);
    } else {
        p = c->data;
    }
    *len = c->len;

out:
    return p;
}

DWORD GetResourceSize(HMODULE module, char* name, char* type, const char* desc)
{
    DWORD len = 0;
    return (GetResource(module, name, type, desc, &len, FALSE) == NULL) ? 0 : len;
}

/* Release the resource cache (called at exit) */
void free_resource_cache(void)
{
    cached_resource_t* c = cache_head;
    while (c != NULL) {
        cached_resource_t* next = c->next;
        free(c->data);
        free(c);
        c = next;
    }
    cache_head = NULL;
}

/* Full path of a file under the Rufus source/data root (res/...) */
BOOL GetResourceFilePath(const char* rel_path, char* out, size_t out_len)
{
    return resolve_path(rel_path, out, out_len);
}
