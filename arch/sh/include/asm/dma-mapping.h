#ifndef __ASM_SH_DMA_MAPPING_H
#define __ASM_SH_DMA_MAPPING_H

extern struct dma_map_ops *dma_ops;
extern void no_iommu_init(void);

static inline struct dma_map_ops *get_dma_ops(struct device *dev)
{
	return dma_ops;
}

static inline int dma_supported(struct device *dev, u64 mask)
{
	struct dma_map_ops *ops = get_dma_ops(dev);

	if (ops->dma_supported)
		return ops->dma_supported(dev, mask);

	return 1;
}

static inline int dma_set_mask(struct device *dev, u64 mask)
{
	struct dma_map_ops *ops = get_dma_ops(dev);

	if (!dev->dma_mask || !dma_supported(dev, mask))
		return -EIO;
	if (ops->set_dma_mask)
		return ops->set_dma_mask(dev, mask);

	*dev->dma_mask = mask;

	return 0;
}

void *dma_alloc_coherent(struct device *dev, size_t size,
			 dma_addr_t *dma_handle, gfp_t flag);

void dma_free_coherent(struct device *dev, size_t size,
		       void *vaddr, dma_addr_t dma_handle);

void dma_cache_sync(struct device *dev, void *vaddr, size_t size,
		    enum dma_data_direction dir);

#define dma_alloc_noncoherent(d, s, h, f) dma_alloc_coherent(d, s, h, f)
#define dma_free_noncoherent(d, s, v, h) dma_free_coherent(d, s, v, h)
#define dma_is_consistent(d, h) (1)

static inline int dma_get_cache_alignment(void)
{
	/*
	 * Each processor family will define its own L1_CACHE_SHIFT,
	 * L1_CACHE_BYTES wraps to this, so this is always safe.
	 */
	return L1_CACHE_BYTES;
}

static inline int dma_mapping_error(struct device *dev, dma_addr_t dma_addr)
{
	struct dma_map_ops *ops = get_dma_ops(dev);

	if (ops->mapping_error)
		return ops->mapping_error(dev, dma_addr);

	return dma_addr == 0;
}

#include <asm-generic/dma-coherent.h>
#include <asm-generic/dma-mapping-common.h>

#endif /* __ASM_SH_DMA_MAPPING_H */
