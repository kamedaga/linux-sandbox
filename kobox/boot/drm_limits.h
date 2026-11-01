/* SPDX-License-Identifier: MIT */
#ifndef KOBOX_BOOT_DRM_LIMITS_H
#define KOBOX_BOOT_DRM_LIMITS_H

/* Generation-wide metadata budgets, not preallocated GPU memory. Xorg and
 * Minecraft share this pool: on Iris Xe the desktop left only 204 of the old
 * 256 slots free, so atlas uploads failed with ENOSPC. Keep frontend, sandbox
 * ownership and client mapping-lease ledgers on the same bounded budget. */
enum {
    KOBOX_DRM_MAPPING_LIMIT = 4096,
    KOBOX_DRM_PRIME_LIMIT = 64,
    KOBOX_DRM_SYNC_FILE_LIMIT = 256,
};

#endif
