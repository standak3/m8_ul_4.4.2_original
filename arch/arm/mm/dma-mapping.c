/*
 *  linux/arch/arm/mm/dma-mapping.c
 *
 *  Copyright (C) 2000-2004 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  DMA uncached mapping support.
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/highmem.h>
#include <linux/memblock.h>
#include <linux/slab.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/vmalloc.h>

#include <asm/memory.h>
#include <asm/highmem.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/sizes.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/system_info.h>
#include <asm/dma-contiguous.h>
#include <asm/dma-iommu.h>

#include <htc_debug/stability/htc_report_meminfo.h>

#include "mm.h"

static void __dma_page_cpu_to_dev(struct page *, unsigned long,
		size_t, enum dma_data_direction);
static void __dma_page_dev_to_cpu(struct page *, unsigned long,
		size_t, enum dma_data_direction);

/**
 * arm_dma_map_page - map a portion of a page for streaming DMA
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @page: page that buffer resides in
 * @offset: offset into page for start of buffer
 * @size: size of buffer to map
 * @dir: DMA transfer direction
 *
 * Ensure that any data held in the cache is appropriately discarded
 * or written back.
 *
 * The device owns this memory once this call has completed.  The CPU
 * can regain ownership by calling dma_unmap_page().
 */
static dma_addr_t arm_dma_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size, enum dma_data_direction dir,
	     struct dma_attrs *attrs)
{
	if (!arch_is_coherent())
		__dma_page_cpu_to_dev(page, offset, size, dir);
	return pfn_to_dma(dev, page_to_pfn(page)) + offset;
}

static void arm_dma_unmap_page(struct device *dev, dma_addr_t handle,
		size_t size, enum dma_data_direction dir,
		struct dma_attrs *attrs)
{
	if (!arch_is_coherent())
		__dma_page_dev_to_cpu(pfn_to_page(dma_to_pfn(dev, handle)),
				      handle & ~PAGE_MASK, size, dir);
}

static void arm_dma_sync_single_for_cpu(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	unsigned int offset = handle & (PAGE_SIZE - 1);
	struct page *page = pfn_to_page(dma_to_pfn(dev, handle-offset));
	if (!arch_is_coherent())
		__dma_page_dev_to_cpu(page, offset, size, dir);
}

static void arm_dma_sync_single_for_device(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	unsigned int offset = handle & (PAGE_SIZE - 1);
	struct page *page = pfn_to_page(dma_to_pfn(dev, handle-offset));
	if (!arch_is_coherent())
		__dma_page_cpu_to_dev(page, offset, size, dir);
}

static int arm_dma_set_mask(struct device *dev, u64 dma_mask);

struct dma_map_ops arm_dma_ops = {
	.alloc			= arm_dma_alloc,
	.free			= arm_dma_free,
	.mmap			= arm_dma_mmap,
	.map_page		= arm_dma_map_page,
	.unmap_page		= arm_dma_unmap_page,
	.map_sg			= arm_dma_map_sg,
	.unmap_sg		= arm_dma_unmap_sg,
	.sync_single_for_cpu	= arm_dma_sync_single_for_cpu,
	.sync_single_for_device	= arm_dma_sync_single_for_device,
	.sync_sg_for_cpu	= arm_dma_sync_sg_for_cpu,
	.sync_sg_for_device	= arm_dma_sync_sg_for_device,
	.set_dma_mask		= arm_dma_set_mask,
};
EXPORT_SYMBOL(arm_dma_ops);

static u64 get_coherent_dma_mask(struct device *dev)
{
	u64 mask = (u64)arm_dma_limit;

	if (dev) {
		mask = dev->coherent_dma_mask;

		if (mask == 0) {
			dev_warn(dev, "coherent DMA mask is unset\n");
			return 0;
		}

		if ((~mask) & (u64)arm_dma_limit) {
			dev_warn(dev, "coherent DMA mask %#llx is smaller "
				 "than system GFP_DMA mask %#llx\n",
				 mask, (u64)arm_dma_limit);
			return 0;
		}
	}

	return mask;
}

static void __dma_clear_buffer(struct page *page, size_t size)
{
	if (!PageHighMem(page)) {
		void *ptr = page_address(page);
		if (ptr) {
			memset(ptr, 0, size);
			dmac_flush_range(ptr, ptr + size);
			outer_flush_range(__pa(ptr), __pa(ptr) + size);
		}
	} else {
		phys_addr_t base = __pfn_to_phys(page_to_pfn(page));
		phys_addr_t end = base + size;
		while (size > 0) {
			void *ptr = kmap_atomic(page);
			memset(ptr, 0, PAGE_SIZE);
			dmac_flush_range(ptr, ptr + PAGE_SIZE);
			kunmap_atomic(ptr);
			page++;
			size -= PAGE_SIZE;
		}
		outer_flush_range(base, end);
	}
}

static struct page *__dma_alloc_buffer(struct device *dev, size_t size, gfp_t gfp)
{
	unsigned long order = get_order(size);
	struct page *page, *p, *e;

	page = alloc_pages(gfp, order);
	if (!page)
		return NULL;

	split_page(page, order);
	for (p = page + (size >> PAGE_SHIFT), e = page + (1 << order); p < e; p++)
		__free_page(p);

	__dma_clear_buffer(page, size);

	add_meminfo_total_pages(NR_DMA_PAGES, size >> PAGE_SHIFT);

	return page;
}

static void __dma_free_buffer(struct page *page, size_t size)
{
	struct page *e = page + (size >> PAGE_SHIFT);

	while (page < e) {
		__free_page(page);
		page++;
	}

	sub_meminfo_total_pages(NR_DMA_PAGES, size >> PAGE_SHIFT);
}

#ifdef CONFIG_MMU
#ifdef CONFIG_HUGETLB_PAGE
#error ARM Coherent DMA allocator does not (yet) support huge TLB
#endif

static void *__alloc_remap_buffer(struct device *dev, size_t size, gfp_t gfp,
				 pgprot_t prot, struct page **ret_page,
				 const void *caller);

static void *
__dma_alloc_remap(struct page *page, size_t size, gfp_t gfp, pgprot_t prot,
	const void *caller)
{
	struct vm_struct *area;
	unsigned long addr;

	area = get_vm_area_caller(size, VM_ARM_DMA_CONSISTENT | VM_USERMAP,
				  caller);
	if (!area)
		return NULL;
	addr = (unsigned long)area->addr;
	area->phys_addr = __pfn_to_phys(page_to_pfn(page));

	if (ioremap_page_range(addr, addr + size, area->phys_addr, prot)) {
		vunmap((void *)addr);
		return NULL;
	}
	return (void *)addr;
}

static void __dma_free_remap(void *cpu_addr, size_t size, bool no_warn)
{
	unsigned int flags = VM_ARM_DMA_CONSISTENT | VM_USERMAP;
	struct vm_struct *area = find_vm_area(cpu_addr);
	if (!area || (area->flags & flags) != flags) {
		if (!no_warn)
			WARN(1, "trying to free invalid coherent area: %p\n",
				cpu_addr);
		return;
	}
	unmap_kernel_range((unsigned long)cpu_addr, size);
	vunmap(cpu_addr);
}

static void *__alloc_from_contiguous(struct device *dev, size_t size,
				     pgprot_t prot, struct page **ret_page,
				     bool no_kernel_mapping, const void *caller);

struct dma_pool {
	size_t size;
	spinlock_t lock;
	unsigned long *bitmap;
	unsigned long nr_pages;
	void *vaddr;
	struct page *page;
};

static struct dma_pool atomic_pool = {
	.size = SZ_256K,
};

static int __init early_coherent_pool(char *p)
{
	atomic_pool.size = memparse(p, &p);
	return 0;
}
early_param("coherent_pool", early_coherent_pool);

static int __init atomic_pool_init(void)
{
	struct dma_pool *pool = &atomic_pool;
	pgprot_t prot = pgprot_dmacoherent(pgprot_kernel);
	unsigned long nr_pages = pool->size >> PAGE_SHIFT;
	unsigned long *bitmap;
	struct page *page;
	void *ptr;
	int bitmap_size = BITS_TO_LONGS(nr_pages) * sizeof(long);

	bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!bitmap)
		goto no_bitmap;

	if (IS_ENABLED(CONFIG_CMA))
		ptr = __alloc_from_contiguous(NULL, pool->size, prot, &page,
						false, atomic_pool_init);
	else
		ptr = __alloc_remap_buffer(NULL, pool->size, GFP_KERNEL, prot,
					   &page, NULL);
	if (ptr) {
		spin_lock_init(&pool->lock);
		pool->vaddr = ptr;
		pool->page = page;
		pool->bitmap = bitmap;
		pool->nr_pages = nr_pages;
		pr_info("DMA: preallocated %u KiB pool for atomic coherent allocations\n",
		       (unsigned)pool->size / 1024);
		return 0;
	}
	kfree(bitmap);
no_bitmap:
	pr_err("DMA: failed to allocate %u KiB pool for atomic coherent allocation\n",
	       (unsigned)pool->size / 1024);
	return -ENOMEM;
}
postcore_initcall(atomic_pool_init);

struct dma_contig_early_reserve {
	phys_addr_t base;
	unsigned long size;
};

static struct dma_contig_early_reserve dma_mmu_remap[MAX_CMA_AREAS] __initdata;

static int dma_mmu_remap_num __initdata;

void __init dma_contiguous_early_fixup(phys_addr_t base, unsigned long size)
{
	dma_mmu_remap[dma_mmu_remap_num].base = base;
	dma_mmu_remap[dma_mmu_remap_num].size = size;
	dma_mmu_remap_num++;
}

void __init dma_contiguous_remap(void)
{
	int i;
	for (i = 0; i < dma_mmu_remap_num; i++) {
		phys_addr_t start = dma_mmu_remap[i].base;
		phys_addr_t end = start + dma_mmu_remap[i].size;
		struct map_desc map;
		unsigned long addr;

		if (end > arm_lowmem_limit)
			end = arm_lowmem_limit;
		if (start >= end)
			continue;

		map.pfn = __phys_to_pfn(start);
		map.virtual = __phys_to_virt(start);
		map.length = end - start;
		map.type = MT_MEMORY_DMA_READY;

		for (addr = __phys_to_virt(start); addr < __phys_to_virt(end);
		     addr += PMD_SIZE)
			pmd_clear(pmd_off_k(addr));

		iotable_init(&map, 1);
	}
}

static int __dma_update_pte(pte_t *pte, pgtable_t token, unsigned long addr,
			    void *data)
{
	struct page *page = virt_to_page(addr);
	pgprot_t prot = *(pgprot_t *)data;

	set_pte_ext(pte, mk_pte(page, prot), 0);
	return 0;
}

static int __dma_clear_pte(pte_t *pte, pgtable_t token, unsigned long addr,
			    void *data)
{
	pte_clear(&init_mm, addr, pte);
	return 0;
}

static void __dma_remap(struct page *page, size_t size, pgprot_t prot,
			bool no_kernel_map)
{
	unsigned long start = (unsigned long) page_address(page);
	unsigned end = start + size;
	int (*func)(pte_t *pte, pgtable_t token, unsigned long addr,
			    void *data);

	if (no_kernel_map)
		func = __dma_clear_pte;
	else
		func = __dma_update_pte;

	apply_to_page_range(&init_mm, start, size, func, &prot);
	dsb();
	flush_tlb_kernel_range(start, end);
}

static void *__alloc_remap_buffer(struct device *dev, size_t size, gfp_t gfp,
				 pgprot_t prot, struct page **ret_page,
				 const void *caller)
{
	struct page *page;
	void *ptr;
	page = __dma_alloc_buffer(dev, size, gfp);
	if (!page)
		return NULL;

	ptr = __dma_alloc_remap(page, size, gfp, prot, caller);
	if (!ptr) {
		__dma_free_buffer(page, size);
		return NULL;
	}

	*ret_page = page;
	return ptr;
}

static void *__alloc_from_pool(size_t size, struct page **ret_page)
{
	struct dma_pool *pool = &atomic_pool;
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned int pageno;
	unsigned long flags;
	void *ptr = NULL;
	size_t align;

	if (!pool->vaddr) {
		WARN(1, "coherent pool not initialised!\n");
		return NULL;
	}

	align = PAGE_SIZE << get_order(size);

	spin_lock_irqsave(&pool->lock, flags);
	pageno = bitmap_find_next_zero_area(pool->bitmap, pool->nr_pages,
					    0, count, (1 << align) - 1);
	if (pageno < pool->nr_pages) {
		bitmap_set(pool->bitmap, pageno, count);
		ptr = pool->vaddr + PAGE_SIZE * pageno;
		*ret_page = pool->page + pageno;
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	return ptr;
}

static int __free_from_pool(void *start, size_t size)
{
	struct dma_pool *pool = &atomic_pool;
	unsigned long pageno, count;
	unsigned long flags;

	if (start < pool->vaddr || start > pool->vaddr + pool->size)
		return 0;

	if (start + size > pool->vaddr + pool->size) {
		WARN(1, "freeing wrong coherent size from pool\n");
		return 0;
	}

	pageno = (start - pool->vaddr) >> PAGE_SHIFT;
	count = size >> PAGE_SHIFT;

	spin_lock_irqsave(&pool->lock, flags);
	bitmap_clear(pool->bitmap, pageno, count);
	spin_unlock_irqrestore(&pool->lock, flags);

	return 1;
}

#define NO_KERNEL_MAPPING_DUMMY	0x2222
static void *__alloc_from_contiguous(struct device *dev, size_t size,
				     pgprot_t prot, struct page **ret_page,
				     bool no_kernel_mapping,
				     const void *caller)
{
	unsigned long order = get_order(size);
	size_t count = size >> PAGE_SHIFT;
	struct page *page;
	void *ptr;

	page = dma_alloc_from_contiguous(dev, count, order);
	if (!page)
		return NULL;

	__dma_clear_buffer(page, size);

	if (!PageHighMem(page)) {
		__dma_remap(page, size, prot, no_kernel_mapping);
		ptr = page_address(page);
	} else {
		if (no_kernel_mapping) {
			ptr = (void *)NO_KERNEL_MAPPING_DUMMY;
		} else {
			ptr = __dma_alloc_remap(page, size, GFP_KERNEL, prot,
						caller);
			if (!ptr) {
				dma_release_from_contiguous(dev, page, count);
				return NULL;
			}
		}
	}
	*ret_page = page;
	return ptr;
}

static void __free_from_contiguous(struct device *dev, struct page *page,
				   void *cpu_addr, size_t size)
{
	if (!PageHighMem(page))
		__dma_remap(page, size, pgprot_kernel, false);
	else
		__dma_free_remap(cpu_addr, size, true);
	dma_release_from_contiguous(dev, page, size >> PAGE_SHIFT);
}

static inline pgprot_t __get_dma_pgprot(struct dma_attrs *attrs, pgprot_t prot)
{
	if (dma_get_attr(DMA_ATTR_WRITE_COMBINE, attrs))
		prot = pgprot_writecombine(prot);
	else if (dma_get_attr(DMA_ATTR_STRONGLY_ORDERED, attrs))
		prot = pgprot_stronglyordered(prot);
	
	else if (!dma_get_attr(DMA_ATTR_NON_CONSISTENT, attrs))
		prot = pgprot_dmacoherent(prot);

	return prot;
}

#define nommu() 0

#else	

#define nommu() 1

#define __alloc_remap_buffer(dev, size, gfp, prot, ret, c)	NULL
#define __alloc_from_pool(dev, size, ret_page, c)		NULL
#define __alloc_from_contiguous(dev, size, prot, ret, w)	NULL
#define __free_from_pool(cpu_addr, size)			0
#define __free_from_contiguous(dev, page, size)			do { } while (0)
#define __dma_free_remap(cpu_addr, size)			do { } while (0)
#define __get_dma_pgprot(attrs, prot)				__pgprot(0)

#endif	

static void *__alloc_simple_buffer(struct device *dev, size_t size, gfp_t gfp,
				   struct page **ret_page)
{
	struct page *page;
	page = __dma_alloc_buffer(dev, size, gfp);
	if (!page)
		return NULL;

	*ret_page = page;
	return page_address(page);
}



static void *__dma_alloc(struct device *dev, size_t size, dma_addr_t *handle,
			 gfp_t gfp, pgprot_t prot, const void *caller,
			 bool no_kernel_mapping)
{
	u64 mask = get_coherent_dma_mask(dev);
	struct page *page;
	void *addr;

#ifdef CONFIG_DMA_API_DEBUG
	u64 limit = (mask + 1) & ~mask;
	if (limit && size >= limit) {
		dev_warn(dev, "coherent allocation too big (requested %#x mask %#llx)\n",
			size, mask);
		return NULL;
	}
#endif

	if (!mask)
		return NULL;

	if (mask < 0xffffffffULL)
		gfp |= GFP_DMA;

	gfp &= ~(__GFP_COMP);

	*handle = DMA_ERROR_CODE;
	size = PAGE_ALIGN(size);

	if (arch_is_coherent() || nommu())
		addr = __alloc_simple_buffer(dev, size, gfp, &page);
	else if (gfp & GFP_ATOMIC)
		addr = __alloc_from_pool(size, &page);
	else if (!IS_ENABLED(CONFIG_CMA))
		addr = __alloc_remap_buffer(dev, size, gfp, prot, &page, caller);
	else
		addr = __alloc_from_contiguous(dev, size, prot, &page,
						no_kernel_mapping, caller);

	if (addr)
		*handle = pfn_to_dma(dev, page_to_pfn(page));

	return addr;
}

void *arm_dma_alloc(struct device *dev, size_t size, dma_addr_t *handle,
		    gfp_t gfp, struct dma_attrs *attrs)
{
	pgprot_t prot = __get_dma_pgprot(attrs, pgprot_kernel);
	void *memory;
	bool no_kernel_mapping = dma_get_attr(DMA_ATTR_NO_KERNEL_MAPPING,
					attrs);

	if (dma_alloc_from_coherent(dev, size, handle, &memory))
		return memory;

	return __dma_alloc(dev, size, handle, gfp, prot,
			   __builtin_return_address(0), no_kernel_mapping);
}

int arm_dma_mmap(struct device *dev, struct vm_area_struct *vma,
		 void *cpu_addr, dma_addr_t dma_addr, size_t size,
		 struct dma_attrs *attrs)
{
	int ret = -ENXIO;
#ifdef CONFIG_MMU
	unsigned long pfn = dma_to_pfn(dev, dma_addr);
	vma->vm_page_prot = __get_dma_pgprot(attrs, vma->vm_page_prot);

	if (dma_mmap_from_coherent(dev, vma, cpu_addr, size, &ret))
		return ret;

	ret = remap_pfn_range(vma, vma->vm_start,
			      pfn + vma->vm_pgoff,
			      vma->vm_end - vma->vm_start,
			      vma->vm_page_prot);
#endif	

	return ret;
}

void arm_dma_free(struct device *dev, size_t size, void *cpu_addr,
		  dma_addr_t handle, struct dma_attrs *attrs)
{
	struct page *page = pfn_to_page(dma_to_pfn(dev, handle));

	if (dma_release_from_coherent(dev, get_order(size), cpu_addr))
		return;

	size = PAGE_ALIGN(size);

	if (arch_is_coherent() || nommu()) {
		__dma_free_buffer(page, size);
	} else if (__free_from_pool(cpu_addr, size)) {
		return;
	} else if (!IS_ENABLED(CONFIG_CMA)) {
		__dma_free_remap(cpu_addr, size, false);
		__dma_free_buffer(page, size);
	} else {
		WARN_ON(irqs_disabled());
		__free_from_contiguous(dev, page, cpu_addr, size);
	}
}

static void dma_cache_maint_page(struct page *page, unsigned long offset,
	size_t size, enum dma_data_direction dir,
	void (*op)(const void *, size_t, int))
{
	unsigned long pfn;
	size_t left = size;

	pfn = page_to_pfn(page) + offset / PAGE_SIZE;
	offset %= PAGE_SIZE;

	do {
		size_t len = left;
		void *vaddr;

		page = pfn_to_page(pfn);

		if (PageHighMem(page)) {
			if (len + offset > PAGE_SIZE)
				len = PAGE_SIZE - offset;

			if (cache_is_vipt_nonaliasing()) {
				vaddr = kmap_atomic(page);
				op(vaddr + offset, len, dir);
				kunmap_atomic(vaddr);
			} else {
				vaddr = kmap_high_get(page);
				if (vaddr) {
					op(vaddr + offset, len, dir);
					kunmap_high(page);
				}
			}
		} else {
			vaddr = page_address(page) + offset;
			op(vaddr, len, dir);
		}
		offset = 0;
		pfn++;
		left -= len;
	} while (left);
}

static void __dma_page_cpu_to_dev(struct page *page, unsigned long off,
	size_t size, enum dma_data_direction dir)
{
	unsigned long paddr;

	dma_cache_maint_page(page, off, size, dir, dmac_map_area);

	paddr = page_to_phys(page) + off;
	if (dir == DMA_FROM_DEVICE) {
		outer_inv_range(paddr, paddr + size);
	} else {
		outer_clean_range(paddr, paddr + size);
	}
	
}

static void __dma_page_dev_to_cpu(struct page *page, unsigned long off,
	size_t size, enum dma_data_direction dir)
{
	unsigned long paddr = page_to_phys(page) + off;

	
	
	if (dir != DMA_TO_DEVICE)
		outer_inv_range(paddr, paddr + size);

	dma_cache_maint_page(page, off, size, dir, dmac_unmap_area);

	if (dir != DMA_TO_DEVICE && off == 0 && size >= PAGE_SIZE)
		set_bit(PG_dcache_clean, &page->flags);
}

int arm_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
		enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct dma_map_ops *ops = get_dma_ops(dev);
	struct scatterlist *s;
	int i, j;

	for_each_sg(sg, s, nents, i) {
#ifdef CONFIG_NEED_SG_DMA_LENGTH
		s->dma_length = s->length;
#endif
		s->dma_address = ops->map_page(dev, sg_page(s), s->offset,
						s->length, dir, attrs);
		if (dma_mapping_error(dev, s->dma_address))
			goto bad_mapping;
	}
	return nents;

 bad_mapping:
	for_each_sg(sg, s, i, j)
		ops->unmap_page(dev, sg_dma_address(s), sg_dma_len(s), dir, attrs);
	return 0;
}

void arm_dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,
		enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct dma_map_ops *ops = get_dma_ops(dev);
	struct scatterlist *s;

	int i;

	for_each_sg(sg, s, nents, i)
		ops->unmap_page(dev, sg_dma_address(s), sg_dma_len(s), dir, attrs);
}

void arm_dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct dma_map_ops *ops = get_dma_ops(dev);
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		ops->sync_single_for_cpu(dev, sg_dma_address(s), s->length,
					 dir);
}

void arm_dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct dma_map_ops *ops = get_dma_ops(dev);
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		ops->sync_single_for_device(dev, sg_dma_address(s), s->length,
					    dir);
}

int dma_supported(struct device *dev, u64 mask)
{
	if (mask < (u64)arm_dma_limit)
		return 0;
	return 1;
}
EXPORT_SYMBOL(dma_supported);

static int arm_dma_set_mask(struct device *dev, u64 dma_mask)
{
	if (!dev->dma_mask || !dma_supported(dev, dma_mask))
		return -EIO;

	*dev->dma_mask = dma_mask;

	return 0;
}

#define PREALLOC_DMA_DEBUG_ENTRIES	4096

static int __init dma_debug_do_init(void)
{
	dma_debug_init(PREALLOC_DMA_DEBUG_ENTRIES);
	return 0;
}
fs_initcall(dma_debug_do_init);

#ifdef CONFIG_ARM_DMA_USE_IOMMU


static inline dma_addr_t __alloc_iova(struct dma_iommu_mapping *mapping,
				      size_t size)
{
	unsigned int order = get_order(size);
	unsigned int align = 0;
	unsigned int count, start;
	unsigned long flags;

	count = ((PAGE_ALIGN(size) >> PAGE_SHIFT) +
		 (1 << mapping->order) - 1) >> mapping->order;

	if (order > mapping->order)
		align = (1 << (order - mapping->order)) - 1;

	spin_lock_irqsave(&mapping->lock, flags);
	start = bitmap_find_next_zero_area(mapping->bitmap, mapping->bits, 0,
					   count, align);
	if (start > mapping->bits) {
		spin_unlock_irqrestore(&mapping->lock, flags);
		return DMA_ERROR_CODE;
	}

	bitmap_set(mapping->bitmap, start, count);
	spin_unlock_irqrestore(&mapping->lock, flags);

	return mapping->base + (start << (mapping->order + PAGE_SHIFT));
}

static inline void __free_iova(struct dma_iommu_mapping *mapping,
			       dma_addr_t addr, size_t size)
{
	unsigned int start = (addr - mapping->base) >>
			     (mapping->order + PAGE_SHIFT);
	unsigned int count = ((size >> PAGE_SHIFT) +
			      (1 << mapping->order) - 1) >> mapping->order;
	unsigned long flags;

	spin_lock_irqsave(&mapping->lock, flags);
	bitmap_clear(mapping->bitmap, start, count);
	spin_unlock_irqrestore(&mapping->lock, flags);
}

static struct page **__iommu_alloc_buffer(struct device *dev, size_t size, gfp_t gfp)
{
	struct page **pages;
	int count = size >> PAGE_SHIFT;
	int array_size = count * sizeof(struct page *);
	int i = 0;

	if (array_size <= PAGE_SIZE)
		pages = kzalloc(array_size, gfp);
	else
		pages = vzalloc(array_size);
	if (!pages)
		return NULL;

	while (count) {
		int j, order = __fls(count);

		pages[i] = alloc_pages(gfp | __GFP_NOWARN, order);
		while (!pages[i] && order)
			pages[i] = alloc_pages(gfp | __GFP_NOWARN, --order);
		if (!pages[i])
			goto error;

		if (order)
			split_page(pages[i], order);
		j = 1 << order;
		while (--j)
			pages[i + j] = pages[i] + j;

		__dma_clear_buffer(pages[i], PAGE_SIZE << order);
		i += 1 << order;
		count -= 1 << order;
	}

	return pages;
error:
	while (--i)
		if (pages[i])
			__free_pages(pages[i], 0);
	if (array_size < PAGE_SIZE)
		kfree(pages);
	else
		vfree(pages);
	return NULL;
}

static int __iommu_free_buffer(struct device *dev, struct page **pages, size_t size)
{
	int count = size >> PAGE_SHIFT;
	int array_size = count * sizeof(struct page *);
	int i;
	for (i = 0; i < count; i++)
		if (pages[i])
			__free_pages(pages[i], 0);
	if (array_size < PAGE_SIZE)
		kfree(pages);
	else
		vfree(pages);
	return 0;
}

static void *
__iommu_alloc_remap(struct page **pages, size_t size, gfp_t gfp, pgprot_t prot,
		    const void *caller)
{
	unsigned int i, nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct vm_struct *area;
	unsigned long p;

	area = get_vm_area_caller(size, VM_ARM_DMA_CONSISTENT | VM_USERMAP,
				  caller);
	if (!area)
		return NULL;

	area->pages = pages;
	area->nr_pages = nr_pages;
	p = (unsigned long)area->addr;

	for (i = 0; i < nr_pages; i++) {
		phys_addr_t phys = __pfn_to_phys(page_to_pfn(pages[i]));
		if (ioremap_page_range(p, p + PAGE_SIZE, phys, prot))
			goto err;
		p += PAGE_SIZE;
	}
	return area->addr;
err:
	unmap_kernel_range((unsigned long)area->addr, size);
	vunmap(area->addr);
	return NULL;
}

static dma_addr_t
__iommu_create_mapping(struct device *dev, struct page **pages, size_t size)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	dma_addr_t dma_addr, iova;
	int i, ret = DMA_ERROR_CODE;

	dma_addr = __alloc_iova(mapping, size);
	if (dma_addr == DMA_ERROR_CODE)
		return dma_addr;

	iova = dma_addr;
	for (i = 0; i < count; ) {
		unsigned int next_pfn = page_to_pfn(pages[i]) + 1;
		phys_addr_t phys = page_to_phys(pages[i]);
		unsigned int len, j;

		for (j = i + 1; j < count; j++, next_pfn++)
			if (page_to_pfn(pages[j]) != next_pfn)
				break;

		len = (j - i) << PAGE_SHIFT;
		ret = iommu_map(mapping->domain, iova, phys, len, 0);
		if (ret < 0)
			goto fail;
		iova += len;
		i = j;
	}
	return dma_addr;
fail:
	iommu_unmap(mapping->domain, dma_addr, iova-dma_addr);
	__free_iova(mapping, dma_addr, size);
	return DMA_ERROR_CODE;
}

static int __iommu_remove_mapping(struct device *dev, dma_addr_t iova, size_t size)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;

	size = PAGE_ALIGN((iova & ~PAGE_MASK) + size);
	iova &= PAGE_MASK;

	iommu_unmap(mapping->domain, iova, size);
	__free_iova(mapping, iova, size);
	return 0;
}

static struct page **__iommu_get_pages(void *cpu_addr)
{
	struct vm_struct *area;

	area = find_vm_area(cpu_addr);
	if (area && (area->flags & VM_ARM_DMA_CONSISTENT))
		return area->pages;
	return NULL;
}

static void *arm_iommu_alloc_attrs(struct device *dev, size_t size,
	    dma_addr_t *handle, gfp_t gfp, struct dma_attrs *attrs)
{
	pgprot_t prot = __get_dma_pgprot(attrs, pgprot_kernel);
	struct page **pages;
	void *addr = NULL;

	*handle = DMA_ERROR_CODE;
	size = PAGE_ALIGN(size);

	pages = __iommu_alloc_buffer(dev, size, gfp);
	if (!pages)
		return NULL;

	*handle = __iommu_create_mapping(dev, pages, size);
	if (*handle == DMA_ERROR_CODE)
		goto err_buffer;

	addr = __iommu_alloc_remap(pages, size, gfp, prot,
				   __builtin_return_address(0));
	if (!addr)
		goto err_mapping;

	return addr;

err_mapping:
	__iommu_remove_mapping(dev, *handle, size);
err_buffer:
	__iommu_free_buffer(dev, pages, size);
	return NULL;
}

static int arm_iommu_mmap_attrs(struct device *dev, struct vm_area_struct *vma,
		    void *cpu_addr, dma_addr_t dma_addr, size_t size,
		    struct dma_attrs *attrs)
{
	unsigned long uaddr = vma->vm_start;
	unsigned long usize = vma->vm_end - vma->vm_start;
	struct page **pages = __iommu_get_pages(cpu_addr);

	vma->vm_page_prot = __get_dma_pgprot(attrs, vma->vm_page_prot);

	if (!pages)
		return -ENXIO;

	do {
		int ret = vm_insert_page(vma, uaddr, *pages++);
		if (ret) {
			pr_err("Remapping memory failed: %d\n", ret);
			return ret;
		}
		uaddr += PAGE_SIZE;
		usize -= PAGE_SIZE;
	} while (usize > 0);

	return 0;
}

void arm_iommu_free_attrs(struct device *dev, size_t size, void *cpu_addr,
			  dma_addr_t handle, struct dma_attrs *attrs)
{
	struct page **pages = __iommu_get_pages(cpu_addr);
	size = PAGE_ALIGN(size);

	if (!pages) {
		WARN(1, "trying to free invalid coherent area: %p\n", cpu_addr);
		return;
	}

	unmap_kernel_range((unsigned long)cpu_addr, size);
	vunmap(cpu_addr);

	__iommu_remove_mapping(dev, handle, size);
	__iommu_free_buffer(dev, pages, size);
}

static int __map_sg_chunk(struct device *dev, struct scatterlist *sg,
			  size_t size, dma_addr_t *handle,
			  enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova, iova_base;
	int ret = 0;
	unsigned int count;
	struct scatterlist *s;

	size = PAGE_ALIGN(size);
	*handle = DMA_ERROR_CODE;

	iova_base = iova = __alloc_iova(mapping, size);
	if (iova == DMA_ERROR_CODE)
		return -ENOMEM;

	for (count = 0, s = sg; count < (size >> PAGE_SHIFT); s = sg_next(s)) {
		phys_addr_t phys = page_to_phys(sg_page(s));
		unsigned int len = PAGE_ALIGN(s->offset + s->length);

		if (!arch_is_coherent())
			__dma_page_cpu_to_dev(sg_page(s), s->offset, s->length, dir);

		ret = iommu_map(mapping->domain, iova, phys, len, 0);
		if (ret < 0)
			goto fail;
		count += len >> PAGE_SHIFT;
		iova += len;
	}
	*handle = iova_base;

	return 0;
fail:
	iommu_unmap(mapping->domain, iova_base, count * PAGE_SIZE);
	__free_iova(mapping, iova_base, size);
	return ret;
}

int arm_iommu_map_sg(struct device *dev, struct scatterlist *sg, int nents,
		     enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct scatterlist *s = sg, *dma = sg, *start = sg;
	int i, count = 0;
	unsigned int offset = s->offset;
	unsigned int size = s->offset + s->length;
	unsigned int max = dma_get_max_seg_size(dev);

	for (i = 1; i < nents; i++) {
		s = sg_next(s);

		s->dma_address = DMA_ERROR_CODE;
		s->dma_length = 0;

		if (s->offset || (size & ~PAGE_MASK) || size + s->length > max) {
			if (__map_sg_chunk(dev, start, size, &dma->dma_address,
			    dir) < 0)
				goto bad_mapping;

			dma->dma_address += offset;
			dma->dma_length = size - offset;

			size = offset = s->offset;
			start = s;
			dma = sg_next(dma);
			count += 1;
		}
		size += s->length;
	}
	if (__map_sg_chunk(dev, start, size, &dma->dma_address, dir) < 0)
		goto bad_mapping;

	dma->dma_address += offset;
	dma->dma_length = size - offset;

	return count+1;

bad_mapping:
	for_each_sg(sg, s, count, i)
		__iommu_remove_mapping(dev, sg_dma_address(s), sg_dma_len(s));
	return 0;
}

void arm_iommu_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,
			enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		if (sg_dma_len(s))
			__iommu_remove_mapping(dev, sg_dma_address(s),
					       sg_dma_len(s));
		if (!arch_is_coherent())
			__dma_page_dev_to_cpu(sg_page(s), s->offset,
					      s->length, dir);
	}
}

void arm_iommu_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		if (!arch_is_coherent())
			__dma_page_dev_to_cpu(sg_page(s), s->offset, s->length, dir);

}

void arm_iommu_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		if (!arch_is_coherent())
			__dma_page_cpu_to_dev(sg_page(s), s->offset, s->length, dir);
}


static dma_addr_t arm_iommu_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size, enum dma_data_direction dir,
	     struct dma_attrs *attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t dma_addr;
	int ret, len = PAGE_ALIGN(size + offset);

	if (!arch_is_coherent())
		__dma_page_cpu_to_dev(page, offset, size, dir);

	dma_addr = __alloc_iova(mapping, len);
	if (dma_addr == DMA_ERROR_CODE)
		return dma_addr;

	ret = iommu_map(mapping->domain, dma_addr, page_to_phys(page), len, 0);
	if (ret < 0)
		goto fail;

	return dma_addr + offset;
fail:
	__free_iova(mapping, dma_addr, len);
	return DMA_ERROR_CODE;
}

static void arm_iommu_unmap_page(struct device *dev, dma_addr_t handle,
		size_t size, enum dma_data_direction dir,
		struct dma_attrs *attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = handle & PAGE_MASK;
	struct page *page = phys_to_page(iommu_iova_to_phys(mapping->domain, iova));
	int offset = handle & ~PAGE_MASK;
	int len = PAGE_ALIGN(size + offset);

	if (!iova)
		return;

	if (!arch_is_coherent())
		__dma_page_dev_to_cpu(page, offset, size, dir);

	iommu_unmap(mapping->domain, iova, len);
	__free_iova(mapping, iova, len);
}

static void arm_iommu_sync_single_for_cpu(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = handle & PAGE_MASK;
	struct page *page = phys_to_page(iommu_iova_to_phys(mapping->domain, iova));
	unsigned int offset = handle & ~PAGE_MASK;

	if (!iova)
		return;

	if (!arch_is_coherent())
		__dma_page_dev_to_cpu(page, offset, size, dir);
}

static void arm_iommu_sync_single_for_device(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = handle & PAGE_MASK;
	struct page *page = phys_to_page(iommu_iova_to_phys(mapping->domain, iova));
	unsigned int offset = handle & ~PAGE_MASK;

	if (!iova)
		return;

	__dma_page_cpu_to_dev(page, offset, size, dir);
}

struct dma_map_ops iommu_ops = {
	.alloc		= arm_iommu_alloc_attrs,
	.free		= arm_iommu_free_attrs,
	.mmap		= arm_iommu_mmap_attrs,

	.map_page		= arm_iommu_map_page,
	.unmap_page		= arm_iommu_unmap_page,
	.sync_single_for_cpu	= arm_iommu_sync_single_for_cpu,
	.sync_single_for_device	= arm_iommu_sync_single_for_device,

	.map_sg			= arm_iommu_map_sg,
	.unmap_sg		= arm_iommu_unmap_sg,
	.sync_sg_for_cpu	= arm_iommu_sync_sg_for_cpu,
	.sync_sg_for_device	= arm_iommu_sync_sg_for_device,
};

struct dma_iommu_mapping *
arm_iommu_create_mapping(struct bus_type *bus, dma_addr_t base, size_t size,
			 int order)
{
	unsigned int count = size >> (PAGE_SHIFT + order);
	unsigned int bitmap_size = BITS_TO_LONGS(count) * sizeof(long);
	struct dma_iommu_mapping *mapping;
	int err = -ENOMEM;

	if (!count)
		return ERR_PTR(-EINVAL);

	mapping = kzalloc(sizeof(struct dma_iommu_mapping), GFP_KERNEL);
	if (!mapping)
		goto err;

	mapping->bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!mapping->bitmap)
		goto err2;

	mapping->base = base;
	mapping->bits = BITS_PER_BYTE * bitmap_size;
	mapping->order = order;
	spin_lock_init(&mapping->lock);

	mapping->domain = iommu_domain_alloc(bus);
	if (!mapping->domain)
		goto err3;

	kref_init(&mapping->kref);
	return mapping;
err3:
	kfree(mapping->bitmap);
err2:
	kfree(mapping);
err:
	return ERR_PTR(err);
}

static void release_iommu_mapping(struct kref *kref)
{
	struct dma_iommu_mapping *mapping =
		container_of(kref, struct dma_iommu_mapping, kref);

	iommu_domain_free(mapping->domain);
	kfree(mapping->bitmap);
	kfree(mapping);
}

void arm_iommu_release_mapping(struct dma_iommu_mapping *mapping)
{
	if (mapping)
		kref_put(&mapping->kref, release_iommu_mapping);
}

int arm_iommu_attach_device(struct device *dev,
			    struct dma_iommu_mapping *mapping)
{
	int err;

	err = iommu_attach_device(mapping->domain, dev);
	if (err)
		return err;

	kref_get(&mapping->kref);
	dev->archdata.mapping = mapping;
	set_dma_ops(dev, &iommu_ops);

	pr_info("Attached IOMMU controller to %s device.\n", dev_name(dev));
	return 0;
}

#endif
