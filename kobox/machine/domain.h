/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_MACHINE_DOMAIN_H
#define KOBOX_MACHINE_DOMAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

enum kobox_machine_notification {
	KOBOX_MACHINE_TICK,
	KOBOX_MACHINE_RESCHEDULE,
	KOBOX_MACHINE_CALL_FUNCTION,
	KOBOX_MACHINE_DEVICE_IRQ,
	KOBOX_MACHINE_CONTROL_EVENT,
	KOBOX_MACHINE_VM_EVENT,
	KOBOX_MACHINE_NOTIFICATION_COUNT,
};

/* Internal results, not native errno values or a wire ABI. */
enum kobox_machine_result {
	KOBOX_MACHINE_OK,
	KOBOX_MACHINE_INVALID,
	KOBOX_MACHINE_BUSY,
	KOBOX_MACHINE_OVERFLOW,
	KOBOX_MACHINE_CLOSED,
	KOBOX_MACHINE_NOT_LOCK_FREE,
};

/*
 * The backend serializes owner/handoff/accepting and notification publication
 * with its execution lock, masking native upcalls around that lock. Hold the
 * lock through native delivery so the owner's thread handle cannot expire.
 * IRQ state and pending counters must also be accessible from a nested upcall;
 * initialization rejects non-lock-free atomics rather than hiding a host lock.
 * Owner is an opaque identity, never a runnable queue or a scheduling choice.
 * irq_take/irq_return run only on that owner with native upcalls masked. The
 * caller may unmask native delivery during the Linux callback; after it returns
 * it must mask again and fetch the current domain anew (migration is allowed).
 * Logical IRQ enable can dispatch recursively. No backend may hold its owner
 * lock while calling Linux, or replace logical IRQ depth with native masking.
 */
struct kobox_machine_domain {
	void *owner;
	bool handoff_released;
	bool accepting;
	atomic_uint irq_depth;
	atomic_bool stop_requested;
	atomic_bool stopped;
	atomic_uint_fast64_t sequence;
	atomic_uint_fast64_t pending[KOBOX_MACHINE_NOTIFICATION_COUNT];
};

enum kobox_machine_result kobox_machine_domain_init(
	struct kobox_machine_domain *domain);
bool kobox_machine_domain_can_enter(const struct kobox_machine_domain *domain,
				    const void *owner);
enum kobox_machine_result kobox_machine_domain_enter(
	struct kobox_machine_domain *domain, void *owner);
void kobox_machine_domain_release(struct kobox_machine_domain *domain);
enum kobox_machine_result kobox_machine_domain_handoff(
	struct kobox_machine_domain *domain, const void *previous, void *next);
void kobox_machine_domain_handoff_finish(struct kobox_machine_domain *domain);
void kobox_machine_domain_handoff_abort(struct kobox_machine_domain *domain,
				       void *previous);
enum kobox_machine_result kobox_machine_domain_close(
	struct kobox_machine_domain *domain);
void kobox_machine_domain_reopen(struct kobox_machine_domain *domain);
bool kobox_machine_domain_pending(const struct kobox_machine_domain *domain);
bool kobox_machine_domain_should_wait(const struct kobox_machine_domain *domain,
				     uint64_t observed_sequence);
enum kobox_machine_result kobox_machine_domain_notify(
	struct kobox_machine_domain *domain,
	enum kobox_machine_notification notification);
enum kobox_machine_result kobox_machine_domain_irq_disable(
	struct kobox_machine_domain *domain);
enum kobox_machine_result kobox_machine_domain_irq_enable(
	struct kobox_machine_domain *domain, bool *dispatch);
bool kobox_machine_domain_irq_take(struct kobox_machine_domain *domain,
				   enum kobox_machine_notification *notification,
				   uint64_t *count);
bool kobox_machine_domain_irq_return(struct kobox_machine_domain *domain);

#endif
