/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_MEMORY_PORT_H
#define KOBOX_LINUX_MEMORY_PORT_H

#include "host.h"
#include <linux/init.h>

int kobox_linux_memory_bind(const struct kobox_linux_memory_layout *layout);
int __init kobox_linux_memory_setup_arch(void);
void kobox_linux_memory_set_cpu(unsigned int cpu);
void kobox_linux_memory_image_permissions(unsigned long offset,
					 unsigned long length, bool writable);

#endif
