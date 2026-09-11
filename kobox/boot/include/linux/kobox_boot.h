/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_HOSTED_BOOT_H
#define KOBOX_LINUX_HOSTED_BOOT_H

#include <linux/compiler.h>
#include <linux/types.h>

struct page;

void kobox_linux_memory_sync_direct(struct page *page, unsigned int nr);
int kobox_linux_memory_publish(unsigned long start, unsigned long end);
bool kobox_linux_boot_host_init(void);
void __noreturn kobox_linux_boot_run_init(void);
bool kobox_linux_boot_text_address(unsigned long address);

#endif
