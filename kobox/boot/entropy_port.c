// SPDX-License-Identifier: GPL-2.0-only
#include "entropy_port.h"
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/random.h>
#include <linux/sched.h>

int kobox_linux_entropy_seed(const void *bytes, size_t length)
{
	if (!bytes || length != 64 || current->mm || irqs_disabled() || in_atomic())
		return -EINVAL;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	/* The trusted native owner supplies fresh RDSEED output and fails closed
	 * without it. Credit only the Linux pool's initialization threshold. */
	add_hwgenerator_randomness(bytes, length, 256, false);
	return rng_is_initialized() ? 0 : -EAGAIN;
}
