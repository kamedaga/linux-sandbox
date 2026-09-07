/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_POSIX_MACHINE_H
#define KOBOX_TASK_POSIX_MACHINE_H

#include "host.h"

/* One Linux core owns the CPU domains in this process. Init/destroy run
 * outside Linux execution; destroy requires all tasks to have left them.
 */
extern const struct kobox_linux_memory_host_operations kobox_task_posix_memory_operations;
extern const struct kobox_linux_task_host_operations kobox_task_posix_operations;

int kobox_task_posix_init(kobox_linux_task_notification_fn dispatch);
int kobox_task_posix_destroy(void);

#endif
