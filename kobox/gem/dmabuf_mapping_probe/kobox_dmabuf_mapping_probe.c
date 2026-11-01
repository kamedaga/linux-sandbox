// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/file.h>
#include "../drm_dmabuf_mapping.h"
#include "../i915_mapping.h"
static bool require_i915_api;
module_param(require_i915_api, bool, 0400);

struct probe_buffer {
	struct page *pages[3];
	void *address;
	unsigned int vmaps, vunmaps, accesses, releases;
	unsigned int failure;
	int cpu_error;
	enum dma_data_direction direction;
};

static struct sg_table *probe_attach(struct dma_buf_attachment *attach,
	enum dma_data_direction direction)
{
	return ERR_PTR(-EOPNOTSUPP);
}
static void probe_detach(struct dma_buf_attachment *attach,
	struct sg_table *table, enum dma_data_direction direction) { }
static void probe_release(struct dma_buf *buffer)
{
	struct probe_buffer *probe = buffer->priv;
	probe->releases++;
}
static int probe_vmap(struct dma_buf *buffer, struct iosys_map *map)
{
	struct probe_buffer *probe = buffer->priv;
	probe->vmaps++;
	if (probe->failure == 1)
		return -ENOMEM;
	if (probe->failure == 2) {
		iosys_map_set_vaddr_iomem(map, (void __iomem *)PAGE_SIZE);
		return 0;
	}
	probe->address = vmap(probe->pages, 3, VM_MAP, PAGE_KERNEL);
	if (!probe->address)
		return -ENOMEM;
	iosys_map_set_vaddr(map, probe->address + (probe->failure == 3));
	return 0;
}
static void probe_vunmap(struct dma_buf *buffer, struct iosys_map *map)
{
	struct probe_buffer *probe = buffer->priv;
	probe->vunmaps++;
	if (probe->address) {
		vunmap(probe->address);
		probe->address = NULL;
	}
}
static int probe_cpu(struct dma_buf *buffer, enum dma_data_direction direction)
{
	struct probe_buffer *probe = buffer->priv;
	probe->accesses++;
	probe->direction = direction;
	return probe->cpu_error;
}
static const struct dma_buf_ops probe_ops = {
	.map_dma_buf = probe_attach, .unmap_dma_buf = probe_detach,
	.release = probe_release, .vmap = probe_vmap, .vunmap = probe_vunmap,
	.begin_cpu_access = probe_cpu, .end_cpu_access = probe_cpu,
};

static int probe_case(unsigned int failure)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct kobox_drm_dmabuf_mapping mapping = {0};
	struct kobox_linux_drm_map_pages result, sentinel;
	struct probe_buffer *probe;
	struct dma_buf *buffer;
	u64 pages[3] = {0};
	unsigned int index;
	int error = -ENOMEM, mapped;

	probe = kzalloc(sizeof(*probe), GFP_KERNEL);
	if (!probe)
		return error;
	probe->failure = failure;
	for (index = 0; index < 3; index++) {
		probe->pages[index] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!probe->pages[index])
			goto free_pages;
	}
	info.ops = &probe_ops;
	info.size = 3 * PAGE_SIZE;
	info.flags = O_RDWR;
	info.priv = probe;
	buffer = dma_buf_export(&info);
	if (IS_ERR(buffer)) {
		error = PTR_ERR(buffer);
		goto free_pages;
	}
	memset(&sentinel, 0xa5, sizeof(sentinel));
	result = sentinel;
	/* Short vectors and mismatched extents must fail before any vmap. */
	if (kobox_drm_dmabuf_map(buffer, info.size, pages, 2, &result,
		&mapping) != -ENOSPC || probe->vmaps ||
	    kobox_drm_dmabuf_map(buffer, PAGE_SIZE, pages, 3, &result,
		&mapping) != -EINVAL || probe->vmaps ||
	    memcmp(&result, &sentinel, sizeof(result))) {
		error = -EUCLEAN;
		goto put;
	}
	get_dma_buf(buffer); /* Observe lifetime after the mapping consumes a ref. */
	mapped = kobox_drm_dmabuf_map(buffer, info.size, pages, 3,
		&result, &mapping);
	if (!failure) {
		if (mapped || result.length != info.size || result.page_count != 3 ||
		    result.backing_kind != KOBOX_DRM_BACKING_RAM ||
		    result.cache_policy != KOBOX_DRM_CACHE_WB) {
			error = -EUCLEAN;
			goto release_map;
		}
		if (probe->accesses) {
			error = -EUCLEAN;
			goto release_map;
		}
		probe->cpu_error = -EIO;
		if (dma_buf_begin_cpu_access(buffer, DMA_BIDIRECTIONAL) != -EIO) {
			error = -EUCLEAN;
			goto release_map;
		}
		probe->cpu_error = 0;
		if (dma_buf_begin_cpu_access(buffer, DMA_BIDIRECTIONAL)) {
			error = -EUCLEAN;
			goto release_map;
		}
		for (index = 0; index < 3; index++) {
			if (pages[index] != page_to_pfn(probe->pages[index])) {
				error = -EUCLEAN;
				goto release_map;
			}
			((u8 *)mapping.map.vaddr)[index * PAGE_SIZE] = 0x31 + index;
			if (*(u8 *)page_address(probe->pages[index]) != 0x31 + index) {
				error = -EUCLEAN;
				goto release_map;
			}
		}
		probe->cpu_error = -EINTR;
		if (dma_buf_end_cpu_access(buffer, DMA_BIDIRECTIONAL) != -EINTR) {
			error = -EUCLEAN;
			goto release_map;
		}
		probe->cpu_error = 0;
		if (dma_buf_end_cpu_access(buffer, DMA_BIDIRECTIONAL) ||
		    probe->direction != DMA_BIDIRECTIONAL) {
			error = -EUCLEAN;
			goto release_map;
		}
		error = 0;
	} else {
		error = mapped == (failure == 1 ? -ENOMEM : -EOPNOTSUPP) &&
			!mapping.buffer && !memcmp(&result, &sentinel, sizeof(result)) ?
			0 : -EUCLEAN;
	}
release_map:
	if (mapping.buffer)
		kobox_drm_dmabuf_unmap(&mapping);
	else
		dma_buf_put(buffer);
	if (probe->releases || probe->accesses != (failure ? 0 : 4) || probe->vmaps != 1 ||
	    probe->vunmaps != (failure == 1 ? 0 : 1))
		error = -EUCLEAN;
put:
	/* This is the final reference owned by the test. Release synchronously
	 * before freeing the callback context, without relying on userspace exit. */
	__fput_sync(buffer->file);
	if (probe->releases != 1)
		return -EUCLEAN;
free_pages:
	for (index = 0; index < 3; index++)
		if (probe->pages[index])
			__free_page(probe->pages[index]);
	kfree(probe);
	return error;
}

static int __init mapping_probe_init(void)
{
    if(require_i915_api) {
        const struct kobox_i915_mapping_api *api=kobox_linux_drm_i915_mapping_get();
        if(!api) {pr_err("i915 mapping API lookup unavailable\n");return -ENODEV;}
        pr_info("i915 mapping API lookup and module lifetime pin PASS\n");
        kobox_linux_drm_i915_mapping_put();
    }

	unsigned int failure;
	int error;
	for (failure = 0; failure < 4; failure++) {
		error = probe_case(failure);
		if (error) {
			pr_err("kobox-dmabuf: mapping probe case=%u status=%d\n", failure, error);
			return error;
		}
	}
	pr_info("kobox-dmabuf: real vmap pages, lifetime, CPU sync error/retry and refusal cleanup PASS\n");
	return 0;
}
static void __exit mapping_probe_exit(void) { }
module_init(mapping_probe_init);
module_exit(mapping_probe_exit);
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_DESCRIPTION("Pinned DMA-BUF RAM page mapping regression probe");
