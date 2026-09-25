/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DRM_MEMORY_POLICY_H
#define KOBOX_BOOT_DRM_MEMORY_POLICY_H

/*
 * The hosted GPU core has a finite RAM pool and no killable Linux userspace
 * tasks. Keep room for command completion, GEM destruction and display work;
 * entering the Linux OOM killer can otherwise terminate the entire core.
 * These are admission reserves, not preallocated or permanently pinned RAM.
 */
static inline int kobox_drm_memory_admit(unsigned long long free_bytes,
				 unsigned long long requested_bytes,
				 int render_client)
{
	const unsigned long long reserve = (render_client ? 32ull : 16ull) << 20;
	unsigned long long pages;

	/* VIRTGPU_RESOURCE_CREATE with size zero still allocates one page. */
	if (requested_bytes > ~0ull - 4095)
		return 0;
	pages = requested_bytes ? (requested_bytes + 4095) / 4096 : 1;
	if (free_bytes < reserve || pages > (free_bytes - reserve) / 4096)
		return 0;
	free_bytes -= reserve + pages * 4096;
	/* Page arrays, SG entries and per-object/fence metadata need room too. */
	return free_bytes >= 65536 && pages <= (free_bytes - 65536) / 64;
}

#endif
