/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __IO_PGTABLE_H
#define __IO_PGTABLE_H

#include <linux/bitops.h>
#include <linux/iommu.h>

/*
 * Public API for use by IOMMU drivers
 */
enum io_pgtable_fmt {
	ARM_32_LPAE_S1,
	ARM_32_LPAE_S2,
	ARM_64_LPAE_S1,
	ARM_64_LPAE_S2,
	ARM_V7S,
	ARM_MALI_LPAE,
	AMD_IOMMU_V1,
	AMD_IOMMU_V2,
	APPLE_DART,
	APPLE_DART2,
	IO_PGTABLE_NUM_FMTS,
};

/**
 * struct iommu_flush_ops - IOMMU callbacks for TLB and page table management.
 *
 * @tlb_flush_all:  Synchronously invalidate the entire TLB context.
 * @tlb_flush_walk: Synchronously invalidate all intermediate TLB state
 *                  (sometimes referred to as the "walk cache") for a virtual
 *                  address range.
 * @tlb_add_page:   Optional callback to queue up leaf TLB invalidation for a
 *                  single page.  IOMMUs that cannot batch TLB invalidation
 *                  operations efficiently will typically issue them here, but
 *                  others may decide to update the iommu_iotlb_gather structure
 *                  and defer the invalidation until iommu_iotlb_sync() instead.
 *
 * Note that these can all be called in atomic context and must therefore
 * not block.
 */
struct iommu_flush_ops {
	void (*tlb_flush_all)(void *cookie);
	void (*tlb_flush_walk)(unsigned long iova, size_t size, size_t granule,
			       void *cookie);
	void (*tlb_add_page)(struct iommu_iotlb_gather *gather,
			     unsigned long iova, size_t granule, void *cookie);
};

/**
 * struct io_pgtable_cfg - Configuration data for a set of page tables.
 *
 * @quirks:        A bitmap of hardware quirks that require some special
 *                 action by the low-level page table allocator.
 * @pgsize_bitmap: A bitmap of page sizes supported by this set of page
 *                 tables.
 * @ias:           Input address (iova) size, in bits.
 * @oas:           Output address (paddr) size, in bits.
 * @coherent_walk  A flag to indicate whether or not page table walks made
 *                 by the IOMMU are coherent with the CPU caches.
 * @tlb:           TLB management callbacks for this set of tables.
 * @iommu_dev:     The device representing the DMA configuration for the
 *                 page table walker.
 */
struct io_pgtable_cfg {
	/*
	 * IO_PGTABLE_QUIRK_ARM_NS: (ARM formats) Set NS and NSTABLE bits in
	 *	stage 1 PTEs, for hardware which insists on validating them
	 *	even in	non-secure state where they should normally be ignored.
	 *
	 * IO_PGTABLE_QUIRK_NO_PERMS: Ignore the IOMMU_READ, IOMMU_WRITE and
	 *	IOMMU_NOEXEC flags and map everything with full access, for
	 *	hardware which does not implement the permissions of a given
	 *	format, and/or requires some format-specific default value.
	 *
	 * IO_PGTABLE_QUIRK_ARM_MTK_EXT: (ARM v7s format) MediaTek IOMMUs extend
	 *	to support up to 35 bits PA where the bit32, bit33 and bit34 are
	 *	encoded in the bit9, bit4 and bit5 of the PTE respectively.
	 *
	 * IO_PGTABLE_QUIRK_ARM_MTK_TTBR_EXT: (ARM v7s format) MediaTek IOMMUs
	 *	extend the translation table base support up to 35 bits PA, the
	 *	encoding format is same with IO_PGTABLE_QUIRK_ARM_MTK_EXT.
	 *
	 * IO_PGTABLE_QUIRK_ARM_TTBR1: (ARM LPAE format) Configure the table
	 *	for use in the upper half of a split address space.
	 *
	 * IO_PGTABLE_QUIRK_ARM_OUTER_WBWA: Override the outer-cacheability
	 *	attributes set in the TCR for a non-coherent page-table walker.
	 *
	 * IO_PGTABLE_QUIRK_ARM_HD: Enables dirty tracking in stage 1 pagetable.
	 * IO_PGTABLE_QUIRK_ARM_S2FWB: Use the FWB format for the MemAttrs bits
	 *
	 * IO_PGTABLE_QUIRK_NO_WARN: Do not WARN_ON() on conflicting
	 *	mappings, but silently return -EEXISTS.  Normally an attempt
	 *	to map over an existing mapping would indicate some sort of
	 *	kernel bug, which would justify the WARN_ON().  But for GPU
	 *	drivers, this could be under control of userspace.  Which
	 *	deserves an error return, but not to spam dmesg.
	 */
	#define IO_PGTABLE_QUIRK_ARM_NS			BIT(0)
	#define IO_PGTABLE_QUIRK_NO_PERMS		BIT(1)
	#define IO_PGTABLE_QUIRK_ARM_MTK_EXT		BIT(3)
	#define IO_PGTABLE_QUIRK_ARM_MTK_TTBR_EXT	BIT(4)
	#define IO_PGTABLE_QUIRK_ARM_TTBR1		BIT(5)
	#define IO_PGTABLE_QUIRK_ARM_OUTER_WBWA		BIT(6)
	#define IO_PGTABLE_QUIRK_ARM_HD			BIT(7)
	#define IO_PGTABLE_QUIRK_ARM_S2FWB		BIT(8)
	#define IO_PGTABLE_QUIRK_NO_WARN		BIT(9)
	unsigned long			quirks;
	unsigned long			pgsize_bitmap;
	unsigned int			ias;
	unsigned int			oas;
	bool				coherent_walk;
	const struct iommu_flush_ops	*tlb;
	struct device			*iommu_dev;

	/**
	 * @alloc: Custom page allocator.
	 *
	 * Optional hook used to allocate page tables. If this function is NULL,
	 * @free must be NULL too.
	 *
	 * Memory returned should be zeroed and suitable for dma_map_single() and
	 * virt_to_phys().
	 *
	 * Not all formats support custom page allocators. Before considering
	 * passing a non-NULL value, make sure the chosen page format supports
	 * this feature.
	 */
	void *(*alloc)(void *cookie, size_t size, gfp_t gfp);

	/**
	 * @free: Custom page de-allocator.
	 *
	 * Optional hook used to free page tables allocated with the @alloc
	 * hook. Must be non-NULL if @alloc is not NULL, must be NULL
	 * otherwise.
	 */
	void (*free)(void *cookie, void *pages, size_t size);

	/* Low-level data specific to the table format */
	union {
		struct {
			u64	ttbr;
			struct {
				u32	ips:3;
				u32	tg:2;
				u32	sh:2;
				u32	orgn:2;
				u32	irgn:2;
				u32	tsz:6;
			}	tcr;
			u64	mair;
		} arm_lpae_s1_cfg;

		struct {
			u64	vttbr;
			struct {
				u32	ps:3;
				u32	tg:2;
				u32	sh:2;
				u32	orgn:2;
				u32	irgn:2;
				u32	sl:2;
				u32	tsz:6;
			}	vtcr;
		} arm_lpae_s2_cfg;

		struct {
			u32	ttbr;
			u32	tcr;
			u32	nmrr;
			u32	prrr;
		} arm_v7s_cfg;

		struct {
			u64	transtab;
			u64	memattr;
		} arm_mali_lpae_cfg;

		struct {
			u64 ttbr[4];
			u32 n_ttbrs;
		} apple_dart_cfg;

		struct {
			int nid;
		} amd;
	};
};

/**
 * struct arm_lpae_io_pgtable_walk_data - information from a pgtable walk
 *
 * @ptes:     The recorded PTE values from the walk
 */
struct arm_lpae_io_pgtable_walk_data {
	u64 ptes[4];
};

/**
 * struct io_pgtable_ops - Page table manipulation API for IOMMU drivers.
 *
 * @map_pages:    Map a physically contiguous range of pages of the same size.
 * @unmap_pages:  Unmap a range of virtually contiguous pages of the same size.
 * @iova_to_phys: Translate iova to physical address.
 * @pgtable_walk: (optional) Perform a page table walk for a given iova.
 *
 * These functions map directly onto the iommu_ops member functions with
 * the same names.
 */
struct io_pgtable_ops {
	int (*map_pages)(struct io_pgtable_ops *ops, unsigned long iova,
			 phys_addr_t paddr, size_t pgsize, size_t pgcount,
			 int prot, gfp_t gfp, size_t *mapped);
	size_t (*unmap_pages)(struct io_pgtable_ops *ops, unsigned long iova,
			      size_t pgsize, size_t pgcount,
			      struct iommu_iotlb_gather *gather);
	phys_addr_t (*iova_to_phys)(struct io_pgtable_ops *ops,
				    unsigned long iova);
	int (*pgtable_walk)(struct io_pgtable_ops *ops, unsigned long iova, void *wd);
	int (*read_and_clear_dirty)(struct io_pgtable_ops *ops,
				    unsigned long iova, size_t size,
				    unsigned long flags,
				    struct iommu_dirty_bitmap *dirty);
};

/**
 * alloc_io_pgtable_ops() - Allocate a page table allocator for use by an IOMMU.
 *
 * @fmt:    The page table format.
 * @cfg:    The page table configuration. This will be modified to represent
 *          the configuration actually provided by the allocator (e.g. the
 *          pgsize_bitmap may be restricted).
 * @cookie: An opaque token provided by the IOMMU driver and passed back to
 *          the callback routines in cfg->tlb.
 */
struct io_pgtable_ops *alloc_io_pgtable_ops(enum io_pgtable_fmt fmt,
					    struct io_pgtable_cfg *cfg,
					    void *cookie);

/**
 * free_io_pgtable_ops() - Free an io_pgtable_ops structure. The caller
 *                         *must* ensure that the page table is no longer
 *                         live, but the TLB can be dirty.
 *
 * @ops: The ops returned from alloc_io_pgtable_ops.
 */
void free_io_pgtable_ops(struct io_pgtable_ops *ops);


/*
 * Internal structures for page table allocator implementations.
 */

/**
 * struct io_pgtable - Internal structure describing a set of page tables.
 *
 * @fmt:    The page table format.
 * @cookie: An opaque token provided by the IOMMU driver and passed back to
 *          any callback routines.
 * @cfg:    A copy of the page table configuration.
 * @ops:    The page table operations in use for this set of page tables.
 */
struct io_pgtable {
	enum io_pgtable_fmt	fmt;
	void			*cookie;
	struct io_pgtable_cfg	cfg;
	struct io_pgtable_ops	ops;
};

#define io_pgtable_ops_to_pgtable(x) container_of((x), struct io_pgtable, ops)

static inline void io_pgtable_tlb_flush_all(struct io_pgtable *iop)
{
	if (iop->cfg.tlb && iop->cfg.tlb->tlb_flush_all)
		iop->cfg.tlb->tlb_flush_all(iop->cookie);
}

static inline void
io_pgtable_tlb_flush_walk(struct io_pgtable *iop, unsigned long iova,
			  size_t size, size_t granule)
{
	if (iop->cfg.tlb && iop->cfg.tlb->tlb_flush_walk)
		iop->cfg.tlb->tlb_flush_walk(iova, size, granule, iop->cookie);
}

static inline void
io_pgtable_tlb_add_page(struct io_pgtable *iop,
			struct iommu_iotlb_gather * gather, unsigned long iova,
			size_t granule)
{
	if (iop->cfg.tlb && iop->cfg.tlb->tlb_add_page)
		iop->cfg.tlb->tlb_add_page(gather, iova, granule, iop->cookie);
}

/**
 * enum io_pgtable_caps - IO page table backend capabilities.
 */
enum io_pgtable_caps {
	/** @IO_PGTABLE_CAP_CUSTOM_ALLOCATOR: Backend accepts custom page table allocators. */
	IO_PGTABLE_CAP_CUSTOM_ALLOCATOR = BIT(0),
};

/**
 * struct io_pgtable_init_fns - Alloc/free a set of page tables for a
 *                              particular format.
 *
 * @alloc: Allocate a set of page tables described by cfg.
 * @free:  Free the page tables associated with iop.
 * @caps:  Combination of @io_pgtable_caps flags encoding the backend capabilities.
 */
struct io_pgtable_init_fns {
	struct io_pgtable *(*alloc)(struct io_pgtable_cfg *cfg, void *cookie);
	void (*free)(struct io_pgtable *iop);
	u32 caps;
};

extern struct io_pgtable_init_fns io_pgtable_arm_32_lpae_s1_init_fns;
extern struct io_pgtable_init_fns io_pgtable_arm_32_lpae_s2_init_fns;
extern struct io_pgtable_init_fns io_pgtable_arm_64_lpae_s1_init_fns;
extern struct io_pgtable_init_fns io_pgtable_arm_64_lpae_s2_init_fns;
extern struct io_pgtable_init_fns io_pgtable_arm_v7s_init_fns;
extern struct io_pgtable_init_fns io_pgtable_arm_mali_lpae_init_fns;
extern struct io_pgtable_init_fns io_pgtable_amd_iommu_v1_init_fns;
extern struct io_pgtable_init_fns io_pgtable_amd_iommu_v2_init_fns;
extern struct io_pgtable_init_fns io_pgtable_apple_dart_init_fns;

#endif /* __IO_PGTABLE_H */
