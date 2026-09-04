/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_LINUX_BOTTOM_HALF_H
#define KOBOX_PROVIDER_LINUX_BOTTOM_HALF_H

#include <linux/instruction_pointer.h>
#include <linux/preempt.h>

void __local_bh_disable_ip(unsigned long ip, unsigned int count);
void __local_bh_enable_ip(unsigned long ip, unsigned int count);
void _local_bh_enable(void);

static inline void local_bh_disable(void)
{
	__local_bh_disable_ip(_THIS_IP_, SOFTIRQ_DISABLE_OFFSET);
}

static inline void local_bh_enable_ip(unsigned long ip)
{
	__local_bh_enable_ip(ip, SOFTIRQ_DISABLE_OFFSET);
}

static inline void local_bh_enable(void)
{
	__local_bh_enable_ip(_THIS_IP_, SOFTIRQ_DISABLE_OFFSET);
}

#ifdef CONFIG_PREEMPT_RT
bool local_bh_blocked(void);
#else
static inline bool local_bh_blocked(void)
{
	return false;
}
#endif

#endif /* KOBOX_PROVIDER_LINUX_BOTTOM_HALF_H */
