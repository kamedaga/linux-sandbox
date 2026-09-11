// SPDX-License-Identifier: GPL-2.0-only

#include "../boot/resource_port.h"
#include "../provider/device_resource_interfaces.h"

#include <kobox2/closure_layout.h>
#include <kobox2/pci_function_layout.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/workqueue.h>

static struct kobox_linux_resource_binding binding;
static const struct kobox_pci_function_resource_operations *pci;
static void *bar;
static bool fail_after_map;
module_param(fail_after_map, bool, 0400);
static bool heartbeat;
module_param(heartbeat, bool, 0400);
static struct delayed_work heartbeat_work;

static void heartbeat_fn(struct work_struct *work)
{
	u32 *counter = (u32 *)bar + 2;

	WRITE_ONCE(*counter, READ_ONCE(*counter) + 1);
	queue_delayed_work(system_wq, &heartbeat_work, 1);
}

static int __init resource_test_init(void)
{
	static const uint8_t digest[32] = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;
	struct kobox_linux_resource_binding denied;
	struct kobox_pci_function_identity identity;
	u64 bar_address, length;
	u32 flags, value;
	int error;

	error = kobox_linux_resource_bind(THIS_MODULE, 1, 0,
		KB2_PCI_FUNCTION_REQUIRED_RIGHTS, digest, &binding);
	if (error)
		return error;
	if (!binding.generation || !binding.object_id ||
	    binding.type != KB2_CLOSURE_RESOURCE_DEVICE)
		return -EINVAL;
	if (kobox_linux_resource_bind(THIS_MODULE, 2, 0, 0, digest, &denied) != -EACCES ||
	    kobox_linux_resource_bind(THIS_MODULE, 1, 0, ~0ULL, digest, &denied) != -EACCES)
		return -EINVAL;
	pci = binding.operations;
	if (pci->base.size != sizeof(*pci) || !pci->identity || !pci->config_read ||
	    !pci->config_write || !pci->bar_info || !pci->bar_map || !pci->bar_unmap ||
	    !pci->bar_read || !pci->bar_write)
		return -EINVAL;
	if (pci->identity(binding.object, &identity) ||
	    identity.generation != binding.generation ||
	    identity.object_id != binding.object_id || identity.vendor_id != 0x1af4 ||
	    pci->config_read(binding.object, 0, 4, &value) || value != 0x10501af4 ||
	    pci->config_write(binding.object, 0x40, 4, 0x12345678) ||
	    pci->config_read(binding.object, 0x40, 4, &value) || value != 0x12345678 ||
	    pci->bar_info(binding.object, 0, &bar_address, &length, &flags) || length != PAGE_SIZE ||
	    flags != KB2_PCI_FUNCTION_BAR_FLAG_MEMORY)
		return -EINVAL;
	error = pci->bar_map(binding.object, 0, 0, PAGE_SIZE,
		KB2_PCI_FUNCTION_MAP_PROTECTION_READ | KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE,
		KB2_PCI_FUNCTION_CACHE_UC_MINUS, NULL, &bar);
	if (error)
		return error;
	if (pci->bar_write(binding.object, 0, 0, 4, 0x72657331) ||
	    pci->bar_read(binding.object, 0, 0, 4, &value) || value != 0x72657331 ||
	    READ_ONCE(*(u32 *)bar) != 0x72657331) {
		pci->bar_unmap(binding.object, bar, PAGE_SIZE);
		bar = NULL;
		return -EINVAL;
	}
	WRITE_ONCE(*((u32 *)bar + 1), 0x62617234);
	if (fail_after_map) {
		error = pci->bar_unmap(binding.object, bar, PAGE_SIZE);
		bar = NULL;
		return error ? error : -ECANCELED;
	}
	if (heartbeat) {
		INIT_DELAYED_WORK(&heartbeat_work, heartbeat_fn);
		queue_delayed_work(system_wq, &heartbeat_work, 1);
	}
	pr_info("Native resource module: PCI config/BAR and grant visibility passed\n");
	return 0;
}

static void __exit resource_test_exit(void)
{
	if (heartbeat)
		disable_delayed_work_sync(&heartbeat_work);
	/* The parent checks this store through its own mapping after process exit. */
	WARN_ON(pci->bar_write(binding.object, 0, 0, 4, 0x72657332));
	WARN_ON(pci->bar_unmap(binding.object, bar, PAGE_SIZE));
}

module_init(resource_test_init);
module_exit(resource_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Native module resource-port conformance test, not a PCI driver");
