/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_TASK_BOOT_H
#define KOBOX_LINUX_TASK_BOOT_H

#include "host.h"

struct task_struct;

int kobox_linux_task_bind_boot(const struct kobox_linux_task_layout *layout,
			       struct kobox_linux_task_report *report);
int kobox_linux_task_install_secondary_entry(void (*entry)(void));
int kobox_linux_task_prepare_cpus(void);
int kobox_linux_task_kick_cpu(unsigned int cpu, struct task_struct *idle);
int kobox_linux_task_register_clocksource(void);
void kobox_linux_task_idle_exit(void);
int kobox_linux_task_verify_boot(void);

#endif
