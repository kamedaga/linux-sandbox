/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DRM_QUERY_H
#define KOBOX_BOOT_DRM_QUERY_H

#include "drm_file.h"
#include <kobox2/gpu.h>

/* Native-side GPL dispatcher. Decode on the receiver's native stack, not
 * Linux's small kernel stack. Publish the resulting private plan once; only
 * execute runs in the opening Linux task, with its borrowed render file.
 */
struct kobox_drm_query {
	uint64_t session_id, capability;
	uint32_t command_id;
	size_t capacity[3], offset[3];
	size_t output_size;
};

struct kobox_drm_query_api {
	int (*version)(struct kobox_linux_drm_file *, const size_t *,
		       struct kobox_linux_drm_version *);
	int (*get_cap)(struct kobox_linux_drm_file *, uint64_t, uint64_t *);
};

int kobox_drm_query_prepare(struct kobox_drm_query *out, uint64_t generation,
			    uint64_t session_id, const unsigned char *bytes,
			    size_t size, const kb2_gpu_region_t *output_region);
/* Negative return means infrastructure failure. Driver errors are encoded
 * into canonical command completions and return zero, not loop termination.
 * output and completion are private, disjoint caller-owned staging buffers.
 */
int kobox_drm_query_execute(const struct kobox_drm_query *query,
			   const struct kobox_drm_query_api *api,
			   struct kobox_linux_drm_file *file,
			   unsigned char *output, size_t output_size,
			   unsigned char *completion, size_t completion_capacity,
			   size_t *completion_size);

#endif
