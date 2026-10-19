/*
 * Rufus Linux port: worker mode (rufus --worker ...).
 *
 * The GTK front-end runs as the desktop user. Writing to a drive requires
 * root, so the GUI runs the actual operation through this worker mode of
 * the CLI, started with pkexec for real devices (or directly for image
 * files). Everything that upstream displays from its format thread goes
 * back to the GUI over stdout, and the answers to its questions come back
 * over stdin, with a simple line protocol (fields separated by 0x1F, line
 * breaks inside fields encoded as 0x1E):
 *
 *   worker -> GUI   "@@R L <log line>"
 *                   "@@R S <info>\x1f<status text>"
 *                   "@@R P <percent>\x1f<progress text>"
 *                   "@@R Q <type>\x1f<title>\x1f<message>"          (Notification)
 *                   "@@R C <style>\x1f<mask>\x1f<username index>\x1f<title>\x1f<message>[\x1f<choice>...]"
 *                   "@@R E <ErrorStatus>"                          (operation ended)
 *   GUI -> worker   "A <answer>[\x1f<username>]"                    (answer to Q/C)
 *                   "X"                                            (cancel, as upstream's Cancel button)
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

BOOL worker_mode = FALSE;
extern char unattend_username[];

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t answer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t answer_cond = PTHREAD_COND_INITIALIZER;
static BOOL answer_ready = FALSE;
static char answer[512];

/* Write a protocol line, encoding the line breaks of each field */
static void emit(char kind, const char* fmt, ...)
{
    char buf[8192], *p;
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    for (p = buf; *p != 0; p++) {
        if (*p == '\n')
            *p = 0x1e;
        else if (*p == '\r' || *p == '\033')
            *p = ' ';
    }
    pthread_mutex_lock(&out_lock);
    fprintf(stdout, "@@R %c %s\n", kind, buf);
    fflush(stdout);
    pthread_mutex_unlock(&out_lock);
}

static void worker_log(const char* line)
{
    char buf[4096];
    size_t len;

    snprintf(buf, sizeof(buf), "%s", line);
    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = 0;
    emit('L', "%s", buf);
}

static void worker_status(BOOL info, const char* text)
{
    emit('S', "%d\x1f%s", info ? 1 : 0, text);
}

static void worker_progress(const char* label, const char* text, double percent)
{
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();

    (void)label;
    if ((now - last < 200) && (percent < 100.0))
        return;
    last = now;
    emit('P', "%.1f\x1f%s", percent, (text != NULL) ? text : "");
}

/* Wait for the GUI's answer */
static void wait_answer(char* out, size_t len)
{
    pthread_mutex_lock(&answer_lock);
    while (!answer_ready)
        pthread_cond_wait(&answer_cond, &answer_lock);
    answer_ready = FALSE;
    snprintf(out, len, "%s", answer);
    pthread_mutex_unlock(&answer_lock);
}

static int worker_notification(int type, const char* title, const char* msg)
{
    char a[512];

    emit('Q', "%d\x1f%s\x1f%s", type, (title != NULL) ? title : "", msg);
    wait_answer(a, sizeof(a));
    return atoi(a);
}

static int worker_selection(char* title, char* message, selection_dialog_options_t* options)
{
    char buf[8192], a[512], *sep;
    uint32_t i;
    int r;

    snprintf(buf, sizeof(buf), "%d\x1f%d\x1f%d\x1f%s\x1f%s", options->style, options->mask,
             options->username_index, title, message);
    for (i = 0; i < options->choices.Index; i++) {
        size_t len = strlen(buf);
        snprintf(&buf[len], sizeof(buf) - len, "\x1f%s", options->choices.String[i]);
    }
    emit('C', "%s", buf);
    wait_answer(a, sizeof(a));
    r = atoi(a);
    sep = strchr(a, 0x1f);
    if (sep != NULL && sep[1] != 0)
        snprintf(unattend_username, MAX_USERNAME_LENGTH, "%s", &sep[1]);
    return r;
}

/* Read the GUI's answers and cancel requests */
static void* stdin_reader(void* arg)
{
    char line[512];

    (void)arg;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == 'X') {
            uprintf("Cancellation requested by the user");
            ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
        } else if (line[0] == 'A') {
            pthread_mutex_lock(&answer_lock);
            snprintf(answer, sizeof(answer), "%s", &line[(line[1] == ' ') ? 2 : 1]);
            answer_ready = TRUE;
            pthread_cond_broadcast(&answer_cond);
            pthread_mutex_unlock(&answer_lock);
        }
    }
    /* The GUI went away: cancel, and release any pending question */
    ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
    pthread_mutex_lock(&answer_lock);
    snprintf(answer, sizeof(answer), "%d", IDCANCEL);
    answer_ready = TRUE;
    pthread_cond_broadcast(&answer_cond);
    pthread_mutex_unlock(&answer_lock);
    return NULL;
}

static void worker_exit(void)
{
    emit('E', "%lu", (unsigned long)ErrorStatus);
}

static void on_signal(int sig)
{
    (void)sig;
    ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
}

void worker_init(void)
{
    pthread_t thread;
    struct sigaction sa = { 0 };

    worker_mode = TRUE;
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_hook = worker_log;
    status_hook = worker_status;
    progress_hook = worker_progress;
    notification_hook = worker_notification;
    selection_hook = worker_selection;
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    pthread_create(&thread, NULL, stdin_reader, NULL);
    pthread_detach(thread);
    atexit(worker_exit);
}
