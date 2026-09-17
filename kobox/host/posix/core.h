/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_CORE_H
#define KOBOX_POSIX_CORE_H

#include "../../boot/core.h"
#include "../../boot/fixed_image.h"

struct kobox_posix_core;

/* Open validates and maps the fixed, fully linked core without a libc dynamic
 * loader. On failure output remains NULL. Close is for an unstarted core only;
 * live Linux core teardown requires all users and asynchronous entries stopped.
 */
int kobox_posix_core_open(const char *path, struct kobox_posix_core **out);
int kobox_posix_core_close(struct kobox_posix_core **core);
struct kobox_boot_core *kobox_posix_core_boot(struct kobox_posix_core *core);
const struct kobox_fixed_image *kobox_posix_core_image(
	struct kobox_posix_core *core);
void *kobox_posix_core_base(struct kobox_posix_core *core);

#endif
