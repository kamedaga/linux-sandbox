/* SPDX-License-Identifier: GPL-2.0-only */
#include "domain.h"

#include <limits.h>
#include <stdio.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
		return 1; \
	} \
} while (0)

int main(void)
{
	struct kobox_machine_domain cpu;
	int first, second;
	enum kobox_machine_notification notification;
	uint64_t count;
	bool dispatch;

	CHECK(kobox_machine_domain_init(&cpu) == KOBOX_MACHINE_OK);
	CHECK(kobox_machine_domain_enter(&cpu, &first) == KOBOX_MACHINE_OK);
	CHECK(!kobox_machine_domain_can_enter(&cpu, &second));
	CHECK(kobox_machine_domain_close(&cpu) == KOBOX_MACHINE_BUSY);
	CHECK(kobox_machine_domain_handoff(&cpu, &second, &first) ==
	      KOBOX_MACHINE_INVALID);
	CHECK(cpu.owner == &first);
	CHECK(kobox_machine_domain_handoff(&cpu, &first, &second) ==
	      KOBOX_MACHINE_OK);
	CHECK(!kobox_machine_domain_can_enter(&cpu, &second));
	/* A wake during transfer remains pending before the next owner enters. */
	CHECK(kobox_machine_domain_notify(&cpu, KOBOX_MACHINE_RESCHEDULE) ==
	      KOBOX_MACHINE_OK);
	CHECK(!kobox_machine_domain_should_wait(&cpu, 1));
	kobox_machine_domain_handoff_abort(&cpu, &first);
	CHECK(kobox_machine_domain_can_enter(&cpu, &first));
	CHECK(!kobox_machine_domain_can_enter(&cpu, &second));
	CHECK(kobox_machine_domain_handoff(&cpu, &first, &second) ==
	      KOBOX_MACHINE_OK);
	kobox_machine_domain_handoff_finish(&cpu);
	CHECK(kobox_machine_domain_enter(&cpu, &second) == KOBOX_MACHINE_OK);
	CHECK(kobox_machine_domain_irq_disable(&cpu) == KOBOX_MACHINE_OK);
	CHECK(kobox_machine_domain_irq_disable(&cpu) == KOBOX_MACHINE_OK);
	CHECK(!kobox_machine_domain_irq_take(&cpu, &notification, &count));
	CHECK(kobox_machine_domain_irq_enable(&cpu, &dispatch) == KOBOX_MACHINE_OK);
	CHECK(!dispatch);
	CHECK(kobox_machine_domain_irq_enable(&cpu, &dispatch) == KOBOX_MACHINE_OK);
	CHECK(dispatch);
	CHECK(kobox_machine_domain_irq_take(&cpu, &notification, &count));
	CHECK(notification == KOBOX_MACHINE_RESCHEDULE && count == 1);
	CHECK(!kobox_machine_domain_irq_take(&cpu, &notification, &count));
	/* Linux enables IRQs inside the callback, allowing nested delivery. */
	CHECK(kobox_machine_domain_irq_enable(&cpu, &dispatch) == KOBOX_MACHINE_OK);
	CHECK(kobox_machine_domain_notify(&cpu, KOBOX_MACHINE_TICK) ==
	      KOBOX_MACHINE_OK);
	CHECK(kobox_machine_domain_irq_take(&cpu, &notification, &count));
	CHECK(notification == KOBOX_MACHINE_TICK && count == 1);
	CHECK(kobox_machine_domain_irq_return(&cpu));
	CHECK(kobox_machine_domain_irq_disable(&cpu) == KOBOX_MACHINE_OK);
	CHECK(kobox_machine_domain_irq_return(&cpu));
	CHECK(!kobox_machine_domain_irq_return(&cpu));
	CHECK(kobox_machine_domain_irq_enable(&cpu, &dispatch) ==
	      KOBOX_MACHINE_INVALID);
	CHECK(!dispatch);
	CHECK(kobox_machine_domain_should_wait(&cpu, 2));
	CHECK(!kobox_machine_domain_should_wait(&cpu, 1));
	atomic_store(&cpu.irq_depth, UINT_MAX);
	CHECK(kobox_machine_domain_irq_disable(&cpu) == KOBOX_MACHINE_OVERFLOW);
	CHECK(atomic_load(&cpu.irq_depth) == UINT_MAX);
	atomic_store(&cpu.irq_depth, 0);
	atomic_store(&cpu.sequence, UINT64_MAX);
	CHECK(kobox_machine_domain_notify(&cpu, KOBOX_MACHINE_TICK) ==
	      KOBOX_MACHINE_OVERFLOW);
	CHECK(!kobox_machine_domain_pending(&cpu));
	atomic_store(&cpu.sequence, 2);
	atomic_store(&cpu.pending[KOBOX_MACHINE_TICK], UINT64_MAX);
	CHECK(kobox_machine_domain_notify(&cpu, KOBOX_MACHINE_TICK) ==
	      KOBOX_MACHINE_OVERFLOW);
	CHECK(atomic_load(&cpu.sequence) == 2);
	CHECK(kobox_machine_domain_irq_take(&cpu, &notification, &count));
	CHECK(count == UINT64_MAX);
	CHECK(kobox_machine_domain_irq_return(&cpu));
	kobox_machine_domain_release(&cpu);
	CHECK(kobox_machine_domain_close(&cpu) == KOBOX_MACHINE_OK);
	CHECK(kobox_machine_domain_notify(&cpu, KOBOX_MACHINE_TICK) ==
	      KOBOX_MACHINE_CLOSED);
	kobox_machine_domain_reopen(&cpu);
	CHECK(kobox_machine_domain_notify(&cpu, KOBOX_MACHINE_TICK) ==
	      KOBOX_MACHINE_OK);
	CHECK(kobox_machine_domain_close(&cpu) == KOBOX_MACHINE_BUSY);
	puts("machine domain: ownership, handoff, pending, nested IRQ, overflow OK");
	return 0;
}
