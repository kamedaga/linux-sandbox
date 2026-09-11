/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_CLIENT_TASK_GATE_H
#define KOBOX_CLIENT_TASK_GATE_H

#include "../task/host.h"

/* Task-port prerequisite, not an external syscall/uaccess conformance Gate. */
struct kobox_client_task_report {
	size_t size;
	uint64_t warnings;
	uint32_t tasks, switches, mappings, credentials, files, reaped;
	uint32_t phase, line;
	int32_t result;
};

#endif
