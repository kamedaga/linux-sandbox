/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_VM_POSIX_H
#define KOBOX_LINUX_VM_POSIX_H

#include "host.h"

extern const struct kobox_linux_vm_host_operations kobox_vm_posix_operations;
void kobox_vm_posix_notify(void *context);
int kobox_vm_posix_probe(void *space, uint64_t address, unsigned int write,
	uint64_t value, uint64_t sequence);

#endif
