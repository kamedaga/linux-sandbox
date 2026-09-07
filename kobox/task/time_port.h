/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_TIME_PORT_H
#define KOBOX_TASK_TIME_PORT_H

#include "host.h"

const struct kobox_linux_task_host_operations *kobox_task_host(void);
void kobox_task_clock_init(void);
void kobox_task_clock_interrupt(void);
void kobox_task_clock_stop(void);
int kobox_task_time_gate(struct kobox_linux_task_report *report);

#endif
