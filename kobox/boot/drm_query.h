/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DRM_QUERY_H
#define KOBOX_BOOT_DRM_QUERY_H

#include "drm_service.h"
#include <kobox2/gpu.h>

/* Native-side GPL dispatcher. Decode on the receiver's native stack, not
 * Linux's small kernel stack. Publish the resulting private plan once; only
 * execute runs in the opening Linux task, with its borrowed render file.
 */
struct kobox_drm_query {
	uint64_t generation, session_id, capability;
	uint32_t command_set_id, command_id;
	uint32_t object_id, mode_flags;
	uint32_t poll_events;
	size_t capacity[4], offset[4];
	size_t output_size, aux_size;
	size_t command_offset, command_size, handles_offset, handle_count;
	size_t input_syncobjs_offset, input_syncobj_count;
	size_t output_syncobjs_offset, output_syncobj_count;
	int64_t timeout_nsec;
	uint64_t fence_deadline_ns;
	uint32_t syncobj_flags, exec_flags, ring_index;
	uint32_t prime_handle, prime_flags;
	uint64_t prime_token;
	size_t mapping_page_capacity;
	uint32_t mapping_handle, mapping_rights;
	bool aux_input, aux_output;
	unsigned char inline_data[KB2_GPU_DRM_MODE_RECORD_FB2_SIZE];
};

struct kobox_drm_query_api {
	int (*version)(struct kobox_linux_drm_file *, const size_t *,
		       struct kobox_linux_drm_version *);
	int (*get_cap)(struct kobox_linux_drm_file *, uint64_t, uint64_t *);
	int (*set_client_cap)(struct kobox_linux_drm_file *, uint64_t, uint64_t);
	int (*master)(struct kobox_linux_drm_file *, bool);
	int (*resources)(struct kobox_linux_drm_file *, uint32_t *, size_t,
		uint32_t *, size_t, uint32_t *, size_t, uint32_t *, size_t,
		struct kobox_linux_drm_resources *);
	int (*connector)(struct kobox_linux_drm_file *, uint32_t,
		struct kobox_linux_drm_mode *, size_t,
		struct kobox_linux_drm_property_value *, size_t,
		uint32_t *, size_t, struct kobox_linux_drm_connector *);
	int (*encoder)(struct kobox_linux_drm_file *, uint32_t,
		struct kobox_linux_drm_encoder *);
	int (*set_crtc)(struct kobox_linux_drm_file *, uint32_t, uint32_t,
		uint32_t, uint32_t, const uint32_t *, size_t,
		const struct kobox_linux_drm_mode *);
	int (*page_flip)(struct kobox_linux_drm_file *, uint32_t, uint32_t,
		uint32_t, uint32_t, uint64_t);
	int (*create_dumb)(struct kobox_linux_drm_file *,
		struct kobox_linux_drm_dumb_buffer *);
	int (*add_fb2)(struct kobox_linux_drm_file *,
		struct kobox_linux_drm_fb2 *);
	int (*poll_events)(struct kobox_linux_drm_file *, uint32_t, uint32_t *);
	int (*read_events)(struct kobox_linux_drm_file *, void *, size_t, size_t *);
	int (*gem_close)(struct kobox_linux_drm_file *, uint32_t);
	int (*prime_export)(struct kobox_linux_drm_service *, uint64_t,
		uint32_t, uint32_t, uint64_t *, size_t,
		struct kobox_linux_drm_service_prime *);
	int (*prime_import)(struct kobox_linux_drm_service *, uint64_t,
		uint64_t, uint32_t *);
	int (*syncobj_create)(struct kobox_linux_drm_file *, uint32_t, uint32_t *);
	int (*syncobj_destroy)(struct kobox_linux_drm_file *, uint32_t);
	int (*syncobj_wait)(struct kobox_linux_drm_file *, const void *, size_t,
		int64_t, uint32_t, uint64_t, uint32_t *);
	int (*syncobj_array)(struct kobox_linux_drm_file *, const void *, size_t,
		bool);
	int (*virtgpu_getparam)(struct kobox_linux_drm_file *, uint64_t, uint64_t *);
	int (*virtgpu_get_caps)(struct kobox_linux_drm_file *, uint32_t, uint32_t,
		void *, size_t);
	int (*virtgpu_context_init)(struct kobox_linux_drm_file *, uint32_t,
		uint32_t, uint32_t, uint64_t, const void *, size_t);
	int (*virtgpu_execbuffer)(struct kobox_linux_drm_file *, uint32_t, uint32_t,
		const void *, size_t, const void *, size_t,
		const void *, size_t, const void *, size_t);
	int (*virtgpu_resource_create)(struct kobox_linux_drm_file *,
		struct kobox_linux_virtgpu_resource_create *);
	int (*virtgpu_resource_info)(struct kobox_linux_drm_file *,
		struct kobox_linux_virtgpu_resource_info *);
	int (*virtgpu_transfer)(struct kobox_linux_drm_file *, bool,
		const struct kobox_linux_virtgpu_transfer *);
	int (*virtgpu_wait)(struct kobox_linux_drm_file *, uint32_t, uint32_t);
	int (*virtgpu_map)(struct kobox_linux_drm_service *, uint64_t, uint32_t,
		uint32_t, uint64_t *, size_t,
		struct kobox_linux_drm_service_mapping *);
};

struct kobox_drm_query_result {
	uint64_t mapping_id, mapping_length, mapping_page_count;
	uint32_t mapping_rights, mapping_cache_policy, attachment_class;
};

int kobox_drm_query_prepare(struct kobox_drm_query *out, uint64_t generation,
			    uint64_t session_id, uint32_t queue_class,
			    const unsigned char *bytes,
			    size_t size, const kb2_gpu_region_t *region);
/* Negative return means infrastructure failure. Driver errors are encoded
 * into canonical command completions and return zero, not loop termination.
 * output and completion are private, disjoint caller-owned staging buffers.
 */
int kobox_drm_query_execute(const struct kobox_drm_query *query,
			   const struct kobox_drm_query_api *api,
			   struct kobox_linux_drm_file *file,
			   unsigned char *output, size_t output_size,
			   unsigned char *aux, size_t aux_size,
			   unsigned char *completion, size_t completion_capacity,
			   size_t *completion_size);
int kobox_drm_query_execute_service(const struct kobox_drm_query *query,
			   const struct kobox_drm_query_api *api,
			   struct kobox_linux_drm_service *service,
			   uint64_t file_cookie,
			   struct kobox_linux_drm_file *file,
			   unsigned char *output, size_t output_size,
			   unsigned char *aux, size_t aux_size,
			   unsigned char *completion, size_t completion_capacity,
			   size_t *completion_size,
			   struct kobox_drm_query_result *query_result);

#endif
