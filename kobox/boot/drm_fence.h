/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DRM_FENCE_H
#define KOBOX_BOOT_DRM_FENCE_H

#if defined(__KERNEL__) && !defined(KOBOX_DRM_FENCE_UNIT_TEST)
#include <linux/dma-fence.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#elif !defined(KOBOX_DRM_FENCE_UNIT_TEST)
#include <stdint.h>
#endif

struct kobox_drm_fence_entry;

struct kobox_drm_fence_result {
	uint64_t session, correlation;
	int status;
};

#if defined(__KERNEL__) || defined(KOBOX_DRM_FENCE_UNIT_TEST)

/* Service-owned rather than file-owned: an exported fence must still finish
 * after its submitting DRM file has closed. The owner alone admits/retires;
 * callbacks only enqueue ready entries and ring a nonblocking doorbell. */
struct kobox_drm_fences {
	struct task_struct *owner;
	struct list_head owned, ready;
	spinlock_t ready_lock;
	u64 watermark;
	void *context;
	int (*notify)(void *context);
	int notify_error;
	bool stopping;
};

int kobox_drm_fences_init(struct kobox_drm_fences *queue,
			 int (*notify)(void *), void *context);
/* Reserve before submitting any work; a failed allocation cannot strand an
 * already accepted submission without its completion bookkeeping. */
int kobox_drm_fence_reserve(struct kobox_drm_fences *queue,
			   u64 session, u64 correlation,
			   struct kobox_drm_fence_entry **out);
/* Takes the caller's fence reference on success, including already-signaled
 * fences. A failed call leaves that reference with the caller. */
int kobox_drm_fence_arm(struct kobox_drm_fence_entry *entry,
			struct dma_fence *fence);
int kobox_drm_fence_cancel(struct kobox_drm_fence_entry **entry);
/* One completed result, zero when idle, negative on host notification error.
 * Consuming a result synchronizes with its callback before freeing storage. */
int kobox_drm_fences_take(struct kobox_drm_fences *queue,
			  struct kobox_drm_fence_result *result);
/* Terminal backend retirement only, not ordinary DRM file close. No success
 * notification is fabricated for unfinished work. */
int kobox_drm_fences_quiesce(struct kobox_drm_fences *queue);

#endif

#endif
