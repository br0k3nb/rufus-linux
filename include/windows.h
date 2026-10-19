/*
 * Rufus Linux port: <windows.h> stand-in.
 *
 * Vendored upstream code (ms-sys, libfat, ...) includes <windows.h>;
 * on Linux that resolves to this shim, which pulls in the port's
 * Win32 compatibility layer instead.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RUFUS_LINUX_WINDOWS_H
#define RUFUS_LINUX_WINDOWS_H
#include "win32compat.h"
#endif
