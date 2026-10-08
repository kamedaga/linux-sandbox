// SPDX-License-Identifier: GPL-2.0-only
#include "input_port.h"
#include "diagnostic.h"
#include "../arch/x86_64/host_call.h"
#include "../task/time_port.h"

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/bitmap.h>
#include <linux/input.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define KOBOX_INPUT_RECORD_CAPACITY 1024U
#define KOBOX_INPUT_READ_BATCH_MAX 64U

struct kobox_linux_input_source {
	struct input_handle handle;
	struct list_head node;
	struct kobox_linux_input_device_info info;
	struct kobox_linux_input_port *port;
};

struct kobox_linux_input_port {
	struct input_handler handler;
	spinlock_t lock;
	struct list_head sources;
	struct kobox_linux_input_record *records;
	size_t head, count;
	uint32_t next_id;
	uint64_t next_sequence, overwritten;
	struct kobox_linux_input_notify notify;
	int notify_error;
};

static const struct input_device_id usb_input_ids[] = {
	{ .flags = INPUT_DEVICE_ID_MATCH_BUS, .bustype = BUS_USB },
	{ },
};

/* The lock orders ring publication and notification. A full bounded read
 * re-notifies remaining data, so the host may yield without losing an edge. */
static void notify_records(struct kobox_linux_input_port *port)
{
	int result;

	if (!port->count || !port->notify.notify || port->notify_error)
		return;
	result = kobox_host_call(port->notify.notify(port->notify.context));
	if (result)
		port->notify_error = result;
}

/* Called with port->lock. Losing the oldest record is explicit in both the
 * cumulative counter and the sequence gap; consumers must resnapshot. */
static void append_record(struct kobox_linux_input_port *port,
			  struct kobox_linux_input_record *record)
{
	const struct kobox_linux_task_host_operations *host = kobox_task_host();
	size_t tail;

	record->sequence = port->next_sequence++;
	/* Linux timekeeping starts at this sandbox's boot, not at host boot.
	 * evdev consumers compare these timestamps with their host monotonic
	 * clock (for debounce/gesture deadlines), so publish that clock domain
	 * at capture rather than guessing an offset later at the receiver. */
	BUG_ON(!host || kobox_host_call(host->monotonic_ns(&record->monotonic_ns)));
	if (port->count == KOBOX_INPUT_RECORD_CAPACITY) {
		port->head = (port->head + 1) % KOBOX_INPUT_RECORD_CAPACITY;
		port->count--;
		port->overwritten++;
	}
	tail = (port->head + port->count) % KOBOX_INPUT_RECORD_CAPACITY;
	port->records[tail] = *record;
	port->count++;
	if (port->count == 1)
		notify_records(port);
}

static unsigned int on_events(struct input_handle *handle,
			      struct input_value *values, unsigned int count)
{
	struct kobox_linux_input_source *source = handle->private;
	struct kobox_linux_input_port *port = source->port;
	unsigned long flags;
	unsigned int index;

	spin_lock_irqsave(&port->lock, flags);
	for (index = 0; index < count; index++) {
		const unsigned int type = values[index].type;
		const unsigned int code = values[index].code;
		const int value = values[index].value;
		struct kobox_linux_input_record record = {
			.device_id = source->info.id,
			.kind = KOBOX_INPUT_EVENT,
			.type = type,
			.code = code,
			.value = value,
		};
		/* This state accompanies the snapshot after a sequence gap. Keep
		 * it under the same lock that orders event records. */
		if (type == EV_KEY && code < KEY_CNT) {
			if (value)
				source->info.key_state[code / 64] |= 1ULL << (code % 64);
			else
				source->info.key_state[code / 64] &= ~(1ULL << (code % 64));
		} else if (type == EV_ABS && code < ABS_CNT) {
			source->info.abs_value[code] = value;
		} else if (type == EV_LED && code < LED_CNT) {
			if (value)
				source->info.led_state |= 1ULL << code;
			else
				source->info.led_state &= ~(1ULL << code);
		} else if (type == EV_SW && code < SW_CNT) {
			if (value)
				source->info.sw_state |= 1ULL << code;
			else
				source->info.sw_state &= ~(1ULL << code);
		}
		append_record(port, &record);
	}
	spin_unlock_irqrestore(&port->lock, flags);
	return count;
}

static int connect_source(struct input_handler *handler, struct input_dev *dev,
			  const struct input_device_id *id)
{
	struct kobox_linux_input_port *port = handler->private;
	struct kobox_linux_input_source *source;
	struct kobox_linux_input_record record = { .kind = KOBOX_INPUT_DEVICE_ADD };
	unsigned long flags;
	int result;

	(void)id;
	source = kzalloc(sizeof(*source), GFP_KERNEL);
	if (!source)
		return -ENOMEM;
	source->port = port;
	source->handle.dev = dev;
	source->handle.handler = handler;
	source->handle.name = "kobox-input";
	source->handle.private = source;
	source->info.bus = dev->id.bustype;
	source->info.vendor = dev->id.vendor;
	source->info.product = dev->id.product;
	source->info.version = dev->id.version;
	strscpy(source->info.name, dev->name ?: "", sizeof(source->info.name));
	strscpy(source->info.phys, dev->phys ?: "", sizeof(source->info.phys));
	strscpy(source->info.uniq, dev->uniq ?: "", sizeof(source->info.uniq));
	source->info.property_bits = dev->propbit[0];
	source->info.event_bits = dev->evbit[0];
	bitmap_copy((unsigned long *)source->info.key_bits, dev->keybit, KEY_CNT);
	bitmap_copy((unsigned long *)source->info.key_state, dev->key, KEY_CNT);
	source->info.rel_bits = dev->relbit[0];
	source->info.abs_bits = dev->absbit[0];
	source->info.msc_bits = dev->mscbit[0];
	source->info.led_bits = dev->ledbit[0];
	source->info.snd_bits = dev->sndbit[0];
	source->info.sw_bits = dev->swbit[0];
	source->info.led_state = dev->led[0];
	source->info.snd_state = dev->snd[0];
	source->info.sw_state = dev->sw[0];
	if (dev->absinfo)
		for (unsigned int axis = 0; axis < ABS_CNT; axis++)
			source->info.abs_value[axis] = dev->absinfo[axis].value;
	result = input_register_handle(&source->handle);
	if (result)
		goto free_source;
	spin_lock_irqsave(&port->lock, flags);
	if (!port->next_id) {
		spin_unlock_irqrestore(&port->lock, flags);
		result = -ENOSPC;
		goto unregister_handle;
	}
	source->info.id = port->next_id++;
	list_add_tail(&source->node, &port->sources);
	record.device_id = source->info.id;
	append_record(port, &record);
	spin_unlock_irqrestore(&port->lock, flags);
	result = input_open_device(&source->handle);
	if (!result) {
		/* Keep device arrival visible until the event service is connected. */
		kobox_linux_boot_diagnostic("kobox input: attached id=%u bus=%04x name=%s\n",
			source->info.id, source->info.bus, source->info.name);
		return 0;
	}
	spin_lock_irqsave(&port->lock, flags);
	list_del(&source->node);
	record.kind = KOBOX_INPUT_DEVICE_REMOVE;
	append_record(port, &record);
	spin_unlock_irqrestore(&port->lock, flags);
unregister_handle:
	input_unregister_handle(&source->handle);
free_source:
	kfree(source);
	return result;
}

static void disconnect_source(struct input_handle *handle)
{
	struct kobox_linux_input_source *source = handle->private;
	struct kobox_linux_input_port *port = source->port;
	struct kobox_linux_input_record record = {
		.device_id = source->info.id,
		.kind = KOBOX_INPUT_DEVICE_REMOVE,
	};
	unsigned long flags;

	input_close_device(handle);
	spin_lock_irqsave(&port->lock, flags);
	list_del(&source->node);
	append_record(port, &record);
	spin_unlock_irqrestore(&port->lock, flags);
	input_unregister_handle(handle);
	kobox_linux_boot_diagnostic("kobox input: detached id=%u name=%s\n",
		source->info.id, source->info.name);
	kfree(source);
}

int kobox_linux_input_port_open(struct kobox_linux_input_port **out)
{
	struct kobox_linux_input_port *port;
	int result;

	if (!out || *out)
		return -EINVAL;
	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	port->records = kvcalloc(KOBOX_INPUT_RECORD_CAPACITY,
				 sizeof(*port->records), GFP_KERNEL);
	if (!port->records) {
		kfree(port);
		return -ENOMEM;
	}
	spin_lock_init(&port->lock);
	INIT_LIST_HEAD(&port->sources);
	port->next_id = 1;
	port->next_sequence = 1;
	port->handler.private = port;
	port->handler.events = on_events;
	port->handler.connect = connect_source;
	port->handler.disconnect = disconnect_source;
	port->handler.name = "kobox-usb-input";
	port->handler.id_table = usb_input_ids;
	result = input_register_handler(&port->handler);
	if (result) {
		kvfree(port->records);
		kfree(port);
		return result;
	}
	*out = port;
	return 0;
}

void kobox_linux_input_port_close(struct kobox_linux_input_port *port)
{
	if (!port)
		return;
	input_unregister_handler(&port->handler);
	kvfree(port->records);
	kfree(port);
}

/* Binding is single-owner and includes pending arrivals. No input can slip
 * between this initial recheck and later empty-to-nonempty notifications. */
int kobox_linux_input_port_bind_notify(struct kobox_linux_input_port *port,
			 const struct kobox_linux_input_notify *notify)
{
	unsigned long flags;
	int result;

	if (!port || !notify || !notify->notify)
		return -EINVAL;
	spin_lock_irqsave(&port->lock, flags);
	if (port->notify.notify) {
		spin_unlock_irqrestore(&port->lock, flags);
		return -EBUSY;
	}
	port->notify = *notify;
	notify_records(port);
	result = port->notify_error;
	spin_unlock_irqrestore(&port->lock, flags);
	return result;
}

int kobox_linux_input_port_read(struct kobox_linux_input_port *port,
			      struct kobox_linux_input_record *records,
			      size_t capacity, size_t *count,
			      uint64_t *overwritten)
{
	unsigned long flags;
	size_t index, available;
	int result;

	if (!port || !records || !capacity || !count || !overwritten)
		return -EINVAL;
	spin_lock_irqsave(&port->lock, flags);
	available = min3(capacity, port->count,
			 (size_t)KOBOX_INPUT_READ_BATCH_MAX);
	for (index = 0; index < available; index++)
		records[index] = port->records[(port->head + index) %
						KOBOX_INPUT_RECORD_CAPACITY];
	port->head = (port->head + available) % KOBOX_INPUT_RECORD_CAPACITY;
	port->count -= available;
	*count = available;
	*overwritten = port->overwritten;
	notify_records(port);
	result = port->notify_error;
	spin_unlock_irqrestore(&port->lock, flags);
	return result;
}

int kobox_linux_input_port_snapshot(struct kobox_linux_input_port *port,
			  struct kobox_linux_input_device_info *devices,
			  size_t capacity, size_t *count,
			  uint64_t *next_sequence)
{
	struct kobox_linux_input_source *source;
	unsigned long flags;
	size_t total = 0;

	if (!port || !count || !next_sequence || (capacity && !devices))
		return -EINVAL;
	spin_lock_irqsave(&port->lock, flags);
	list_for_each_entry(source, &port->sources, node) {
		if (total < capacity)
			devices[total] = source->info;
		total++;
	}
	*count = total;
	*next_sequence = port->next_sequence;
	spin_unlock_irqrestore(&port->lock, flags);
	return total > capacity ? -ENOSPC : 0;
}
