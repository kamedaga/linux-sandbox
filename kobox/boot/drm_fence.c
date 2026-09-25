// SPDX-License-Identifier: GPL-2.0-only
#include "drm_fence.h"

#ifndef KOBOX_DRM_FENCE_UNIT_TEST
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include "../arch/x86_64/host_call.h"
#endif

struct kobox_drm_fence_entry {
	struct list_head owned, ready;
	struct kobox_drm_fences *queue;
	struct dma_fence *fence;
	struct dma_fence_cb callback;
	struct kobox_drm_fence_result result;
};

static int fence_owner(struct kobox_drm_fences *queue)
{
	return queue && queue->owner == current && !in_interrupt() &&
		!irqs_disabled() ? 0 : -EPERM;
}

int kobox_drm_fences_init(struct kobox_drm_fences *queue,
			 int (*notify)(void *), void *context)
{
	if (!queue || queue->owner || !notify || in_interrupt() || irqs_disabled())
		return -EINVAL;
	INIT_LIST_HEAD(&queue->owned);
	INIT_LIST_HEAD(&queue->ready);
	spin_lock_init(&queue->ready_lock);
	queue->watermark = 0;
	queue->notify_error = 0;
	queue->stopping = false;
	queue->notify = notify;
	queue->context = context;
	queue->owner = current;
	return 0;
}

int kobox_drm_fence_reserve(struct kobox_drm_fences *queue,
			   u64 session, u64 correlation,
			   struct kobox_drm_fence_entry **out)
{
	struct kobox_drm_fence_entry *entry;
	int error = fence_owner(queue);

	if (error)
		return error;
	if (!out || *out || !session || !correlation || correlation <= queue->watermark)
		return -EINVAL;
	if (queue->stopping)
		return -ESHUTDOWN;
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->queue = queue;
	entry->result.session = session;
	entry->result.correlation = correlation;
	INIT_LIST_HEAD(&entry->ready);
	list_add_tail(&entry->owned, &queue->owned);
	queue->watermark = correlation;
	*out = entry;
	return 0;
}

static void fence_ready(struct kobox_drm_fence_entry *entry, int status)
{
	struct kobox_drm_fences *queue = entry->queue;
	unsigned long flags;
	bool wake;
	int error;

	spin_lock_irqsave(&queue->ready_lock, flags);
	entry->result.status = status ? status : -EIO;
	wake = list_empty(&queue->ready);
	list_add_tail(&entry->ready, &queue->ready);
	spin_unlock_irqrestore(&queue->ready_lock, flags);
	if (!wake)
		return;
	error = kobox_host_call(queue->notify(queue->context));
	if (error) {
		spin_lock_irqsave(&queue->ready_lock, flags);
		if (!queue->notify_error)
			queue->notify_error = error < 0 ? error : -EIO;
		spin_unlock_irqrestore(&queue->ready_lock, flags);
	}
}

static void fence_callback(struct dma_fence *fence, struct dma_fence_cb *callback)
{
	struct kobox_drm_fence_entry *entry =
		container_of(callback, struct kobox_drm_fence_entry, callback);

	fence_ready(entry, dma_fence_get_status_locked(fence));
}

int kobox_drm_fence_arm(struct kobox_drm_fence_entry *entry,
			struct dma_fence *fence)
{
	int error;

	if (!entry || !fence || entry->fence)
		return -EINVAL;
	error = fence_owner(entry->queue);
	if (error)
		return error;
	if (entry->queue->stopping)
		return -ESHUTDOWN;
	entry->fence = fence;
	error = dma_fence_add_callback(fence, &entry->callback, fence_callback);
	if (error == -ENOENT) {
		fence_ready(entry, dma_fence_get_status(fence));
		return 0;
	}
	if (error)
		entry->fence = NULL;
	return error;
}

static void fence_retire(struct kobox_drm_fence_entry *entry)
{
	struct kobox_drm_fences *queue = entry->queue;
	unsigned long flags;

	/* Never hold ready_lock while acquiring fence->lock: signaling takes
	 * them in the opposite order. Removal also waits out an executing callback,
	 * including its host doorbell, before either entry or queue can be freed. */
	if (entry->fence)
		dma_fence_remove_callback(entry->fence, &entry->callback);
	spin_lock_irqsave(&queue->ready_lock, flags);
	list_del_init(&entry->ready);
	spin_unlock_irqrestore(&queue->ready_lock, flags);
	list_del(&entry->owned);
	if (entry->fence)
		dma_fence_put(entry->fence);
	kfree(entry);
}

int kobox_drm_fence_cancel(struct kobox_drm_fence_entry **entry)
{
	int error;

	if (!entry || !*entry)
		return -EINVAL;
	error = fence_owner((*entry)->queue);
	if (error)
		return error;
	fence_retire(*entry);
	*entry = NULL;
	return 0;
}

int kobox_drm_fences_take(struct kobox_drm_fences *queue,
			  struct kobox_drm_fence_result *result)
{
	struct kobox_drm_fence_entry *entry = NULL;
	unsigned long flags;
	int error = fence_owner(queue);

	if (error)
		return error;
	if (!result)
		return -EINVAL;
	spin_lock_irqsave(&queue->ready_lock, flags);
	error = queue->notify_error;
	if (!error && !list_empty(&queue->ready)) {
		entry = list_first_entry(&queue->ready, struct kobox_drm_fence_entry, ready);
		*result = entry->result;
		list_del_init(&entry->ready);
	}
	spin_unlock_irqrestore(&queue->ready_lock, flags);
	if (error || !entry)
		return error;
	fence_retire(entry);
	return 1;
}

int kobox_drm_fences_quiesce(struct kobox_drm_fences *queue)
{
	int error = fence_owner(queue);

	if (error)
		return error;
	queue->stopping = true;
	while (!list_empty(&queue->owned))
		fence_retire(list_first_entry(&queue->owned,
			struct kobox_drm_fence_entry, owned));
	return 0;
}
