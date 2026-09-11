// SPDX-License-Identifier: GPL-2.0-only

#include "irq_host.h"
#include "../arch/x86_64/host_call.h"

#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/list.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/xarray.h>

struct kobox_linux_irq_port {
	struct list_head list;
	struct pci_dev *device;
	struct kobox_linux_irq_host host;
	struct irq_domain *wired, *parent;
	struct fwnode_handle *fwnode;
	struct xarray routes;
	unsigned int intx, saved_irq;
};

struct hosted_irq {
	struct kobox_linux_irq_port *port;
	struct irq_domain *domain;
	struct kobox_linux_irq_route route;
	unsigned int virq;
};

static LIST_HEAD(irq_ports);
static DEFINE_MUTEX(irq_ports_lock);

static void mask(struct irq_data *data)
{
	struct hosted_irq *irq = irq_data_get_irq_chip_data(data);
	const struct kobox_linux_irq_host *host = &irq->port->host;

	if (kobox_host_call(host->mask(host->context, irq->route.hwirq, irq->route.cookie, 1)))
		panic("host IRQ mask failed\n");
}

static void unmask(struct irq_data *data)
{
	struct hosted_irq *irq = irq_data_get_irq_chip_data(data);
	const struct kobox_linux_irq_host *host = &irq->port->host;

	if (kobox_host_call(host->mask(host->context, irq->route.hwirq, irq->route.cookie, 0)))
		panic("host IRQ unmask failed\n");
}

static void ack(struct irq_data *data)
{
	struct hosted_irq *irq = irq_data_get_irq_chip_data(data);
	const struct kobox_linux_irq_host *host = &irq->port->host;

	if (kobox_host_call(host->ack(host->context, irq->route.hwirq, irq->route.cookie)))
		panic("host IRQ acknowledgement failed\n");
}

static int affinity(struct irq_data *data, const struct cpumask *mask, bool force)
{
	struct hosted_irq *irq = irq_data_get_irq_chip_data(data);
	const struct kobox_linux_irq_host *host = &irq->port->host;
	unsigned int cpu = cpumask_first_and(mask, cpu_online_mask);
	int result;

	if (cpu >= nr_cpu_ids)
		return -EINVAL;
	result = kobox_host_call(host->affinity(host->context, irq->route.hwirq, irq->route.cookie, cpu));
	if (result)
		return result < 0 ? result : -EIO;
	irq_data_update_effective_affinity(data, cpumask_of(cpu));
	/* The host interrupt remapper keeps address/data stable on migration. */
	return IRQ_SET_MASK_OK_DONE;
}

static int retrigger(struct irq_data *data)
{
	struct hosted_irq *irq = irq_data_get_irq_chip_data(data);
	const struct kobox_linux_irq_host *host = &irq->port->host;

	if (kobox_host_call(host->retrigger(host->context, irq->route.hwirq, irq->route.cookie)))
		panic("host IRQ retrigger failed\n");
	return 1;
}

static void compose(struct irq_data *data, struct msi_msg *message)
{
	struct hosted_irq *irq = irq_data_get_irq_chip_data(data);

	*message = (struct msi_msg) {
		.address_lo = lower_32_bits(irq->route.address),
		.address_hi = upper_32_bits(irq->route.address),
		.data = irq->route.data,
	};
}

static int active(struct irq_data *data, enum irqchip_irq_state which, bool *state)
{
	struct hosted_irq *irq = irq_data_get_irq_chip_data(data);
	const struct kobox_linux_irq_host *host = &irq->port->host;
	unsigned int in_flight;
	int result;

	if (which != IRQCHIP_STATE_ACTIVE)
		return -EOPNOTSUPP;
	result = kobox_host_call(host->active(host->context, irq->route.hwirq, irq->route.cookie, &in_flight));
	if (result || in_flight > 1)
		panic("host IRQ active-state query failed\n");
	*state = in_flight;
	return 0;
}

static void release_resources(struct irq_data *data)
{
	struct hosted_irq *irq = irq_data_get_irq_chip_data(data);
	const struct kobox_linux_irq_host *host = &irq->port->host;
	unsigned long flags;

	local_irq_save(flags);
	mask(data);
	local_irq_restore(flags);
	/* Retire host deliveries before the descriptor or its handler is reused.
	 * Device-side MSI-X masking alone does not cancel a queued CPU doorbell.
	 */
	synchronize_rcu();
	local_irq_save(flags);
	if (kobox_host_call(host->quiesce(host->context, irq->route.hwirq, irq->route.cookie)))
		panic("host IRQ pending notification drain failed\n");
	local_irq_restore(flags);
}

static struct irq_chip host_chip = {
	.name = "host-route", .irq_mask = mask, .irq_unmask = unmask,
	.irq_ack = ack, .irq_set_affinity = affinity,
	.irq_retrigger = retrigger, .irq_compose_msi_msg = compose,
	.irq_get_irqchip_state = active,
	.irq_release_resources = release_resources,
};

static void release_route(struct kobox_linux_irq_port *port,
			  const struct kobox_linux_irq_route *route)
{
	unsigned long flags;

	local_irq_save(flags);
	if (kobox_host_call(port->host.release(port->host.context, route->hwirq, route->cookie)))
		panic("host IRQ invalidation failed; retaining route\n");
	local_irq_restore(flags);
}

static void free_irqs(struct irq_domain *domain, unsigned int virq,
		      unsigned int count)
{
	struct kobox_linux_irq_port *port = domain->host_data;
	unsigned int index;

	for (index = 0; index < count; index++) {
		struct irq_data *data = irq_domain_get_irq_data(domain, virq + index);
		struct hosted_irq *irq = irq_data_get_irq_chip_data(data);
		unsigned long flags;

		local_irq_save(flags);
		mask(data);
		local_irq_restore(flags);
		xa_erase(&port->routes, irq->route.hwirq);
		/* A dispatched event holds RCU through generic_handle_irq(). Wait
		 * before Linux can recycle virq, not merely before freeing metadata.
		 */
		synchronize_rcu();
		release_route(port, &irq->route);
		irq_domain_reset_irq_data(data);
		kfree(irq);
	}
}

static int allocate_irqs(struct irq_domain *domain, unsigned int virq,
			 unsigned int count, void *argument)
{
	struct kobox_linux_irq_port *port = domain->host_data;
	msi_alloc_info_t *info = argument;
	struct kobox_linux_irq_route *routes;
	enum kobox_linux_irq_mode mode = KOBOX_IRQ_INTX;
	unsigned int index = 0, cpu, done = 0;
	unsigned long flags;
	int result;

	if (domain != port->wired) {
		if (!info || !info->desc || info->desc->dev != &port->device->dev)
			return -EINVAL;
		mode = info->desc->pci.msi_attrib.is_msix ? KOBOX_IRQ_MSIX : KOBOX_IRQ_MSI;
		index = info->desc->msi_index;
	}
	if (!count || (mode == KOBOX_IRQ_INTX && count != 1))
		return -EINVAL;
	routes = kcalloc(count, sizeof(*routes), GFP_KERNEL);
	if (!routes)
		return -ENOMEM;
	cpu = cpumask_first_and(irq_get_affinity_mask(virq), cpu_online_mask);
	local_irq_save(flags);
	result = kobox_host_call(port->host.allocate(port->host.context, mode, index, count, cpu, routes));
	local_irq_restore(flags);
	if (result) {
		kfree(routes);
		return result < 0 ? result : -EIO;
	}
	for (index = 0; index < count; index++) {
		struct hosted_irq *irq;

		result = -EPROTO;
		if (!routes[index].cookie || routes[index].hwirq > ULONG_MAX)
			goto rollback;
		if (mode == KOBOX_IRQ_MSI &&
		    (routes[index].address != routes[0].address ||
		     (routes[0].data & (count - 1)) ||
		     routes[index].data != routes[0].data + index))
			goto rollback;
		irq = kzalloc(sizeof(*irq), GFP_KERNEL);
		if (!irq) {
			result = -ENOMEM;
			goto rollback;
		}
		irq->port = port;
		irq->domain = domain;
		irq->route = routes[index];
		irq->virq = virq + index;
		result = xa_insert(&port->routes, routes[index].hwirq, irq, GFP_KERNEL);
		if (result) {
			kfree(irq);
			goto rollback;
		}
		irq_domain_set_info(domain, irq->virq, irq->route.hwirq, &host_chip, irq,
			mode == KOBOX_IRQ_INTX ? handle_level_irq : handle_edge_irq, NULL, NULL);
		irq_data_update_effective_affinity(irq_domain_get_irq_data(domain, irq->virq),
						   cpumask_of(cpu));
		done++;
	}
	kfree(routes);
	return 0;
rollback:
	if (done)
		free_irqs(domain, virq, done);
	for (index = done; index < count; index++)
		release_route(port, &routes[index]);
	kfree(routes);
	return result;
}

static const struct irq_domain_ops domain_ops = {
	.alloc = allocate_irqs, .free = free_irqs,
};

static bool init_msi_info(struct device *device, struct irq_domain *domain,
			  struct irq_domain *parent, struct msi_domain_info *info)
{
	if (!msi_lib_init_dev_msi_info(device, domain, parent, info))
		return false;
	info->handler = handle_edge_irq;
	info->handler_name = "edge";
	info->chip->irq_retrigger = irq_chip_retrigger_hierarchy;
	info->chip->irq_release_resources = irq_chip_release_resources_parent;
	return true;
}

static const struct msi_parent_ops parent_ops = {
	.supported_flags = MSI_GENERIC_FLAGS_MASK | MSI_FLAG_MULTI_PCI_MSI | MSI_FLAG_PCI_MSIX,
	.required_flags = MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
			  MSI_FLAG_PCI_MSI_STARTUP_PARENT,
	.chip_flags = MSI_CHIP_FLAG_SET_ACK,
	.bus_select_token = DOMAIN_BUS_NEXUS,
	.bus_select_mask = MATCH_PCI_MSI,
	.prefix = "host-",
	.init_dev_msi_info = init_msi_info,
};

void kobox_linux_irq_dispatch(unsigned int cpu)
{
	struct kobox_linux_irq_port *port;

	if (!in_hardirq() || !irqs_disabled() || cpu != raw_smp_processor_id())
		panic("host device IRQ outside its CPU execution domain\n");
	rcu_read_lock();
	list_for_each_entry_rcu(port, &irq_ports, list) {
		unsigned int budget;

		for (budget = 0; budget < 64; budget++) {
			struct kobox_linux_irq_event event;
			struct hosted_irq *irq;
			int result = kobox_host_call(port->host.next(port->host.context, cpu, &event));

			if (result == -EAGAIN)
				break;
			if (result)
				panic("host IRQ receive failed\n");
			irq = xa_load(&port->routes, event.hwirq);
			if (irq && irq->route.cookie == event.cookie &&
			    generic_handle_domain_irq(irq->domain, event.hwirq) &&
			    kobox_host_call(port->host.ack(port->host.context, event.hwirq, event.cookie)))
				panic("host IRQ retirement acknowledgement failed\n");
		}
		if (budget == 64)
			kobox_linux_task_device_irq_raise(cpu);
	}
	rcu_read_unlock();
}

int kobox_linux_irq_attach(struct pci_dev *device,
			   const struct kobox_linux_irq_host *host,
			   struct kobox_linux_irq_port **out)
{
	struct kobox_linux_irq_port *port;
	int result = -ENOMEM;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!device || device->driver || device->dev.driver ||
	    device->msi_enabled || device->msix_enabled || dev_get_msi_domain(&device->dev) ||
	    !host || host->size != sizeof(*host) || !host->context || !host->allocate ||
	    !host->release || !host->mask || !host->ack || !host->quiesce ||
	    !host->active || !host->affinity ||
	    !host->retrigger || !host->next)
		return -EINVAL;
	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	port->device = pci_dev_get(device);
	port->host = *host;
	port->saved_irq = device->irq;
	xa_init(&port->routes);
	port->fwnode = irq_domain_alloc_named_fwnode(dev_name(&device->dev));
	if (!port->fwnode)
		goto out;
	port->wired = irq_domain_create_hierarchy(NULL, 0, 0, port->fwnode, &domain_ops, port);
	port->parent = msi_create_parent_irq_domain(&(struct irq_domain_info) {
		.fwnode = port->fwnode, .ops = &domain_ops, .host_data = port,
	}, &parent_ops);
	if (!port->wired || !port->parent)
		goto out;
	result = irq_domain_alloc_irqs(port->wired, 1, NUMA_NO_NODE, NULL);
	/* A host may grant message interrupts without an INTx route. Preserve
	 * that boundary: no invented wired IRQ and no successful INTx fallback.
	 */
	if (result == -EOPNOTSUPP) {
		irq_domain_remove(port->wired);
		port->wired = NULL;
		result = 0;
	} else if (result < 0) {
		goto out;
	}
	port->intx = result;
	device->irq = port->intx;
	dev_set_msi_domain(&device->dev, port->parent);
	mutex_lock(&irq_ports_lock);
	list_add_tail_rcu(&port->list, &irq_ports);
	mutex_unlock(&irq_ports_lock);
	*out = port;
	return 0;
out:
	if (port->parent)
		irq_domain_remove(port->parent);
	if (port->wired)
		irq_domain_remove(port->wired);
	irq_domain_free_fwnode(port->fwnode);
	xa_destroy(&port->routes);
	pci_dev_put(port->device);
	kfree(port);
	return result;
}

int kobox_linux_irq_detach(struct kobox_linux_irq_port *port)
{
	if (!port)
		return -EINVAL;
	if (port->device->msi_enabled || port->device->msix_enabled ||
	    port->parent->mapcount || (port->intx && irq_has_action(port->intx)))
		return -EBUSY;
	if (port->intx)
		irq_domain_free_irqs(port->intx, 1);
	mutex_lock(&irq_ports_lock);
	list_del_rcu(&port->list);
	mutex_unlock(&irq_ports_lock);
	synchronize_rcu();
	if (port->device->dev.msi.data)
		msi_remove_device_irq_domain(&port->device->dev, MSI_DEFAULT_DOMAIN);
	dev_set_msi_domain(&port->device->dev, NULL);
	port->device->irq = port->saved_irq;
	irq_domain_remove(port->parent);
	if (port->wired)
		irq_domain_remove(port->wired);
	irq_domain_free_fwnode(port->fwnode);
	xa_destroy(&port->routes);
	pci_dev_put(port->device);
	kfree(port);
	return 0;
}
