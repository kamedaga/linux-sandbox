/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_I915_MAPPING_H
#define KOBOX_I915_MAPPING_H
#include "../boot/drm_file.h"
#define KOBOX_I915_MAP_CURRENT 0u
#define KOBOX_I915_MAP_WB 1u
#define KOBOX_I915_MAP_WC 2u
/* Process-local driver adapter. Keep this symbol's module reference through
 * release; failed restoration retains its cookie for explicit retry. */
struct kobox_i915_mapping_api {
    size_t size;
    int (*acquire)(struct file *, uint32_t, uint64_t, uint32_t, uint32_t,
        uint64_t *, size_t, struct kobox_linux_drm_map_pages *, void **);
    int (*release)(void **);
    int (*quiesce)(void);
};
extern const struct kobox_i915_mapping_api kobox_i915_mapping_api;
#endif
