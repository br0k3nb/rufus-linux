/*
 * Rufus Linux port: on UNIX, wimlib's tchar is char (UTF-8), so upstream's
 * UTF-8 'U' wrappers (wimlib.h, _RUFUS sections) are the plain functions.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RUFUS_LINUX_WIMLIB_PORT_H
#define RUFUS_LINUX_WIMLIB_PORT_H
#define wimlib_open_wimU        wimlib_open_wim
#define wimlib_extract_imageU   wimlib_extract_image
#define wimlib_extract_pathsU   wimlib_extract_paths
#define wimlib_splitU           wimlib_split
#define wimlib_add_treeU        wimlib_add_tree
#endif
