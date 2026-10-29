/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PS2_IO_COMPILE_H
#define KOBOX_PS2_IO_COMPILE_H

/* Compile the unchanged i8042 module against the hosted machine's PIO
 * boundary. Include the architecture definitions first, so a later include
 * cannot put privileged IN/OUT instructions back into this module. */
#ifdef MODULE
#include <asm/io.h>
#include "../../ps2_port.h"
#undef inb
#undef outb
#define inb(port) kobox_linux_ps2_inb(port)
#define outb(value, port) kobox_linux_ps2_outb(value, port)
/* i8042.c supplies its own printk prefix before its normal includes. */
#undef pr_fmt
#endif

#endif
