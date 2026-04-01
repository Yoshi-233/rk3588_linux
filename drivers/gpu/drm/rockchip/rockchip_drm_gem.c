// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author:Mark Yao <mark.yao@rock-chips.com>
 */

#include <linux/dma-buf-cache.h>
#include <linux/iommu.h>
#include <linux/vmalloc.h>

#include <drm/drm.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_vma_manager.h>

#include <linux/genalloc.h>
#include <linux/iommu.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/rockchip/rockchip_sip.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_gem.h"

static u32 bank_bit_first = 12;
static u32 bank_bit_mask = 0x7;

struct page_info {
	struct page *page;
	struct list_head list;
};

#define PG_ROUND       8

static int rockchip_gem_iommu_map(struct rockchip_gem_object *rk_obj)
{
	struct drm_device *drm = rk_obj->base.dev;
	struct rockchip_drm_private *private = drm->dev_private;
	int prot = IOMMU_READ | IOMMU_WRITE;
	ssize_t ret;

	mutex_lock(&private->mm_lock);
	ret = drm_mm_insert_node_generic(&private->mm, &rk_obj->mm,
					 rk_obj->base.size, PAGE_SIZE,
					 0, 0);
	mutex_unlock(&private->mm_lock);

	if (ret < 0) {
		DRM_ERROR("out of I/O virtual memory: %zd\n", ret);
		return ret;
	}

	rk_obj->dma_addr = rk_obj->mm.start;

	ret = iommu_map_sgtable(private->domain, rk_obj->dma_addr, rk_obj->sgt,
				prot);
	if (ret < rk_obj->base.size) {
		DRM_ERROR("failed to map buffer: size=%zd request_size=%zd\n",
			  ret, rk_obj->base.size);
		ret = -ENOMEM;
		goto err_remove_node;
	}

	iommu_flush_iotlb_all(private->domain);

	rk_obj->size = ret;

	return 0;

err_remove_node:
	mutex_lock(&private->mm_lock);
	drm_mm_remove_node(&rk_obj->mm);
	mutex_unlock(&private->mm_lock);

	return ret;
}

static int rockchip_gem_iommu_unmap(struct rockchip_gem_object *rk_obj)
{
	struct drm_device *drm = rk_obj->base.dev;
	struct rockchip_drm_private *private = drm->dev_private;

	iommu_unmap(private->domain, rk_obj->dma_addr, rk_obj->size);

	mutex_lock(&private->mm_lock);

	drm_mm_remove_node(&rk_obj->mm);

	mutex_unlock(&private->mm_lock);

	return 0;
}

static void rockchip_gem_free_list(struct list_head lists[])
{
	struct page_info *info, *tmp_info;
	int i;

	for (i = 0; i < PG_ROUND; i++) {
		list_for_each_entry_safe(info, tmp_info, &lists[i], list) {
			list_del(&info->list);
			kfree(info);
		}
	}
}

void rockchip_gem_get_ddr_info(void)
{
	struct dram_addrmap_info *ddr_map_info;

	ddr_map_info = sip_smc_get_dram_map();
	if (ddr_map_info) {
		bank_bit_first = ddr_map_info->bank_bit_first;
		bank_bit_mask = ddr_map_info->bank_bit_mask;
	}
}

static int rockchip_gem_get_pages(struct rockchip_gem_object *rk_obj)
{
	struct drm_device *drm = rk_obj->base.dev;
	int ret, i;
	struct scatterlist *s;
	unsigned int cur_page;
	struct page **pages, **dst_pages;
	int j;
	int n_pages;
	unsigned long chunk_pages;
	unsigned long remain;
	struct list_head lists[PG_ROUND];
	dma_addr_t phys;
	int end = 0;
	unsigned int bit_index;
	unsigned int block_index[PG_ROUND] = {0};
	struct page_info *info;
	unsigned int maximum;

	for (i = 0; i < PG_ROUND; i++)
		INIT_LIST_HEAD(&lists[i]);

	pages = drm_gem_get_pages(&rk_obj->base);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	rk_obj->pages = pages;

	rk_obj->num_pages = rk_obj->base.size >> PAGE_SHIFT;

	n_pages = rk_obj->num_pages;

	dst_pages = __vmalloc(sizeof(struct page *) * n_pages,
			GFP_KERNEL | __GFP_HIGHMEM);
	if (!dst_pages) {
		ret = -ENOMEM;
		goto err_put_pages;
	}

	DRM_DEBUG_KMS("bank_bit_first = 0x%x, bank_bit_mask = 0x%x\n",
		      bank_bit_first, bank_bit_mask);

	cur_page = 0;
	remain = n_pages;
	/* look for the end of the current chunk */
	while (remain) {
		for (j = cur_page + 1; j < n_pages; ++j) {
			if (page_to_pfn(pages[j]) !=
				page_to_pfn(pages[j - 1]) + 1)
				break;
		}

		chunk_pages = j - cur_page;
		if (chunk_pages >= PG_ROUND) {
			for (i = 0; i < chunk_pages; i++)
				dst_pages[end + i] = pages[cur_page + i];
			end += chunk_pages;
		} else {
			for (i = 0; i < chunk_pages; i++) {
				info = kmalloc(sizeof(*info), GFP_KERNEL);
				if (!info) {
					ret = -ENOMEM;
					goto err_put_list;
				}

				INIT_LIST_HEAD(&info->list);
				info->page = pages[cur_page + i];
				phys = page_to_phys(info->page);
				bit_index = ((phys >> bank_bit_first) & bank_bit_mask) % PG_ROUND;
				list_add_tail(&info->list, &lists[bit_index]);
				block_index[bit_index]++;
			}
		}

		cur_page = j;
		remain -= chunk_pages;
	}

	maximum = block_index[0];
	for (i = 1; i < PG_ROUND; i++)
		maximum = max(maximum, block_index[i]);

	for (i = 0; i < maximum; i++) {
		for (j = 0; j < PG_ROUND; j++) {
			if (!list_empty(&lists[j])) {
				struct page_info *info;

				info = list_first_entry(&lists[j],
							struct page_info, list);
				dst_pages[end++] = info->page;
				list_del(&info->list);
				kfree(info);
			}
		}
	}

	DRM_DEBUG_KMS("%s, %d, end = %d, n_pages = %d\n", __func__, __LINE__,
			end, n_pages);
	rk_obj->sgt = drm_prime_pages_to_sg(rk_obj->base.dev,
					    dst_pages, rk_obj->num_pages);
	if (IS_ERR(rk_obj->sgt)) {
		ret = PTR_ERR(rk_obj->sgt);
		goto err_put_list;
	}

	rk_obj->pages = dst_pages;

	/*
	 * Fake up the SG table so that dma_sync_sg_for_device() can be used
	 * to flush the pages associated with it.
	 *
	 * TODO: Replace this by drm_clflush_sg() once it can be implemented
	 * without relying on symbols that are not exported.
	 */
	for_each_sgtable_sg(rk_obj->sgt, s, i)
		sg_dma_address(s) = sg_phys(s);

	dma_sync_sgtable_for_device(drm->dev, rk_obj->sgt, DMA_TO_DEVICE);

	kvfree(pages);

	return 0;

err_put_list:
	rockchip_gem_free_list(lists);
	kvfree(dst_pages);
err_put_pages:
	drm_gem_put_pages(&rk_obj->base, rk_obj->pages, false, false);
	return ret;
}

static void rockchip_gem_put_pages(struct rockchip_gem_object *rk_obj)
{
	sg_free_table(rk_obj->sgt);
	kfree(rk_obj->sgt);
	drm_gem_put_pages(&rk_obj->base, rk_obj->pages, true, true);
}

static inline void *drm_calloc_large(size_t nmemb, size_t size);
static inline void drm_free_large(void *ptr);
static void rockchip_gem_free_dma(struct rockchip_gem_object *rk_obj);
static int rockchip_gem_alloc_dma(struct rockchip_gem_object *rk_obj,
				  bool alloc_kmap)
{
	struct drm_gem_object *obj = &rk_obj->base;
	struct drm_device *drm = obj->dev;
	struct sg_table *sgt;
	int ret, i;
	struct scatterlist *s;

	rk_obj->dma_attrs = DMA_ATTR_WRITE_COMBINE;

	if (!alloc_kmap)
		rk_obj->dma_attrs |= DMA_ATTR_NO_KERNEL_MAPPING;

	rk_obj->kvaddr = dma_alloc_attrs(drm->dev, obj->size,
					 &rk_obj->dma_handle, GFP_KERNEL,
					 rk_obj->dma_attrs);
	if (!rk_obj->kvaddr) {
		DRM_ERROR("failed to allocate %zu byte dma buffer", obj->size);
		return -ENOMEM;
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto err_dma_free;
	}

	ret = dma_get_sgtable_attrs(drm->dev, sgt, rk_obj->kvaddr,
				    rk_obj->dma_handle, obj->size,
				    rk_obj->dma_attrs);
	if (ret) {
		DRM_ERROR("failed to allocate sgt, %d\n", ret);
		goto err_sgt_free;
	}

	for_each_sg(sgt->sgl, s, sgt->nents, i)
		sg_dma_address(s) = sg_phys(s);

	rk_obj->num_pages = rk_obj->base.size >> PAGE_SHIFT;

	rk_obj->pages = drm_calloc_large(rk_obj->num_pages,
					 sizeof(*rk_obj->pages));
	if (!rk_obj->pages) {
		ret = -ENOMEM;
		DRM_ERROR("failed to allocate pages.\n");
		goto err_sg_table_free;
	}

	if (drm_prime_sg_to_page_addr_arrays(sgt, rk_obj->pages, NULL,
					     rk_obj->num_pages)) {
		DRM_ERROR("invalid sgtable.\n");
		ret = -EINVAL;
		goto err_page_free;
	}

	rk_obj->sgt = sgt;

	return 0;

err_page_free:
	drm_free_large(rk_obj->pages);
err_sg_table_free:
	sg_free_table(sgt);
err_sgt_free:
	kfree(sgt);
err_dma_free:
	dma_free_attrs(drm->dev, obj->size, rk_obj->kvaddr,
		       rk_obj->dma_handle, rk_obj->dma_attrs);

	return ret;
}

static inline void *drm_calloc_large(size_t nmemb, size_t size)
{
	if (size != 0 && nmemb > SIZE_MAX / size)
		return NULL;

	if (size * nmemb <= PAGE_SIZE)
		return kcalloc(nmemb, size, GFP_KERNEL);

	return __vmalloc(size * nmemb,
			 GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO);
}

static inline void drm_free_large(void *ptr)
{
	kvfree(ptr);
}

static int rockchip_gem_alloc_secure(struct rockchip_gem_object *rk_obj)
{
	struct drm_gem_object *obj = &rk_obj->base;
	struct drm_device *drm = obj->dev;
	struct rockchip_drm_private *private = drm->dev_private;
	unsigned long paddr;
	struct sg_table *sgt;
	int ret = 0, i;

	if (!private->secure_buffer_pool) {
		DRM_ERROR("No secure buffer pool found\n");
		return -ENOMEM;
	}

	paddr = gen_pool_alloc(private->secure_buffer_pool, rk_obj->base.size);
	if (!paddr) {
		DRM_ERROR("failed to allocate secure buffer\n");
		return -ENOMEM;
	}

	rk_obj->dma_handle = paddr;
	rk_obj->num_pages = rk_obj->base.size >> PAGE_SHIFT;

	rk_obj->pages = drm_calloc_large(rk_obj->num_pages,
					 sizeof(*rk_obj->pages));
	if (!rk_obj->pages) {
		DRM_ERROR("failed to allocate pages.\n");
		ret = -ENOMEM;
		goto err_buf_free;
	}

	i = 0;
	while (i < rk_obj->num_pages) {
		rk_obj->pages[i] = phys_to_page(paddr);
		paddr += PAGE_SIZE;
		i++;
	}
	sgt = drm_prime_pages_to_sg(obj->dev, rk_obj->pages, rk_obj->num_pages);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_free_pages;
	}

	rk_obj->sgt = sgt;

	return 0;

err_free_pages:
	drm_free_large(rk_obj->pages);
err_buf_free:
	gen_pool_free(private->secure_buffer_pool, paddr, rk_obj->base.size);

	return ret;
}

static void rockchip_gem_free_secure(struct rockchip_gem_object *rk_obj)
{
	struct drm_gem_object *obj = &rk_obj->base;
	struct drm_device *drm = obj->dev;
	struct rockchip_drm_private *private = drm->dev_private;

	drm_free_large(rk_obj->pages);
	sg_free_table(rk_obj->sgt);
	kfree(rk_obj->sgt);
	gen_pool_free(private->secure_buffer_pool, rk_obj->dma_handle,
		      rk_obj->base.size);
}

static inline bool is_vop_enabled(void)
{
	return (IS_ENABLED(CONFIG_ROCKCHIP_VOP) || IS_ENABLED(CONFIG_ROCKCHIP_VOP2));
}

/* 真正完成图形缓冲区的物理内存分配、内核虚拟地址映射、IOMMU 外设地址映射、硬件访问就绪准备 */
static int rockchip_gem_alloc_buf(struct rockchip_gem_object *rk_obj,
				  bool alloc_kmap)
{
	struct drm_gem_object *obj = &rk_obj->base;
	struct drm_device *drm = obj->dev;
	struct rockchip_drm_private *private = drm->dev_private;
	int ret = 0;

	/*
		1.!private->domain：平台未开启 IOMMU，同时is_vop_enabled()为真
			（内核编译了 VOP/VOP2 显示控制器驱动），此时显示控制器没有
			 IOMMU 的地址翻译能力，只能访问物理地址连续的内存。
		2.强制给rk_obj->flags加上ROCKCHIP_BO_CONTIG标志，无论用户是否传入该标志，
			都强制走连续内存分配路径，避免显示控制器访问内存失败导致黑屏。
		3.is_vop_enabled()：仅当显示控制器驱动开启时才强制连续内存，纯 GPU 渲染场景无需该限制。
	*/
	if (!private->domain && is_vop_enabled())
		rk_obj->flags |= ROCKCHIP_BO_CONTIG;

	if (rk_obj->flags & ROCKCHIP_BO_SECURE) {
		/*
			触发条件：用户传入的 flags 设置了ROCKCHIP_BO_SECURE，
				用于 DRM 数字版权保护、高清付费内容、安全生物识别的安全显示场景。
			逐行逻辑：
			1. rk_obj->buf_type = ROCKCHIP_GEM_BUF_TYPE_SECURE：标记 buffer 类型为安全内存，
				后续释放、映射都会走安全内存专属逻辑。
			2. rk_obj->flags |= ROCKCHIP_BO_CONTIG：安全内存必须是物理连续的，强制加上连续内存标志。
			3. if (alloc_kmap) 安全校验：安全内存位于 ARM TrustZone 安全世界，
				普通世界的 Linux 内核绝对无法直接访问，如果用户要求分配内核映射，
				直接报错返回-EINVAL，符合安全隔离的硬性要求。
			4. ret = rockchip_gem_alloc_secure(rk_obj)：调用安全内存分配函数，核心逻辑：
				从提前预留的安全内存池private->secure_buffer_pool分配物理连续内存；
				填充物理地址、pages 数组、sg_table 散列表；
				完成安全内存的所有元数据初始化。
		*/
		rk_obj->buf_type = ROCKCHIP_GEM_BUF_TYPE_SECURE;
		rk_obj->flags |= ROCKCHIP_BO_CONTIG;
		if (alloc_kmap) {
			DRM_ERROR("Not allow alloc secure buffer with kmap\n");
			return -EINVAL;
		}
		ret = rockchip_gem_alloc_secure(rk_obj);
		if (ret)
			return ret;
	} else if (rk_obj->flags & ROCKCHIP_BO_CONTIG) {
		/*
			触发条件：无安全内存标志，但设置了ROCKCHIP_BO_CONTIG（用户传入 / 无 IOMMU 强制添加），
			走 CMA（连续内存分配器）路径，分配物理地址连续的内存。
			ret = rockchip_gem_alloc_dma(rk_obj, alloc_kmap)：调用 CMA 内存分配函数，核心逻辑：
				通过dma_alloc_attrs从内核 CMA 区域分配物理连续内存，
					默认带DMA_ATTR_WRITE_COMBINE写合并属性；
				若alloc_kmap=false，添加DMA_ATTR_NO_KERNEL_MAPPING标志，
					不分配内核虚拟地址，节省内核地址空间；
				生成dma_handle（物理地址）、kvaddr（内核虚拟地址，按需分配）；
				生成 sg_table 散列表、填充 pages 数组，为后续 IOMMU 映射、dma-buf 跨进程共享做准备。
		*/
		rk_obj->buf_type = ROCKCHIP_GEM_BUF_TYPE_CMA;
		ret = rockchip_gem_alloc_dma(rk_obj, alloc_kmap);
		if (ret)
			return ret;
	} else {
		/*
			无安全标志、无连续内存标志（平台开启 IOMMU 时的默认路径），
			内存来自之前rockchip_gem_alloc_object创建的匿名 tmpfs 文件，
			分配离散的物理页，通过 IOMMU 映射给外设使用。
		*/
		rk_obj->buf_type = ROCKCHIP_GEM_BUF_TYPE_SHMEM;
		/*
			ret = rockchip_gem_get_pages(rk_obj)：SHMEM 路径的核心，
				真正为 tmpfs 文件分配物理内存，核心逻辑：
			调用drm_gem_get_pages，从 tmpfs 文件分配所有需要的物理页，
				存入rk_obj->pages数组，完成物理内存的实际分配；
			执行瑞芯微专属的DDR Bank 交错优化：把物理页打散到不同的 DDR Bank，
				避免 Bank 冲突，大幅提升内存带宽性能；
			生成 sg_table 散列表，同步 DMA 地址，为后续 IOMMU 映射做准备。
		*/
		ret = rockchip_gem_get_pages(rk_obj);
		if (ret < 0)
			return ret;

		if (alloc_kmap) {
			/*
				调用vmap，把rk_obj->pages里的离散物理页，映射成内核虚拟地址空间的连续地址，
					赋值给rk_obj->kvaddr，内核态可直接通过该地址连续访问整个 buffer；
				页属性设置为pgprot_writecombine写合并模式，和 CMA 路径保持一致，
					兼顾 CPU 读写性能和缓存一致性；
				若 vmap 失败，返回-ENOMEM，跳转到err_iommu_free错误处理分支，回滚已分配的资源。
			*/
			rk_obj->kvaddr = vmap(rk_obj->pages, rk_obj->num_pages,
					      VM_MAP,
					      pgprot_writecombine(PAGE_KERNEL));
			if (!rk_obj->kvaddr) {
				DRM_ERROR("failed to vmap() buffer\n");
				ret = -ENOMEM;
				goto err_iommu_free;
			}
		}
	}

	/*
		if (private->domain) 平台开启 IOMMU：
			调用rockchip_gem_iommu_map完成 IOMMU 映射，核心逻辑：
				从 DRM 的 MM 内存管理器分配一段连续的 IOVA 地址空间，大小和 buffer 一致；
				调用iommu_map_sgtable，把 sg_table 里的物理页（无论连续 / 离散）映射到分配的 IOVA 地址上；
				刷新 IOMMU 的 TLB，确保映射生效；
				把映射后的连续 IOVA 地址赋值给rk_obj->dma_addr，这就是外设最终使用的访问地址。
			映射失败跳转到err_free错误处理分支，回滚所有资源。
		else if (is_vop_enabled()) 无 IOMMU 且开启了显示控制器：
			无 IOMMU 时，外设只能直接访问物理地址，因此直接把rk_obj->dma_addr赋值为物理地址rk_obj->dma_handle；
			WARN_ON(!rk_obj->dma_handle)：内核警告，若物理地址为空，说明之前的连续内存分配异常，方便调试定位问题。
	*/
	if (private->domain) {
		ret = rockchip_gem_iommu_map(rk_obj);
		if (ret < 0)
			goto err_free;
	} else if (is_vop_enabled()) {
		WARN_ON(!rk_obj->dma_handle);
		rk_obj->dma_addr = rk_obj->dma_handle;
	}

	return 0;

err_iommu_free:
	if (private->domain)
		rockchip_gem_iommu_unmap(rk_obj);
err_free:
	if (rk_obj->buf_type == ROCKCHIP_GEM_BUF_TYPE_SECURE)
		rockchip_gem_free_secure(rk_obj);
	else if (rk_obj->buf_type == ROCKCHIP_GEM_BUF_TYPE_CMA)
		rockchip_gem_free_dma(rk_obj);
	else
		rockchip_gem_put_pages(rk_obj);
	return ret;
}

static void rockchip_gem_free_dma(struct rockchip_gem_object *rk_obj)
{
	struct drm_gem_object *obj = &rk_obj->base;
	struct drm_device *drm = obj->dev;

	drm_free_large(rk_obj->pages);
	sg_free_table(rk_obj->sgt);
	kfree(rk_obj->sgt);
	dma_free_attrs(drm->dev, obj->size, rk_obj->kvaddr,
		       rk_obj->dma_handle, rk_obj->dma_attrs);
}

static void rockchip_gem_free_buf(struct rockchip_gem_object *rk_obj)
{
	struct drm_device *drm = rk_obj->base.dev;
	struct rockchip_drm_private *private = drm->dev_private;

	if (private->domain)
		rockchip_gem_iommu_unmap(rk_obj);

	if (rk_obj->buf_type == ROCKCHIP_GEM_BUF_TYPE_SHMEM) {
		vunmap(rk_obj->kvaddr);
		rockchip_gem_put_pages(rk_obj);
	} else if (rk_obj->buf_type == ROCKCHIP_GEM_BUF_TYPE_SECURE) {
		rockchip_gem_free_secure(rk_obj);
	} else {
		rockchip_gem_free_dma(rk_obj);
	}
}

static int rockchip_drm_gem_object_mmap_iommu(struct drm_gem_object *obj,
					      struct vm_area_struct *vma)
{
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);
	unsigned int count = obj->size >> PAGE_SHIFT;
	unsigned long user_count = vma_pages(vma);

	if (user_count == 0)
		return -ENXIO;

	return vm_map_pages(vma, rk_obj->pages, count);
}

static int rockchip_drm_gem_object_mmap_dma(struct drm_gem_object *obj,
					    struct vm_area_struct *vma)
{
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);
	struct drm_device *drm = obj->dev;

	return dma_mmap_attrs(drm->dev, vma, rk_obj->kvaddr, rk_obj->dma_addr,
			      obj->size, rk_obj->dma_attrs);
}

static int rockchip_drm_gem_object_mmap(struct drm_gem_object *obj,
					struct vm_area_struct *vma)
{
	int ret;
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);

	/* default is wc. */
	if (rk_obj->flags & ROCKCHIP_BO_CACHABLE)
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

	/*
	 * We allocated a struct page table for rk_obj, so clear
	 * VM_PFNMAP flag that was set by drm_gem_mmap_obj()/drm_gem_mmap().
	 */
	vma->vm_flags &= ~VM_PFNMAP;

	if (rk_obj->buf_type == ROCKCHIP_GEM_BUF_TYPE_SECURE) {
		DRM_ERROR("Disallow mmap for secure buffer\n");
		ret = -EINVAL;
	} else if (rk_obj->pages) {
		ret = rockchip_drm_gem_object_mmap_iommu(obj, vma);
	} else {
		ret = rockchip_drm_gem_object_mmap_dma(obj, vma);
	}

	if (ret)
		drm_gem_vm_close(vma);

	return ret;
}

int rockchip_gem_mmap_buf(struct drm_gem_object *obj,
			  struct vm_area_struct *vma)
{
	int ret;

	ret = drm_gem_mmap_obj(obj, obj->size, vma);
	if (ret)
		return ret;

	return rockchip_drm_gem_object_mmap(obj, vma);
}

/* drm driver mmap file operations */
int rockchip_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_gem_object *obj;
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

	/*
	 * Set vm_pgoff (used as a fake buffer offset by DRM) to 0 and map the
	 * whole buffer from the start.
	 */
	vma->vm_pgoff = 0;

	obj = vma->vm_private_data;

	return rockchip_drm_gem_object_mmap(obj, vma);
}

static void rockchip_gem_release_object(struct rockchip_gem_object *rk_obj)
{
	drm_gem_object_release(&rk_obj->base);
	kfree(rk_obj);
}

/*
	创建并初始化瑞芯微定制的 GEM 对象结构体、配置内存分配规则、
		完成 DRM 标准 GEM 对象的基础初始化，为后续真正的物理内存分配（rockchip_gem_alloc_buf）做全链路的前置准备。
	关键区分：这个函数只分配「管理内存的结构体」，不分配真正的图形缓冲区物理内存，
		物理内存的分配在后续的rockchip_gem_alloc_buf中完成。
*/
static struct rockchip_gem_object *
rockchip_gem_alloc_object(struct drm_device *drm, unsigned int size,
			  unsigned int flags)
{
	/*
		Linux 内核address_space结构体指针，每个文件 inode 都对应一个地址空间，用来管理文件的页缓存。
		GEM 对象会绑定一个匿名 tmpfs 文件，这个变量就是该文件的页缓存管理结构，用来控制后续物理内存的分配规则。
	*/
	struct address_space *mapping;
	struct rockchip_gem_object *rk_obj;
	struct drm_gem_object *obj;

	/*
		CONFIG_ARM_LPAE	ARM 
			大物理地址扩展宏，开启后支持超过 4GB 的物理地址。
			瑞芯微 ARM64 平台默认开启，此时默认强制从 4GB 
			以内的 DMA32 区域分配内存，因为显示控制器 VOP、
			Mali GPU 等外设大多只支持 32 位地址寻址，避免访问不到 4GB 以上的高地址内存。
		GFP_HIGHUSER	
			图形缓冲区的标准分配标志，含义：
			1. 从用户空间可用的高端内存（HIGHMEM）分配；
			2. 分配的内存可被 mmap 到用户空间（给 Weston、Android 显示系统等应用使用）；
			3. 允许内核在内存紧张时进行回收。
		__GFP_RECLAIMABLE	
			标记内存是可回收的，内核内存管理系统在内存紧张时，
			可对该内存进行回收 / 换出，避免被当成不可回收内存长期占用，
			导致系统 OOM。
		__GFP_DMA32	
			强制从ZONE_DMA32内存区域分配，保证物理地址在 4GB 以内，
			专门给有 32 位地址寻址限制的外设使用。
	*/
#ifdef CONFIG_ARM_LPAE
	gfp_t gfp_mask = GFP_HIGHUSER | __GFP_RECLAIMABLE | __GFP_DMA32;
#else
	gfp_t gfp_mask = GFP_HIGHUSER | __GFP_RECLAIMABLE;
#endif

	if (flags & ROCKCHIP_BO_DMA32)
		gfp_mask |= __GFP_DMA32;

	size = round_up(size, PAGE_SIZE);

	rk_obj = kzalloc(sizeof(*rk_obj), GFP_KERNEL);
	if (!rk_obj)
		return ERR_PTR(-ENOMEM);

	obj = &rk_obj->base;
	
	/*
		这个接口完成 GEM 对象的核心初始化，是整个 GEM 生命周期的起点，它做的事情包括：
		1.初始化 GEM 对象的引用计数为 1，这是 GEM 对象生命周期管理的核心，引用计数归 0 时自动释放对象；
		2.绑定 GEM 对象和 DRM 驱动设备，把obj->dev设置为传入的drm设备；
		3.设置obj->size为页对齐后的缓冲区大小，记录 GEM 对象对应的内存总大小；
		4.初始化 GEM 对象的锁、链表节点、文件操作相关字段，为后续的 handle 创建、mmap 映射、dma-buf 导出共享做准备；
		5.创建 GEM 对象对应的匿名 tmpfs 文件（obj->filp），这是 DRM GEM 框架的核心设计：
			每个 GEM 对象都对应一个匿名文件，通过文件系统的页缓存机制管理图形内存，生命周期和文件完全绑定。
	*/
	drm_gem_object_init(drm, obj, size);

	/*
		file_inode(obj->filp)->i_mapping
		obj->filp：上一步drm_gem_object_init创建的匿名 tmpfs 文件的struct file指针；
		file_inode()：内核标准函数，从文件指针获取对应的 
			inode 节点（Linux 文件系统中描述文件的核心结构）；
		i_mapping：inode 对应的address_space地址空间，也就是这个匿名文件的页缓存管理结构，
			后续 GEM 缓冲区的物理内存，就是通过这个地址空间分配的。
		mapping_set_gfp_mask()
			内核标准函数，作用是覆盖地址空间默认的内存分配掩码；
			核心目的：后续内核为这个 GEM 对象分配物理内存页时，
				会强制使用我们这里设置的gfp_mask，而不是 tmpfs 的默认掩码，
				确保内存分配完全符合我们传入的flags要求（比如强制 DMA32、可回收等）。
	*/
	mapping = file_inode(obj->filp)->i_mapping;
	// 连续物理页cma不需要使用tmpfs
	// SHMEM 类型 buffer（无 ROCKCHIP_BO_CONTIG 标志）
	// 物理页完全来自这个 tmpfs 文件：rockchip_gem_get_pages 从 tmpfs 分配物理页，存入 rk_obj->pages 数组；
	// 后续 rockchip_gem_iommu_map 函数，就是把 rk_obj->pages 里的这些物理页，映射到 IOMMU 域，
	// 生成外设可访问的连续 IOVA 地址，存入 rk_obj->dma_addr，给 VOP/VDU/GPU 等外设使用；
	// 结论：这种场景下，IOMMU 映射的物理内存，100% 来自这个 tmpfs 文件。
	/*
		这两行的本质是覆盖 tmpfs 默认的 GFP 内存分配掩码。tmpfs 默认的 GFP 掩码仅为 GFP_HIGHUSER，
		无法满足瑞芯微平台的硬件需求，
		必须通过这个方式，让后续 tmpfs 分配物理页时，严格遵守你设置的分配规则（比如强制 DMA32、可回收等）。
	*/
	mapping_set_gfp_mask(mapping, gfp_mask);

	/*
		1.GEM 对象的生命周期管理
			Linux 内核中，文件的引用计数机制是最成熟、最健壮的。GEM 对象绑定匿名 tmpfs 文件后，
			整个生命周期和文件完全绑定：
			用户态创建 GEM handle、mmap 映射、导出 dma-buf 共享，都会增加文件的引用计数；
			用户态关闭 handle、解除 mmap、关闭 dma-buf fd，都会减少引用计数；
			引用计数归 0 时，内核自动销毁 tmpfs 文件，释放对应的物理内存，从根本上避免内存泄漏。
			这就是 DRM GEM 的「file-backed」核心设计思想，也是 Linux 所有子系统共享内存的通用方案。
		2.图形内存的统一管理载体
			你传入的size对应的图形缓冲区，本质上就是这个 tmpfs 文件的「内容」：
			缓冲区的物理内存，就是 tmpfs 文件的页缓存（page cache）；
			你代码里的drm_gem_get_pages，本质就是从这个 tmpfs 文件的address_space里，
				获取 / 分配所有的物理页；
			内核内存管理系统会像管理普通文件的页缓存一样，管理 GEM 内存，支持内存回收、
				swap 换出（开启 swap 时），避免长期占用不可回收的内核内存。
		3.跨进程零拷贝共享的基础
				嵌入式 Linux/Android 的显示链路中，GPU、视频解码器、摄像头 ISP、
			显示合成器（Weston/SurfaceFlinger）之间的零拷贝图像共享，核心就是这个 tmpfs 文件：
				一个进程创建 GEM 缓冲区后，可通过dma_buf_export把匿名 tmpfs 
			文件封装成 dma-buf，拿到一个文件描述符 fd；
				通过 UNIX 域套接字，把这个 fd 传给另一个进程；
			另一个进程拿到 fd 后，mmap 就能直接访问同一块物理内存，无需任何数据拷贝；
			整个过程，共享的核心就是 tmpfs 文件的物理页。
		4.用户态 mmap 映射的标准接口
			Linux 用户态要访问内核分配的物理内存，最标准、最安全的方式就是 mmap 一个文件。
				GEM 对象绑定 tmpfs 文件后，用户态可直接通过 DRM 的 mmap 接口，把 tmpfs 
			文件的物理页映射到用户态虚拟地址空间，实现 CPU 对图形缓冲区的直接读写，无需复杂的内核驱动交互。
	*/
	return rk_obj;
}

struct rockchip_gem_object *
rockchip_gem_create_object(struct drm_device *drm, unsigned int size,
			   bool alloc_kmap, unsigned int flags)
{
	struct rockchip_gem_object *rk_obj;
	int ret;

	rk_obj = rockchip_gem_alloc_object(drm, size, flags);
	if (IS_ERR(rk_obj))
		return rk_obj;
	rk_obj->flags = flags;

	ret = rockchip_gem_alloc_buf(rk_obj, alloc_kmap);
	if (ret)
		goto err_free_rk_obj;

	return rk_obj;

err_free_rk_obj:
	rockchip_gem_release_object(rk_obj);
	return ERR_PTR(ret);
}

/*
 * rockchip_gem_destroy - destroy gem object
 *
 * The dma_buf_unmap_attachment and dma_buf_detach will be re-defined if
 * CONFIG_DMABUF_CACHE is enabled.
 *
 * Same as drm_prime_gem_destroy
 */
static void rockchip_gem_destroy(struct drm_gem_object *obj, struct sg_table *sg)
{
	struct dma_buf_attachment *attach;
	struct dma_buf *dma_buf;

	attach = obj->import_attach;
	if (sg)
		dma_buf_unmap_attachment(attach, sg, DMA_BIDIRECTIONAL);
	dma_buf = attach->dmabuf;
	dma_buf_detach(attach->dmabuf, attach);
	/* remove the reference */
	dma_buf_put(dma_buf);
}

/*
 * rockchip_gem_free_object - (struct drm_driver)->gem_free_object_unlocked
 * callback function
 */
void rockchip_gem_free_object(struct drm_gem_object *obj)
{
	struct drm_device *drm = obj->dev;
	struct rockchip_drm_private *private = drm->dev_private;
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);

	if (obj->import_attach) {
		if (private->domain) {
			rockchip_gem_iommu_unmap(rk_obj);
		} else {
			dma_unmap_sgtable(drm->dev, rk_obj->sgt,
					  DMA_BIDIRECTIONAL, 0);
		}
		drm_free_large(rk_obj->pages);
		rockchip_gem_destroy(obj, rk_obj->sgt);
	} else {
		rockchip_gem_free_buf(rk_obj);
	}

	rockchip_gem_release_object(rk_obj);
}

/*
 * rockchip_gem_create_with_handle - allocate an object with the given
 * size and create a gem handle on it
 *
 * returns a struct rockchip_gem_object* on success or ERR_PTR values
 * on failure.
 */
static struct rockchip_gem_object *
rockchip_gem_create_with_handle(struct drm_file *file_priv,
				struct drm_device *drm, unsigned int size,
				unsigned int *handle, unsigned int flags)
{
	struct rockchip_gem_object *rk_obj;
	struct drm_gem_object *obj;
	int ret;
	bool alloc_kmap = flags & ROCKCHIP_BO_ALLOC_KMAP ? true : false;

	rk_obj = rockchip_gem_create_object(drm, size, alloc_kmap, flags);
	if (IS_ERR(rk_obj))
		return ERR_CAST(rk_obj);

	obj = &rk_obj->base;

	/*
	 * allocate a id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, obj, handle);
	if (ret)
		goto err_handle_create;

	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(obj);

	return rk_obj;

err_handle_create:
	rockchip_gem_free_object(obj);

	return ERR_PTR(ret);
}

/*
 * rockchip_gem_dumb_create - (struct drm_driver)->dumb_create callback
 * function
 *
 * This aligns the pitch and size arguments to the minimum required. wrap
 * this into your own function if you need bigger alignment.
 */
int rockchip_gem_dumb_create(struct drm_file *file_priv,
			     struct drm_device *dev,
			     struct drm_mode_create_dumb *args)
{
	struct rockchip_gem_object *rk_obj;
	u32 min_pitch = args->width * DIV_ROUND_UP(args->bpp, 8);

	/*
	 * align to 64 bytes since Mali requires it.
	 */
	args->pitch = ALIGN(min_pitch, 64);
	args->size = args->pitch * args->height;

	rk_obj = rockchip_gem_create_with_handle(file_priv, dev, args->size,
						 &args->handle, args->flags);

	return PTR_ERR_OR_ZERO(rk_obj);
}

/*
 * Allocate a sg_table for this GEM object.
 * Note: Both the table's contents, and the sg_table itself must be freed by
 *       the caller.
 * Returns a pointer to the newly allocated sg_table, or an ERR_PTR() error.
 */
struct sg_table *rockchip_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);
	struct drm_device *drm = obj->dev;
	struct sg_table *sgt;
	int ret;

	if (rk_obj->pages)
		return drm_prime_pages_to_sg(obj->dev, rk_obj->pages, rk_obj->num_pages);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable_attrs(drm->dev, sgt, rk_obj->kvaddr,
				    rk_obj->dma_addr, obj->size,
				    rk_obj->dma_attrs);
	if (ret) {
		DRM_ERROR("failed to allocate sgt, %d\n", ret);
		kfree(sgt);
		return ERR_PTR(ret);
	}

	return sgt;
}

static int
rockchip_gem_iommu_map_sg(struct drm_device *drm,
			  struct dma_buf_attachment *attach,
			  struct sg_table *sg,
			  struct rockchip_gem_object *rk_obj)
{
	rk_obj->sgt = sg;
	return rockchip_gem_iommu_map(rk_obj);
}

static int
rockchip_gem_dma_map_sg(struct drm_device *drm,
			struct dma_buf_attachment *attach,
			struct sg_table *sg,
			struct rockchip_gem_object *rk_obj)
{
	int err = dma_map_sgtable(drm->dev, sg, DMA_BIDIRECTIONAL, 0);
	if (err)
		return err;

	if (drm_prime_get_contiguous_size(sg) < attach->dmabuf->size) {
		DRM_ERROR("failed to map sg_table to contiguous linear address.\n");
		dma_unmap_sgtable(drm->dev, sg, DMA_BIDIRECTIONAL, 0);
		return -EINVAL;
	}

	rk_obj->dma_addr = sg_dma_address(sg->sgl);
	rk_obj->sgt = sg;
	return 0;
}

struct drm_gem_object *
rockchip_gem_prime_import_sg_table(struct drm_device *drm,
				   struct dma_buf_attachment *attach,
				   struct sg_table *sg)
{
	struct rockchip_drm_private *private = drm->dev_private;
	struct rockchip_gem_object *rk_obj;
	int ret;

	rk_obj = rockchip_gem_alloc_object(drm, attach->dmabuf->size, 0);
	if (IS_ERR(rk_obj))
		return ERR_CAST(rk_obj);

	if (private->domain)
		ret = rockchip_gem_iommu_map_sg(drm, attach, sg, rk_obj);
	else
		ret = rockchip_gem_dma_map_sg(drm, attach, sg, rk_obj);

	if (ret < 0) {
		DRM_ERROR("failed to import sg table: %d\n", ret);
		goto err_free_rk_obj;
	}

	rk_obj->num_pages = rk_obj->base.size >> PAGE_SHIFT;
	rk_obj->pages = drm_calloc_large(rk_obj->num_pages, sizeof(*rk_obj->pages));
	if (!rk_obj->pages) {
		DRM_ERROR("failed to allocate pages.\n");
		ret = -ENOMEM;
		goto err_free_rk_obj;
	}

	ret = drm_prime_sg_to_page_addr_arrays(sg, rk_obj->pages, NULL, rk_obj->num_pages);
	if (ret < 0) {
		DRM_ERROR("invalid sgtable.\n");
		drm_free_large(rk_obj->pages);
		goto err_free_rk_obj;
	}

	return &rk_obj->base;

err_free_rk_obj:
	rockchip_gem_release_object(rk_obj);
	return ERR_PTR(ret);
}

void *rockchip_gem_prime_vmap(struct drm_gem_object *obj)
{
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);

	if (rk_obj->pages)
		return vmap(rk_obj->pages, rk_obj->num_pages, VM_MAP,
			    pgprot_writecombine(PAGE_KERNEL));

	if (rk_obj->dma_attrs & DMA_ATTR_NO_KERNEL_MAPPING)
		return NULL;

	return rk_obj->kvaddr;
}

void rockchip_gem_prime_vunmap(struct drm_gem_object *obj, void *vaddr)
{
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);

	if (rk_obj->pages) {
		vunmap(vaddr);
		return;
	}

	/* Nothing to do if allocated by DMA mapping API. */
}

int rockchip_gem_create_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	/*
		struct drm_rockchip_gem_create {
			uint64_t size;
			uint32_t flags;
			uint32_t handle;
		};
	*/
	struct drm_rockchip_gem_create *args = data;
	struct rockchip_gem_object *rk_obj;

	rk_obj = rockchip_gem_create_with_handle(file_priv, dev, args->size,
						 &args->handle, args->flags);
	return PTR_ERR_OR_ZERO(rk_obj);
}

int rockchip_gem_map_offset_ioctl(struct drm_device *drm, void *data,
				  struct drm_file *file_priv)
{
	struct drm_rockchip_gem_map_off *args = data;

	return drm_gem_dumb_map_offset(file_priv, drm, args->handle,
				       &args->offset);
}

int rockchip_gem_get_phys_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv)
{
	struct drm_rockchip_gem_phys *args = data;
	struct rockchip_gem_object *rk_obj;
	struct drm_gem_object *obj;
	int ret = 0;

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!obj) {
		DRM_ERROR("failed to lookup gem object.\n");
		return -EINVAL;
	}
	rk_obj = to_rockchip_obj(obj);

	if (!(rk_obj->flags & ROCKCHIP_BO_CONTIG)) {
		DRM_ERROR("Can't get phys address from non-continue buf.\n");
		ret = -EINVAL;
		goto out;
	}

	args->phy_addr = page_to_phys(rk_obj->pages[0]);

out:
	drm_gem_object_put(obj);

	return ret;
}

int rockchip_gem_prime_begin_cpu_access(struct drm_gem_object *obj,
					enum dma_data_direction dir)
{
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);
	struct drm_device *drm = obj->dev;

	if (!rk_obj->sgt)
		return 0;

	dma_sync_sg_for_cpu(drm->dev, rk_obj->sgt->sgl,
			    rk_obj->sgt->nents, dir);
	return 0;
}

int rockchip_gem_prime_end_cpu_access(struct drm_gem_object *obj,
				      enum dma_data_direction dir)
{
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);
	struct drm_device *drm = obj->dev;

	if (!rk_obj->sgt)
		return 0;

	dma_sync_sg_for_device(drm->dev, rk_obj->sgt->sgl,
			       rk_obj->sgt->nents, dir);
	return 0;
}
