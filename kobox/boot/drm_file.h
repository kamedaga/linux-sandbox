/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DRM_FILE_H
#define KOBOX_BOOT_DRM_FILE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

struct kobox_linux_drm_file;

/* Private, typed results, never a wire layout or a userspace pointer table. */
struct kobox_linux_drm_version {
	int major, minor, patchlevel;
	size_t name_length, date_length, description_length;
	char name[128];
	char date[32];
	char description[256];
};

/* The caller must supply the render devt discovered under its authorized
 * device. This is not an API for opening peer-supplied major/minor numbers.
 * All calls, including close, belong to the opening Linux task (no mm).
 * The internal FD must never be shared, duplicated, or passed to a client.
 */
#ifdef __KERNEL__
int kobox_linux_drm_open(dev_t render, struct kobox_linux_drm_file **out);
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
/* Successful ownership consumption clears *file, even if flush fails.
 * A rejected owner/FD identity leaves ownership intact and blocks unload.
 */
int kobox_linux_drm_close(struct kobox_linux_drm_file **file);

#endif
