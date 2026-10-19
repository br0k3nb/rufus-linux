/*
 * Rufus Linux port: localization.
 *
 * Parses the upstream res/loc/rufus.loc file (the same data the Windows
 * build embeds) and provides lmprintf() with upstream semantics: a ring of
 * concurrent message buffers, message IDs from resource.h.
 *
 * The parser handles the commands that appear in rufus.loc:
 *   l "xx-XX" "Display Name" <lcids>   locale definition
 *   v <version>                        locale file version
 *   b "xx-XX"                          locale base
 *   a <attributes>                     locale attributes (RTL...)
 *   g IDD_XXX                          dialog group
 *   t ID "text"                        localized text for a resource ID
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"
#include "localization.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Name -> ID table generated at build time from ../src/resource.h */
#include "res_ids.h"

char* loc_filename = NULL;
char* embedded_loc_filename = "rufus.loc";
int   loc_line_nr = 0;
BOOL  en_msg_mode = FALSE;

static loc_entry* locales = NULL;
static size_t locale_count = 0, locale_capacity = 0;
static int current_locale = 0;      /* index into locales[], en-US is 0 */
static int default_locale = 0;

/* Ring of message buffers (upstream localization.h) */
static char message_buffers[LOC_MESSAGE_NB][LOC_MESSAGE_SIZE];
static int message_index = 0;

static char* data_dirs[] = {
    "res/loc",              /* running from the repo */
    "../res/loc",
    "./loc",
    "/usr/share/rufus/loc",
    "/usr/local/share/rufus/loc",
    NULL
};

static FILE* open_loc_file(const char* filename)
{
    int i;
    char path[MAX_PATH];
    FILE* f = fopen(filename, "r");
    if (f != NULL)
        return f;
    /* Same lookup as the other resources (source tree, install prefix...) */
    snprintf(path, sizeof(path), "res/loc/%s", filename);
    if (GetResourceFilePath(path, path, sizeof(path)) && (f = fopen(path, "r")) != NULL)
        return f;
    for (i = 0; data_dirs[i] != NULL; i++) {
        snprintf(path, sizeof(path), "%s/%s", data_dirs[i], filename);
        f = fopen(path, "r");
        if (f != NULL)
            return f;
    }
    return NULL;
}

static int find_res_id(const char* name)
{
    size_t i;
    for (i = 0; i < res_id_count; i++)
        if (strcmp(res_ids[i].name, name) == 0)
            return res_ids[i].id;
    return -1;
}

/* Unescape a .loc quoted string in place */
static void unescape(char* s)
{
    char* w = s;
    while (*s != 0) {
        if (*s == '\\' && s[1] != 0) {
            s++;
            switch (*s) {
            case 'n': *w++ = '\n'; break;
            case 't': *w++ = '\t'; break;
            case 'r': *w++ = '\r'; break;
            case '"': *w++ = '"'; break;
            case '\\': *w++ = '\\'; break;
            default: *w++ = *s; break;
            }
            s++;
        } else {
            *w++ = *s++;
        }
    }
    *w = 0;
}

/* Parse a leading quoted string; returns pointer past the closing quote */
static char* parse_quoted(char* p, char* out, size_t out_size)
{
    size_t n = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return NULL;
    p++;
    while (*p != 0 && *p != '"') {
        if (*p == '\\' && p[1] != 0) {
            if (n + 2 < out_size) {
                out[n++] = *p++;
                out[n++] = *p++;
            } else {
                p += 2;
            }
        } else {
            if (n + 1 < out_size)
                out[n++] = *p;
            p++;
        }
    }
    if (*p != '"')
        return NULL;
    p++;
    out[n] = 0;
    unescape(out);
    return p;
}

static loc_entry* add_locale(const char* name, const char* display)
{
    loc_entry* e;
    if (locale_count >= locale_capacity) {
        size_t new_capacity = (locale_capacity == 0) ? 16 : locale_capacity * 2;
        loc_entry* new_locales = (loc_entry*)realloc(locales, new_capacity * sizeof(loc_entry));
        if (new_locales == NULL)
            return NULL;
        memset(&new_locales[locale_capacity], 0,
               (new_capacity - locale_capacity) * sizeof(loc_entry));
        locales = new_locales;
        locale_capacity = new_capacity;
    }
    e = &locales[locale_count++];
    snprintf(e->name, sizeof(e->name), "%s", (name[0] == 0) ? "en-US" : name);
    snprintf(e->display, sizeof(e->display), "%s", display);
    return e;
}

void _init_localization(BOOL reinit)
{
    FILE* f;
    char line[LOC_MESSAGE_SIZE + 128];
    int cur = -1;

    if (locales != NULL)
        return;     /* already initialized */

    f = open_loc_file(embedded_loc_filename);
    if (f == NULL) {
        fprintf(stderr, "warning: could not find localization file '%s', "
                        "messages will be shown as IDs\n", embedded_loc_filename);
        return;
    }
    loc_filename = embedded_loc_filename;

    while (fgets(line, sizeof(line), f) != NULL) {
        char* p = line;
        loc_line_nr++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0)
            continue;

        switch (*p) {
        case 'l': {
            /* l "en-US" "English (English)" <lcids> */
            char name[32], display[128];
            char* q = parse_quoted(p + 1, name, sizeof(name));
            if (q == NULL)
                break;
            q = parse_quoted(q, display, sizeof(display));
            if (add_locale(name, display) != NULL)
                cur = (int)(locale_count - 1);
            break;
        }
        case 'v':
        case 'b':   /* version / base locale: no state to keep */
            break;
        case 'a': {
            /* a 0x1 : locale attributes */
            if (cur >= 0) {
                unsigned long attr = strtoul(p + 1, NULL, 0);
                locales[cur].attributes |= (int)attr;
            }
            break;
        }
        case 'g':   /* g IDD_XXX : dialog group */
            break;
        case 't': {
            /* t ID "text" */
            char token[64], text[LOC_MESSAGE_SIZE];
            char* q = p + 1;
            int id;
            sscanf(q, " %63s", token);
            /* skip past the token to the quoted text */
            q = strstr(q, token);
            if (q == NULL || cur < 0)
                break;
            q += strlen(token);
            q = parse_quoted(q, text, sizeof(text));
            if (q == NULL)
                break;
            id = find_res_id(token);
            if (id <= 0 || id >= LOC_ID_RANGE)
                break;
            if (locales[cur].texts == NULL) {
                locales[cur].texts = (char**)calloc(LOC_ID_RANGE, sizeof(char*));
                if (locales[cur].texts == NULL)
                    break;
            }
            free(locales[cur].texts[id]);
            locales[cur].texts[id] = strdup(text);
            break;
        }
        default:
            break;
        }
    }
    fclose(f);

    if (locale_count == 0) {
        free(locales);
        locales = NULL;
        return;
    }

    /* Default locale selection from the environment */
    {
        const char* env = getenv("LC_ALL");
        char want[64];
        size_t i;
        if (env == NULL || *env == 0)
            env = getenv("LC_MESSAGES");
        if (env == NULL || *env == 0)
            env = getenv("LANG");
        if (env != NULL && *env != 0) {
            snprintf(want, sizeof(want), "%s", env);
            {   /* strip ".<codeset>" suffix and convert "_" to "-" */
                char* dot = strchr(want, '.');
                char* q;
                if (dot != NULL)
                    *dot = 0;
                for (q = want; *q != 0; q++)
                    if (*q == '_')
                        *q = '-';
            }
            for (i = 0; i < locale_count; i++) {
                if (strcasecmp(locales[i].name, want) == 0) {
                    current_locale = (int)i;
                    break;
                }
            }
            if (current_locale == default_locale) {
                /* Fall back on the language part ("pt-BR" env => "pt-PT" match) */
                size_t lang_len = strcspn(want, "-");
                for (i = 1; i < locale_count; i++) {
                    if (strncasecmp(locales[i].name, want, lang_len) == 0 &&
                        locales[i].name[lang_len] == '-') {
                        current_locale = (int)i;
                        break;
                    }
                }
            }
        }
    }
    (void)reinit;
}

void _exit_localization(BOOL reinit)
{
    size_t i, j;
    if (locales == NULL)
        return;
    for (i = 0; i < locale_count; i++) {
        if (locales[i].texts != NULL) {
            for (j = 0; j < LOC_ID_RANGE; j++)
                free(locales[i].texts[j]);
            free(locales[i].texts);
        }
    }
    free(locales);
    locales = NULL;
    locale_count = locale_capacity = 0;
    current_locale = default_locale;
    (void)reinit;
}

int get_locale_count(void)
{
    return (int)locale_count;
}

const char* get_locale_name(int index)
{
    if (index < 0 || index >= (int)locale_count)
        return "en-US";
    return locales[index].name;
}

const char* get_locale_display_name(int index)
{
    if (index < 0 || index >= (int)locale_count)
        return "English (English)";
    return locales[index].display;
}

int get_locale_attributes(int index)
{
    if (index < 0 || index >= (int)locale_count)
        return 0;
    return locales[index].attributes;
}

int get_locale_index_from_name(const char* locale_name)
{
    size_t i;
    if (locales == NULL)
        init_localization();
    if (locale_name == NULL || locales == NULL)
        return -1;
    for (i = 0; i < locale_count; i++)
        if (strcasecmp(locales[i].name, locale_name) == 0)
            return (int)i;
    return -1;
}

char* get_current_locale_name(void)
{
    if (locales == NULL || locale_count == 0)
        return (char*)"en-US";
    return locales[current_locale].name;
}

BOOL set_locale(const char* locale_name)
{
    int index;
    if (locales == NULL)
        init_localization();
    if (locales == NULL)
        return FALSE;
    if (locale_name == NULL || strcasecmp(locale_name, "en-US") == 0 ||
        strcasecmp(locale_name, "en") == 0) {
        current_locale = default_locale;
        return TRUE;
    }
    index = get_locale_index_from_name(locale_name);
    if (index < 0)
        return FALSE;
    current_locale = index;
    return TRUE;
}

/* Upstream swaps between the current locale's message table and the
 * built-in (English) default table. There is no separate built-in table
 * here: the "en-US" locale loaded from the .loc file plays that role, so
 * this toggles the "use default locale" flag instead. */
void toggle_default_locale(void)
{
    static BOOL old_en_msg_mode = FALSE;
    static BOOL toggled = FALSE;

    if (!toggled) {
        old_en_msg_mode = en_msg_mode;
        en_msg_mode = TRUE;
        toggled = TRUE;
    } else {
        en_msg_mode = old_en_msg_mode;
        toggled = FALSE;
    }
}

static const char* get_text(int id)
{
    /* MSG_000 is the built-in "%s" message (upstream localization.c
     * default_msg_table), it is never part of the .loc files */
    if (id == MSG_000)
        return "%s";
    if (locales == NULL && id >= MSG_000)
        return NULL;
    if (id <= 0 || id >= LOC_ID_RANGE)
        return NULL;
    if (!en_msg_mode && current_locale < (int)locale_count &&
        locales[current_locale].texts != NULL && locales[current_locale].texts[id] != NULL)
        return locales[current_locale].texts[id];
    if (default_locale < (int)locale_count && locales[default_locale].texts != NULL &&
        locales[default_locale].texts[id] != NULL)
        return locales[default_locale].texts[id];
    return NULL;
}

const char* get_localized_text(int id)
{
    const char* text = get_text(id);
    return (text != NULL) ? text : "";
}

/* Format string (with % placeholders) of a MSG_* id in the current locale.
 * May return NULL when the id is not localized: callers must then use a
 * fallback. This is what PrintStatusInfo()/lmprintf() feed to vsnprintf(). */
const char* get_msg_format(int id)
{
    return get_text(id);
}

/* Same, but forced to the default locale: upstream uses this for the log,
 * which is always kept in English. */
const char* get_msg_format_en(int id)
{
    if (id <= 0 || id >= LOC_ID_RANGE)
        return NULL;
    if (default_locale < (int)locale_count && locales[default_locale].texts != NULL &&
        locales[default_locale].texts[id] != NULL)
        return locales[default_locale].texts[id];
    return NULL;
}

const char* get_name_from_id(int id)
{
    size_t i;
    for (i = 0; i < res_id_count; i++)
        if (res_ids[i].id == id)
            return res_ids[i].name;
    return "ID_UNKNOWN";
}

char* lmprintf(uint32_t msg_id, ...)
{
    va_list args;
    char* buffer;
    const char* format;
    int id = (int)(msg_id & MSG_MASK);

    message_index = (message_index + 1) % LOC_MESSAGE_NB;
    buffer = message_buffers[message_index];
    format = get_text(id);
    va_start(args, msg_id);
    if (format != NULL)
        vsnprintf(buffer, LOC_MESSAGE_SIZE, format, args);
    else if (id >= MSG_000 && id < MSG_MAX)
        snprintf(buffer, LOC_MESSAGE_SIZE, "[MSG_%03d]", id - MSG_000);
    else
        snprintf(buffer, LOC_MESSAGE_SIZE, "[ID_%d]", id);
    va_end(args);
    return buffer;
}

/* Dialog localization is a GUI concern; the GTK front-end implements
 * these (the CLI prints plain text and ignores them). */
void apply_localization(int dlg_id, HWND hDlg) { (void)dlg_id; (void)hDlg; }
void reset_localization(int dlg_id) { (void)dlg_id; }
void free_dialog_list(void) { }
BOOL get_supported_locales(const char* filename) { (void)filename; return TRUE; }
