/* SPDX-License-Identifier: GPL-2.0-only */
#include "domain.h"

#include <limits.h>

enum kobox_machine_result kobox_machine_domain_init(
	struct kobox_machine_domain *domain)
{
	unsigned int index;

	domain->owner = NULL;
	domain->handoff_released = true;
	domain->accepting = true;
	atomic_init(&domain->irq_depth, 0);
	atomic_init(&domain->stop_requested, false);
	atomic_init(&domain->stopped, false);
	atomic_init(&domain->sequence, 0);
	if (!atomic_is_lock_free(&domain->irq_depth) ||
	    !atomic_is_lock_free(&domain->stop_requested) ||
	    !atomic_is_lock_free(&domain->stopped) ||
	    !atomic_is_lock_free(&domain->sequence))
		return KOBOX_MACHINE_NOT_LOCK_FREE;
	for (index = 0; index < KOBOX_MACHINE_NOTIFICATION_COUNT; index++) {
		atomic_init(&domain->pending[index], 0);
		if (!atomic_is_lock_free(&domain->pending[index]))
			return KOBOX_MACHINE_NOT_LOCK_FREE;
	}
	return KOBOX_MACHINE_OK;
}

bool kobox_machine_domain_can_enter(const struct kobox_machine_domain *domain,
				    const void *owner)
{
	return !domain->owner ||
		(domain->owner == owner && domain->handoff_released);
}

enum kobox_machine_result kobox_machine_domain_enter(
	struct kobox_machine_domain *domain, void *owner)
{
	if (!owner)
		return KOBOX_MACHINE_INVALID;
	if (!kobox_machine_domain_can_enter(domain, owner))
		return KOBOX_MACHINE_BUSY;
	domain->owner = owner;
	return KOBOX_MACHINE_OK;
}

void kobox_machine_domain_release(struct kobox_machine_domain *domain)
{
	domain->owner = NULL;
	domain->handoff_released = true;
}

enum kobox_machine_result kobox_machine_domain_handoff(
	struct kobox_machine_domain *domain, const void *previous, void *next)
{
	if (!previous || !next || previous == next ||
	    domain->owner != previous || !domain->handoff_released)
		return KOBOX_MACHINE_INVALID;
	domain->handoff_released = false;
	domain->owner = next;
	return KOBOX_MACHINE_OK;
}

void kobox_machine_domain_handoff_finish(struct kobox_machine_domain *domain)
{
	domain->handoff_released = true;
}

void kobox_machine_domain_handoff_abort(struct kobox_machine_domain *domain,
				       void *previous)
{
	domain->owner = previous;
	domain->handoff_released = true;
}

bool kobox_machine_domain_pending(const struct kobox_machine_domain *domain)
{
	unsigned int index;

	for (index = 0; index < KOBOX_MACHINE_NOTIFICATION_COUNT; index++) {
		if (atomic_load_explicit(&domain->pending[index],
					 memory_order_acquire))
			return true;
	}
	return false;
}

enum kobox_machine_result kobox_machine_domain_close(
	struct kobox_machine_domain *domain)
{
	if (domain->owner || atomic_load_explicit(&domain->irq_depth,
						memory_order_acquire) ||
	    kobox_machine_domain_pending(domain))
		return KOBOX_MACHINE_BUSY;
	domain->accepting = false;
	return KOBOX_MACHINE_OK;
}

void kobox_machine_domain_reopen(struct kobox_machine_domain *domain)
{
	domain->accepting = true;
}

bool kobox_machine_domain_should_wait(const struct kobox_machine_domain *domain,
				     uint64_t observed_sequence)
{
	/* A new sequence can be observed before its upcall is delivered. */
	return atomic_load_explicit(&domain->sequence, memory_order_acquire) ==
		observed_sequence && !kobox_machine_domain_pending(domain);
}

static enum kobox_machine_result increment(atomic_uint_fast64_t *counter)
{
	uint_fast64_t value = atomic_load_explicit(counter, memory_order_relaxed);

	for (;;) {
		if (value == UINT64_MAX)
			return KOBOX_MACHINE_OVERFLOW;
		if (atomic_compare_exchange_weak_explicit(counter, &value,
				value + 1, memory_order_release, memory_order_relaxed))
			return KOBOX_MACHINE_OK;
	}
}

enum kobox_machine_result kobox_machine_domain_notify(
	struct kobox_machine_domain *domain,
	enum kobox_machine_notification notification)
{
	enum kobox_machine_result result;

	if (notification < 0 || notification >= KOBOX_MACHINE_NOTIFICATION_COUNT)
		return KOBOX_MACHINE_INVALID;
	if (!domain->accepting)
		return KOBOX_MACHINE_CLOSED;
	/* The lock serializes producers; consumers may run in a remote upcall.
	 * Check sequence exhaustion before publishing pending, so a consumer
	 * cannot drain an increment which would then need to be rolled back.
	 */
	if (atomic_load_explicit(&domain->sequence, memory_order_relaxed) ==
	    UINT64_MAX)
		return KOBOX_MACHINE_OVERFLOW;
	result = increment(&domain->pending[notification]);
	if (result != KOBOX_MACHINE_OK)
		return result;
	atomic_fetch_add_explicit(&domain->sequence, 1, memory_order_release);
	return KOBOX_MACHINE_OK;
}

enum kobox_machine_result kobox_machine_domain_irq_disable(
	struct kobox_machine_domain *domain)
{
	unsigned int depth = atomic_load_explicit(&domain->irq_depth,
						memory_order_acquire);

	for (;;) {
		if (depth == UINT_MAX)
			return KOBOX_MACHINE_OVERFLOW;
		if (atomic_compare_exchange_weak_explicit(&domain->irq_depth,
				&depth, depth + 1, memory_order_acq_rel,
				memory_order_acquire))
			return KOBOX_MACHINE_OK;
	}
}

enum kobox_machine_result kobox_machine_domain_irq_enable(
	struct kobox_machine_domain *domain, bool *dispatch)
{
	*dispatch = false;
	if (!atomic_load_explicit(&domain->irq_depth, memory_order_acquire))
		return KOBOX_MACHINE_INVALID;
	*dispatch = atomic_fetch_sub_explicit(&domain->irq_depth, 1,
					     memory_order_acq_rel) == 1;
	return KOBOX_MACHINE_OK;
}

bool kobox_machine_domain_irq_take(struct kobox_machine_domain *domain,
				   enum kobox_machine_notification *notification,
				   uint64_t *count)
{
	unsigned int index;

	if (atomic_load_explicit(&domain->irq_depth, memory_order_acquire))
		return false;
	for (index = 0; index < KOBOX_MACHINE_NOTIFICATION_COUNT; index++) {
		*count = atomic_exchange_explicit(&domain->pending[index], 0,
						 memory_order_acq_rel);
		if (*count) {
			atomic_store_explicit(&domain->irq_depth, 1,
					      memory_order_release);
			*notification = (enum kobox_machine_notification)index;
			return true;
		}
	}
	return false;
}

bool kobox_machine_domain_irq_return(struct kobox_machine_domain *domain)
{
	if (atomic_load_explicit(&domain->irq_depth, memory_order_acquire) != 1)
		return false;
	atomic_store_explicit(&domain->irq_depth, 0, memory_order_release);
	return true;
}
