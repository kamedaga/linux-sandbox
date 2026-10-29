/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_ENTROPY_PORT_H
#define KOBOX_LINUX_ENTROPY_PORT_H
#include "../task/host.h"
#ifdef __KERNEL__
/* Trusted host OS CSPRNG output only. Call before loading key-using drivers
 * or starting userspace. Guest program bytes and timestamps are not entropy. */
int kobox_linux_entropy_seed(const void *bytes, size_t length);
#endif
#endif
