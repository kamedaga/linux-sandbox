/* SPDX-License-Identifier: MIT */
#ifndef KOBOX_BOOT_DRM_LIMITS_H
#define KOBOX_BOOT_DRM_LIMITS_H

/* Generation-wide metadata budgets, not preallocated GPU memory. The old
 * 64-mapping pool was shared by Xorg and every browser process; FHD browsing
 * exhausted it with only 26 MiB mapped. Keep frontend/ownership ledgers equal. */
enum {
    KOBOX_DRM_MAPPING_LIMIT = 256,
    KOBOX_DRM_PRIME_LIMIT = 64,
};

#endif
