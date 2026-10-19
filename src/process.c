/*
 * Rufus Linux port: external command execution.
 *
 * Port of upstream stdfn.c RunCommandWithProgress(): runs a console command,
 * optionally streaming its output to the log and/or parsing progress
 * percentages out of it with the vendored cregex engine (exactly like the
 * Windows build does for oscdimg, chkdsk, ...).
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rfus.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Vendored cregex (compiled unchanged from ../src) */
#include "cregex.h"

DWORD RunCommandWithProgress(const char* cmd, const char* dir, BOOL log, int msg, const char* pattern)
{
    int pipefd[2] = { -1, -1 };
    pid_t pid;
    DWORD ret = 0;
    char* matches[REGEX_VM_MAX_MATCHES];
    cregex_node_t* node = NULL;
    cregex_program_t* program = NULL;

    if (cmd == NULL || cmd[0] == 0) {
        SetLastError(ERROR_BAD_ARGUMENTS);
        return ERROR_BAD_ARGUMENTS;
    }

    if (msg != 0) {
        node = cregex_parse(pattern);
        if (node != NULL) {
            program = cregex_compile_node(node);
            cregex_parse_free(node);
        }
        if (node == NULL || program == NULL)
            uprintf("Internal error: Failed to parse '%s'", pattern);
    }

    if (log || msg != 0) {
        if (pipe(pipefd) != 0) {
            uprintf("Could not set commandline pipe: %s", strerror(errno));
            ret = errno;
            goto out;
        }
    }

    pid = fork();
    if (pid < 0) {
        uprintf("Unable to launch command '%s': %s", cmd, strerror(errno));
        ret = errno;
        goto out;
    }

    if (pid == 0) {
        /* Child: run through /bin/sh so that quoted arguments and shell
         * builtins behave as on Windows' cmd line handling */
        if (dir != NULL && dir[0] != 0) {
            if (chdir(dir) != 0)
                _exit(127);
        }
        if (pipefd[1] >= 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        } else {
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd >= 0) {
                dup2(nullfd, STDOUT_FILENO);
                dup2(nullfd, STDERR_FILENO);
                if (nullfd > 2)
                    close(nullfd);
            }
        }
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }

    /* Parent */
    close(pipefd[1]);
    pipefd[1] = -1;

    if (program != NULL)
        UpdateProgressWithInfoInit(NULL, FALSE);

    for (;;) {
        struct pollfd pfd;
        int rc;

        /* Check for user cancel */
        if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)) {
            kill(pid, SIGTERM);
            if (waitpid(pid, NULL, 0) < 0 && !cli_batch_mode)
                uprintf("Could not terminate command: %s", strerror(errno));
            else
                uprintf("Command was terminated by user");
            ret = ERROR_CANCELLED;
            goto out;
        }

        if (pipefd[0] >= 0) {
            pfd.fd = pipefd[0];
            pfd.events = POLLIN;
            pfd.revents = 0;
            rc = poll(&pfd, 1, 100);
            if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
                char output[8192];
                ssize_t got = read(pipefd[0], output, sizeof(output) - 1);
                if (got > 0) {
                    output[got] = 0;
                    if (program != NULL) {
                        if (cregex_program_run(program, output, (const char**)matches, ARRAYSIZE(matches)) > 0 &&
                            matches[2] != NULL && matches[3] != NULL) {
                            float f = 0.0f;
                            char saved = matches[3][0];
                            matches[3][0] = '\0';
                            IGNORE_RETVAL(sscanf(matches[2], "%f", &f));
                            matches[3][0] = saved;
                            UpdateProgressWithInfo(OP_FORMAT, msg, (uint64_t)(f * 100.0f), 100 * 100ULL);
                        } else if (log) {
                            /* output may contain a '%' so don't feed it as a naked format string */
                            uprintf("%s", output);
                        }
                    } else if (log) {
                        uprintf("%s", output);
                    }
                }
            }
        } else {
            usleep(100 * 1000);
        }

        rc = waitpid(pid, &ret, WNOHANG);
        if (rc == pid)
            break;
        if (rc < 0) {
            ret = errno;
            break;
        }
    }

    if (WIFEXITED(ret)) {
        ret = (DWORD)WEXITSTATUS(ret);
    } else if (WIFSIGNALED(ret)) {
        ret = (DWORD)(0x100 + WTERMSIG(ret));
    }

out:
    cregex_compile_free(program);
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    return ret;
}
