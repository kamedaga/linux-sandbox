// SPDX-License-Identifier: GPL-2.0-only

#include "pci_host.h"
#include "exception.h"

#include <linux/io.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/virtio_pci.h>
#include <linux/vmalloc.h>

/* Transport prerequisite only. Device negotiation, queues and DRM still
 * belong to the native upstream modules and their separate integration Gate.
 */
int kobox_linux_pci_hardware_verify(const struct kobox_linux_pci_host *host,
				   struct kobox_linux_pci_report *report,
				   int (*mapping_faults)(void *address))
{
	struct pci_host_bridge *bridge = NULL;
	struct pci_dev *device = NULL;
	void __iomem *common = NULL;
	unsigned int capability = 0, visits = 0;
	u32 offset, length;
	u8 type, bar;
	bool enabled = false;
	int result;

	if (!report || report->size != sizeof(*report) || !mapping_faults)
		return -EINVAL;
	report->phase = 1;
	result = kobox_linux_pci_scan(host, &bridge);
	if (result)
		goto out;
	report->scans++;
	device = pci_get_slot(bridge->bus, host->devfn);
	result = -ENODEV;
	if (!device || device->vendor != 0x1af4 || device->device != 0x1050 ||
	    device->driver || device->dev.driver)
		goto remove;
	/* No guest firmware ran in QEMU. Resource sizing and placement are
	 * upstream PCI operations, not fixture-written BAR values.
	 */
	pci_assign_unassigned_bus_resources(bridge->bus);
	result = pci_enable_device(device);
	if (result)
		goto remove;
	enabled = true;
	result = -EINVAL;
	report->phase = 2;
	if (!pci_find_capability(device, PCI_CAP_ID_MSIX))
		goto remove;
	report->capabilities++;
	for (capability = pci_find_capability(device, PCI_CAP_ID_VNDR); capability;
	     capability = pci_find_next_capability(device, capability, PCI_CAP_ID_VNDR)) {
		if (++visits > 48 ||
		    pci_read_config_byte(device, capability + offsetof(struct virtio_pci_cap, cfg_type),
					 &type))
			goto remove;
		if (type == VIRTIO_PCI_CAP_COMMON_CFG)
			break;
	}
	report->phase = 4;
	if (!capability ||
	    pci_read_config_byte(device, capability + offsetof(struct virtio_pci_cap, bar), &bar) ||
	    pci_read_config_dword(device, capability + offsetof(struct virtio_pci_cap, offset), &offset) ||
	    pci_read_config_dword(device, capability + offsetof(struct virtio_pci_cap, length), &length))
		goto remove;
	report->phase = 5;
	if (bar >= PCI_STD_NUM_BARS || length < sizeof(struct virtio_pci_common_cfg) ||
	    offset > pci_resource_len(device, bar) || length > pci_resource_len(device, bar) - offset ||
	    !device->resource[bar].parent)
		goto remove;
	report->capabilities++;
	report->bar_start = pci_resource_start(device, bar);
	report->bar_size = pci_resource_len(device, bar);
	common = pci_iomap_range(device, bar, offset, length);
	if (!common)
		goto remove;
	report->mappings++;
	report->phase = 3;
	if (mapping_faults((void __force *)common) != 1)
		goto remove;
	iowrite32(1, common + offsetof(struct virtio_pci_common_cfg, device_feature_select));
	if ((ioread32(common + offsetof(struct virtio_pci_common_cfg, device_feature)) & 3) != 3)
		goto remove;
	/* Reading back the selector tests register side effects, not shared RAM. */
	if (ioread32(common + offsetof(struct virtio_pci_common_cfg, device_feature_select)) != 1)
		goto remove;
	iowrite32(0, common + offsetof(struct virtio_pci_common_cfg, device_feature_select));
	result = 0;
remove:
	if (common) {
		void *retired = (void __force *)common;

		pci_iounmap(device, common);
		vm_unmap_aliases();
		if (mapping_faults(retired) != 1)
			result = -EINVAL;
		else
			report->revoked_mappings++;
	}
	if (enabled)
		pci_disable_device(device);
	pci_dev_put(device);
	if (kobox_linux_pci_remove(bridge)) {
		result = -EBUSY;
	} else {
		rcu_barrier();
		report->removals++;
	}
out:
	report->warnings = kobox_linux_exception_warnings();
	if (!result && report->warnings)
		result = -EINVAL;
	report->result = result;
	return result;
}
