// SPDX-License-Identifier: GPL-2.0-only
#include "net_port.h"
#include "module_launch.h"
#include "dma_host.h"
#ifndef KOBOX_BOOT_WIFI
#define KOBOX_BOOT_WIFI 0
#endif
#if KOBOX_BOOT_WIFI
#include "wifi_service.h"
#include <linux/jiffies.h>
#include <linux/sched.h>
#endif
#include "../arch/x86_64/host_call.h"

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/mm.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <net/net_namespace.h>

#define KOBOX_NET_RX_CAPACITY 128U

struct kobox_linux_net_port {
	struct net_device *device;
	struct packet_type packets;
	spinlock_t rx_lock;
	struct kobox_linux_net_frame *rx;
	uint32_t head, count;
	uint64_t sequence, drops;
#if KOBOX_BOOT_WIFI
	struct kobox_linux_wifi_service *wifi;
	u64 association_sequence;
	bool wireless, authorized;
#endif
};

static bool belongs_to_pci_function(const struct net_device *net,
				    const struct pci_dev *pci)
{
	const struct device *parent;

	/* virtio-net is parented by a virtio_device below virtio-pci, while a
	 * native PCI NIC such as r8169 is parented directly by the PCI device. */
	for (parent = net->dev.parent; parent; parent = parent->parent)
		if (parent == &pci->dev)
			return true;
	return false;
}

static int receive_packet(struct sk_buff *skb, struct net_device *device,
			  struct packet_type *type,
			  struct net_device *original)
{
	struct kobox_linux_net_port *port = container_of(type,
						struct kobox_linux_net_port, packets);
	struct kobox_linux_net_frame *slot;
	unsigned long flags;
	size_t length;

	(void)original;
	if (device != port->device || !skb_mac_header_was_set(skb) ||
	    skb->len > KOBOX_NET_FRAME_MAX - ETH_HLEN)
		goto drop;
	length = ETH_HLEN + skb->len;
	spin_lock_irqsave(&port->rx_lock, flags);
#if KOBOX_BOOT_WIFI
	if (port->wireless && (!port->authorized || skb->protocol == cpu_to_be16(ETH_P_PAE))) {
		port->drops++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		goto release;
	}
#endif
	if (port->count == KOBOX_NET_RX_CAPACITY) {
		port->drops++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		goto release;
	}
	slot = &port->rx[(port->head + port->count) % KOBOX_NET_RX_CAPACITY];
	memcpy(slot->bytes, skb_mac_header(skb), ETH_HLEN);
	if (skb_copy_bits(skb, 0, slot->bytes + ETH_HLEN, skb->len)) {
		port->drops++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		goto release;
	}
	slot->length = length;
#if KOBOX_BOOT_WIFI
	if (port->wireless && !kobox_wifi_data_frame_allowed(slot->bytes, length)) {
		port->drops++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		goto release;
	}
#endif
	port->count++;
	port->sequence++;
	spin_unlock_irqrestore(&port->rx_lock, flags);
	goto release;
drop:
	spin_lock_irqsave(&port->rx_lock, flags);
	port->drops++;
	spin_unlock_irqrestore(&port->rx_lock, flags);
release:
	consume_skb(skb);
	return 0;
}

static void report_open_progress(
	void (*progress)(void *context, unsigned phase,
			 size_t module_index, int status),
	void *context, unsigned phase, int status)
{
	if (progress)
		kobox_host_call((progress(context, phase, 0, status), 0));
}

static void report_open_metric(
	void (*progress)(void *context, unsigned phase,
			 size_t module_index, int status),
	void *context, unsigned phase, size_t value, int status)
{
	if (progress)
		kobox_host_call((progress(context, phase, value, status), 0));
}

int kobox_linux_net_port_open(struct kobox_linux_net_port **out,
			     struct pci_dev *pci,
			     void (*progress)(void *context, unsigned phase,
					      size_t module_index, int status),
			     void *progress_context)
{
	struct kobox_linux_net_port *port;
	struct net_device *device;
	struct kobox_linux_dma_snapshot dma_before = {0}, dma_after;
	struct sysinfo memory_before, memory_after;
	bool have_dma_snapshot;
	int result = -ENODEV;
#if KOBOX_BOOT_WIFI
	unsigned long deadline = jiffies + 30 * HZ;
#endif

	if (!out || !pci)
		return -EINVAL;
	*out = NULL;
	port = vzalloc(sizeof(*port));
	if (!port)
		return -ENOMEM;
	port->rx = vzalloc(sizeof(*port->rx) * KOBOX_NET_RX_CAPACITY);
	if (!port->rx) {
		result = -ENOMEM;
		goto free_port;
	}
	spin_lock_init(&port->rx_lock);
	report_open_progress(progress, progress_context,
			     KOBOX_MODULE_PROGRESS_NET_ALLOCATED, 0);
	report_open_progress(progress, progress_context,
			     KOBOX_MODULE_PROGRESS_NET_RTNL_WAIT, 0);
#if KOBOX_BOOT_WIFI
retry_device:
#endif
	rtnl_lock();
	report_open_progress(progress, progress_context,
			     KOBOX_MODULE_PROGRESS_NET_RTNL_HELD, 0);
	for_each_netdev(&init_net, device) {
		if (!belongs_to_pci_function(device, pci))
			continue;
		if (device->type != ARPHRD_ETHER || device->mtu < 68 ||
		    device->mtu > KOBOX_NET_FRAME_MAX - ETH_HLEN - VLAN_HLEN)
			continue;
		dev_hold(device);
		port->device = device;
#if KOBOX_BOOT_WIFI
		port->wireless = device->ieee80211_ptr != NULL;
#endif
		report_open_progress(progress, progress_context,
				     KOBOX_MODULE_PROGRESS_NET_DEVICE_FOUND, 0);
		report_open_progress(progress, progress_context,
				     KOBOX_MODULE_PROGRESS_NET_DRIVER_OPEN, 0);
		have_dma_snapshot = !kobox_linux_dma_snapshot(&pci->dev,
							      &dma_before);
		si_meminfo(&memory_before);
		result = dev_open(device, NULL);
		report_open_progress(progress, progress_context,
				     KOBOX_MODULE_PROGRESS_NET_DRIVER_OPENED, result);
		if (result) {
			/* This snapshot survives the driver's own failed-open unwind.
			 * Do not infer ENOMEM from host RAM alone: failed IOMMU
			 * publication and Linux page allocation have different owners. */
			if (have_dma_snapshot &&
			    !kobox_linux_dma_snapshot(&pci->dev, &dma_after)) {
				report_open_metric(progress, progress_context,
					KOBOX_MODULE_PROGRESS_NET_DMA_MAPS,
					dma_after.attempts - dma_before.attempts,
					(int)(dma_after.published - dma_before.published));
				report_open_metric(progress, progress_context,
					KOBOX_MODULE_PROGRESS_NET_DMA_FAILURE,
					dma_after.failures - dma_before.failures,
					dma_after.last_error);
				if (dma_after.failures != dma_before.failures)
					report_open_metric(progress, progress_context,
						KOBOX_MODULE_PROGRESS_NET_DMA_FAILURE_DETAIL,
						dma_after.last_failure_bytes,
						dma_after.last_error);
			}
			si_meminfo(&memory_after);
			report_open_metric(progress, progress_context,
				KOBOX_MODULE_PROGRESS_NET_FREE_PAGES,
				memory_after.freeram,
				(int)memory_before.freeram);
		}
		break;
	}
	rtnl_unlock();
#if KOBOX_BOOT_WIFI
	/* iwlwifi firmware completion can register the netdev after PCI probe
	 * returned. Wait for that actual object; never substitute a dummy NIC. */
	if (!port->device && time_before(jiffies, deadline)) {
		schedule_timeout_uninterruptible(1);
		goto retry_device;
	}
#endif
	if (result)
		goto put_device;
	port->packets.type = cpu_to_be16(ETH_P_ALL);
	port->packets.dev = port->device;
	port->packets.func = receive_packet;
	dev_add_pack(&port->packets);
	report_open_progress(progress, progress_context,
			     KOBOX_MODULE_PROGRESS_NET_PACKET_ATTACHED, 0);
	*out = port;
	return 0;
put_device:
	if (port->device)
		dev_put(port->device);
	vfree(port->rx);
free_port:
	vfree(port);
	return result;
}

void kobox_linux_net_port_close(struct kobox_linux_net_port *port)
{
	if (!port)
		return;
	dev_remove_pack(&port->packets);
	synchronize_net();
	rtnl_lock();
	dev_close(port->device);
	rtnl_unlock();
	dev_put(port->device);
	vfree(port->rx);
	vfree(port);
}

int kobox_linux_net_port_info(struct kobox_linux_net_port *port,
			     struct kobox_linux_net_info *info)
{
	unsigned long flags;

	if (!port || !info)
		return -EINVAL;
	memset(info, 0, sizeof(*info));
#if KOBOX_BOOT_WIFI
	if (port->wifi) {
		int error = kobox_linux_wifi_service_poll_events(port->wifi);

		if (error)
			return error;
	}
#endif
	spin_lock_irqsave(&port->rx_lock, flags);
	info->sequence = port->sequence;
	info->rx_drops = port->drops;
#if KOBOX_BOOT_WIFI
	info->association_sequence = port->association_sequence;
#endif
	spin_unlock_irqrestore(&port->rx_lock, flags);
	info->mtu = READ_ONCE(port->device->mtu);
	memcpy(info->mac, port->device->dev_addr, ETH_ALEN);
	info->carrier = netif_carrier_ok(port->device);
#if KOBOX_BOOT_WIFI
	if (port->wireless && !READ_ONCE(port->authorized))
		info->carrier = 0;
#endif
	return 0;
}

int kobox_linux_net_port_read(struct kobox_linux_net_port *port,
			     struct kobox_linux_net_frame *frames,
			     size_t capacity, size_t *count)
{
	unsigned long flags;
	size_t done = 0;

	if (!port || !frames || !count || !capacity || capacity > KOBOX_NET_RX_CAPACITY)
		return -EINVAL;
	while (done < capacity) {
		spin_lock_irqsave(&port->rx_lock, flags);
		if (!port->count) {
			spin_unlock_irqrestore(&port->rx_lock, flags);
			break;
		}
		frames[done] = port->rx[port->head];
		port->head = (port->head + 1) % KOBOX_NET_RX_CAPACITY;
		port->count--;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		done++;
	}
	*count = done;
	return 0;
}

int kobox_linux_net_port_write(struct kobox_linux_net_port *port,
			      const void *frame, size_t length)
{
	struct sk_buff *skb;
	unsigned int headroom;
	int result;

	if (!port || !frame || length < ETH_HLEN ||
	    length > KOBOX_NET_FRAME_MAX ||
	    length > READ_ONCE(port->device->mtu) + ETH_HLEN + VLAN_HLEN)
		return -EMSGSIZE;
	if (!netif_running(port->device) || !netif_carrier_ok(port->device))
		return -ENETDOWN;
#if KOBOX_BOOT_WIFI
	if (port->wireless) {
		if (!READ_ONCE(port->authorized))
			return -ENETDOWN;
		if (!kobox_wifi_data_frame_allowed(frame, length))
			return -EPERM;
	}
#endif
	headroom = LL_RESERVED_SPACE(port->device);
	skb = alloc_skb(headroom + length, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, headroom);
	skb_put_data(skb, frame, length);
	skb->dev = port->device;
	skb_reset_mac_header(skb);
	/* dev_queue_xmit() also offers outgoing frames to packet taps, which
	 * expect the network header to point inside this Ethernet skb. */
	skb_set_network_header(skb, ETH_HLEN);
	skb->protocol = ((const struct ethhdr *)frame)->h_proto;
	skb->ip_summed = CHECKSUM_NONE;
	result = dev_queue_xmit(skb);
	return result == NET_XMIT_SUCCESS || result == NET_XMIT_CN ? 0 : -ENOBUFS;
}

int kobox_linux_net_port_name(struct kobox_linux_net_port *port, char *name, size_t capacity)
{
	if (!port || !name || capacity < IFNAMSIZ)
		return -EINVAL;
	rtnl_lock();
	strscpy(name, port->device->name, capacity);
	rtnl_unlock();
	return 0;
}

int kobox_linux_net_port_wifi_bind(struct kobox_linux_net_port *port,
	struct kobox_linux_wifi_service *service)
{
#if KOBOX_BOOT_WIFI
	if (!port || !port->wireless)
		return -EOPNOTSUPP;
	if (service && port->wifi)
		return -EBUSY;
	port->wifi = service;
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

#if KOBOX_BOOT_WIFI
static void update_authorization(struct kobox_linux_net_port *port,
	bool authorized, bool new_association)
{
	unsigned long flags;

	if (!port || !port->wireless)
		return;
	spin_lock_irqsave(&port->rx_lock, flags);
	if (port->authorized != authorized || new_association) {
		WRITE_ONCE(port->authorized, authorized);
		port->association_sequence++;
		/* No frame accepted under an old authentication state may survive
		 * disconnect/reconfigure and leak into a later connection. */
		port->drops += port->count;
		port->head = port->count = 0;
		port->sequence++;
	}
	spin_unlock_irqrestore(&port->rx_lock, flags);
}
#endif

void kobox_linux_net_port_wifi_authorize(struct kobox_linux_net_port *port, bool authorized)
{
#if KOBOX_BOOT_WIFI
	update_authorization(port, authorized, false);
#endif
}

void kobox_linux_net_port_wifi_association(struct kobox_linux_net_port *port)
{
#if KOBOX_BOOT_WIFI
	/* CONNECTED while already authorized is still a new association. Close
	 * and flush before an asynchronous STATUS can prove the new state. */
	update_authorization(port, false, true);
#endif
}

__attribute__((visibility("default")))
int kobox_linux_net_wifi_request(struct kobox_linux_net_port *port,
	const void *request, size_t length, void *reply, size_t capacity, size_t *received)
{
#if KOBOX_BOOT_WIFI
	if (port && port->wifi)
		return kobox_linux_wifi_service_request(port->wifi, request, length, reply, capacity, received);
#endif
	return -EOPNOTSUPP;
}

__attribute__((visibility("default")))
int kobox_linux_net_wifi_configure(struct kobox_linux_net_port *port,
	const void *config, size_t length)
{
#if KOBOX_BOOT_WIFI
	if (port && port->wifi)
		return kobox_linux_wifi_service_configure(port->wifi, config, length);
#endif
	return -EOPNOTSUPP;
}

__attribute__((visibility("default")))
int kobox_linux_net_wifi_events(struct kobox_linux_net_port *port, u32 *events)
{
#if KOBOX_BOOT_WIFI
	if (port && port->wifi)
		return kobox_linux_wifi_service_events(port->wifi, events);
#endif
	return -EOPNOTSUPP;
}
