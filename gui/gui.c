/*
 * Rufus Linux port: GTK3 front-end (rufus-gui).
 *
 * Mirrors upstream's main dialog (rufus.rc IDD_DIALOG): Drive Properties,
 * Format Options and Status sections, with the same controls, texts (from
 * res/loc/rufus.loc) and logic (the option dependencies come from the
 * shared image.c/bootcheck.c code).
 *
 * The GUI runs as the desktop user. Image analysis, checksums and device
 * enumeration happen in-process. The format/write and save operations are
 * run by the CLI in worker mode ("rufus --worker", see src/worker.c), with
 * pkexec when a real device is involved: the worker's questions (upstream's
 * Notification()/SelectionDialog() calls) are answered with GTK dialogs.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"
#include "localization.h"

#include <gtk/gtk.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAX_FAT32_SIZE
#define MAX_FAT32_SIZE  (2 * TB)    /* as upstream rufus.h */
#endif
extern BOOL use_fake_units;

#define PT_MBR  "mbr"
#define PT_GPT  "gpt"
#define PT_SFD  "sfd"

static struct {
    GtkWidget *window, *device_combo, *save_btn, *boot_combo, *hash_btn, *select_btn;
    GtkWidget *imgopt_label, *imgopt_combo, *persist_box, *persist_scale, *persist_spin, *persist_units;
    GtkWidget *pt_combo, *tt_combo, *adv_drive_exp, *list_hdd_chk, *old_bios_chk, *uefi_val_chk;
    GtkWidget *label_entry, *fs_combo, *cluster_combo, *adv_fmt_exp, *quick_chk, *extlabel_chk;
    GtkWidget *bb_chk, *bb_combo, *progress, *status_label, *start_btn, *close_btn;
    GtkWidget *lang_btn, *about_btn, *settings_btn, *log_btn;
    GtkWidget *hdr_drive, *hdr_format, *hdr_status, *lbl_device, *lbl_boot, *lbl_pt, *lbl_tt;
    GtkWidget *lbl_label, *lbl_fs, *lbl_cluster;
    GtkTextBuffer* log_buffer;
    GtkWidget* log_window;
} ui;

static char drive_signature[4096] = "";
static BOOL busy = FALSE;               /* an operation (worker, scan or hash) is running */
static BOOL updating = FALSE;           /* the UI is being populated: ignore change signals */
static BOOL user_changed_label = FALSE;
static GPid worker_pid = 0;
static int worker_stdin = -1;
static GString* worker_line = NULL;
static unsigned long worker_error = 0;
static char app_exe_dir[MAX_PATH] = "";
static BOOL sb_data_fetched = FALSE;
static char virtual_drive[MAX_PATH] = "";   /* --device FILE: image file used as a drive */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static const char* T(int id, const char* def)
{
    const char* s = get_localized_text(id);
    return (s != NULL && s[0] != 0) ? s : def;
}

static char* M(uint32_t id)
{
    return lmprintf(id);
}

static void set_label(GtkWidget* w, const char* text, BOOL bold)
{
    if (bold) {
        char* markup = g_markup_printf_escaped("<b><big>%s</big></b>", text);
        gtk_label_set_markup(GTK_LABEL(w), markup);
        g_free(markup);
    } else {
        gtk_label_set_text(GTK_LABEL(w), text);
    }
}

static const char* combo_id(GtkWidget* combo)
{
    const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    return (id != NULL) ? id : "";
}

static int combo_int(GtkWidget* combo, int def)
{
    const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    return (id != NULL) ? atoi(id) : def;
}

static void combo_add(GtkWidget* combo, int id, const char* text)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", id);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), buf, text);
}

static void combo_select(GtkWidget* combo, int id)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", id);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), buf))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

static void strip_ansi(char* s)
{
    char* p;
    while ((p = strstr(s, "\033[K")) != NULL)
        memmove(p, &p[3], strlen(&p[3]) + 1);
}

/* ------------------------------------------------------------------ */
/* Log, status and progress (hooks for the shared code)                */
/* ------------------------------------------------------------------ */
static gboolean idle_log(gpointer data)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(ui.log_buffer, &end);
    gtk_text_buffer_insert(ui.log_buffer, &end, (char*)data, -1);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void gui_log(const char* line)
{
    size_t len = strlen(line);
    char* copy = (len > 0 && line[len - 1] == '\n') ? g_strdup(line) : g_strconcat(line, "\n", NULL);
    /* Keep a copy on the console, as upstream does with OutputDebugString */
    fputs(copy, stderr);
    g_idle_add(idle_log, copy);
}

static gboolean idle_status(gpointer data)
{
    gtk_label_set_text(GTK_LABEL(ui.status_label), (char*)data);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void gui_status(BOOL info, const char* text)
{
    (void)info;
    g_idle_add(idle_status, g_strdup(text));
}

typedef struct { double percent; char* text; } gui_progress_t;
static gboolean idle_progress(gpointer data)
{
    gui_progress_t* p = (gui_progress_t*)data;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), p->percent / 100.0);
    if (p->text != NULL && p->text[0] != 0)
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress), p->text);
    g_free(p->text);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void gui_progress(const char* label, const char* text, double percent)
{
    gui_progress_t* p = g_new0(gui_progress_t, 1);
    (void)label;
    p->percent = percent;
    p->text = g_strdup((text != NULL) ? text : "");
    strip_ansi(p->text);
    g_idle_add(idle_progress, p);
}

/* ------------------------------------------------------------------ */
/* Message boxes and selection dialogs                                 */
/* ------------------------------------------------------------------ */
static int run_message_box(int type, const char* title, const char* msg)
{
    GtkMessageType mt = GTK_MESSAGE_OTHER;
    GtkWidget* dlg;
    int r, buttons = type & 0x0F;

    /* Automated tests: accept the default answer */
    if (getenv("RUFUS_GUI_AUTOANSWER") != NULL) {
        uprintf("[auto-answer] %s: %s", (title != NULL) ? title : "", msg);
        return (buttons == MB_YESNO || buttons == MB_YESNOCANCEL) ? IDYES :
               (buttons == MB_ABORTRETRYIGNORE) ? IDIGNORE : (buttons == MB_RETRYCANCEL) ? IDCANCEL : IDOK;
    }

    switch (type & 0xF0) {
    case MB_ICONERROR:       mt = GTK_MESSAGE_ERROR; break;
    case MB_ICONQUESTION:    mt = GTK_MESSAGE_QUESTION; break;
    case MB_ICONWARNING:     mt = GTK_MESSAGE_WARNING; break;
    case MB_ICONINFORMATION: mt = GTK_MESSAGE_INFO; break;
    }
    dlg = gtk_message_dialog_new(GTK_WINDOW(ui.window), GTK_DIALOG_MODAL, mt, GTK_BUTTONS_NONE, "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), (title != NULL) ? title : APPLICATION_NAME);
    switch (buttons) {
    case MB_OKCANCEL:
        gtk_dialog_add_buttons(GTK_DIALOG(dlg), M(MSG_007), IDCANCEL, "OK", IDOK, NULL);
        break;
    case MB_ABORTRETRYIGNORE:
        gtk_dialog_add_buttons(GTK_DIALOG(dlg), M(MSG_038), IDABORT, "Retry", IDRETRY, "Ignore", IDIGNORE, NULL);
        break;
    case MB_YESNOCANCEL:
        gtk_dialog_add_buttons(GTK_DIALOG(dlg), M(MSG_007), IDCANCEL, M(MSG_009), IDNO, M(MSG_008), IDYES, NULL);
        break;
    case MB_YESNO:
        gtk_dialog_add_buttons(GTK_DIALOG(dlg), M(MSG_009), IDNO, M(MSG_008), IDYES, NULL);
        break;
    case MB_RETRYCANCEL:
        gtk_dialog_add_buttons(GTK_DIALOG(dlg), M(MSG_007), IDCANCEL, "Retry", IDRETRY, NULL);
        break;
    case MB_CLOSE:
        gtk_dialog_add_buttons(GTK_DIALOG(dlg), M(MSG_006), IDOK, NULL);
        break;
    default:
        gtk_dialog_add_buttons(GTK_DIALOG(dlg), "OK", IDOK, NULL);
        break;
    }
    r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r < 0)  /* closed with the window manager */
        r = (buttons == MB_YESNO) ? IDNO : (buttons == MB_OK || buttons == MB_CLOSE) ? IDOK :
            (buttons == MB_ABORTRETRYIGNORE) ? IDABORT : IDCANCEL;
    return r;
}

/* Selection dialog: radio buttons or check boxes, plus the local account
 * name entry of upstream's Windows User Experience dialog */
static int run_selection_dialog(int style, int mask, int username_index, const char* title,
                                const char* message, char** choices, int n, char* username, size_t username_len)
{
    GtkWidget *dlg, *box, *lbl, **btn, *user_entry = NULL;
    GSList* group = NULL;
    int i, r = -1;

    if (getenv("RUFUS_GUI_AUTOANSWER") != NULL) {
        uprintf("[auto-answer] %s", title);
        return (style == BS_AUTOCHECKBOX) ? mask : 1;
    }

    dlg = gtk_dialog_new_with_buttons(title, GTK_WINDOW(ui.window), GTK_DIALOG_MODAL,
                                      M(MSG_007), GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 6);
    lbl = gtk_label_new(message);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 70);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    btn = g_new0(GtkWidget*, n);
    for (i = 0; i < n; i++) {
        if (style == BS_AUTOCHECKBOX) {
            btn[i] = gtk_check_button_new_with_label(choices[i]);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn[i]), (mask & (1 << i)) != 0);
        } else {
            btn[i] = gtk_radio_button_new_with_label(group, choices[i]);
            group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(btn[i]));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn[i]), i == 0);
        }
        gtk_box_pack_start(GTK_BOX(box), btn[i], FALSE, FALSE, 0);
        if ((style == BS_AUTOCHECKBOX) && (username_index == i + 1)) {
            const char* user = getenv("USER");
            user_entry = gtk_entry_new();
            gtk_entry_set_text(GTK_ENTRY(user_entry), (username != NULL && username[0] != 0) ? username :
                               ((user != NULL) ? user : ""));
            gtk_widget_set_margin_start(user_entry, 24);
            gtk_box_pack_start(GTK_BOX(box), user_entry, FALSE, FALSE, 0);
        }
    }
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        r = 0;
        for (i = 0; i < n; i++)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn[i])))
                r |= 1 << i;
        if (user_entry != NULL && username != NULL)
            snprintf(username, username_len, "%s", gtk_entry_get_text(GTK_ENTRY(user_entry)));
    }
    gtk_widget_destroy(dlg);
    g_free(btn);
    return r;
}

/* The hooks may be called from a worker thread: run the dialogs on the
 * main thread and wait for the answer */
typedef struct {
    int kind, type, r;
    const char *title, *msg;
    selection_dialog_options_t* options;
    GMutex lock;
    GCond cond;
    gboolean done;
} dialog_call_t;

extern char unattend_username[];

static gboolean idle_dialog(gpointer data)
{
    dialog_call_t* c = (dialog_call_t*)data;
    if (c->kind == 0)
        c->r = run_message_box(c->type, c->title, c->msg);
    else
        c->r = run_selection_dialog(c->options->style, c->options->mask, c->options->username_index, c->title,
                                    c->msg, c->options->choices.String, (int)c->options->choices.Index,
                                    unattend_username, MAX_USERNAME_LENGTH);
    g_mutex_lock(&c->lock);
    c->done = TRUE;
    g_cond_signal(&c->cond);
    g_mutex_unlock(&c->lock);
    return G_SOURCE_REMOVE;
}

static int call_dialog(dialog_call_t* c)
{
    if (g_main_context_is_owner(g_main_context_default())) {
        c->done = FALSE;
        idle_dialog(c);
        return c->r;
    }
    g_mutex_init(&c->lock);
    g_cond_init(&c->cond);
    c->done = FALSE;
    g_idle_add(idle_dialog, c);
    g_mutex_lock(&c->lock);
    while (!c->done)
        g_cond_wait(&c->cond, &c->lock);
    g_mutex_unlock(&c->lock);
    g_mutex_clear(&c->lock);
    g_cond_clear(&c->cond);
    return c->r;
}

static int gui_notification(int type, const char* title, const char* msg)
{
    dialog_call_t c = { .kind = 0, .type = type, .title = title, .msg = msg };
    return call_dialog(&c);
}

static int gui_selection(char* title, char* message, selection_dialog_options_t* options)
{
    dialog_call_t c = { .kind = 1, .title = title, .msg = message, .options = options };
    return call_dialog(&c);
}

/* ------------------------------------------------------------------ */
/* Options logic (upstream's combo box updates)                        */
/* ------------------------------------------------------------------ */
static void update_cluster_sizes(void);
static void update_partition_and_target(void);

static BOOL selected_drive_ok(void)
{
    return GetSelectedDriveIndex() >= 0;
}

/* The file systems that can be used for the current selection */
static uint32_t allowed_fs_mask(void)
{
    uint32_t mask = 0;
    uint64_t size = SelectedDrive.DiskSize;

    if (size <= 4 * GB)
        mask |= 1 << FS_FAT16;
    if (size <= MAX_FAT32_SIZE)
        mask |= 1 << FS_FAT32;
    mask |= (1 << FS_NTFS) | (1 << FS_EXFAT) | (1 << FS_UDF) | (1 << FS_EXT2) | (1 << FS_EXT3) | (1 << FS_EXT4);
    switch (boot_type) {
    case BT_MSDOS:
    case BT_FREEDOS:
    case BT_REACTOS:
    case BT_SYSLINUX_V4:
        mask &= (1 << FS_FAT16) | (1 << FS_FAT32);
        break;
    case BT_SYSLINUX_V6:
        mask &= (1 << FS_FAT16) | (1 << FS_FAT32);
        break;
    case BT_GRUB4DOS:
        mask &= (1 << FS_FAT16) | (1 << FS_FAT32) | (1 << FS_NTFS) | (1 << FS_EXFAT);
        break;
    case BT_UEFI_NTFS:
        mask &= (1 << FS_NTFS) | (1 << FS_EXFAT);
        break;
    case BT_IMAGE:
        if (image_path[0] == 0 || write_as_image)
            break;
        mask &= (1 << FS_FAT16) | (1 << FS_FAT32) | (1 << FS_NTFS) | (1 << FS_EXFAT) |
                (1 << FS_EXT2) | (1 << FS_EXT3) | (1 << FS_EXT4);
        if (img_report.has_4GB_file && !IS_FAT32_COMPAT(img_report))
            mask &= ~((1 << FS_FAT16) | (1 << FS_FAT32));
        if (img_report.needs_ntfs)
            mask &= (1 << FS_NTFS);
        break;
    }
    return mask;
}

static void update_file_systems(void)
{
    uint32_t mask = allowed_fs_mask();
    int i, fs = combo_int(ui.fs_combo, -1), preferred;

    updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.fs_combo));
    for (i = 0; i < FS_MAX; i++)
        if (mask & (1 << i))
            combo_add(ui.fs_combo, i, FileSystemLabel[i]);
    preferred = (SelectedDrive.DiskSize > 32 * GB) ? FS_NTFS : FS_FAT32;
    if (boot_type == BT_UEFI_NTFS)
        preferred = FS_NTFS;
    if ((boot_type == BT_IMAGE) && (image_path[0] != 0)) {
        int pfs = GetPreferredFSForImage(mask);
        if (pfs != FS_UNKNOWN)
            preferred = pfs;
    } else if ((fs >= 0) && (mask & (1 << fs))) {
        preferred = fs;
    }
    if (!(mask & (1 << preferred)))
        preferred = (mask & (1 << FS_FAT32)) ? FS_FAT32 : FS_NTFS;
    combo_select(ui.fs_combo, preferred);
    updating = FALSE;
    gtk_widget_set_sensitive(ui.fs_combo, !((boot_type == BT_IMAGE) && write_as_image) && !busy);
    update_cluster_sizes();
}

static void update_cluster_sizes(void)
{
    int fs = combo_int(ui.fs_combo, FS_FAT32);
    uint32_t sz, min = 512, max = 64 * KB;
    char buf[64];

    updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.cluster_combo));
    combo_add(ui.cluster_combo, 0, M(MSG_029));
    if (fs == FS_EXFAT)
        max = 32 * MB;
    else if (IS_EXT(fs))
        min = 1 * KB;
    else if (fs == FS_UDF)
        max = 0;
    for (sz = min; sz != 0 && sz <= max; sz <<= 1) {
        if (sz < KB)
            snprintf(buf, sizeof(buf), "%u %s", sz, M(MSG_026));
        else if (sz < MB)
            snprintf(buf, sizeof(buf), "%u %s", (unsigned)(sz / KB), M(MSG_027));
        else
            snprintf(buf, sizeof(buf), "%u %s", (unsigned)(sz / MB), M(MSG_028));
        combo_add(ui.cluster_combo, (int)sz, buf);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.cluster_combo), 0);
    updating = FALSE;
}

static void update_partition_and_target(void)
{
    BOOL allowed_pt[3], allowed_tt[3];
    const char* cur = combo_id(ui.pt_combo);
    int selected_pt = (strcmp(cur, PT_GPT) == 0) ? PARTITION_STYLE_GPT :
                      (strcmp(cur, PT_MBR) == 0) ? PARTITION_STYLE_MBR :
                      (strcmp(cur, PT_SFD) == 0) ? PARTITION_STYLE_SFD : -1;

    if (!selected_drive_ok())
        return;
    GetPartitionSchemeAndTargetSystem(updating ? -1 : selected_pt, allowed_pt, allowed_tt);
    updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.pt_combo));
    if (allowed_pt[0])
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.pt_combo), PT_MBR, "MBR");
    if (allowed_pt[1])
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.pt_combo), PT_GPT, "GPT");
    if (allowed_pt[2])
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.pt_combo), PT_SFD, "Super Floppy Disk");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui.pt_combo),
        (partition_type == PARTITION_STYLE_GPT) ? PT_GPT : (partition_type == PARTITION_STYLE_MBR) ? PT_MBR : PT_SFD);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.tt_combo));
    if (allowed_tt[0])
        combo_add(ui.tt_combo, TT_BIOS, M(MSG_031));
    if (allowed_tt[1])
        combo_add(ui.tt_combo, TT_UEFI, M(MSG_032));
    if (allowed_tt[2] && !allowed_tt[0])
        combo_add(ui.tt_combo, TT_BIOS, M(MSG_033));
    combo_select(ui.tt_combo, target_type);
    updating = FALSE;
    update_file_systems();
}

static void update_image_options(void)
{
    BOOL show_persistence = (boot_type == BT_IMAGE) && (image_path[0] != 0) && img_report.is_iso &&
                            HAS_PERSISTENCE(img_report);
    BOOL show_wim = (boot_type == BT_IMAGE) && (image_path[0] != 0) && HAS_WININST(img_report);

    gtk_widget_set_visible(ui.imgopt_label, show_persistence || show_wim);
    gtk_widget_set_visible(ui.imgopt_combo, show_wim);
    gtk_widget_set_visible(ui.persist_box, show_persistence);
    if (show_persistence)
        set_label(ui.imgopt_label, M(MSG_123), FALSE);
    else if (show_wim)
        set_label(ui.imgopt_label, T(IDS_IMAGE_OPTION_TXT, "Image Option"), FALSE);
}

static void propose_label(void)
{
    char* lbl = NULL;

    if (user_changed_label)
        return;
    updating = TRUE;
    if ((boot_type == BT_IMAGE) && (image_path[0] != 0) && (img_report.label[0] != 0))
        gtk_entry_set_text(GTK_ENTRY(ui.label_entry), img_report.label);
    else if (selected_drive_ok() && GetDriveLabel(SelectedDrive.DeviceNumber, NULL, &lbl, TRUE) && lbl != NULL && lbl[0] != 0)
        gtk_entry_set_text(GTK_ENTRY(ui.label_entry), lbl);
    else
        gtk_entry_set_text(GTK_ENTRY(ui.label_entry), M(MSG_207));
    updating = FALSE;
}

static void update_start_button(void)
{
    BOOL ok = selected_drive_ok() && !busy && !((boot_type == BT_IMAGE) && (image_path[0] == 0));
    gtk_widget_set_sensitive(ui.start_btn, ok);
}

static void enable_controls(BOOL enable)
{
    GtkWidget* w[] = { ui.device_combo, ui.save_btn, ui.boot_combo, ui.select_btn, ui.hash_btn, ui.imgopt_combo,
        ui.persist_box, ui.pt_combo, ui.tt_combo, ui.adv_drive_exp, ui.label_entry, ui.fs_combo,
        ui.cluster_combo, ui.adv_fmt_exp, ui.lang_btn, ui.settings_btn };
    size_t i;
    for (i = 0; i < G_N_ELEMENTS(w); i++)
        gtk_widget_set_sensitive(w[i], enable);
    gtk_widget_set_visible(ui.hash_btn, (image_path[0] != 0));
    gtk_button_set_label(GTK_BUTTON(ui.close_btn), enable ? M(MSG_006) : M(MSG_007));
    update_start_button();
}

/* ------------------------------------------------------------------ */
/* Devices                                                             */
/* ------------------------------------------------------------------ */
static void refresh_devices(BOOL force)
{
    char sig[4096] = "", entry[256], prev[MAX_PATH] = "";
    int i, count, n = 0;
    BOOL list_hdd = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.list_hdd_chk));

    if (busy)
        return;
    if (virtual_drive[0] != 0) {
        /* A virtual drive (image file) replaces the device list, as in the CLI */
        char id[16], text[MAX_PATH + 64];
        struct stat st;
        if (!force || stat(virtual_drive, &st) != 0 || !SelectDriveByPath(virtual_drive))
            return;
        updating = TRUE;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.device_combo));
        snprintf(id, sizeof(id), "%d", (int)DriveIndex);
        snprintf(text, sizeof(text), "Virtual drive (%s) [%s]", virtual_drive,
                 SizeToHumanReadable((uint64_t)st.st_size, FALSE, FALSE));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.device_combo), id, text);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui.device_combo), 0);
        updating = FALSE;
        PrintStatus(0, MSG_208, 1);
        g_signal_emit_by_name(ui.device_combo, "changed");
        return;
    }
    if (selected_drive_ok())
        snprintf(prev, sizeof(prev), "%s", GetPhysicalName(SelectedDrive.DeviceNumber));
    GetDevices(DRIVE_INDEX_MIN);
    count = GetDriveCount();
    for (i = 0; i < count; i++) {
        uint64_t size = 0;
        BOOL usb = FALSE, removable = FALSE;
        uint16_t vid = 0, pid = 0;
        const char* name = "";
        if (!GetDriveEntryInfo(i, &size, &usb, &removable, &vid, &pid, &name))
            continue;
        snprintf(entry, sizeof(entry), "%s|%llu;", GetDriveEntryPath(i), (unsigned long long)size);
        if (strlen(sig) + strlen(entry) < sizeof(sig))
            strcat(sig, entry);
    }
    strcat(sig, list_hdd ? "H" : "");
    if (!force && strcmp(sig, drive_signature) == 0)
        return;
    snprintf(drive_signature, sizeof(drive_signature), "%s", sig);

    updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.device_combo));
    for (i = 0; i < count; i++) {
        uint64_t size = 0;
        BOOL usb = FALSE, removable = FALSE;
        uint16_t vid = 0, pid = 0;
        const char* name = "";
        char text[256], id[16];
        if (!GetDriveEntryInfo(i, &size, &usb, &removable, &vid, &pid, &name))
            continue;
        if (IsFilteredDrive(DRIVE_INDEX_MIN + i))
            continue;
        if (!usb && !removable && !list_hdd)
            continue;
        if (usb && !list_hdd && (IsHDD(DRIVE_INDEX_MIN + i, vid, pid, name) > 0))
            continue;
        snprintf(text, sizeof(text), "%s [%s]", name, SizeToHumanReadable(size, FALSE, use_fake_units));
        snprintf(id, sizeof(id), "%d", DRIVE_INDEX_MIN + i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.device_combo), id, text);
        if (strcmp(GetDriveEntryPath(i), prev) == 0)
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui.device_combo), id);
        n++;
    }
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(ui.device_combo)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui.device_combo), 0);
    updating = FALSE;
    PrintStatus(0, (n == 1) ? MSG_208 : MSG_209, n);
    if (n == 0) {
        gtk_widget_set_sensitive(ui.save_btn, FALSE);
        update_start_button();
    } else {
        g_signal_emit_by_name(ui.device_combo, "changed");
    }
}

static gboolean poll_devices(gpointer data)
{
    (void)data;
    refresh_devices(FALSE);
    return G_SOURCE_CONTINUE;
}

static void on_device_changed(GtkComboBox* combo, gpointer data)
{
    char fs_name[64];
    int idx = combo_int(GTK_WIDGET(combo), -1);

    (void)data;
    if (updating || idx < 0)
        return;
    if ((virtual_drive[0] == 0) && !SelectDriveByIndex((DWORD)idx))
        return;
    GetDrivePartitionData(SelectedDrive.DeviceNumber, fs_name, sizeof(fs_name), TRUE);
    gtk_widget_set_sensitive(ui.save_btn, !busy);
    propose_label();
    update_partition_and_target();
    update_start_button();
}

/* ------------------------------------------------------------------ */
/* Boot selection and image                                            */
/* ------------------------------------------------------------------ */
static void populate_boot_selection(void)
{
    char buf[128];

    updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.boot_combo));
    combo_add(ui.boot_combo, BT_NON_BOOTABLE, M(MSG_279));
    if (image_path[0] != 0) {
        const char* base = strrchr(image_path, '/');
        combo_add(ui.boot_combo, BT_IMAGE, (base != NULL) ? &base[1] : image_path);
    } else {
        snprintf(buf, sizeof(buf), "%s", lmprintf(MSG_281, M(MSG_280)));
        combo_add(ui.boot_combo, BT_IMAGE, buf);
    }
    combo_add(ui.boot_combo, BT_FREEDOS, "FreeDOS");
    combo_add(ui.boot_combo, BT_MSDOS, "MS-DOS");
    combo_add(ui.boot_combo, BT_SYSLINUX_V4, "Syslinux 4.07");
    combo_add(ui.boot_combo, BT_SYSLINUX_V6, "Syslinux 6.04");
    combo_add(ui.boot_combo, BT_REACTOS, "ReactOS");
    combo_add(ui.boot_combo, BT_GRUB4DOS, "Grub4DOS 0.4.6a");
    combo_add(ui.boot_combo, BT_UEFI_NTFS, "UEFI:NTFS");
    combo_select(ui.boot_combo, boot_type);
    updating = FALSE;
}

static void on_boot_changed(GtkComboBox* combo, gpointer data)
{
    (void)data;
    if (updating)
        return;
    boot_type = combo_int(GTK_WIDGET(combo), BT_NON_BOOTABLE);
    write_as_image = (boot_type == BT_IMAGE) && (image_path[0] != 0) && !img_report.is_iso && !img_report.is_windows_img;
    update_image_options();
    propose_label();
    update_partition_and_target();
    update_start_button();
}

static gpointer scan_thread(gpointer data)
{
    BOOL r;
    (void)data;
    /* Upstream retrieves the latest UEFI revocation data on startup */
    if (!sb_data_fetched) {
        FetchSecureBootData();
        sb_data_fetched = TRUE;
    }
    r = ScanImage();
    return GINT_TO_POINTER(r);
}

static gboolean scan_done(gpointer data)
{
    GThread* thread = (GThread*)data;
    BOOL r = GPOINTER_TO_INT(g_thread_join(thread));

    busy = FALSE;
    if (!r) {
        image_path[0] = 0;
        boot_type = BT_NON_BOOTABLE;
        PrintStatus(0, MSG_203);
    } else {
        const char* base = strrchr(image_path, '/');
        boot_type = BT_IMAGE;
        write_as_image = !img_report.is_iso && !img_report.is_windows_img;
        PrintStatus(0, MSG_205, (base != NULL) ? &base[1] : image_path);
        uprintf("Using image: %s (%s)", (base != NULL) ? &base[1] : image_path,
                SizeToHumanReadable(img_report.image_size, FALSE, FALSE));
    }
    user_changed_label = FALSE;
    populate_boot_selection();
    enable_controls(TRUE);
    update_image_options();
    propose_label();
    update_partition_and_target();
    update_start_button();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress), "");
    if (getenv("RUFUS_GUI_AUTOSTART") != NULL)
        g_signal_emit_by_name(ui.start_btn, "clicked");
    return G_SOURCE_REMOVE;
}

static gpointer scan_wrapper(gpointer data)
{
    gpointer r = scan_thread(data);
    g_idle_add(scan_done, g_thread_self());
    return r;
}

static void set_image(const char* path)
{
    if (busy || path == NULL)
        return;
    snprintf(image_path, sizeof(image_path), "%s", path);
    busy = TRUE;
    enable_controls(FALSE);
    gtk_widget_set_sensitive(ui.close_btn, TRUE);
    PrintStatus(0, MSG_202);
    g_thread_new("scan", scan_wrapper, NULL);
}

static void on_select_clicked(GtkButton* b, gpointer data)
{
    GtkWidget* dlg;
    GtkFileFilter* f;
    char* file;

    (void)b; (void)data;
    dlg = gtk_file_chooser_dialog_new(T(IDC_SELECT, "Select"), GTK_WINDOW(ui.window), GTK_FILE_CHOOSER_ACTION_OPEN,
                                      M(MSG_007), GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Disk or ISO image");
    {
        const char* pat[] = { "*.iso", "*.img", "*.vhd", "*.vhdx", "*.gz", "*.xz", "*.bz2", "*.zst", "*.lzma",
                              "*.zip", "*.Z", "*.vtsi", "*.wim", "*.esd", "*.bin", "*.raw", "*.dd" };
        size_t i;
        for (i = 0; i < G_N_ELEMENTS(pat); i++) {
            char* up = g_ascii_strup(pat[i], -1);
            gtk_file_filter_add_pattern(f, pat[i]);
            gtk_file_filter_add_pattern(f, up);
            g_free(up);
        }
    }
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "*.*");
    gtk_file_filter_add_pattern(f, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    if (image_path[0] != 0)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), image_path);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_widget_destroy(dlg);
        set_image(file);
        g_free(file);
        return;
    }
    gtk_widget_destroy(dlg);
}

static void on_drag_data(GtkWidget* w, GdkDragContext* ctx, gint x, gint y, GtkSelectionData* sel,
                         guint info, guint time, gpointer data)
{
    gchar** uris = gtk_selection_data_get_uris(sel);
    (void)w; (void)x; (void)y; (void)info; (void)data;
    if (uris != NULL && uris[0] != NULL) {
        char* path = g_filename_from_uri(uris[0], NULL, NULL);
        set_image(path);
        g_free(path);
    }
    g_strfreev(uris);
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

/* ------------------------------------------------------------------ */
/* Checksums                                                           */
/* ------------------------------------------------------------------ */
static gpointer hash_thread(gpointer data)
{
    DWORD_PTR affinity[HASH_MAX + 1] = { 0 };
    (void)data;
    return GINT_TO_POINTER(HashThread(affinity));
}

static gboolean hash_done(gpointer data)
{
    GThread* thread = (GThread*)data;
    int r = GPOINTER_TO_INT(g_thread_join(thread));
    char msg[1024];

    busy = FALSE;
    enable_controls(TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress), "");
    PrintStatus(0, MSG_210);
    if (r != 0)
        return G_SOURCE_REMOVE;
    snprintf(msg, sizeof(msg), "MD5:\t%s\nSHA1:\t%s\nSHA256:\t%s%s%s", hash_str[0], hash_str[1], hash_str[2],
             enable_extra_hashes ? "\nSHA512:\t" : "", enable_extra_hashes ? hash_str[3] : "");
    {
        /* Selectable text, so that the checksums can be copied */
        GtkWidget* dlg = gtk_dialog_new_with_buttons(M(MSG_314), GTK_WINDOW(ui.window), GTK_DIALOG_MODAL,
                                                     M(MSG_006), GTK_RESPONSE_CLOSE, NULL);
        GtkWidget* lbl = gtk_label_new(msg);
        gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
        gtk_widget_set_margin_start(lbl, 16);
        gtk_widget_set_margin_end(lbl, 16);
        gtk_widget_set_margin_top(lbl, 16);
        gtk_widget_set_margin_bottom(lbl, 16);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), lbl, TRUE, TRUE, 0);
        gtk_widget_show_all(dlg);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
    return G_SOURCE_REMOVE;
}

static gpointer hash_wrapper(gpointer data)
{
    gpointer r = hash_thread(data);
    g_idle_add(hash_done, g_thread_self());
    return r;
}

static void on_hash_clicked(GtkButton* b, gpointer data)
{
    (void)b; (void)data;
    if (busy || image_path[0] == 0)
        return;
    busy = TRUE;
    ErrorStatus = 0;
    enable_controls(FALSE);
    g_thread_new("hash", hash_wrapper, NULL);
}

/* ------------------------------------------------------------------ */
/* Worker (format / save operations)                                   */
/* ------------------------------------------------------------------ */
static void worker_write(const char* line)
{
    if (worker_stdin >= 0 && write(worker_stdin, line, strlen(line)) < 0)
        uprintf("Could not send '%s' to the worker", line);
}

static char* decode_field(char* s)
{
    char* p;
    for (p = s; *p != 0; p++)
        if (*p == 0x1e)
            *p = '\n';
    return s;
}

static void handle_worker_line(char* line)
{
    char reply[600];

    if (strncmp(line, "@@R ", 4) != 0 || line[4] == 0 || line[5] != ' ') {
        gui_log(line);
        return;
    }
    switch (line[4]) {
    case 'L':
        gui_log(decode_field(&line[6]));
        break;
    case 'S': {
        char* sep = strchr(&line[6], 0x1f);
        gtk_label_set_text(GTK_LABEL(ui.status_label), decode_field((sep != NULL) ? &sep[1] : &line[6]));
        break;
    }
    case 'P': {
        char* sep = strchr(&line[6], 0x1f);
        double percent = g_ascii_strtod(&line[6], NULL);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), percent / 100.0);
        if (sep != NULL)
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress), decode_field(&sep[1]));
        break;
    }
    case 'Q': {
        char** f = g_strsplit(&line[6], "\x1f", 3);
        int r = run_message_box(atoi(f[0]), (f[1] != NULL) ? decode_field(f[1]) : APPLICATION_NAME,
                                (f[1] != NULL && f[2] != NULL) ? decode_field(f[2]) : "");
        snprintf(reply, sizeof(reply), "A %d\n", r);
        worker_write(reply);
        g_strfreev(f);
        break;
    }
    case 'C': {
        char** f = g_strsplit(&line[6], "\x1f", -1);
        char user[MAX_USERNAME_LENGTH] = "";
        int n = (int)g_strv_length(f), r;
        if (n >= 5) {
            int i;
            for (i = 5; i < n; i++)
                decode_field(f[i]);
            r = run_selection_dialog(atoi(f[0]), atoi(f[1]), atoi(f[2]), decode_field(f[3]), decode_field(f[4]),
                                     &f[5], n - 5, user, sizeof(user));
        } else {
            r = -1;
        }
        snprintf(reply, sizeof(reply), "A %d\x1f%s\n", r, user);
        worker_write(reply);
        g_strfreev(f);
        break;
    }
    case 'E':
        worker_error = strtoul(&line[6], NULL, 10);
        break;
    default:
        gui_log(line);
        break;
    }
}

static gboolean on_worker_output(GIOChannel* ch, GIOCondition cond, gpointer data)
{
    gchar buf[4096];
    ssize_t n, i;
    (void)data;

    if (!(cond & (G_IO_IN | G_IO_HUP)))
        return G_SOURCE_REMOVE;
    /* Read what is available (GIOChannel's buffered reads would block) */
    n = read(g_io_channel_unix_get_fd(ch), buf, sizeof(buf));
    if (n <= 0)
        return G_SOURCE_REMOVE;
    for (i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            handle_worker_line(worker_line->str);
            g_string_truncate(worker_line, 0);
        } else {
            g_string_append_c(worker_line, buf[i]);
        }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_worker_stderr(GIOChannel* ch, GIOCondition cond, gpointer data)
{
    gchar buf[4096];
    ssize_t n;
    (void)data;

    if (!(cond & (G_IO_IN | G_IO_HUP)))
        return G_SOURCE_REMOVE;
    n = read(g_io_channel_unix_get_fd(ch), buf, sizeof(buf) - 1);
    if (n <= 0)
        return G_SOURCE_REMOVE;
    buf[n] = 0;
    gui_log(buf);
    return G_SOURCE_CONTINUE;
}

static gboolean take_snapshot(gpointer data);

static void on_worker_exit(GPid pid, gint status, gpointer data)
{
    gboolean ok = g_spawn_check_exit_status(status, NULL);
    (void)data;

    g_spawn_close_pid(pid);
    worker_pid = 0;
    if (worker_stdin >= 0)
        close(worker_stdin);
    worker_stdin = -1;
    if (worker_line->len > 0) {
        handle_worker_line(worker_line->str);
        g_string_truncate(worker_line, 0);
    }
    busy = FALSE;
    enable_controls(TRUE);
    drive_signature[0] = 0;
    refresh_devices(TRUE);
    if (ok) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), 1.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress), M(MSG_210));
        PrintStatus(0, MSG_210);
        gtk_window_set_urgency_hint(GTK_WINDOW(ui.window), TRUE);
    } else {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), 0.0);
        if (IS_ERROR(worker_error) && SCODE_CODE(worker_error) == ERROR_CANCELLED) {
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress), M(MSG_211));
            PrintStatus(0, MSG_211);
        } else if (WIFEXITED(status) && (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127)) {
            /* pkexec: authorization dismissed or failed */
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress), "");
            PrintStatus(0, MSG_211);
            run_message_box(MB_OK | MB_ICONERROR, M(MSG_288), M(MSG_289));
        } else {
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress), M(MSG_212));
            PrintStatus(0, MSG_212);
            run_message_box(MB_OK | MB_ICONERROR, M(MSG_042),
                            IS_ERROR(worker_error) ? StrError((DWORD)worker_error, FALSE) : M(MSG_051));
        }
    }
    if (getenv("RUFUS_GUI_AUTOSTART") != NULL) {
        printf("GUI worker exit: %s\n", ok ? "success" : "failure");
        if (getenv("RUFUS_GUI_SNAPSHOT") != NULL)
            take_snapshot(getenv("RUFUS_GUI_SNAPSHOT"));
        else
            gtk_main_quit();
    }
}

/* Start the worker with the given arguments (NULL terminated) */
static BOOL start_worker(GPtrArray* args)
{
    GError* err = NULL;
    int out_fd, err_fd;
    GIOChannel *ch_out, *ch_err;
    char rufus_cli[MAX_PATH];
    BOOL need_root = (geteuid() != 0) && selected_drive_ok() && !IsVirtualDrive(SelectedDrive.DeviceNumber);

    snprintf(rufus_cli, sizeof(rufus_cli), "%s/rufus", app_exe_dir);
    if (need_root)
        g_ptr_array_insert(args, 0, g_strdup("pkexec"));
    g_ptr_array_insert(args, need_root ? 1 : 0, g_strdup(rufus_cli));
    g_ptr_array_add(args, NULL);
    {
        char* cmd = g_strjoinv(" ", (gchar**)args->pdata);
        uprintf("Running: %s", cmd);
        g_free(cmd);
    }
    worker_error = 0;
    if (!g_spawn_async_with_pipes(NULL, (gchar**)args->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL, NULL, &worker_pid, &worker_stdin, &out_fd, &err_fd, &err)) {
        run_message_box(MB_OK | MB_ICONERROR, M(MSG_042), err->message);
        g_error_free(err);
        return FALSE;
    }
    ch_out = g_io_channel_unix_new(out_fd);
    g_io_channel_set_encoding(ch_out, NULL, NULL);
    g_io_channel_set_close_on_unref(ch_out, TRUE);
    g_io_add_watch(ch_out, G_IO_IN | G_IO_HUP | G_IO_ERR, on_worker_output, NULL);
    g_io_channel_unref(ch_out);
    ch_err = g_io_channel_unix_new(err_fd);
    g_io_channel_set_encoding(ch_err, NULL, NULL);
    g_io_channel_set_close_on_unref(ch_err, TRUE);
    g_io_add_watch(ch_err, G_IO_IN | G_IO_HUP | G_IO_ERR, on_worker_stderr, NULL);
    g_io_channel_unref(ch_err);
    g_child_watch_add(worker_pid, on_worker_exit, NULL);
    busy = TRUE;
    enable_controls(FALSE);
    gtk_widget_set_sensitive(ui.close_btn, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), 0.0);
    return TRUE;
}

static void add_common_args(GPtrArray* a)
{
    g_ptr_array_add(a, g_strdup("--worker"));
    g_ptr_array_add(a, g_strdup("-l"));
    g_ptr_array_add(a, g_strdup(get_current_locale_name()));
    g_ptr_array_add(a, g_strdup("--device"));
    g_ptr_array_add(a, g_strdup(GetPhysicalName(SelectedDrive.DeviceNumber)));
}

static void on_start_clicked(GtkButton* b, gpointer data)
{
    GPtrArray* a;
    const char* pt = combo_id(ui.pt_combo);
    int fs = combo_int(ui.fs_combo, FS_FAT32), cs = combo_int(ui.cluster_combo, 0);
    static const char* boot_names[BT_MAX] = { "none", "msdos", "freedos", "image", "syslinux4", "syslinux6",
                                              "reactos", "grub4dos", "grub2", "uefi-ntfs" };
    (void)b; (void)data;

    if (busy || !selected_drive_ok())
        return;
    a = g_ptr_array_new_with_free_func(g_free);
    add_common_args(a);
    if (boot_type == BT_IMAGE) {
        g_ptr_array_add(a, g_strdup("-i"));
        g_ptr_array_add(a, g_strdup(image_path));
    } else if (boot_type == BT_NON_BOOTABLE) {
        g_ptr_array_add(a, g_strdup("--format"));
    } else {
        g_ptr_array_add(a, g_strdup_printf("--boot=%s", boot_names[boot_type]));
    }
    if (pt[0] != 0)
        g_ptr_array_add(a, g_strdup_printf("--partition-scheme=%s", pt));
    if (combo_int(ui.tt_combo, TT_BIOS) == TT_UEFI)
        g_ptr_array_add(a, g_strdup("--target=uefi"));
    if (!((boot_type == BT_IMAGE) && write_as_image)) {
        g_ptr_array_add(a, g_strdup_printf("--filesystem=%s", FileSystemLabel[fs]));
        g_ptr_array_add(a, g_strdup_printf("--label=%s", gtk_entry_get_text(GTK_ENTRY(ui.label_entry))));
        if (cs != 0)
            g_ptr_array_add(a, g_strdup_printf("--cluster-size=%d", cs));
    }
    g_ptr_array_add(a, g_strdup(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.quick_chk)) ? "--quick" : "--full"));
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.extlabel_chk)))
        g_ptr_array_add(a, g_strdup("--no-extended-label"));
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.old_bios_chk)))
        g_ptr_array_add(a, g_strdup("--old-bios-fixes"));
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.uefi_val_chk)))
        g_ptr_array_add(a, g_strdup("--uefi-validation"));
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.bb_chk))) {
        static const char* passes[] = { "1", "2", "4,slc", "4,mlc", "4,tlc" };
        int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.bb_combo));
        g_ptr_array_add(a, g_strdup_printf("--bad-blocks=%s", passes[(sel >= 0 && sel < 5) ? sel : 0]));
    }
    if (gtk_widget_get_visible(ui.persist_box)) {
        double v = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.persist_spin));
        if (v > 0)
            g_ptr_array_add(a, g_strdup_printf("--persistence=%.0f%s", v,
                gtk_combo_box_get_active(GTK_COMBO_BOX(ui.persist_units)) == 1 ? "G" : "M"));
    }
    ErrorStatus = 0;
    PrintStatus(0, MSG_225);
    start_worker(a);
    g_ptr_array_free(a, TRUE);
}

static void on_save_clicked(GtkButton* b, gpointer data)
{
    GtkWidget* dlg;
    GtkFileFilter* f;
    char name[64];
    struct { const char* name; const char* pat; } types[] = {
        { NULL, "*.vhdx" }, { NULL, "*.vhd" }, { "Raw disk image", "*.img" }, { NULL, "*.iso" } };
    size_t i;
    (void)b; (void)data;

    if (busy || !selected_drive_ok())
        return;
    types[0].name = M(MSG_342);
    types[1].name = M(MSG_343);
    types[3].name = M(MSG_036);
    dlg = gtk_file_chooser_dialog_new(M(MSG_304), GTK_WINDOW(ui.window), GTK_FILE_CHOOSER_ACTION_SAVE,
                                      M(MSG_007), GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    for (i = 0; i < G_N_ELEMENTS(types); i++) {
        char* label = g_strdup_printf("%s (%s)", types[i].name, types[i].pat);
        f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, label);
        gtk_file_filter_add_pattern(f, types[i].pat);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
        g_free(label);
    }
    snprintf(name, sizeof(name), "%s.vhdx", gtk_entry_get_text(GTK_ENTRY(ui.label_entry)));
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), name);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        GPtrArray* a = g_ptr_array_new_with_free_func(g_free);
        add_common_args(a);
        g_ptr_array_add(a, g_strdup_printf("--save=%s", file));
        gtk_widget_destroy(dlg);
        start_worker(a);
        g_ptr_array_free(a, TRUE);
        g_free(file);
        return;
    }
    gtk_widget_destroy(dlg);
}

static void on_close_clicked(GtkButton* b, gpointer data)
{
    (void)b; (void)data;
    if (worker_pid != 0) {
        /* Cancel, as upstream's Cancel button (the worker may run as root,
         * so ask it through its stdin rather than with a signal) */
        PrintStatus(0, MSG_201);
        worker_write("X\n");
        return;
    }
    if (busy) {
        ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
        return;
    }
    gtk_main_quit();
}

static gboolean on_delete(GtkWidget* w, GdkEvent* e, gpointer data)
{
    (void)w; (void)e; (void)data;
    if (busy) {
        on_close_clicked(NULL, NULL);
        return TRUE;
    }
    gtk_main_quit();
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Log, about, settings, language                                      */
/* ------------------------------------------------------------------ */
static void on_log_response(GtkDialog* d, gint response, gpointer data)
{
    (void)data;
    if (response == 1) {
        gtk_text_buffer_set_text(ui.log_buffer, "", -1);
    } else if (response == 2) {
        GtkWidget* dlg = gtk_file_chooser_dialog_new(T(IDC_LOG_SAVE, "Save"), GTK_WINDOW(d), GTK_FILE_CHOOSER_ACTION_SAVE,
                                                     M(MSG_007), GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "rufus.log");
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            GtkTextIter s, e;
            char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
            char* text;
            gtk_text_buffer_get_bounds(ui.log_buffer, &s, &e);
            text = gtk_text_buffer_get_text(ui.log_buffer, &s, &e, FALSE);
            if (g_file_set_contents(file, text, -1, NULL))
                PrintStatus(0, MSG_216, file);
            g_free(text);
            g_free(file);
        }
        gtk_widget_destroy(dlg);
    } else {
        gtk_widget_hide(GTK_WIDGET(d));
    }
}

static void on_log_clicked(GtkButton* b, gpointer data)
{
    GtkWidget *sw, *tv;
    (void)b; (void)data;
    if (ui.log_window == NULL) {
        ui.log_window = gtk_dialog_new_with_buttons("Log", GTK_WINDOW(ui.window), 0,
            T(IDC_LOG_CLEAR, "Clear"), 1, T(IDC_LOG_SAVE, "Save"), 2, M(MSG_006), GTK_RESPONSE_CLOSE, NULL);
        gtk_window_set_default_size(GTK_WINDOW(ui.log_window), 720, 480);
        sw = gtk_scrolled_window_new(NULL, NULL);
        tv = gtk_text_view_new_with_buffer(ui.log_buffer);
        gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
        gtk_container_add(GTK_CONTAINER(sw), tv);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(ui.log_window))), sw, TRUE, TRUE, 0);
        g_signal_connect(ui.log_window, "response", G_CALLBACK(on_log_response), NULL);
        g_signal_connect(ui.log_window, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    }
    gtk_widget_show_all(ui.log_window);
    gtk_window_present(GTK_WINDOW(ui.log_window));
}

static void on_about_clicked(GtkButton* b, gpointer data)
{
    char version[64];
    const char* authors[] = { "Pete Batard (Rufus)", "The Rufus Linux port authors", NULL };
    (void)b; (void)data;
    snprintf(version, sizeof(version), "%d.%d.%d (Linux)", RUFUS_VERSION_MAJOR, RUFUS_VERSION_MINOR, RUFUS_VERSION_BUILD);
    gtk_show_about_dialog(GTK_WINDOW(ui.window), "program-name", "Rufus", "version", version,
        "comments", "The Reliable USB Formatting Utility", "website", APPLICATION_URL,
        "license-type", GTK_LICENSE_GPL_3_0, "authors", authors, "logo-icon-name", "drive-removable-media", NULL);
}

static gpointer update_check_thread(gpointer data)
{
    CheckForUpdates(GPOINTER_TO_INT(data));
    return NULL;
}

static void on_settings_clicked(GtkButton* b, gpointer data)
{
    GtkWidget *dlg, *grid, *freq, *betas, *hashes, *lbl;
    int interval = ReadSetting32(SETTING_UPDATE_INTERVAL), r;
    (void)b; (void)data;

    dlg = gtk_dialog_new_with_buttons(T(IDS_UPDATE_SETTINGS_GRP, "Settings"), GTK_WINDOW(ui.window), GTK_DIALOG_MODAL,
        T(IDC_CHECK_NOW, "Check Now"), 1, M(MSG_006), GTK_RESPONSE_CLOSE, NULL);
    grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    lbl = gtk_label_new(T(IDS_UPDATE_FREQUENCY_TXT, "Check for updates"));
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 0, 1, 1);
    freq = gtk_combo_box_text_new();
    combo_add(freq, -1, M(MSG_013));
    combo_add(freq, 86400, M(MSG_014));
    combo_add(freq, 604800, M(MSG_015));
    combo_add(freq, 2629800, M(MSG_016));
    combo_select(freq, (interval == 0) ? 86400 : interval);
    gtk_grid_attach(GTK_GRID(grid), freq, 1, 0, 1, 1);
    betas = gtk_check_button_new_with_label(T(IDS_INCLUDE_BETAS_TXT, "Include beta versions"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(betas), ReadSettingBool(SETTING_INCLUDE_BETAS));
    gtk_grid_attach(GTK_GRID(grid), betas, 0, 1, 2, 1);
    hashes = gtk_check_button_new_with_label(M(MSG_312));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hashes), enable_extra_hashes);
    gtk_grid_attach(GTK_GRID(grid), hashes, 0, 2, 2, 1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), grid, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);
    r = gtk_dialog_run(GTK_DIALOG(dlg));
    WriteSetting32(SETTING_UPDATE_INTERVAL, combo_int(freq, 86400));
    WriteSettingBool(SETTING_INCLUDE_BETAS, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(betas)));
    enable_extra_hashes = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hashes));
    WriteSettingBool(SETTING_ENABLE_EXTRA_HASHES, enable_extra_hashes);
    gtk_widget_destroy(dlg);
    if (r == 1)
        g_thread_unref(g_thread_new("update", update_check_thread, GINT_TO_POINTER(TRUE)));
}

static void apply_texts(void);

static void on_language(GtkMenuItem* item, gpointer data)
{
    (void)item;
    if (set_locale((const char*)data)) {
        apply_texts();
        populate_boot_selection();
        update_partition_and_target();
        refresh_devices(TRUE);
    }
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */
static void on_label_changed(GtkEditable* e, gpointer data)
{
    (void)e; (void)data;
    if (!updating)
        user_changed_label = TRUE;
}

static void on_fs_changed(GtkComboBox* c, gpointer data)
{
    (void)c; (void)data;
    if (!updating)
        update_cluster_sizes();
}

static void on_pt_changed(GtkComboBox* c, gpointer data)
{
    const char* id = combo_id(GTK_WIDGET(c));
    (void)data;
    if (updating || id[0] == 0)
        return;
    partition_type = (strcmp(id, PT_GPT) == 0) ? PARTITION_STYLE_GPT : (strcmp(id, PT_MBR) == 0) ? PARTITION_STYLE_MBR : PARTITION_STYLE_SFD;
    update_partition_and_target();
}

static void on_tt_changed(GtkComboBox* c, gpointer data)
{
    (void)data;
    if (updating)
        return;
    target_type = combo_int(GTK_WIDGET(c), TT_BIOS);
    update_file_systems();
}

static void on_list_hdd_toggled(GtkToggleButton* t, gpointer data)
{
    (void)t; (void)data;
    refresh_devices(TRUE);
}

static void on_bb_toggled(GtkToggleButton* t, gpointer data)
{
    (void)data;
    gtk_widget_set_sensitive(ui.bb_combo, gtk_toggle_button_get_active(t));
}

static void on_persist_scale(GtkRange* r, gpointer data)
{
    (void)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.persist_spin), gtk_range_get_value(r));
}

static GtkWidget* add_header(GtkWidget* box, GtkWidget** lbl)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    *lbl = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(row), *lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 4);
    return row;
}

static GtkWidget* field_label(void)
{
    GtkWidget* l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    return l;
}

static void apply_texts(void)
{
    char buf[256];
    static const char* bb_passes[] = { NULL, NULL, "SLC", "MLC", "TLC" };
    int i, sel = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.bb_combo));

    gtk_window_set_title(GTK_WINDOW(ui.window), "Rufus " G_STRINGIFY(RUFUS_VERSION_MAJOR) "." G_STRINGIFY(RUFUS_VERSION_MINOR)
                         "." G_STRINGIFY(RUFUS_VERSION_BUILD));
    set_label(ui.hdr_drive, T(IDS_DRIVE_PROPERTIES_TXT, "Drive Properties"), TRUE);
    set_label(ui.hdr_format, T(IDS_FORMAT_OPTIONS_TXT, "Format Options"), TRUE);
    set_label(ui.hdr_status, T(IDS_STATUS_TXT, "Status"), TRUE);
    set_label(ui.lbl_device, T(IDS_DEVICE_TXT, "Device"), FALSE);
    set_label(ui.lbl_boot, T(IDS_BOOT_SELECTION_TXT, "Boot selection"), FALSE);
    set_label(ui.lbl_pt, T(IDS_PARTITION_TYPE_TXT, "Partition scheme"), FALSE);
    set_label(ui.lbl_tt, T(IDS_TARGET_SYSTEM_TXT, "Target system"), FALSE);
    set_label(ui.lbl_label, T(IDS_LABEL_TXT, "Volume label"), FALSE);
    set_label(ui.lbl_fs, T(IDS_FILE_SYSTEM_TXT, "File system"), FALSE);
    set_label(ui.lbl_cluster, T(IDS_CLUSTER_SIZE_TXT, "Cluster size"), FALSE);
    gtk_button_set_label(GTK_BUTTON(ui.select_btn), T(IDC_SELECT, "Select"));
    gtk_button_set_label(GTK_BUTTON(ui.start_btn), T(IDC_START, "Start"));
    gtk_button_set_label(GTK_BUTTON(ui.close_btn), busy ? M(MSG_007) : M(MSG_006));
    snprintf(buf, sizeof(buf), "%s", lmprintf(MSG_121, M(MSG_119)));
    gtk_expander_set_label(GTK_EXPANDER(ui.adv_drive_exp), buf);
    snprintf(buf, sizeof(buf), "%s", lmprintf(MSG_121, M(MSG_120)));
    gtk_expander_set_label(GTK_EXPANDER(ui.adv_fmt_exp), buf);
    gtk_button_set_label(GTK_BUTTON(ui.list_hdd_chk), T(IDC_LIST_USB_HDD, "List USB Hard Drives"));
    gtk_button_set_label(GTK_BUTTON(ui.old_bios_chk), T(IDC_OLD_BIOS_FIXES, "Add fixes for old BIOSes"));
    gtk_button_set_label(GTK_BUTTON(ui.uefi_val_chk), T(IDC_UEFI_MEDIA_VALIDATION, "Enable runtime UEFI media validation"));
    gtk_button_set_label(GTK_BUTTON(ui.quick_chk), T(IDC_QUICK_FORMAT, "Quick format"));
    gtk_button_set_label(GTK_BUTTON(ui.extlabel_chk), T(IDC_EXTENDED_LABEL, "Create extended label and icon files"));
    gtk_button_set_label(GTK_BUTTON(ui.bb_chk), T(IDC_BAD_BLOCKS, "Check device for bad blocks"));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.bb_combo));
    for (i = 0; i < 5; i++) {
        if (i < 2)
            snprintf(buf, sizeof(buf), (i == 0) ? lmprintf(MSG_034, 1) : lmprintf(MSG_035, 2, ""), "");
        else
            snprintf(buf, sizeof(buf), "%s", lmprintf(MSG_035, 4, lmprintf(MSG_087, bb_passes[i])));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.bb_combo), buf);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.bb_combo), (sel >= 0) ? sel : 0);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.imgopt_combo));
    combo_add(ui.imgopt_combo, IMOP_WIN_STANDARD, M(MSG_117));
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.imgopt_combo), 0);
    gtk_widget_set_tooltip_text(ui.hash_btn, M(MSG_272));
    gtk_widget_set_tooltip_text(ui.save_btn, M(MSG_304));
    gtk_widget_set_tooltip_text(ui.lang_btn, M(MSG_273));
    gtk_widget_set_tooltip_text(ui.about_btn, M(MSG_302));
    gtk_widget_set_tooltip_text(ui.settings_btn, M(MSG_301));
    gtk_widget_set_tooltip_text(ui.log_btn, M(MSG_303));
    gtk_widget_set_tooltip_text(ui.persist_scale, M(MSG_125));
    update_image_options();
}

static GtkWidget* icon_button(const char* icon)
{
    GtkWidget* b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    return b;
}

static GtkWidget* ellipsized_combo(void)
{
    GtkWidget* c = gtk_combo_box_text_new();
    GList* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(c));
    if (cells != NULL)
        g_object_set(cells->data, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    g_list_free(cells);
    gtk_widget_set_size_request(c, 100, -1);
    return c;
}

static void build_window(void)
{
    GtkWidget *vbox, *grid, *row, *exp_box, *bottom, *menu, *units;
    GtkTargetEntry targets[] = { { "text/uri-list", 0, 0 } };
    int i;

    /* RUFUS_GUI_SNAPSHOT: render offscreen and save a screenshot (tests) */
    ui.window = (getenv("RUFUS_GUI_SNAPSHOT") != NULL) ? gtk_offscreen_window_new() : gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if (GTK_IS_WINDOW(ui.window) && !GTK_IS_OFFSCREEN_WINDOW(ui.window))
        gtk_window_set_resizable(GTK_WINDOW(ui.window), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(ui.window), "drive-removable-media");
    g_signal_connect(ui.window, "delete-event", G_CALLBACK(on_delete), NULL);
    gtk_drag_dest_set(ui.window, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
    g_signal_connect(ui.window, "drag-data-received", G_CALLBACK(on_drag_data), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_widget_set_size_request(vbox, 470, -1);
    gtk_container_add(GTK_CONTAINER(ui.window), vbox);

    /* Drive Properties */
    add_header(vbox, &ui.hdr_drive);
    ui.lbl_device = field_label();
    gtk_box_pack_start(GTK_BOX(vbox), ui.lbl_device, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    ui.device_combo = ellipsized_combo();
    gtk_box_pack_start(GTK_BOX(row), ui.device_combo, TRUE, TRUE, 0);
    ui.save_btn = icon_button("document-save");
    gtk_box_pack_start(GTK_BOX(row), ui.save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    ui.lbl_boot = field_label();
    gtk_box_pack_start(GTK_BOX(vbox), ui.lbl_boot, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    ui.boot_combo = ellipsized_combo();
    gtk_box_pack_start(GTK_BOX(row), ui.boot_combo, TRUE, TRUE, 0);
    ui.hash_btn = icon_button("dialog-password");
    gtk_box_pack_start(GTK_BOX(row), ui.hash_btn, FALSE, FALSE, 0);
    ui.select_btn = gtk_button_new_with_label("");
    gtk_box_pack_start(GTK_BOX(row), ui.select_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    ui.imgopt_label = field_label();
    gtk_box_pack_start(GTK_BOX(vbox), ui.imgopt_label, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    ui.imgopt_combo = gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(row), ui.imgopt_combo, TRUE, TRUE, 0);
    ui.persist_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    ui.persist_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 8192, 128);
    gtk_scale_set_draw_value(GTK_SCALE(ui.persist_scale), FALSE);
    gtk_box_pack_start(GTK_BOX(ui.persist_box), ui.persist_scale, TRUE, TRUE, 0);
    ui.persist_spin = gtk_spin_button_new_with_range(0, 2 * 1024 * 1024, 1);
    gtk_box_pack_start(GTK_BOX(ui.persist_box), ui.persist_spin, FALSE, FALSE, 0);
    units = ui.persist_units = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(units), "MB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(units), "GB");
    gtk_combo_box_set_active(GTK_COMBO_BOX(units), 0);
    gtk_box_pack_start(GTK_BOX(ui.persist_box), units, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), ui.persist_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    ui.lbl_pt = field_label();
    ui.lbl_tt = field_label();
    ui.pt_combo = gtk_combo_box_text_new();
    ui.tt_combo = gtk_combo_box_text_new();
    gtk_grid_attach(GTK_GRID(grid), ui.lbl_pt, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui.lbl_tt, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui.pt_combo, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui.tt_combo, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    ui.adv_drive_exp = gtk_expander_new("");
    exp_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.list_hdd_chk = gtk_check_button_new_with_label("");
    ui.old_bios_chk = gtk_check_button_new_with_label("");
    ui.uefi_val_chk = gtk_check_button_new_with_label("");
    gtk_box_pack_start(GTK_BOX(exp_box), ui.list_hdd_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_box), ui.old_bios_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_box), ui.uefi_val_chk, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ui.adv_drive_exp), exp_box);
    gtk_box_pack_start(GTK_BOX(vbox), ui.adv_drive_exp, FALSE, FALSE, 4);

    /* Format Options */
    add_header(vbox, &ui.hdr_format);
    ui.lbl_label = field_label();
    gtk_box_pack_start(GTK_BOX(vbox), ui.lbl_label, FALSE, FALSE, 0);
    ui.label_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(vbox), ui.label_entry, FALSE, FALSE, 0);
    grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    ui.lbl_fs = field_label();
    ui.lbl_cluster = field_label();
    ui.fs_combo = gtk_combo_box_text_new();
    ui.cluster_combo = gtk_combo_box_text_new();
    gtk_grid_attach(GTK_GRID(grid), ui.lbl_fs, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui.lbl_cluster, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui.fs_combo, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui.cluster_combo, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    ui.adv_fmt_exp = gtk_expander_new("");
    exp_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.quick_chk = gtk_check_button_new_with_label("");
    ui.extlabel_chk = gtk_check_button_new_with_label("");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.quick_chk), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.extlabel_chk), TRUE);
    gtk_box_pack_start(GTK_BOX(exp_box), ui.quick_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_box), ui.extlabel_chk, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.bb_chk = gtk_check_button_new_with_label("");
    ui.bb_combo = gtk_combo_box_text_new();
    gtk_widget_set_sensitive(ui.bb_combo, FALSE);
    gtk_box_pack_start(GTK_BOX(row), ui.bb_chk, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), ui.bb_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_box), row, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ui.adv_fmt_exp), exp_box);
    gtk_box_pack_start(GTK_BOX(vbox), ui.adv_fmt_exp, FALSE, FALSE, 4);

    /* Status */
    add_header(vbox, &ui.hdr_status);
    ui.progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.progress), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.progress), "");
    gtk_box_pack_start(GTK_BOX(vbox), ui.progress, FALSE, FALSE, 4);

    bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    ui.lang_btn = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(ui.lang_btn), gtk_image_new_from_icon_name("preferences-desktop-locale", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_relief(GTK_BUTTON(ui.lang_btn), GTK_RELIEF_NONE);
    menu = gtk_menu_new();
    for (i = 0; i < get_locale_count(); i++) {
        GtkWidget* item = gtk_menu_item_new_with_label(get_locale_display_name(i));
        g_signal_connect(item, "activate", G_CALLBACK(on_language), (gpointer)get_locale_name(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(ui.lang_btn), menu);
    ui.about_btn = icon_button("help-about");
    ui.settings_btn = icon_button("preferences-system");
    ui.log_btn = icon_button("text-x-generic");
    gtk_box_pack_start(GTK_BOX(bottom), ui.lang_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), ui.about_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), ui.settings_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), ui.log_btn, FALSE, FALSE, 0);
    ui.close_btn = gtk_button_new_with_label("");
    ui.start_btn = gtk_button_new_with_label("");
    gtk_widget_set_size_request(ui.start_btn, 100, -1);
    gtk_widget_set_size_request(ui.close_btn, 100, -1);
    gtk_box_pack_end(GTK_BOX(bottom), ui.close_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), ui.start_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bottom, FALSE, FALSE, 6);

    ui.status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ui.status_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(ui.status_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), ui.status_label, FALSE, FALSE, 0);

    g_signal_connect(ui.device_combo, "changed", G_CALLBACK(on_device_changed), NULL);
    g_signal_connect(ui.boot_combo, "changed", G_CALLBACK(on_boot_changed), NULL);
    g_signal_connect(ui.select_btn, "clicked", G_CALLBACK(on_select_clicked), NULL);
    g_signal_connect(ui.hash_btn, "clicked", G_CALLBACK(on_hash_clicked), NULL);
    g_signal_connect(ui.save_btn, "clicked", G_CALLBACK(on_save_clicked), NULL);
    g_signal_connect(ui.pt_combo, "changed", G_CALLBACK(on_pt_changed), NULL);
    g_signal_connect(ui.tt_combo, "changed", G_CALLBACK(on_tt_changed), NULL);
    g_signal_connect(ui.fs_combo, "changed", G_CALLBACK(on_fs_changed), NULL);
    g_signal_connect(ui.label_entry, "changed", G_CALLBACK(on_label_changed), NULL);
    g_signal_connect(ui.list_hdd_chk, "toggled", G_CALLBACK(on_list_hdd_toggled), NULL);
    g_signal_connect(ui.bb_chk, "toggled", G_CALLBACK(on_bb_toggled), NULL);
    g_signal_connect(ui.persist_scale, "value-changed", G_CALLBACK(on_persist_scale), NULL);
    g_signal_connect(ui.start_btn, "clicked", G_CALLBACK(on_start_clicked), NULL);
    g_signal_connect(ui.close_btn, "clicked", G_CALLBACK(on_close_clicked), NULL);
    g_signal_connect(ui.log_btn, "clicked", G_CALLBACK(on_log_clicked), NULL);
    g_signal_connect(ui.about_btn, "clicked", G_CALLBACK(on_about_clicked), NULL);
    g_signal_connect(ui.settings_btn, "clicked", G_CALLBACK(on_settings_clicked), NULL);
}

static gboolean take_snapshot(gpointer data)
{
    GdkPixbuf* pb = gtk_offscreen_window_get_pixbuf(GTK_OFFSCREEN_WINDOW(ui.window));
    if (pb != NULL) {
        gdk_pixbuf_save(pb, (const char*)data, "png", NULL, NULL);
        g_object_unref(pb);
    }
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char** argv)
{
    ssize_t len;
    char* slash;

    setlocale(LC_ALL, "");
    gtk_init(&argc, &argv);

    len = readlink("/proc/self/exe", app_exe_dir, sizeof(app_exe_dir) - 1);
    if (len > 0) {
        app_exe_dir[len] = 0;
        slash = strrchr(app_exe_dir, '/');
        if (slash != NULL)
            *slash = 0;
    }
    snprintf(app_dir, sizeof(app_dir), "%s", app_exe_dir);
    init_settings();
    init_app_data_dir();
    init_localization();
    cpu_has_sha1_accel = DetectSHA1Acceleration();
    cpu_has_sha256_accel = DetectSHA256Acceleration();
    enable_extra_hashes = ReadSettingBool(SETTING_ENABLE_EXTRA_HASHES);
    boot_type = BT_IMAGE;
    image_path[0] = 0;

    memset(&ui, 0, sizeof(ui));
    ui.log_buffer = gtk_text_buffer_new(NULL);
    worker_line = g_string_new(NULL);
    log_hook = gui_log;
    status_hook = gui_status;
    progress_hook = gui_progress;
    notification_hook = gui_notification;
    selection_hook = gui_selection;

    build_window();
    apply_texts();
    populate_boot_selection();
    gtk_widget_show_all(ui.window);
    update_image_options();
    gtk_widget_set_visible(ui.hash_btn, FALSE);
    uprintf("Rufus %d.%d.%d (Linux port)", RUFUS_VERSION_MAJOR, RUFUS_VERSION_MINOR, RUFUS_VERSION_BUILD);
    refresh_devices(TRUE);
    update_start_button();
    g_timeout_add_seconds(2, poll_devices, NULL);
    /* Background update check, if due (as upstream) */
    g_thread_unref(g_thread_new("update", update_check_thread, GINT_TO_POINTER(FALSE)));
    {
        int a;
        for (a = 1; a < argc; a++) {
            if (strcmp(argv[a], "--device") == 0 && a + 1 < argc) {
                char* rp = realpath_dup(argv[++a]);
                snprintf(virtual_drive, sizeof(virtual_drive), "%s", (rp != NULL) ? rp : argv[a]);
                free(rp);
                refresh_devices(TRUE);
            } else if (argv[a][0] != '-') {
                set_image(argv[a]);
            }
        }
    }
    if (getenv("RUFUS_GUI_SNAPSHOT") != NULL) {
        const char* exp = getenv("RUFUS_GUI_EXPAND");
        if (exp != NULL) {
            gtk_expander_set_expanded(GTK_EXPANDER(ui.adv_drive_exp), TRUE);
            gtk_expander_set_expanded(GTK_EXPANDER(ui.adv_fmt_exp), TRUE);
        }
        g_timeout_add(atoi(getenv("RUFUS_GUI_SNAPSHOT_DELAY") ? getenv("RUFUS_GUI_SNAPSHOT_DELAY") : "1500"),
                      take_snapshot, getenv("RUFUS_GUI_SNAPSHOT"));
    }
    gtk_main();
    return 0;
}
