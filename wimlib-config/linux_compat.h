/*
 * Force-included when building the vendored wimlib on Linux: Rufus' own
 * additions to wimlib (the libcdio "ISO|path" support) were only ever built
 * for Windows and use a few MSVCRT names.
 */
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#define _lseeki64           lseek
#ifndef tsnprintf
#define tsnprintf           snprintf
#endif
#define _stat64             stat
#define InterlockedIncrement16(p)   __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement16(p)   __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define Sleep(ms)           usleep((ms) * 1000)
/* wimlib expects the XSI strerror_r() (int), but _GNU_SOURCE gives glibc's */
extern int __xpg_strerror_r(int errnum, char* buf, size_t buflen);
#define strerror_r          __xpg_strerror_r
