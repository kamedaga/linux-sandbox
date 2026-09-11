/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_DMA_CONSUMER_TEST_H
#define KOBOX_DMA_CONSUMER_TEST_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Native test-device ioctls, not a controller protocol or DRM API. */
struct kobox_dma_consumer_attach {
	__s32 fd;
	__u32 segments;
};

struct kobox_dma_consumer_reclaim {
	__u32 objects, pages;
	__u32 references[4], mappings[4], flags[4];
};

#define KOBOX_DMA_CONSUMER_ATTACH _IOWR('K', 0, struct kobox_dma_consumer_attach)
#define KOBOX_DMA_CONSUMER_TRANSFER _IO('K', 1)
#define KOBOX_DMA_CONSUMER_DETACH _IO('K', 2)
#define KOBOX_DMA_CONSUMER_RECLAIM _IOR('K', 3, struct kobox_dma_consumer_reclaim)

#endif
