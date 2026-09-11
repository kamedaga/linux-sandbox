/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TEST_TASK_PORT_H
#define KOBOX_TEST_TASK_PORT_H

#include "../../task/host.h"
#include <linux/types.h>

struct task_struct;

/* Read-only observation and native join for the task-port conformance Gate.
 * Implemented only in Gate builds; no test workload enters production core.
 */
u64 *kobox_task_gate_switches(unsigned int cpu);
u64 *kobox_task_gate_ipis(unsigned int cpu);
int kobox_task_gate_join(struct task_struct *task);
int kobox_task_smp_gate(struct kobox_linux_task_report *report);
bool kobox_task_smp_report_ready(const struct kobox_linux_task_report *report);

#endif
