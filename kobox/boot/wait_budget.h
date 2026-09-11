/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_WAIT_BUDGET_H
#define KOBOX_BOOT_WAIT_BUDGET_H

/* Gate observations, not a replacement for Linux's timeout implementation. */
struct kobox_wait_budget {
	unsigned long asleep;
	unsigned long slept;
};

static inline void kobox_wait_budget_wake(struct kobox_wait_budget *budget,
					unsigned long wake)
{
	budget->slept += wake - budget->asleep;
}

static inline unsigned long
kobox_wait_budget_remaining(const struct kobox_wait_budget *budget,
			    unsigned long timeout)
{
	return budget->slept < timeout ? timeout - budget->slept : 0;
}

#endif
