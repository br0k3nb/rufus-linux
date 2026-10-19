/*
 * Rufus Linux port: asynchronous file I/O (upstream winio.h API).
 *
 * Upstream implements these over Windows overlapped I/O. The port keeps the
 * exact same API and semantics (one outstanding request per handle, file
 * offset advanced by GetSizeAsync()) with a worker thread per handle doing
 * pread()/pwrite(), so that reading/hashing/writing can overlap like they
 * do on Windows. Implemented in io.c.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RUFUS_LINUX_WINIO_H
#define RUFUS_LINUX_WINIO_H

#include "win32compat.h"

HANDLE  CreateFileAsync(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                        DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes);
VOID    CloseFileAsync(HANDLE h);
BOOL    ReadFileAsync(HANDLE h, LPVOID lpBuffer, DWORD nNumberOfBytesToRead);
BOOL    WriteFileAsync(HANDLE h, LPVOID lpBuffer, DWORD nNumberOfBytesToWrite);
BOOL    WaitFileAsync(HANDLE h, DWORD dwTimeout);
BOOL    GetSizeAsync(HANDLE h, LPDWORD lpNumberOfBytes);
/* PORT: access to the underlying (synchronous) handle and offset */
HANDLE  GetAsyncFileHandle(HANDLE h);

#endif
