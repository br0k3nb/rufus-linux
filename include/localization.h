/*
 * Rufus Linux port: localization API.
 *
 * Adapted from upstream src/localization.h, keeping the same names and
 * macros for the parts the port implements, so that ported modules read
 * the same way. The dialog-control application functions are implemented
 * by the GTK front-end.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RUFUS_LINUX_LOCALIZATION_H
#define RUFUS_LINUX_LOCALIZATION_H

#include <stdint.h>
#include <stddef.h>

#pragma once

// Number of concurrent localization messages (i.e. messages we can concurrently
// reference at the same time). Must be a power of 2.
#define LOC_MESSAGE_NB          32
#define LOC_MESSAGE_SIZE        2048
#define LOC_HTAB_SIZE           1031	// Using a prime speeds up the hash table init

// Largest ID we can localize (covers MSG_000..MSG_MAX and the IDC_*/IDS_*/IDD_* ranges)
#define LOC_ID_RANGE            4096

// Attributes that can be set by a translation
#define LOC_RIGHT_TO_LEFT       0x00000001
#define LOC_NEEDS_UPDATE        0x00000002

#define MSG_RTF                 0x10000000
#define MSG_MASK                0x0FFFFFFF

#define luprint(msg) uprintf("%s(%d): " msg "\n", loc_filename, loc_line_nr)
#define luprintf(msg, ...) uprintf("%s(%d): " msg "\n", loc_filename, loc_line_nr, __VA_ARGS__)

/*
 * List handling functions (stolen from libusb)
 * NB: offsetof() requires '#include <stddef.h>'
 */
struct list_head {
	struct list_head *prev, *next;
};

/* Get an entry from the list
 *  ptr - the address of this list_head element in "type"
 *  type - the data type that contains "member"
 *  member - the list_head element in "type"
 */
#define list_entry(ptr, type, member) \
	((type *)((uintptr_t)(ptr) - (uintptr_t)offsetof(type, member)))

#define list_for_each_entry(pos, head, type, member)			\
	for (pos = list_entry((head)->next, type, member);			\
		 &pos->member != (head);								\
		 pos = list_entry(pos->member.next, type, member))

#define list_for_each_entry_safe(pos, n, head, type, member)	\
	for (pos = list_entry((head)->next, type, member),			\
		 n = list_entry(pos->member.next, type, member);		\
		 &pos->member != (head);								\
		 pos = n, n = list_entry(n->member.next, type, member))

#define list_empty(entry) ((entry)->next == (entry))

static __inline void list_init(struct list_head *entry)
{
	entry->prev = entry->next = entry;
}

static __inline void list_add(struct list_head *entry, struct list_head *head)
{
	entry->next = head->next;
	entry->prev = head;

	head->next->prev = entry;
	head->next = entry;
}

static __inline void list_add_tail(struct list_head *entry,
	struct list_head *head)
{
	entry->next = head;
	entry->prev = head->prev;

	head->prev->next = entry;
	head->prev = entry;
}

static __inline void list_del(struct list_head *entry)
{
	entry->next->prev = entry->prev;
	entry->prev->next = entry->next;
	entry->next = entry->prev = NULL;
}

/* Locale entry as parsed from the .loc file */
typedef struct loc_entry_struct {
	char name[32];			// "en-US"
	char display[128];		// "English (English)"
	int  attributes;		// LOC_RIGHT_TO_LEFT / LOC_NEEDS_UPDATE
	char** texts;			// [LOC_ID_RANGE], lazily allocated
} loc_entry;

extern char* loc_filename, *embedded_loc_filename;
extern int loc_line_nr;
extern BOOL en_msg_mode;

void _init_localization(BOOL reinit);
void _exit_localization(BOOL reinit);
#define init_localization() _init_localization(FALSE)
#define exit_localization() _exit_localization(FALSE)
#define reinit_localization() do {_exit_localization(TRUE); _init_localization(TRUE);} while(0)
void apply_localization(int dlg_id, HWND hDlg);
void reset_localization(int dlg_id);
void free_dialog_list(void);
char* lmprintf(uint32_t msg_id, ...);
const char* get_msg_format(int id);
const char* get_msg_format_en(int id);
BOOL get_supported_locales(const char* filename);
void toggle_default_locale(void);
const char* get_name_from_id(int id);
/* Locale lookup helpers (used by the settings/GUI layer) */
int  get_locale_index_from_name(const char* locale_name);
int  get_locale_count(void);
const char* get_locale_display_name(int index);
const char* get_locale_name(int index);
int  get_locale_attributes(int index);
/* Localized text for a plain resource ID (IDC_*, IDS_*, IDD_*) */
const char* get_localized_text(int id);

#endif /* RUFUS_LINUX_LOCALIZATION_H */
