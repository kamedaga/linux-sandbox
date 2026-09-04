/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_ASM_CACHEFLUSH_H
#define KOBOX_PROVIDER_ASM_CACHEFLUSH_H

void kobox_provider_flush_cache_vmap(unsigned long start, unsigned long end);
void kobox_provider_flush_cache_vunmap(unsigned long start, unsigned long end);

#define flush_cache_vmap kobox_provider_flush_cache_vmap
#define flush_cache_vmap_early kobox_provider_flush_cache_vmap
#define flush_cache_vunmap kobox_provider_flush_cache_vunmap

#include <asm-generic/cacheflush.h>

#endif /* KOBOX_PROVIDER_ASM_CACHEFLUSH_H */
