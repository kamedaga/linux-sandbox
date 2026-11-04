/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_RESIDENT_POLICY_H
#define KOBOX_RESIDENT_POLICY_H
#include "host.h"
#define KOBOX_RESIDENT_BATCH 256U
#define KOBOX_RESIDENT_ALLOC (-3)
/* Size of the contiguous early resident pool. All metadata allocations stay
 * below its boundary; arena PFNs above it are reserved. Zero means invalid. */
static inline uint64_t kobox_resident_initial_bytes(uint64_t arena,
    uint64_t image_end, uint64_t metadata)
{
    const uint64_t alignment = 2ULL << 20;
    uint64_t bytes;
    if (!arena || (arena & 4095) || image_end > arena ||
        metadata > (~(uint64_t)0) - (128ULL << 20) ||
        image_end > (~(uint64_t)0) - (64ULL << 20)) return 0;
    bytes = (128ULL << 20) + metadata;
    if (bytes < image_end + (64ULL << 20)) bytes = image_end + (64ULL << 20);
    if (bytes >= arena) return arena;
    if (bytes > (~(uint64_t)0) - (alignment - 1)) return 0;
    bytes = (bytes + alignment - 1) & ~(alignment - 1);
    return bytes < arena ? bytes : arena;
}
/* Cursor advances exclusively across complete successful native calls. A
 * failed batch may contain backing; retry individual pages to admit only the
 * proven resident prefix. Unproven pages remain owned by the reserved tail. */
static inline int kobox_resident_supply(void *context, uint64_t *cursor,
    uint64_t end, int (*populate)(void *, uint64_t, uint64_t),
    void (*publish)(void *, uint64_t, uint64_t))
{
    uint64_t count = end - *cursor;
    int status;
    if (!count) return 0;
    if (count > KOBOX_RESIDENT_BATCH) count = KOBOX_RESIDENT_BATCH;
    status = populate(context, *cursor, count);
    if (!status) {
        publish(context, *cursor, count);
        *cursor += count;
        return 0;
    }
    if (status != KOBOX_RESIDENT_ALLOC) return status;
    while (count--) {
        status = populate(context, *cursor, 1);
        if (status) return status;
        publish(context, *cursor, 1);
        ++*cursor;
    }
    return 0;
}
#endif
