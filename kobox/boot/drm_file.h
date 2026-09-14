/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DRM_FILE_H
#define KOBOX_BOOT_DRM_FILE_H

#ifdef __KERNEL__
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

struct kobox_linux_drm_file;
struct kobox_linux_drm_mapping;

/* Private, typed results, never a wire layout or a userspace pointer table. */
struct kobox_linux_drm_version {
	int major, minor, patchlevel;
	size_t name_length, date_length, description_length;
	char name[128];
	char date[32];
	char description[256];
};

struct kobox_linux_drm_resources {
	uint32_t fb_count, crtc_count, connector_count, encoder_count;
	uint32_t min_width, max_width, min_height, max_height;
};

struct kobox_linux_drm_mode {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint32_t vrefresh, flags, type;
	char name[32];
};

struct kobox_linux_drm_property_value {
	uint32_t property_id, reserved;
	uint64_t value;
};

struct kobox_linux_drm_connector {
	uint32_t encoder_id, connector_id, connector_type, connector_type_id;
	uint32_t connection, width_mm, height_mm, subpixel;
	uint32_t mode_count, property_count, encoder_count;
};

struct kobox_linux_drm_encoder {
	uint32_t encoder_id, encoder_type, crtc_id;
	uint32_t possible_crtcs, possible_clones;
};

struct kobox_linux_drm_fb2 {
	uint32_t fb_id, width, height, pixel_format, flags;
	uint32_t handles[4], pitches[4], offsets[4];
	uint64_t modifiers[4];
};

struct kobox_linux_drm_dumb_buffer {
	uint32_t height, width, bits_per_pixel, flags;
	uint32_t handle, pitch;
	uint64_t size;
};

struct kobox_linux_virtgpu_resource_create {
	uint32_t target, format, bind, width, height, depth, array_size;
	uint32_t last_level, sample_count, flags, backing_handle;
	uint32_t bo_handle, resource_handle, size, stride;
};

struct kobox_linux_virtgpu_resource_info {
	uint32_t bo_handle, resource_handle, size, blob_memory;
};

struct kobox_linux_virtgpu_transfer {
	uint32_t bo_handle, x, y, z, width, height, depth;
	uint32_t level, offset, stride, layer_stride;
};

struct kobox_linux_drm_map_pages {
	uint64_t length;
	uint32_t cache_policy;
	uint64_t page_count;
};

#ifdef __KERNEL__
/* DRM and GEM are native modules, while the hosted file owner belongs to the
 * boot core. A driver-side provider keeps that dependency in the valid
 * core <- module direction. The provider owns private_mapping until release. */
struct kobox_linux_drm_mapping_operations {
	size_t size;
	int (*map)(struct file *file, uint32_t handle, uint32_t mapping_rights,
		   uint64_t *page_indices, size_t page_capacity,
		   struct kobox_linux_drm_map_pages *result,
		   void **private_mapping);
	void (*release)(void *private_mapping);
};

int kobox_linux_drm_mapping_register(
	const struct kobox_linux_drm_mapping_operations *operations,
	struct module *owner);
void kobox_linux_drm_mapping_unregister(
	const struct kobox_linux_drm_mapping_operations *operations);
#endif

/* The caller must supply the render devt discovered under its authorized
 * device. This is not an API for opening peer-supplied major/minor numbers.
 * All calls, including close, belong to the opening Linux task (no mm).
 * The internal FD must never be shared, duplicated, or passed to a client.
 */
#ifdef __KERNEL__
int kobox_linux_drm_open(dev_t device, uint32_t node_type,
			 struct kobox_linux_drm_file **out);
/* Verify real open-file descriptions and DRM private state are independent;
 * expose no native identity values to the adapter or wire. */
int kobox_linux_drm_distinct(struct kobox_linux_drm_file *left,
			    struct kobox_linux_drm_file *right);
#endif
int kobox_linux_drm_version(struct kobox_linux_drm_file *file,
			    const size_t capacity[3],
			    struct kobox_linux_drm_version *out);
int kobox_linux_drm_get_cap(struct kobox_linux_drm_file *file, uint64_t capability,
			   uint64_t *value);
int kobox_linux_drm_set_client_cap(struct kobox_linux_drm_file *file,
				    uint64_t capability, uint64_t value);
int kobox_linux_drm_master(struct kobox_linux_drm_file *file, bool acquire);
int kobox_linux_drm_resources(struct kobox_linux_drm_file *file,
		uint32_t *fbs, size_t fb_capacity,
		uint32_t *crtcs, size_t crtc_capacity,
		uint32_t *connectors, size_t connector_capacity,
		uint32_t *encoders, size_t encoder_capacity,
		struct kobox_linux_drm_resources *out);
int kobox_linux_drm_connector(struct kobox_linux_drm_file *file,
		uint32_t connector_id,
		struct kobox_linux_drm_mode *modes, size_t mode_capacity,
		struct kobox_linux_drm_property_value *properties,
		size_t property_capacity,
		uint32_t *encoders, size_t encoder_capacity,
		struct kobox_linux_drm_connector *out);
int kobox_linux_drm_encoder(struct kobox_linux_drm_file *file,
		uint32_t encoder_id, struct kobox_linux_drm_encoder *out);
int kobox_linux_drm_set_crtc(struct kobox_linux_drm_file *file,
		uint32_t crtc_id, uint32_t fb_id, uint32_t x, uint32_t y,
		const uint32_t *connectors, size_t connector_count,
		const struct kobox_linux_drm_mode *mode);
int kobox_linux_drm_page_flip(struct kobox_linux_drm_file *file,
		uint32_t crtc_id, uint32_t fb_id, uint32_t flags,
		uint32_t sequence, uint64_t event_token);
int kobox_linux_drm_add_fb2(struct kobox_linux_drm_file *file,
		struct kobox_linux_drm_fb2 *framebuffer);
int kobox_linux_drm_create_dumb(struct kobox_linux_drm_file *file,
		struct kobox_linux_drm_dumb_buffer *buffer);
int kobox_linux_drm_poll_events(struct kobox_linux_drm_file *file,
				uint32_t requested, uint32_t *ready);
int kobox_linux_drm_read_events(struct kobox_linux_drm_file *file,
				void *output, size_t capacity, size_t *bytes);
int kobox_linux_drm_gem_close(struct kobox_linux_drm_file *file,
				      uint32_t handle);
int kobox_linux_drm_prime_export(struct kobox_linux_drm_file *file,
				 uint32_t handle, uint32_t flags, int *dma_fd);
int kobox_linux_drm_prime_import(struct kobox_linux_drm_file *file,
				 int dma_fd, uint32_t *handle);
int kobox_linux_drm_syncobj_create(struct kobox_linux_drm_file *file,
					 uint32_t flags, uint32_t *handle);
int kobox_linux_drm_syncobj_destroy(struct kobox_linux_drm_file *file,
					  uint32_t handle);
int kobox_linux_drm_syncobj_wait(struct kobox_linux_drm_file *file,
				       const void *handle_bytes, size_t handle_count,
				       int64_t timeout_nsec, uint32_t flags,
				       uint64_t fence_deadline_ns,
				       uint32_t *first_signaled);
int kobox_linux_drm_syncobj_array(struct kobox_linux_drm_file *file,
					const void *handle_bytes, size_t handle_count,
					bool signal);
int kobox_linux_drm_virtgpu_getparam(struct kobox_linux_drm_file *file,
				     uint64_t parameter, uint64_t *value);
int kobox_linux_drm_virtgpu_get_caps(struct kobox_linux_drm_file *file,
				     uint32_t capset_id,
				     uint32_t capset_version,
				     void *output, size_t capacity);
int kobox_linux_drm_virtgpu_context_init(struct kobox_linux_drm_file *file,
					 uint32_t parameter_mask,
					 uint32_t capset_id,
					 uint32_t ring_count,
					 uint64_t poll_ring_mask,
					 const void *debug_name,
					 size_t debug_name_size);
int kobox_linux_drm_virtgpu_execbuffer(struct kobox_linux_drm_file *file,
				       uint32_t flags, uint32_t ring_index,
				       const void *command, size_t command_size,
				       const void *handle_bytes, size_t handle_count,
				       const void *input_syncobjs, size_t input_count,
				       const void *output_syncobjs, size_t output_count);
int kobox_linux_drm_virtgpu_resource_create(
	struct kobox_linux_drm_file *file,
	struct kobox_linux_virtgpu_resource_create *resource);
int kobox_linux_drm_virtgpu_resource_info(
	struct kobox_linux_drm_file *file,
	struct kobox_linux_virtgpu_resource_info *resource);
int kobox_linux_drm_virtgpu_transfer(struct kobox_linux_drm_file *file,
				     bool from_host,
				     const struct kobox_linux_virtgpu_transfer *transfer);
int kobox_linux_drm_virtgpu_wait(struct kobox_linux_drm_file *file,
				 uint32_t handle, uint32_t flags);
/* Resolve the real DRM fake offset and retain a pinned shmem GEM object.
 * page_indices are guest physical page numbers copied into private adapter
 * staging. The returned owner is released only by the Linux owner task. */
int kobox_linux_drm_map_pages(struct kobox_linux_drm_file *file,
			      uint32_t handle, uint32_t mapping_rights,
			      uint64_t *page_indices, size_t page_capacity,
			      struct kobox_linux_drm_map_pages *result,
			      struct kobox_linux_drm_mapping **owner);
int kobox_linux_drm_mapping_release(
	struct kobox_linux_drm_mapping **mapping);
/* Successful ownership consumption clears *file, even if flush fails.
 * A rejected owner/FD identity leaves ownership intact and blocks unload.
 */
int kobox_linux_drm_close(struct kobox_linux_drm_file **file);

#endif
