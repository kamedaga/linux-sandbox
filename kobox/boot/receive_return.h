// SPDX-License-Identifier: GPL-2.0-only
#ifndef KOBOX_BOOT_RECEIVE_RETURN_H
#define KOBOX_BOOT_RECEIVE_RETURN_H

#include <linux/atomic.h>

enum {
	RECEIVE_OWNER,
	RECEIVE_EXECUTING,
	RECEIVE_LENT_IDLE,
	RECEIVE_LENT_ADVANCING,
	RECEIVE_RETURNING,
};

enum kobox_receive_return_result {
	KOBOX_RECEIVE_OWNED,
	KOBOX_RECEIVE_WAIT,
	KOBOX_RECEIVE_INVALID,
};

static inline enum kobox_receive_return_result
kobox_receive_reclaim(atomic_t *state)
{
	int observed = atomic_cmpxchg(state, RECEIVE_EXECUTING, RECEIVE_OWNER);

	if (observed == RECEIVE_EXECUTING)
		return KOBOX_RECEIVE_OWNED;
	/* An advancing deputy may finish between observation and CAS. Retry
	 * its new idle state, or the opening task can wait for OWNER forever. */
	for (;;) {
		observed = atomic_read(state);
		if (observed == RECEIVE_LENT_IDLE) {
			if (atomic_cmpxchg(state, RECEIVE_LENT_IDLE,
					   RECEIVE_OWNER) == RECEIVE_LENT_IDLE)
				return KOBOX_RECEIVE_OWNED;
		} else if (observed == RECEIVE_LENT_ADVANCING) {
			if (atomic_cmpxchg(state, RECEIVE_LENT_ADVANCING,
					   RECEIVE_RETURNING) == RECEIVE_LENT_ADVANCING)
				return KOBOX_RECEIVE_WAIT;
		} else {
			return KOBOX_RECEIVE_INVALID;
		}
	}
}

#endif
