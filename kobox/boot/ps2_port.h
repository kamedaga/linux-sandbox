/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_PS2_PORT_H
#define KOBOX_BOOT_PS2_PORT_H

#include "../task/host.h"

/* Borrowed for this sandbox's lifetime. Native callbacks use a platform
 * capability; neither Linux nor a module can expand its two byte ports. */
struct kobox_linux_ps2_host {
	size_t size;
	void *context;
	int (*read)(void *context, uint16_t port, uint8_t *value);
	int (*write)(void *context, uint16_t port, uint8_t value);
	int (*mask)(void *context, int masked);
	int (*quiesce)(void *context);
};

int kobox_linux_ps2_configure(const struct kobox_linux_ps2_host *host);
int kobox_linux_ps2_irq_prepare(void);
int kobox_linux_ps2_irq_dispatch(void);
void kobox_linux_ps2_irq_finish(void);
unsigned char kobox_linux_ps2_inb(unsigned short port);
void kobox_linux_ps2_outb(unsigned char value, unsigned short port);

#endif
