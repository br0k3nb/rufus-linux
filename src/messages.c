/*
 * Rufus Linux port: console implementation of the UI hooks used by ported
 * modules (uprintf, PrintInfo/PrintStatus, Notification, progress).
 *
 * The GTK front-end provides a second implementation of the same hooks.
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
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/time.h>

BOOL enable_debug = FALSE;
BOOL enable_logging = FALSE;
BOOL test_mode = FALSE;
char log_path[MAX_PATH] = "rufus.log";
BOOL cli_batch_mode = FALSE;        /* --batch: never prompt */
BOOL cli_yes_to_all = FALSE;        /* --yes: answer YES to all prompts */

static FILE* log_file = NULL;
/* Set by a front-end (GUI) to display the status/info messages */
void (*status_hook)(BOOL info, const char* text) = NULL;
/* Set by a front-end (GUI) to display the progress instead of a text bar */
void (*progress_hook)(const char* label, const char* text, double percent) = NULL;
/* Set by a front-end (GUI) to receive the log output instead of stdout */
void (*log_hook)(const char* line) = NULL;
static int   progress_last_len = 0;
static uint64_t progress_start_time = 0;
static int   progress_last_percent = -1;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
void init_logging(const char* path)
{
    if (path == NULL)
        path = log_path;
    log_file = fopen(path, "a");
    if (log_file != NULL) {
        time_t t = time(NULL);
        struct tm tm;
        char buf[64];
        localtime_r(&t, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(log_file, "\nRufus Linux %d.%d.%d.%d (port) log started %s\n",
                RUFUS_VERSION_MAJOR, RUFUS_VERSION_MINOR, RUFUS_VERSION_BUILD, 0, buf);
        enable_logging = TRUE;
    }
}

void exit_logging(void)
{
    if (log_file != NULL) {
        fclose(log_file);
        log_file = NULL;
    }
    enable_logging = FALSE;
}

static void log_output(const char* str)
{
    if (log_file != NULL) {
        fputs(str, log_file);
        fflush(log_file);
    }
}

/* Get rid of an active progress line before printing a message over it */
static void clear_progress_line(void)
{
    if (progress_last_len > 0) {
        fprintf(stdout, "\r%*s\r", progress_last_len, "");
        progress_last_len = 0;
    }
}

/* Upstream (stdio.c) strips any trailing whitespace and terminates each
 * uprintf() line itself, so callers never pass a newline */
static void terminate_line(char* buf, size_t size, int n)
{
    char* p = buf + ((n < 0) ? 0 : ((size_t)n >= size - 2) ? size - 2 : (size_t)n);
    while ((p > buf) && ((p[-1] == ' ') || (p[-1] == '\t') || (p[-1] == '\r') || (p[-1] == '\n')))
        *--p = 0;
    *p++ = '\n';
    *p = 0;
}

static void output_line(const char* buf)
{
    if (log_hook != NULL) {
        log_hook(buf);
    } else {
        clear_progress_line();
        fputs(buf, stdout);
        fflush(stdout);
    }
    if (enable_logging)
        log_output(buf);
}

void uprintf(const char* format, ...)
{
    char buf[4096];
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(buf, sizeof(buf) - 2, format, args);
    va_end(args);
    terminate_line(buf, sizeof(buf), n);
    output_line(buf);
}

void wuprintf(const wchar_t* format, ...)
{
    char buf[4096];
    wchar_t out[4096];
    va_list args;
    size_t n;
    va_start(args, format);
    vswprintf(out, ARRAYSIZE(out) - 1, format, args);
    va_end(args);
    n = wcstombs(buf, out, sizeof(buf) - 2);
    buf[sizeof(buf) - 1] = 0;
    terminate_line(buf, sizeof(buf), (n == (size_t)-1) ? 0 : (int)n);
    output_line(buf);
}

void uprintfs(const char* str)
{
    if (log_hook != NULL) {
        log_hook(str);
    } else {
        fputs(str, stdout);
        fflush(stdout);
    }
    if (enable_logging)
        log_output(str);
}

void duprintf(const char* format, ...)
{
    char buf[4096];
    va_list args;
    int n;
    if (!enable_debug)
        return;
    va_start(args, format);
    n = vsnprintf(buf, sizeof(buf) - 2, format, args);
    va_end(args);
    terminate_line(buf, sizeof(buf), n);
    output_line(buf);
}

/* ------------------------------------------------------------------ */
/* Status / info messages (upstream rufus.h:802)                       */
/* ------------------------------------------------------------------ */
void PrintStatusInfo(BOOL info, BOOL debug, unsigned int duration, int msg_id, ...)
{
    char buf[4096];
    const char* format;
    va_list args;

    (void)duration;
    if (debug && !enable_debug)
        return;

    /* Format the localized message ourselves with the caller's arguments,
     * exactly like upstream localization.c:PrintStatusInfo() does (and for
     * the same reason: the localized text contains the % specifiers). */
    format = get_msg_format(msg_id);
    va_start(args, msg_id);
    if (format != NULL) {
        vsnprintf(buf, sizeof(buf), format, args);
    } else {
        snprintf(buf, sizeof(buf), "[MSG_%03d]", msg_id - MSG_000);
    }
    va_end(args);

    if (status_hook != NULL) {
        status_hook(info, buf);
    } else {
        if (progress_last_len > 0) {
            fprintf(stdout, "\r%*s\r", progress_last_len, "");
            progress_last_len = 0;
        }
        printf("%s%s\n", info ? "" : "   ", buf);
        fflush(stdout);
    }
    if (enable_logging) {
        /* As upstream, the log always gets the default (English) text */
        format = get_msg_format_en(msg_id);
        if (format != NULL) {
            va_start(args, msg_id);
            vsnprintf(buf, sizeof(buf), format, args);
            va_end(args);
        }
        log_output(buf);
        log_output("\n");
    }
}

/* ------------------------------------------------------------------ */
/* Notifications / message boxes                                       */
/* ------------------------------------------------------------------ */
static const char* notification_prefix(int type)
{
    switch (type & 0x000000F0) {
    case MB_ICONHAND:        return "Error: ";
    case MB_ICONQUESTION:    return "Question: ";
    case MB_ICONEXCLAMATION: return "Warning: ";
    case MB_ICONASTERISK:    return "Info: ";
    default:                 return "";
    }
}

static int prompt_choice(const char* question)
{
    char answer[32];

    if (cli_yes_to_all)
        return IDYES;
    if (cli_batch_mode || !isatty(fileno(stdin)))
        return IDCANCEL;
    printf("%s [y/n] ", question);
    fflush(stdout);
    if (fgets(answer, sizeof(answer), stdin) == NULL)
        return IDCANCEL;
    if (answer[0] == 'y' || answer[0] == 'Y')
        return IDYES;
    return IDNO;
}

/* Set by a front-end (GUI) to display message boxes and selection dialogs */
int (*notification_hook)(int type, const char* title, const char* msg) = NULL;
int (*selection_hook)(char* title, char* message, selection_dialog_options_t* options) = NULL;

int NotificationEx(int type, const char* dont_display_setting, const notification_info* more_info,
                   const char* title, const char* format, ...)
{
    char msg[4096];
    va_list args;
    int buttons = type & 0x0000000F;

    (void)more_info;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    /* "Don't display this message again" setting, as upstream */
    if ((dont_display_setting != NULL) && ReadSettingBool(dont_display_setting))
        return IDOK;

    if (notification_hook != NULL) {
        uprintf("%s: %s", (title != NULL) ? title : "Notification", msg);
        return notification_hook(type, title, msg);
    }

    if (progress_last_len > 0) {
        fprintf(stdout, "\r%*s\r", progress_last_len, "");
        progress_last_len = 0;
    }
    printf("\n%s%s%s\n", notification_prefix(type),
           (title != NULL) ? title : "", (title != NULL && *title != 0) ? ":" : "");
    printf("%s\n", msg);
    fflush(stdout);
    if (enable_logging) {
        log_output(title != NULL ? title : "");
        log_output(": ");
        log_output(msg);
        log_output("\n");
    }

    switch (buttons) {
    case MB_YESNO:
    case MB_YESNOCANCEL:
        return prompt_choice(msg);
    case MB_OKCANCEL:
        if (cli_yes_to_all || cli_batch_mode || !isatty(fileno(stdin)))
            return IDOK;
        return (prompt_choice("Continue?") == IDYES) ? IDOK : IDCANCEL;
    case MB_RETRYCANCEL:
        return cli_yes_to_all ? IDRETRY : IDCANCEL;
    case MB_ABORTRETRYIGNORE:
        return cli_yes_to_all ? IDRETRY : IDABORT;
    default:
        return IDOK;
    }
}

void CenterDialog(HWND hDlg)
{
    (void)hDlg;
}

void SetTaskbarProgressState(int state)
{
    (void)state;
}

/* ------------------------------------------------------------------ */
/* Progress reporting                                                  */
/* ------------------------------------------------------------------ */
static const char* op_name(int op)
{
    static const char* names[] = {
        "Analyzing", "Bad blocks check", "Zeroing", "Partitioning",
        "Formatting", "Creating file system", "Fixing MBR", "Copying files",
        "Patching", "Finalizing", "Extracting"
    };
    if (op >= 0 && op < OP_MAX)
        return names[op];
    return "Working";
}

static void render_progress(const char* label, const char* extra, double percent)
{
    char line[256];
    int len, bar_width = 30, filled;
    static const char* spin = "|/-\\";
    static int spin_idx = 0;

    if (percent < 0.0)
        percent = 0.0;
    if (percent > 100.0)
        percent = 100.0;
    if (progress_hook != NULL) {
        progress_hook(label, extra, percent);
        return;
    }
    /* A progress bar only makes sense on a terminal */
    if (!isatty(STDOUT_FILENO))
        return;
    filled = (int)(percent * bar_width / 100.0);
    len = snprintf(line, sizeof(line), "\r%s [", label);
    for (int i = 0; i < bar_width; i++)
        len += snprintf(line + len, sizeof(line) - len, "%s",
                        (i < filled) ? "#" :
                        ((i == filled) ? (char[2]){ spin[spin_idx & 3], 0 } : "."));
    len += snprintf(line + len, sizeof(line) - len, "] %5.1f%% %s", percent,
                    (extra != NULL) ? extra : "");
    spin_idx++;
    fputs(line, stdout);
    fflush(stdout);
    progress_last_len = len;
}

static void clear_progress(void)
{
    if (progress_last_len > 0) {
        fprintf(stdout, "\r%*s\r", progress_last_len, "");
        fflush(stdout);
        progress_last_len = 0;
    }
}

void InitProgress(BOOL bOnlyFormat)
{
    (void)bOnlyFormat;
    progress_start_time = GetTickCount64();
    progress_last_percent = -1;
    clear_progress();
}

void UpdateProgress(int op, float percent)
{
    char extra[128] = "";
    if ((int)percent == progress_last_percent)
        return;
    progress_last_percent = (int)percent;
    if (GetTickCount64() - progress_start_time > 500 || percent >= 100.0f) {
        snprintf(extra, sizeof(extra), "%s", op_name(op));
        render_progress("", extra, percent);
    }
}

void UpdateProgressStatus(int op, const char* status)
{
    render_progress(op_name(op), status, 0);
}

void uprint_progress(uint64_t cur_value, uint64_t max_value)
{
    static uint64_t last = 0;
    char extra[64];
    if (max_value == 0)
        return;
    if (cur_value - last < max_value / 200 && cur_value != max_value)
        return;
    last = cur_value;
    snprintf(extra, sizeof(extra), "%s / %s", SizeToHumanReadable(cur_value, FALSE, FALSE),
             SizeToHumanReadable(max_value, FALSE, FALSE));
    render_progress("", extra, 100.0 * (double)cur_value / (double)max_value);
}

/* Port of upstream ui.c _UpdateProgressWithInfo() (percentage + speed + ETA),
 * without the Win32 progress-bar/taskbar belt and whistles. */
void _UpdateProgressWithInfo(int op, int msg, uint64_t processed, uint64_t total, BOOL force)
{
    static uint64_t last_refresh = 0;
    static uint64_t start_time = 0;
    uint64_t now = GetTickCount64();
    uint64_t elapsed, speed = 0, eta = 0;
    double percent = 0.0;
    char msg_data[192];
    char label[256];

    if (op == OP_INIT) {
        start_time = now;
        last_refresh = 0;
        clear_progress();
        return;
    }
    if (!force && (now - last_refresh) < 250)
        return;
    last_refresh = now;

    if (total > 0)
        percent = 100.0 * (double)processed / (double)total;
    if (percent > 100.0)
        percent = 100.0;

    elapsed = now - start_time;
    if (elapsed > 500 && processed > 0)
        speed = processed * 1000 / elapsed;
    if (speed > 0 && total > processed)
        eta = (total - processed) * 1000 / speed;

    if (op == OP_FORMAT || op == OP_FILE_COPY || op == OP_ZERO_MBR ||
        op == OP_BADBLOCKS || op == OP_PATCH) {
        if (speed > 0)
            snprintf(msg_data, sizeof(msg_data), "%s/s, ETA %" PRIu64 ":%02" PRIu64 "\033[K",
                     SizeToHumanReadable(speed, FALSE, FALSE), eta / 60000, (eta / 1000) % 60);
        else
            snprintf(msg_data, sizeof(msg_data), "%s / %s\033[K",
                     SizeToHumanReadable(processed, FALSE, FALSE),
                     SizeToHumanReadable(total, FALSE, FALSE));
        snprintf(label, sizeof(label), "%s", lmprintf((uint32_t)msg, msg_data));
        /* The localized format already embeds msg_data when it has a %s;
         * strip a trailing \033[K duplication if any */
        render_progress("", label, percent);
    } else {
        snprintf(label, sizeof(label), "%s", op_name(op));
        render_progress("", label, percent);
    }
}

/* The check box selections were given on the command line (--wue): the
 * dialog's preset mask is then used without asking */
BOOL cli_selection_preset = FALSE;
extern char unattend_username[];

/*
 * CLI selection dialog. Radio buttons: list the choices and let the user
 * pick one (returns 1 << index). Check boxes: show the choices with their
 * default state and let the user toggle them (returns the mask). In batch
 * or --yes mode the default/preset answer is used, as upstream's dialog
 * would return it if just validated. Returns -1 on cancel.
 */
int SelectionDialog(char* title, char* message, selection_dialog_options_t* options)
{
    char answer[256];
    int i, sel, mask, size = (int)options->choices.Index;
    BOOL interactive = !(cli_yes_to_all || cli_batch_mode || !isatty(fileno(stdin)));

    if (selection_hook != NULL)
        return selection_hook(title, message, options);
    if (size <= 0)
        return -1;

    clear_progress_line();
    printf("\n%s\n%s\n", title, message);
    if (options->style != BS_AUTOCHECKBOX) {
        for (i = 0; i < size; i++)
            printf("  %d) %s%s\n", i + 1, options->choices.String[i], (i == 0) ? " (default)" : "");
        if (!interactive) {
            printf("Using: %s\n", options->choices.String[0]);
            return 1;
        }
        for (;;) {
            printf("Select [1-%d, c to cancel]: ", size);
            fflush(stdout);
            if (fgets(answer, sizeof(answer), stdin) == NULL)
                return -1;
            if (answer[0] == 'c' || answer[0] == 'C')
                return -1;
            if (answer[0] == '\n')
                return 1;
            sel = atoi(answer);
            if (sel >= 1 && sel <= size)
                return 1 << (sel - 1);
        }
    }

    /* Check boxes */
    mask = options->mask;
    mask &= (1 << size) - 1;
    for (;;) {
        for (i = 0; i < size; i++)
            printf("  [%c] %d) %s\n", (mask & (1 << i)) ? 'x' : ' ', i + 1, options->choices.String[i]);
        if (!interactive || cli_selection_preset)
            break;
        printf("Enter numbers to toggle, empty line to accept, c to cancel: ");
        fflush(stdout);
        if (fgets(answer, sizeof(answer), stdin) == NULL)
            return -1;
        if (answer[0] == 'c' || answer[0] == 'C')
            return -1;
        if (answer[0] == '\n')
            break;
        for (char* p = strtok(answer, " ,\t\n"); p != NULL; p = strtok(NULL, " ,\t\n")) {
            sel = atoi(p);
            if (sel >= 1 && sel <= size)
                mask ^= 1 << (sel - 1);
        }
    }
    if ((options->flags & SELECTION_NEEDS_ALL_TO_PROCEED) && (mask != (1 << size) - 1))
        return -1;
    /* Local account name, as entered in upstream's dialog edit box */
    if ((options->username_index > 0) && (mask & (1 << (options->username_index - 1))) &&
        (unattend_username[0] == 0)) {
        const char* user = getenv("SUDO_USER");
        if (user == NULL || user[0] == 0)
            user = getenv("USER");
        if (interactive) {
            printf("Local account name [%s]: ", (user != NULL) ? user : "");
            fflush(stdout);
            if (fgets(answer, sizeof(answer), stdin) != NULL) {
                answer[strcspn(answer, "\r\n")] = 0;
                if (answer[0] != 0)
                    user = answer;
            }
        }
        if (user != NULL)
            snprintf(unattend_username, 128, "%s", user);
    }
    return mask;
}
