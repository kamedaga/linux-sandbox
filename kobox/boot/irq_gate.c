// SPDX-License-Identifier: GPL-2.0-only

#include "irq_gate.h"
#include "dma_gate.h"
#include "exception.h"

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/ktime.h>
#include <linux/pci.h>
#include <linux/sched.h>

struct irq_observer {
	struct completion done;
	const struct kobox_linux_irq_test *test;
	enum kobox_linux_irq_mode mode;
	unsigned int index, cpu, deliveries, errors;
	bool hold, installed;
	u32 *dma;
	u32 dma_value;
};

static irqreturn_t observe(int irq, void *argument)
{
	struct irq_observer *observer = argument;

	if (observer->mode == KOBOX_IRQ_INTX &&
	    observer->test->clear(observer->test->host.context, observer->mode, observer->index))
		observer->errors++;
	if (!in_hardirq() || !irqs_disabled() ||
	    raw_smp_processor_id() != observer->cpu || task_cpu(current) != observer->cpu)
		observer->errors++;
	if (observer->hold) {
		u64 deadline = ktime_get_ns() + 2 * NSEC_PER_SEC;
		int result = observer->test->checkpoint(observer->test->host.context,
						      KOBOX_IRQ_HOLD_ENTER);

		while (!result && ktime_get_ns() < deadline) {
			result = observer->test->checkpoint(observer->test->host.context,
							    KOBOX_IRQ_HOLD_WAIT);
			cpu_relax();
		}
		if (result != 1)
			observer->errors++;
	}
	if (observer->dma) {
		dma_rmb();
		if (READ_ONCE(*observer->dma) != observer->dma_value)
			observer->errors++;
	}
	observer->deliveries++;
	complete(&observer->done);
	return IRQ_HANDLED;
}

static int snapshot(const struct kobox_linux_irq_test *test,
		    struct kobox_linux_irq_test_state *state)
{
	unsigned long flags;
	int result;

	local_irq_save(flags);
	result = test->snapshot(test->host.context, state);
	local_irq_restore(flags);
	return result;
}

static int checkpoint(const struct kobox_linux_irq_test *test,
		      enum kobox_linux_irq_test_checkpoint checkpoint)
{
	unsigned long flags;
	int result;

	local_irq_save(flags);
	result = test->checkpoint(test->host.context, checkpoint);
	local_irq_restore(flags);
	return result;
}

static int verify_synchronization(struct pci_dev *device,
				  struct irq_observer *observer,
				  struct kobox_linux_irq_report *report)
{
	const struct kobox_linux_irq_test *test = observer->test;
	struct kobox_linux_irq_test_state state;
	unsigned int virq = pci_irq_vector(device, 0), before, pass;
	unsigned long flags;
	int result = 0;
	u64 deadline;

#define REQUIRE(condition) do { \
	if (!(condition)) { \
		report->line = __LINE__; \
		result = -EINVAL; \
		goto out; \
	} \
} while (0)
	migrate_disable();
	synchronize_irq(virq);
	observer->cpu = raw_smp_processor_id() ^ 1;
	REQUIRE(!irq_set_affinity(virq, cpumask_of(observer->cpu)));
	for (pass = 0; pass < 2; pass++) {
		observer->hold = true;
		before = observer->deliveries;
		local_irq_save(flags);
		result = test->fire(test->host.context, observer->mode, 0);
		local_irq_restore(flags);
		REQUIRE(!result);
		deadline = ktime_get_ns() + NSEC_PER_SEC;
		do {
			REQUIRE(!snapshot(test, &state));
			if (state.hold == 1)
				break;
			msleep(1);
		} while (ktime_get_ns() < deadline);
		REQUIRE(state.hold == 1 && observer->deliveries == before);
		/* A host device thread releases the held hardirq independently of
		 * both Linux CPUs, which are occupied by the handler and this wait.
		 */
		REQUIRE(!checkpoint(test, KOBOX_IRQ_HOLD_RELEASE_AFTER_DELAY));
		if (pass) {
			local_irq_save(flags);
			result = test->clear(test->host.context, observer->mode, 0);
			local_irq_restore(flags);
			REQUIRE(!result);
			free_irq(virq, observer);
			observer->installed = false;
		} else {
			synchronize_irq(virq);
		}
		REQUIRE(checkpoint(test, KOBOX_IRQ_HOLD_DONE) == 1);
		REQUIRE(wait_for_completion_timeout(&observer->done, 2 * HZ));
		REQUIRE(!observer->errors && observer->deliveries == before + 1);
		observer->hold = false;
		report->deliveries++;
		report->synchronizations++;
	}
	REQUIRE(!request_irq(virq, observe, 0, "host-irq-reinstalled", observer));
	observer->installed = true;
	before = observer->deliveries;
	local_irq_save(flags);
	result = test->fire(test->host.context, observer->mode, 0);
	local_irq_restore(flags);
	REQUIRE(!result);
	REQUIRE(wait_for_completion_timeout(&observer->done, 2 * HZ));
	REQUIRE(!observer->errors && observer->deliveries == before + 1);
	report->deliveries++;
out:
	if (result)
		checkpoint(test, KOBOX_IRQ_HOLD_ABORT);
	migrate_enable();
	return result;
#undef REQUIRE
}

static int verify_dma(struct pci_dev *device, struct irq_observer *observer,
		      struct kobox_linux_irq_report *report)
{
	const struct kobox_linux_irq_test *test = observer->test;
	unsigned int virq = pci_irq_vector(device, 0), before;
	u32 *cpu;
	dma_addr_t address;
	unsigned long flags;
	int result = -EINVAL;

	cpu = dma_alloc_coherent(&device->dev, PAGE_SIZE, &address, GFP_KERNEL);
	if (!cpu)
		return -ENOMEM;
	*cpu = 0;
	observer->dma = cpu;
	observer->dma_value = 0xc0fe0000 | observer->mode;
	before = observer->deliveries;
	local_irq_save(flags);
	result = test->dma_fire(test->host.context, observer->mode, 0,
				address, observer->dma_value);
	local_irq_restore(flags);
	if (!result && (!wait_for_completion_timeout(&observer->done, 2 * HZ) ||
			observer->errors || observer->deliveries != before + 1))
		result = -EINVAL;
	if (!result) {
		report->dma++;
		report->deliveries++;
	}
	/* Stop the source (including queued DMA) before releasing its RAM. */
	local_irq_save(flags);
	if (test->clear(test->host.context, observer->mode, 0))
		panic("DMA IRQ test source shutdown failed\n");
	local_irq_restore(flags);
	synchronize_irq(virq);
	observer->dma = NULL;
	dma_free_coherent(&device->dev, PAGE_SIZE, cpu, address);
	return result;
}

static bool wait_emitted(const struct kobox_linux_irq_test *test,
			 enum kobox_linux_irq_mode mode, unsigned int count)
{
	struct kobox_linux_irq_test_state state;
	u64 deadline = ktime_get_ns() + 200 * NSEC_PER_MSEC;

	/* Used with local IRQs disabled: expiry must not depend on a local tick. */
	do {
		if (snapshot(test, &state))
			return false;
		if (state.emitted[mode] >= count)
			return state.emitted[mode] == count;
		cpu_relax();
	} while (ktime_get_ns() < deadline);
	return false;
}

static int verify_pending_release(struct pci_dev *device,
				  struct irq_observer *observer,
				  struct kobox_linux_irq_report *report)
{
	const struct kobox_linux_irq_test *test = observer->test;
	struct kobox_linux_irq_test_state state;
	unsigned int virq = pci_irq_vector(device, 0), before;
	unsigned long flags;
	int result = 0;

#define REQUIRE(condition) do { \
	if (!(condition)) { \
		report->line = __LINE__; \
		result = -EINVAL; \
		goto out; \
	} \
} while (0)
	synchronize_irq(virq);
	before = observer->deliveries;
	REQUIRE(!snapshot(test, &state));
	local_irq_save(flags);
	test->pause_delivery(test->host.context, 1);
	result = test->fire(test->host.context, observer->mode, 0);
	local_irq_restore(flags);
	REQUIRE(!result);
	REQUIRE(wait_emitted(test, observer->mode, state.emitted[observer->mode] + 1));
	REQUIRE(!snapshot(test, &state) && state.queued == 1);
	REQUIRE(observer->deliveries == before);
	local_irq_save(flags);
	result = test->clear(test->host.context, observer->mode, 0);
	local_irq_restore(flags);
	REQUIRE(!result);
	free_irq(virq, observer);
	observer->installed = false;
	REQUIRE(!snapshot(test, &state) && !state.queued);
	REQUIRE(!request_irq(virq, observe, 0, "host-irq-after-pending", observer));
	observer->installed = true;
	local_irq_save(flags);
	test->pause_delivery(test->host.context, 0);
	local_irq_restore(flags);
	synchronize_rcu();
	REQUIRE(observer->deliveries == before && !completion_done(&observer->done));
	local_irq_save(flags);
	result = test->fire(test->host.context, observer->mode, 0);
	local_irq_restore(flags);
	REQUIRE(!result);
	REQUIRE(wait_for_completion_timeout(&observer->done, 2 * HZ));
	REQUIRE(!observer->errors && observer->deliveries == before + 1);
	report->deliveries++;
	report->pending_free++;
out:
	local_irq_save(flags);
	test->pause_delivery(test->host.context, 0);
	local_irq_restore(flags);
	return result;
#undef REQUIRE
}

static int verify_masks(struct pci_dev *device, unsigned int count,
			struct irq_observer *observers,
			struct kobox_linux_irq_report *report)
{
	struct irq_observer *observer = &observers[0];
	const struct kobox_linux_irq_test *test = observer->test;
	enum kobox_linux_irq_mode mode = observer->mode;
	struct kobox_linux_irq_test_state state;
	unsigned int virq = pci_irq_vector(device, 0), before, cpu, index;
	unsigned long flags;
	u16 control = 0;
	bool disabled = false, function_masked = false, valid;
	int result = 0;

#define REQUIRE(condition) do { \
	if (!(condition)) { \
		report->line = __LINE__; \
		result = -EINVAL; \
		goto out; \
	} \
} while (0)
	before = observer->deliveries;
	/* Require a device mask, not Linux's optional lazy disable/replay path. */
	irq_set_status_flags(virq, IRQ_DISABLE_UNLAZY);
	disable_irq(virq);
	disabled = true;
	local_irq_save(flags);
	result = test->fire(test->host.context, mode, 0);
	local_irq_restore(flags);
	REQUIRE(!result);
	msleep(20);
	REQUIRE(!snapshot(test, &state));
	REQUIRE((state.masked[mode] & 1) && (state.pending[mode] & 1));
	REQUIRE(mode != KOBOX_IRQ_MSIX || (state.msix_pba & 1));
	REQUIRE(observer->deliveries == before);
	enable_irq(virq);
	disabled = false;
	REQUIRE(wait_for_completion_timeout(&observer->done, 2 * HZ));
	REQUIRE(!observer->errors && observer->deliveries == before + 1);
	report->deliveries++;
	report->masks++;

	if (mode == KOBOX_IRQ_MSIX) {
		REQUIRE(!pci_read_config_word(device, device->msix_cap + PCI_MSIX_FLAGS, &control));
		REQUIRE(!pci_write_config_word(device, device->msix_cap + PCI_MSIX_FLAGS,
					      control | PCI_MSIX_FLAGS_MASKALL));
		function_masked = true;
		for (index = 0; index < count; index++) {
			local_irq_save(flags);
			result = test->fire(test->host.context, mode, index);
			local_irq_restore(flags);
			REQUIRE(!result);
		}
		msleep(20);
		REQUIRE(!snapshot(test, &state));
		REQUIRE(state.function_mask && state.msix_pba == BIT(count) - 1);
		REQUIRE(!state.masked[mode]);
		for (index = 0; index < count; index++)
			REQUIRE(!completion_done(&observers[index].done));
		REQUIRE(!pci_write_config_word(device, device->msix_cap + PCI_MSIX_FLAGS, control));
		function_masked = false;
		for (index = 0; index < count; index++) {
			REQUIRE(wait_for_completion_timeout(&observers[index].done, 2 * HZ));
			REQUIRE(!observers[index].errors);
			report->deliveries++;
		}
		REQUIRE(!snapshot(test, &state) && !state.msix_pba);
		report->masks++;
	}

	/* A device message is already queued to this CPU while local IRQs are
	 * disabled. First replay locally, then move a queued message remotely.
	 */
	for (index = 0; index < 2; index++) {
		synchronize_irq(virq);
		migrate_disable();
		cpu = raw_smp_processor_id();
		observer->cpu = index ? cpu ^ 1 : cpu;
		result = irq_set_affinity(virq, cpumask_of(cpu));
		if (result) {
			migrate_enable();
			REQUIRE(!result);
		}
		before = observer->deliveries;
		local_irq_save(flags);
		valid = !snapshot(test, &state);
		valid = valid && !test->fire(test->host.context, mode, 0);
		valid = valid && wait_emitted(test, mode, state.emitted[mode] + 1);
		valid = valid && observer->deliveries == before;
		if (index)
			valid = valid && !irq_set_affinity(virq, cpumask_of(cpu ^ 1));
		local_irq_restore(flags);
		migrate_enable();
		REQUIRE(valid);
		REQUIRE(wait_for_completion_timeout(&observer->done, 2 * HZ));
		REQUIRE(!observer->errors && observer->deliveries == before + 1);
		report->deliveries++;
		if (index)
			report->migrations++;
		else
			report->masks++;
	}
out:
	if (disabled)
		enable_irq(virq);
	irq_clear_status_flags(virq, IRQ_DISABLE_UNLAZY);
	if (function_masked)
		pci_write_config_word(device, device->msix_cap + PCI_MSIX_FLAGS, control);
	return result;
#undef REQUIRE
}

static int verify_mode(struct pci_dev *device, enum kobox_linux_irq_mode mode,
		       const struct kobox_linux_irq_test *test,
		       struct kobox_linux_irq_report *report)
{
	struct kobox_linux_irq_port *port = NULL;
	struct kobox_linux_dma_port *dma_port = NULL;
	struct irq_observer observers[4] = {};
	const unsigned int modes[] = {PCI_IRQ_INTX, PCI_IRQ_MSI, PCI_IRQ_MSIX};
	const unsigned int counts[] = {1, 4, 3};
	unsigned int index, cpu, requested = 0;
	int allocated = 0, result;
	unsigned long flags;

#define REQUIRE(condition) do { \
	if (!(condition)) { \
		report->line = __LINE__; \
		result = -EINVAL; \
		goto out; \
	} \
} while (0)
	result = dma_set_mask_and_coherent(&device->dev, DMA_BIT_MASK(32));
	if (!result)
		result = kobox_linux_dma_attach(&device->dev, &test->dma->host, &dma_port);
	if (!result)
		result = kobox_linux_irq_attach(device, &test->host, &port);
	if (result)
		goto out;
	if (mode == KOBOX_IRQ_MSIX) {
		struct kobox_linux_irq_test_state state;

		local_irq_save(flags);
		test->fail_allocate(test->host.context, 2);
		local_irq_restore(flags);
		allocated = pci_alloc_irq_vectors(device, 3, 3, PCI_IRQ_MSIX);
		local_irq_save(flags);
		test->fail_allocate(test->host.context, 0);
		local_irq_restore(flags);
		REQUIRE(allocated < 0 && !device->msix_enabled && !device->msi_enabled);
		REQUIRE(!snapshot(test, &state) && state.live == 1);
		report->rollbacks++;
	}
	/* Each mode is required independently; there is no fallback flag. */
	allocated = pci_alloc_irq_vectors(device, counts[mode], counts[mode], modes[mode]);
	REQUIRE(allocated == counts[mode]);
	REQUIRE(device->msix_enabled == (mode == KOBOX_IRQ_MSIX));
	REQUIRE(device->msi_enabled == (mode == KOBOX_IRQ_MSI));
	report->vectors += allocated;
	for (index = 0; index < allocated; index++) {
		init_completion(&observers[index].done);
		observers[index].test = test;
		observers[index].mode = mode;
		observers[index].index = index;
		result = request_irq(pci_irq_vector(device, index), observe, 0,
				     "host-irq-route", &observers[index]);
		if (result)
			goto out;
		observers[index].installed = true;
		requested++;
	}
	{
		struct kobox_linux_irq_test_state state;
		unsigned int retired;
		u64 deadline;

		REQUIRE(!snapshot(test, &state));
		retired = state.retired_deliveries;
		local_irq_save(flags);
		result = test->replay_retired(test->host.context, mode);
		local_irq_restore(flags);
		REQUIRE(result >= 0);
		if (result) {
			deadline = ktime_get_ns() + NSEC_PER_SEC;
			do {
				REQUIRE(!snapshot(test, &state));
				if (state.retired_deliveries != retired)
					break;
				msleep(1);
			} while (ktime_get_ns() < deadline);
			REQUIRE(state.retired_deliveries == retired + 1);
			/* The dispatcher holds RCU across lookup and handler entry. */
			synchronize_rcu();
			for (index = 0; index < allocated; index++)
				REQUIRE(!observers[index].deliveries &&
					!completion_done(&observers[index].done));
			report->stale++;
		}
	}
	for (cpu = 0; cpu < 2; cpu++) {
		for (index = 0; index < allocated; index++) {
			struct irq_observer *observer = &observers[index];

			observer->cpu = cpu;
			REQUIRE(!irq_set_affinity(pci_irq_vector(device, index), cpumask_of(cpu)));
			local_irq_save(flags);
			result = test->fire(test->host.context, mode, index);
			local_irq_restore(flags);
			REQUIRE(!result);
			REQUIRE(wait_for_completion_timeout(&observer->done, 2 * HZ));
			REQUIRE(!observer->errors && observer->deliveries == cpu + 1);
			report->deliveries++;
			report->cpu_mask |= BIT(cpu);
		}
	}
	result = verify_masks(device, allocated, observers, report);
	if (!result)
		result = verify_synchronization(device, &observers[0], report);
	if (!result)
		result = verify_dma(device, &observers[0], report);
	if (!result)
		result = verify_pending_release(device, &observers[0], report);
	if (!result) {
		local_irq_save(flags);
		result = test->capture(test->host.context, mode);
		local_irq_restore(flags);
	}
	if (!result)
		report->modes |= BIT(mode);
out:
	/* Device source shutdown is distinct from controller route masking. */
	for (index = 0; index < counts[mode]; index++) {
		local_irq_save(flags);
		if (test->clear(test->host.context, mode, index))
			panic("IRQ test source shutdown failed\n");
		local_irq_restore(flags);
	}
	for (index = 0; index < requested; index++)
		if (observers[index].installed)
			free_irq(pci_irq_vector(device, index), &observers[index]);
	if (allocated > 0)
		pci_free_irq_vectors(device);
	if (port && kobox_linux_irq_detach(port))
		return -EBUSY;
	if (dma_port && kobox_linux_dma_detach(dma_port))
		return -EBUSY;
	return result;
#undef REQUIRE
}

int kobox_linux_irq_verify(const struct kobox_linux_pci_host *pci,
			   const struct kobox_linux_irq_test *test,
			   struct kobox_linux_irq_report *report)
{
	struct pci_host_bridge *bridge = NULL;
	struct pci_dev *device = NULL;
	unsigned int round, mode;
	int result;

	if (!report || report->size != sizeof(*report) || !test ||
	    !test->fire || !test->clear || !test->snapshot || !test->fail_allocate ||
	    !test->checkpoint || !test->capture || !test->replay_retired ||
	    !test->dma || !test->dma_fire || !test->pause_delivery)
		return -EINVAL;
	result = kobox_linux_pci_scan(pci, &bridge);
	if (result)
		goto out;
	device = pci_get_slot(bridge->bus, pci->devfn);
	if (!device) {
		result = -ENODEV;
		goto out;
	}
	/* Reuse the same PCI device, including upstream's cached MSI data.
	 * Recreating the entire device would conceal a stale-domain reference.
	 */
	for (round = 0; round < 2; round++) {
		for (mode = KOBOX_IRQ_INTX; mode <= KOBOX_IRQ_MSIX; mode++) {
			result = verify_mode(device, mode, test, report);
			if (result)
				goto out;
		}
		report->rounds++;
	}
out:
	pci_dev_put(device);
	if (bridge && kobox_linux_pci_remove(bridge))
		return -EBUSY;
	report->warnings = kobox_linux_exception_warnings();
	if (!result && report->warnings)
		result = -EINVAL;
	report->drained = 1;
	report->result = result;
	return result;
}
