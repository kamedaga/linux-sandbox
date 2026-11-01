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
#include <linux/workqueue.h>
#include <net/net_namespace.h>
#if KOBOX_BOOT_WIFI
#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "kobox_net_port"
#endif
#include <net/cfg80211.h>
#endif

#include "net_perf.h"
#ifndef KOBOX_NET_PERF_TRACE
#define KOBOX_NET_PERF_TRACE 0
#endif
#define KOBOX_NET_RX_CAPACITY 128U
#ifndef KOBOX_NET_RX_CONTROL
#define KOBOX_NET_RX_CONTROL 0
#endif

struct kobox_linux_net_port {
	struct net_device *device;
	struct packet_type packets;
	spinlock_t rx_lock;
	struct kobox_linux_net_frame *rx;
	uint32_t head, count;
	uint64_t sequence, drops;
	struct kobox_linux_net_rx_stats stats;
#if KOBOX_NET_PERF_TRACE
 struct { u64 key, irq_hint, tap, queued, dequeued, tx_begin, tx_end; struct kobox_net_tx_cost cost; unsigned scoped; } trace[32];
 unsigned trace_count;
 bool trace_enabled;
 u8 trace_slot[KOBOX_NET_RX_CAPACITY];
#endif
	struct work_struct rx_work;
	int (*wake)(void *);
	void *wake_context;
	netdev_features_t saved_wanted;
	bool features_changed;
#if KOBOX_BOOT_WIFI
	struct kobox_linux_wifi_service *wifi;
	u64 association_sequence;
	bool wireless, authorized;
#endif
};

#if KOBOX_NET_PERF_TRACE
/* One sampled synchronous TX owns the scope. Task identity survives Linux
 * scheduling/migration. BH-disabled and serving-softirq calls retain the
 * owner identity; in_interrupt() cannot distinguish BH-disable from IRQs. No diagnostic lock is held across driver or host calls. */
static DEFINE_RAW_SPINLOCK(tx_probe_lock);
static struct task_struct *tx_probe_owner;
static struct kobox_net_tx_cost tx_probe_cost;
static bool tx_diag_enabled;
static u64 tx_diag_frames, tx_diag_samples, tx_diag_first_owner, tx_diag_wall, tx_scope_stamp;
static char tx_diag_owner_comm[TASK_COMM_LEN];
static u64 tx_mmio_total[2], tx_mmio_owner[2], tx_mmio_bh[2];
static struct kobox_net_tx_cost tx_diag_aggregate;
static struct { u64 tsc, task, peer, state; unsigned kind; } tx_events[8];
static unsigned tx_event_count;
static struct { u64 task, pc, first, writes; unsigned context; char comm[TASK_COMM_LEN]; } tx_writers[4];
int kobox_net_tx_probe_skip_lock(const void *lock) { return lock == &tx_probe_lock; }
void kobox_net_tx_event(unsigned kind, const void *peer, unsigned long state)
{
 unsigned long flags;
 if (!READ_ONCE(tx_diag_enabled) || !READ_ONCE(tx_probe_owner)) return;
 raw_spin_lock_irqsave(&tx_probe_lock,flags);
 if (tx_event_count < ARRAY_SIZE(tx_events)) {
  unsigned i=tx_event_count++;
  tx_events[i].tsc=kobox_net_perf_tsc(); tx_events[i].kind=kind;
  tx_events[i].task=(u64)current; tx_events[i].peer=(u64)peer; tx_events[i].state=state;
 }
 raw_spin_unlock_irqrestore(&tx_probe_lock,flags);
}
void kobox_net_mmio_total(unsigned write, unsigned long long pc)
{
 unsigned long flags; unsigned d=!!write;
 if (!READ_ONCE(tx_diag_enabled)) return;
 raw_spin_lock_irqsave(&tx_probe_lock,flags);
 tx_mmio_total[d]++;
 if (current == tx_probe_owner) tx_mmio_owner[d]++;
 if (softirq_count()) tx_mmio_bh[d]++;
 if (write) for (unsigned i=0;i<ARRAY_SIZE(tx_writers);i++) {
  if (tx_writers[i].task && tx_writers[i].task != (u64)current) continue;
  if (!tx_writers[i].task) {
   tx_writers[i].task=(u64)current; tx_writers[i].pc=pc;
   tx_writers[i].first=kobox_net_perf_tsc(); tx_writers[i].context=preempt_count();
   memcpy(tx_writers[i].comm,current->comm,TASK_COMM_LEN);
  }
  tx_writers[i].writes++; break;
 }
 raw_spin_unlock_irqrestore(&tx_probe_lock,flags);
}
unsigned long long kobox_net_tx_probe_begin(void)
{
 if (READ_ONCE(tx_probe_owner) != current) return 0;
 return kobox_net_perf_tsc();
}
void kobox_net_tx_probe_end_at(unsigned part, unsigned long long begin, unsigned long long pc)
{
 unsigned long flags;
 if (!begin || part >= KOBOX_TX_PARTS) return;
 u64 end = kobox_net_perf_tsc();
 raw_spin_lock_irqsave(&tx_probe_lock, flags);
 if (tx_probe_owner == current) {
  tx_probe_cost.cycles[part] += end - begin;
  tx_probe_cost.calls[part]++;
  if (end-begin > tx_probe_cost.max_cycles) {
   tx_probe_cost.max_cycles=end-begin; tx_probe_cost.max_pc=pc;
   tx_probe_cost.max_part=part;
  }
 }
 raw_spin_unlock_irqrestore(&tx_probe_lock, flags);
}
static bool tx_probe_start(void)
{
 unsigned long flags; bool active = false;
 raw_spin_lock_irqsave(&tx_probe_lock, flags);
 if (!tx_probe_owner) {
  memset(&tx_probe_cost, 0, sizeof(tx_probe_cost));
  WRITE_ONCE(tx_probe_owner, current); active = true;
  tx_diag_samples++;tx_scope_stamp=kobox_net_perf_tsc();
  if (!tx_diag_first_owner) { tx_diag_first_owner=(u64)current;memcpy(tx_diag_owner_comm,current->comm,TASK_COMM_LEN); }
 }
 raw_spin_unlock_irqrestore(&tx_probe_lock, flags);
 return active;
}
static struct kobox_net_tx_cost tx_probe_stop(void)
{
 unsigned long flags; struct kobox_net_tx_cost cost;
 raw_spin_lock_irqsave(&tx_probe_lock, flags);
 cost = tx_probe_cost;tx_diag_wall+=kobox_net_perf_tsc()-tx_scope_stamp;
 for (unsigned i=0;i<KOBOX_TX_PARTS;i++) {
  tx_diag_aggregate.cycles[i]+=cost.cycles[i]; tx_diag_aggregate.calls[i]+=cost.calls[i];
 }
 WRITE_ONCE(tx_probe_owner, NULL);
 raw_spin_unlock_irqrestore(&tx_probe_lock, flags);
 return cost;
}
static u64 last_irq_dispatch;
void kobox_net_perf_irq_mark(void) { WRITE_ONCE(last_irq_dispatch, kobox_net_perf_tsc()); }
#endif
/* Opening-task diagnostic control; counters/rows live under rx_lock. Snapshot
 * is allowed only after stop, so incomplete rows are visible as zero fields. */
int kobox_linux_net_port_perf_request(struct kobox_linux_net_port *port,
 const void *request, size_t length, void *reply, size_t capacity, size_t *received)
{
#if KOBOX_NET_PERF_TRACE
 if(!port||!request||!reply||!received||capacity<3)return -EINVAL;
 *received=0;
 unsigned long flags; int size=0;
 if(length==20 && !memcmp(request,"NET_PERF_TRACE_START",20)) {
  spin_lock_irqsave(&port->rx_lock,flags);
  memset(port->trace,0,sizeof(port->trace));memset(port->trace_slot,255,sizeof(port->trace_slot));
  port->trace_count=0;port->trace_enabled=true;
  raw_spin_lock(&tx_probe_lock);
  memset(tx_mmio_total,0,sizeof(tx_mmio_total));memset(tx_mmio_owner,0,sizeof(tx_mmio_owner));
  memset(tx_mmio_bh,0,sizeof(tx_mmio_bh));memset(&tx_diag_aggregate,0,sizeof(tx_diag_aggregate));
  memset(tx_writers,0,sizeof(tx_writers));tx_event_count=0;tx_diag_frames=0;tx_diag_samples=0;tx_diag_first_owner=0;tx_diag_wall=0;WRITE_ONCE(tx_diag_enabled,true);
  raw_spin_unlock(&tx_probe_lock);spin_unlock_irqrestore(&port->rx_lock,flags);
 } else if(length==19 && !memcmp(request,"NET_PERF_TRACE_STOP",19)) {
  spin_lock_irqsave(&port->rx_lock,flags);port->trace_enabled=false;WRITE_ONCE(tx_diag_enabled,false);spin_unlock_irqrestore(&port->rx_lock,flags);
 } else if(length==19 && !memcmp(request,"NET_PERF_TRACE_DUMP",19)) {
  spin_lock_irqsave(&port->rx_lock,flags);
  if(port->trace_enabled){spin_unlock_irqrestore(&port->rx_lock,flags);return -EBUSY;}
  for(unsigned i=0;i<port->trace_count;i++){
   int n=scnprintf((char *)reply+size,capacity-size,
    "core key=%llx irq_hint=%llu tap=%llu queued=%llu dequeued=%llu tx_begin=%llu tx_end=%llu mr=%llu mw=%llu dm=%llu du=%llu ds=%llu nr=%u nw=%u nm=%u nu=%u ns=%u scoped=%u slow=%u pc=%llx\n",
    port->trace[i].key,port->trace[i].irq_hint,port->trace[i].tap,port->trace[i].queued,
    port->trace[i].dequeued,port->trace[i].tx_begin,port->trace[i].tx_end,
    port->trace[i].cost.cycles[0],port->trace[i].cost.cycles[1],
    port->trace[i].cost.cycles[2],port->trace[i].cost.cycles[3],port->trace[i].cost.cycles[4],
    port->trace[i].cost.calls[0],port->trace[i].cost.calls[1],port->trace[i].cost.calls[2],
    port->trace[i].cost.calls[3],port->trace[i].cost.calls[4],port->trace[i].scoped,port->trace[i].cost.max_part,port->trace[i].cost.max_pc);
   if(n<=0||(size_t)n>=capacity-size-1){spin_unlock_irqrestore(&port->rx_lock,flags);return -ENOSPC;}size+=n;
  }
  /* Fixed maxima: eight core rows, eighteen aggregate parts, eight events,
   * four writers. Stop first; same service serializes TX and control RPCs. */
  raw_spin_lock(&tx_probe_lock);
  size+=scnprintf((char *)reply+size,capacity-size,
   "mmio read=%llu write=%llu owner_read=%llu owner_write=%llu bh_read=%llu bh_write=%llu\n",
   tx_mmio_total[0],tx_mmio_total[1],tx_mmio_owner[0],tx_mmio_owner[1],tx_mmio_bh[0],tx_mmio_bh[1]);
  size+=scnprintf((char *)reply+size,capacity-size,"txscope frames=%llu samples=%llu wall=%llu task=%llx comm=%.*s\n",
   tx_diag_frames,tx_diag_samples,tx_diag_wall,tx_diag_first_owner,TASK_COMM_LEN,tx_diag_owner_comm);
  for(unsigned i=0;i<KOBOX_TX_PARTS;i++)
   size+=scnprintf((char *)reply+size,capacity-size,"txpart id=%u calls=%u cycles=%llu\n",i,tx_diag_aggregate.calls[i],tx_diag_aggregate.cycles[i]);
  for(unsigned i=0;i<tx_event_count;i++)
   size+=scnprintf((char *)reply+size,capacity-size,"txevent kind=%u tsc=%llu task=%llx peer=%llx state=%llx\n",
    tx_events[i].kind,tx_events[i].tsc,tx_events[i].task,tx_events[i].peer,tx_events[i].state);
  for(unsigned i=0;i<ARRAY_SIZE(tx_writers);i++) if(tx_writers[i].task)
   size+=scnprintf((char *)reply+size,capacity-size,"txwriter task=%llx pc=%llx first=%llu writes=%llu context=%x comm=%.*s\n",
    tx_writers[i].task,tx_writers[i].pc,tx_writers[i].first,tx_writers[i].writes,tx_writers[i].context,TASK_COMM_LEN,tx_writers[i].comm);
  raw_spin_unlock(&tx_probe_lock);
  if(size>=capacity-1){spin_unlock_irqrestore(&port->rx_lock,flags);return -ENOSPC;}
  spin_unlock_irqrestore(&port->rx_lock,flags);*received=size;return 1;
 } else return 0;
 if(capacity<3)return -ENOSPC;memcpy(reply,"OK\n",3);*received=3;return 1;
#else
 (void)port;(void)request;(void)length;(void)reply;(void)capacity;(void)received;return 0;
#endif
}

static void rx_wake_work(struct work_struct *work)
{
	struct kobox_linux_net_port *port = container_of(work,
		struct kobox_linux_net_port, rx_work);
	int (*wake)(void *);
	void *context;
	unsigned long flags;
	int result = 0;

	spin_lock_irqsave(&port->rx_lock, flags);
	wake = port->wake;
	context = port->wake_context;
	spin_unlock_irqrestore(&port->rx_lock, flags);
	/* Never enter the native provider while holding an RX/IRQ lock. Unbind
	 * clears admission then joins this work before closing its capability. */
	if (wake)
		result = kobox_host_call(wake(context));
	spin_lock_irqsave(&port->rx_lock, flags);
	if (wake) {
		port->stats.wake_writes++;
		if (result)
			port->stats.wake_errors++;
	}
	spin_unlock_irqrestore(&port->rx_lock, flags);
}

int kobox_linux_net_port_bind_wake(struct kobox_linux_net_port *port,
	int (*wake)(void *), void *context)
{
	unsigned long flags;

	if (!port || (!wake && context))
		return -EINVAL;
	/* Lifecycle owner only: serialize bind/unbind with device close. */
	spin_lock_irqsave(&port->rx_lock, flags);
	port->wake = NULL;
	port->wake_context = NULL;
	spin_unlock_irqrestore(&port->rx_lock, flags);
	cancel_work_sync(&port->rx_work);
	spin_lock_irqsave(&port->rx_lock, flags);
	port->wake = wake;
	port->wake_context = context;
	/* Bind after traffic began must also publish the already nonempty queue. */
	if (!KOBOX_NET_RX_CONTROL && wake && port->count)
		schedule_work(&port->rx_work);
	spin_unlock_irqrestore(&port->rx_lock, flags);
	return 0;
}

int kobox_linux_net_port_rx_stats(struct kobox_linux_net_port *port,
	struct kobox_linux_net_rx_stats *stats)
{
	unsigned long flags;
	if (!port || !stats)
		return -EINVAL;
	spin_lock_irqsave(&port->rx_lock, flags);
	*stats = port->stats;
	stats->queued = port->count;
	spin_unlock_irqrestore(&port->rx_lock, flags);
#if KOBOX_BOOT_WIFI
	/* Device ownership pins wdev until port close. This is a read-only policy
	 * observation, not a firmware awake/asleep measurement or a cfg80211 call. */
	{
		struct wireless_dev *wdev = READ_ONCE(port->device->ieee80211_ptr);
		stats->wireless = wdev != NULL;
		stats->power_save = wdev ? READ_ONCE(wdev->ps) : 0;
	}
#endif
	return 0;
}

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

#if KOBOX_NET_PERF_TRACE
 u64 tap_irq_hint=READ_ONCE(last_irq_dispatch);
 u64 tap_tsc=READ_ONCE(port->trace_enabled)?kobox_net_perf_tsc():0;
#endif
	(void)original;
	/* TX taps still carry their Ethernet header in skb->data. They are not
	 * ingress, and reflecting them into the L3-facing copy path duplicates
	 * that header and can fill RX with our own ACKs. */
	if (!KOBOX_NET_RX_CONTROL && skb->pkt_type == PACKET_OUTGOING)
		goto release;
	if (device != port->device || !skb_mac_header_was_set(skb))
		goto drop;
	length = ETH_HLEN + skb->len;
	spin_lock_irqsave(&port->rx_lock, flags);
	if (skb_is_gso(skb)) {
		port->stats.gso_seen++;
		/* An aggregate carries checksum/offload metadata which our Ethernet
		 * copy protocol cannot represent, even if it fits in 2048 bytes. */
		if (!KOBOX_NET_RX_CONTROL) {
			port->stats.drop_gso++;
			port->drops++;
			spin_unlock_irqrestore(&port->rx_lock, flags);
			goto release;
		}
	}
	if (skb->len > KOBOX_NET_FRAME_MAX - ETH_HLEN) {
		port->stats.drop_oversize++;
		port->drops++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		goto release;
	}
#if KOBOX_BOOT_WIFI
	if (port->wireless && (!port->authorized || skb->protocol == cpu_to_be16(ETH_P_PAE))) {
		port->stats.drop_policy++;
		port->drops++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		goto release;
	}
#endif
	if (port->count == KOBOX_NET_RX_CAPACITY) {
		port->stats.drop_full++;
		port->drops++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		goto release;
	}
	slot = &port->rx[(port->head + port->count) % KOBOX_NET_RX_CAPACITY];
	memcpy(slot->bytes, skb_mac_header(skb), ETH_HLEN);
	if (skb_copy_bits(skb, 0, slot->bytes + ETH_HLEN, skb->len)) {
		port->stats.drop_copy++;
		port->drops++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		goto release;
	}
	slot->length = length;
#if KOBOX_BOOT_WIFI
	if (port->wireless && !kobox_wifi_data_frame_allowed(slot->bytes, length)) {
		port->stats.drop_policy++;
		port->drops++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		goto release;
	}
#endif
#if KOBOX_NET_PERF_TRACE
 {
  unsigned index=(port->head+port->count)%KOBOX_NET_RX_CAPACITY;
  port->trace_slot[index]=255;
  if(port->trace_enabled && port->trace_count<8){
   u64 key=kobox_net_perf_key(slot->bytes,length);
   if(!(key&63)){
    unsigned sample=port->trace_count++;port->trace_slot[index]=sample;
    port->trace[sample].key=key;port->trace[sample].irq_hint=tap_irq_hint;
    port->trace[sample].tap=tap_tsc;port->trace[sample].queued=kobox_net_perf_tsc();
   }
  }
 }
#endif
	port->count++;
	port->sequence++;
	port->stats.packets++;
	port->stats.bytes += length;
	if (port->count > port->stats.high_water)
		port->stats.high_water = port->count;
	if (port->count == 1) {
		port->stats.wake_edges++;
		if (!KOBOX_NET_RX_CONTROL && port->wake)
			schedule_work(&port->rx_work);
	}
	spin_unlock_irqrestore(&port->rx_lock, flags);
	goto release;
drop:
	spin_lock_irqsave(&port->rx_lock, flags);
	port->stats.drop_oversize++;
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

static int require_standalone_frames(struct kobox_linux_net_port *port,
	struct net_device *device)
{
	port->saved_wanted = device->wanted_features;
	port->stats.features_before = device->features;
	port->stats.measurement_control = KOBOX_NET_RX_CONTROL;
	/* A separately built measurement control preserves aggregation and the
	 * timer path, to collect the missing before-counters without guessing. */
	if (KOBOX_NET_RX_CONTROL) {
		port->stats.features_active = device->features;
		return 0;
	}
	device->wanted_features &= ~(NETIF_F_GRO | NETIF_F_GRO_HW | NETIF_F_LRO);
	netdev_update_features(device);
	port->features_changed = true;
	port->stats.features_active = device->features;
	return device->features & (NETIF_F_GRO | NETIF_F_GRO_HW | NETIF_F_LRO) ?
		-EOPNOTSUPP : 0;
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
	INIT_WORK(&port->rx_work, rx_wake_work);
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
		if (!result) {
			/* Apply after ndo_open, which may reset features, and before the
			 * packet tap can accept any aggregate. The transport has neither
			 * GRO/LRO metadata nor checksum-offload metadata. */
			result = require_standalone_frames(port, device);
			if (result)
				dev_close(device);
		}
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
	port->packets.ignore_outgoing = !KOBOX_NET_RX_CONTROL;
	port->packets.func = receive_packet;
	dev_add_pack(&port->packets);
	report_open_progress(progress, progress_context,
			     KOBOX_MODULE_PROGRESS_NET_PACKET_ATTACHED, 0);
	*out = port;
	return 0;
put_device:
	if (port->features_changed) {
		rtnl_lock();
		port->device->wanted_features = port->saved_wanted;
		netdev_update_features(port->device);
		rtnl_unlock();
	}
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
	kobox_linux_net_port_bind_wake(port, NULL, NULL);
	rtnl_lock();
	dev_close(port->device);
	if (port->features_changed) {
		port->device->wanted_features = port->saved_wanted;
		netdev_update_features(port->device);
	}
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
#if KOBOX_NET_PERF_TRACE
  if(port->trace_slot[port->head]<port->trace_count)
   port->trace[port->trace_slot[port->head]].dequeued=kobox_net_perf_tsc();
  port->trace_slot[port->head]=255;
#endif
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
#if KOBOX_NET_PERF_TRACE
 if (READ_ONCE(port->trace_enabled)) __atomic_fetch_add(&tx_diag_frames,1,__ATOMIC_RELAXED);
 u64 key=READ_ONCE(port->trace_enabled)?kobox_net_perf_key(frame,length):0;
 bool probing=READ_ONCE(port->trace_enabled) && !(key&63) && tx_probe_start();
 u64 tx_begin=READ_ONCE(port->trace_enabled)?kobox_net_perf_tsc():0;
#endif
	result = dev_queue_xmit(skb);
#if KOBOX_NET_PERF_TRACE
 u64 tx_end=tx_begin?kobox_net_perf_tsc():0;
 struct kobox_net_tx_cost cost = {0};
 if(probing) cost=tx_probe_stop();
 if(tx_begin){
  u64 end=tx_end;unsigned long f;
  spin_lock_irqsave(&port->rx_lock,f);
  if(port->trace_enabled && !(key&63) && port->trace_count<8){unsigned i=port->trace_count++;
   port->trace[i].key=key;port->trace[i].tx_begin=tx_begin;port->trace[i].tx_end=end;port->trace[i].cost=cost;port->trace[i].scoped=probing;}
  spin_unlock_irqrestore(&port->rx_lock,f);
 }
#endif
	{
		unsigned long flags;
		spin_lock_irqsave(&port->rx_lock, flags);
		port->stats.tx_packets++;
		if (result != NET_XMIT_SUCCESS && result != NET_XMIT_CN)
			port->stats.tx_errors++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
	}
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
