/*
 * Rufus Linux port: link creation helpers for bled's archive extraction.
 *
 * bled references busybox's create_or_remember_link() for archive entries
 * that are hard or symbolic links, but upstream Rufus does not vendor that
 * busybox file (Windows can't create these links anyway). On Linux, create
 * the links right away (busybox defers symlinks until extraction completes,
 * which only matters for archives with unsafe link targets).
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "libbb.h"
#include "bb_archive.h"

void FAST_FUNC create_or_remember_link(llist_t **link_placeholders,
		const char *target, const char *linkname, int hard_link)
{
	(void)link_placeholders;
	if ((hard_link ? link(target, linkname) : symlink(target, linkname)) != 0)
		bb_error_msg("can't create %slink '%s' to '%s'", hard_link ? "hard" : "sym", linkname, target);
}

void FAST_FUNC create_links_from_list(llist_t *list)
{
	(void)list;
}
