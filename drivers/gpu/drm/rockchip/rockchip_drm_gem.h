/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author:Mark Yao <mark.yao@rock-chips.com>
 */

#ifndef _ROCKCHIP_DRM_GEM_H
#define _ROCKCHIP_DRM_GEM_H

#include <linux/dma-direction.h>

#define to_rockchip_obj(x) container_of(x, struct rockchip_gem_object, base)
/*
	1. ROCKCHIP_GEM_BUF_TYPE_CMA
		全称：Contiguous Memory Allocator（连续内存分配器）。
		特点：分配物理地址连续的内存块。
		用途：
			供 Rockchip 显示控制器（VOP）直接使用（如果系统未启用 IOMMU，硬件通常需要物理连续内存）。
			用于需要高带宽、低延迟的场景（如 4K 显示、视频解码输出）。
		注意：CMA 内存是在内核启动时预留的一块固定大小的区域，分配灵活性较低，但能保证大块连续物理内存。
	2. ROCKCHIP_GEM_BUF_TYPE_SHMEM
		全称：Shared Memory（共享内存，通常基于 shmem_file_setup）。
		特点：
			分配的内存物理地址可能不连续（由多个离散的物理页组成）。
			通常配合 IOMMU（IO 内存管理单元） 使用，将离散的物理页映射为连续的 IOVA（IO Virtual Address，IO 虚拟地址） 供硬件访问。
		用途：
			用于 GPU 渲染、通用计算等场景。
			当系统启用 IOMMU 时，这是最常用的缓冲区类型（内存利用率更高，不需要预留大块 CMA）。
	3. ROCKCHIP_GEM_BUF_TYPE_SECURE
		全称：Secure Memory（安全内存）。
		特点：
			位于硬件保护的安全内存区域（TrustZone / TEE 环境）。
			普通操作系统（Linux）无法直接访问，只有安全固件 / 硬件可以访问。
		用途：
			用于 DRM（数字版权管理）内容播放（如高清版权视频）。
			用于安全显示（Secure Display）场景，防止画面被截屏或篡改。
*/
enum rockchip_gem_buf_type {
	ROCKCHIP_GEM_BUF_TYPE_CMA,
	ROCKCHIP_GEM_BUF_TYPE_SHMEM,
	ROCKCHIP_GEM_BUF_TYPE_SECURE,
};

struct rockchip_gem_object {
	struct drm_gem_object base;
	/* 
		存储缓冲区的属性标志。
		常见的标志可能包括：缓存策略（Write-Back / Write-Combine / Uncached）、
			是否需要物理连续、是否为安全内存等。
	*/
	unsigned int flags;
	enum rockchip_gem_buf_type buf_type;

	void *kvaddr;
	/*
		DMA 地址:
			这是供硬件设备（如 VOP、GPU）访问的地址
			如果 IOMMU 启用：这是一个 IOVA（IO Virtual Address）。它是连续的虚拟地址，由 IOMMU 映射到背后离散的物理页上。
			如果 IOMMU 禁用：这就是物理地址（Physical Address）。
	*/
	dma_addr_t dma_addr;	/* iova if iommu enable, otherwise physical address */
	/*
		真实物理地址。
		主要在 IOMMU 禁用 且使用 CMA 内存时使用，记录内存的真实物理起始地址。
	*/
	dma_addr_t dma_handle;	/* physical address */
	/* Used when IOMMU is disabled */
	/*
		用于定义内存的缓存一致性（Cache Coherency）、读写权限等。
		例如：DMA_ATTR_WRITE_COMBINE（写合并，提高写入性能）、
			DMA_ATTR_SKIP_CPU_SYNC（跳过 CPU 缓存同步）等。
	*/
	unsigned long dma_attrs;

	/* Used when IOMMU is enabled */
	/*
		DRM 内存管理节点。
		用于在 IOVA 地址空间中分配和管理一块连续的虚拟地址范围。
		简单说：它记录了这块内存在 “硬件看到的虚拟地址空间” 里的位置和大小。
	*/
	struct drm_mm_node mm;
	/*
		缓冲区占用的物理页数量。
		因为内存通常是按页（Page，通常 4KB）分配的，所以 size = num_pages * PAGE_SIZE
	*/
	unsigned long num_pages;
	struct page **pages;
	/*
		散列表（Scatter-Gather Table）。
			它是 Linux 内核用于描述离散物理内存块的标准结构。
		作用：
			当进行 PRIME（DMA-BUF）共享时，用于将内存导出给其他驱动（如 GPU、视频解码器）。
			配合 IOMMU，将这些离散的页 “粘” 在一起，映射成一个连续的 IOVA 给硬件用。
	*/
	struct sg_table *sgt;
	/* 
		缓冲区的总大小（以字节为单位）。
			这是申请内存时的核心参数，也是进行 mmap、DMA 映射时的重要依据
	*/
	size_t size;
};

struct sg_table *rockchip_gem_prime_get_sg_table(struct drm_gem_object *obj);
struct drm_gem_object *
rockchip_gem_prime_import_sg_table(struct drm_device *dev,
				   struct dma_buf_attachment *attach,
				   struct sg_table *sg);
void *rockchip_gem_prime_vmap(struct drm_gem_object *obj);
void rockchip_gem_prime_vunmap(struct drm_gem_object *obj, void *vaddr);

/* drm driver mmap file operations */
int rockchip_gem_mmap(struct file *filp, struct vm_area_struct *vma);

/* mmap a gem object to userspace. */
int rockchip_gem_mmap_buf(struct drm_gem_object *obj,
			  struct vm_area_struct *vma);

struct rockchip_gem_object *
rockchip_gem_create_object(struct drm_device *drm, unsigned int size,
			   bool alloc_kmap, unsigned int flags);

void rockchip_gem_free_object(struct drm_gem_object *obj);

int rockchip_gem_dumb_create(struct drm_file *file_priv,
			     struct drm_device *dev,
			     struct drm_mode_create_dumb *args);
/*
 * request gem object creation and buffer allocation as the size
 * that it is calculated with framebuffer information such as width,
 * height and bpp.
 */
int rockchip_gem_create_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_priv);

/* get buffer offset to map to user space. */
int rockchip_gem_map_offset_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file_priv);

int rockchip_gem_get_phys_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv);

int rockchip_gem_prime_begin_cpu_access(struct drm_gem_object *obj,
					enum dma_data_direction dir);

int rockchip_gem_prime_end_cpu_access(struct drm_gem_object *obj,
				      enum dma_data_direction dir);

void rockchip_gem_get_ddr_info(void);
#endif /* _ROCKCHIP_DRM_GEM_H */
