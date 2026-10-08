/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_MEMORY_PORT_H
#define KOBOX_LINUX_MEMORY_PORT_H

#include "host.h"
#include <linux/init.h>
#include <asm/pgtable_types.h>

int kobox_linux_memory_bind(const struct kobox_linux_memory_layout *layout);
bool kobox_linux_memory_native_range(unsigned long address, size_t size);
int __init kobox_linux_memory_setup_arch(void);
void kobox_linux_memory_set_cpu(unsigned int cpu);
void kobox_linux_memory_image_permissions(unsigned long offset,
					 unsigned long length, bool writable);
pte_t *kobox_linux_memory_lookup_address(pgd_t *pgd, unsigned long address,
					 unsigned int *level, bool *nx, bool *rw);

#endif
