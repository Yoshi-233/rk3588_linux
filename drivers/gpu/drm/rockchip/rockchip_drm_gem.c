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

/*
	这个函数是Rockchip DRM GEM 驱动 IOMMU 地址映射的核心执行接口，是平台开启 IOMMU 后，
		外设（VOP 显示控制器、GPU、DMA 等）能正常访问 GEM 图形缓冲区的核心环节。
	1.从 DRM 驱动全局的 IOVA（I/O 虚拟地址）空间中，分配一段连续的、页对齐的虚拟地址区间，
		满足外设对连续线性地址的硬性要求；
	2.调用内核 IOMMU 标准 API，把 GEM 对象中已准备好的物理内存（通过sg_table统一封装），
		批量映射到分配的 IOVA 地址上，建立「连续 IOVA → 离散 / 连续物理页」的硬件页表映射；
	3. 最终生成外设可直接访问的rk_obj->dma_addr，完全抹平物理内存连续 / 离散的底层差异，和之前
		的 SHMEM/CMA/SECURE 三种 buffer 类型全场景兼容。
*/
static int rockchip_gem_iommu_map(struct rockchip_gem_object *rk_obj)
{
	struct drm_device *drm = rk_obj->base.dev;
	struct rockchip_drm_private *private = drm->dev_private;
	int prot = IOMMU_READ | IOMMU_WRITE;
	ssize_t ret;

	mutex_lock(&private->mm_lock);
	/*
		DRM 框架封装的区间式内存分配器 API，专为 IOVA、VRAM 等地址空间管理设计
		mm:驱动全局的 IOVA 地址分配器实例，驱动 probe 阶段初始化，管理整个 IOMMU 
			域的虚拟地址空间（通常是 0~4GB，适配 32 位外设寻址）
		GEM 对象专属的struct drm_mm_node节点，用于记录本次分配到的地址区间，分配
			成功后会填充start（起始地址）、size（区间大小）等核心字段
		地址颜色值，用于 NUMA 节点 / 内存 Bank 优化，这里用默认值 0，不开启特殊优化
		分配标志位，默认从低地址开始分配，适配外设 32 位寻址限制
	*/
	ret = drm_mm_insert_node_generic(&private->mm, &rk_obj->mm,
					 rk_obj->base.size, PAGE_SIZE,
					 0, 0);
	mutex_unlock(&private->mm_lock);

	if (ret < 0) {
		DRM_ERROR("out of I/O virtual memory: %zd\n", ret);
		return ret;
	}

	/*
		把分配到的连续 IOVA 起始地址，永久保存到rk_obj->dma_addr字段；
		这个地址就是VOP/GPU 等外设最终写入寄存器、直接访问的地址：
		外设访问dma_addr + 偏移时，IOMMU 硬件会自动通过页表翻译，找到对
			应的物理页，完全感知不到物理内存是连续还是离散；
		无 IOMMU 时，dma_addr等于物理地址；有 IOMMU 时，dma_addr是纯虚
			拟的 IOVA 地址，和物理地址无直接关联。
	*/
	rk_obj->dma_addr = rk_obj->mm.start;

	/*
		批量建立 IOMMU 硬件页表映射
		1.遍历sg_table中的所有有效 scatterlist 条目；
		2.对每个 sg 条目，自动识别物理连续的内存块，按 IOMMU 硬件支持的大页粒度
			（如 2MB/1GB）合并映射，大幅减少页表项数量，降低 TLB miss 率，提升外设访问性能；
		3.从dma_addr起始地址开始，按线性顺序建立「IOVA 地址 → 物理地址」的页表映射；

	*/
	ret = iommu_map_sgtable(private->domain, rk_obj->dma_addr, rk_obj->sgt,
				prot);
	if (ret < rk_obj->base.size) {
		DRM_ERROR("failed to map buffer: size=%zd request_size=%zd\n",
			  ret, rk_obj->base.size);
		ret = -ENOMEM;
		goto err_remove_node;
	}

	/*
		IOTLB（I/O Translation Lookaside Buffer）是 IOMMU 硬件的地址翻译缓存，用来缓存最近使用的页表项，提升地址翻译效率；
		我们刚刚修改了页表，硬件的 IOTLB 中还缓存着旧的无效地址映射，必须刷新 TLB，才能让外设看到最新的页表；
		若不刷新，外设访问 IOVA 地址时会命中旧的 TLB 项，触发 IOMMU 缺页故障、总线错误，导致显示黑屏、内核报错；
		补充优化：高版本内核可使用iommu_flush_iotlb_range()，只刷新本次映射的地址范围，比全量刷新性能更高，原厂代码用全量刷新是为了跨内核版本兼容性。
	*/
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

/*
	这个函数是Rockchip DRM GEM 驱动 SHMEM（离散物理页）类型 buffer 的核心物理内存分配与性能优化接口，
		是平台开启 IOMMU、buffer 无ROCKCHIP_BO_CONTIG/ROCKCHIP_BO_SECURE标志时的默认执行路径。
	1.从 GEM 对象绑定的匿名 tmpfs 文件中，完成物理内存的实际分配与锁定（之前的rockchip_gem_alloc_object
		仅创建了 tmpfs 文件，未分配真实物理页）；
	2.执行瑞芯微专属的DDR Bank 交错打散优化，解决 DDR 多 Bank 访问冲突问题，大幅提升显示 / 
		渲染场景的内存带宽与访问效率；
	3.最终生成标准sg_table散列表，为后续 IOMMU 地址映射、外设访问、dma-buf 跨进程共享做全链路准备。
*/
static int rockchip_gem_get_pages(struct rockchip_gem_object *rk_obj)
{
	/* 
		PG_ROUND:DDR Bank 交错粒度，对应瑞芯微平台 DDR 控制器默认的 8Bank 设计，是优化逻辑的核心基数
	  	bank_bit_first	DDR 物理地址中 Bank 地址位的起始偏移，默认 12（刚好跳过 4KB 页的页内偏移），
			通过安全 SMC 调用从 TrustZone 获取真实 DRAM 硬件配置
		bank_bit_mask	Bank 地址位的掩码，默认0x7（对应 3 位地址，覆盖 8 个 Bank），同样从安全世界获取
		struct page_info 封装结构体，包含struct page *page物理页指针和struct list_head list
			链表节点，用于按 Bank 分类存放物理页
	*/
	struct drm_device *drm = rk_obj->base.dev;
	int ret, i;
	struct scatterlist *s;
	// 原始 page 数组的当前处理索引，用于遍历拆分连续物理页块
	unsigned int cur_page;
	// 原始 page 指针数组，从 tmpfs 分配的物理页原生数组
	// 目标 page 指针数组，存放经过 DDR Bank 优化后的物理页，最终会替换原生pages
	struct page **pages, **dst_pages;
	int j;
	int n_pages;
	// 遍历过程中，当前连续物理页块的页数
	unsigned long chunk_pages;
	// 剩余未处理的物理页数，用于循环终止判断
	unsigned long remain;
	// 链表数组，共 8 个，每个链表对应一个 DDR Bank，用于分类存放对应 Bank 的物理页
	struct list_head lists[PG_ROUND];
	// 存放单个物理页的 CPU 物理地址，用于计算该页所属的 DDR Bank 索引
	dma_addr_t phys;
	// dst_pages数组的当前填充索引，确保 page 按优化顺序线性填充
	int end = 0;
	// 计算得到的 DDR Bank 索引（0~7），用于分类存放 page
	unsigned int bit_index;
	// 统计每个 DDR Bank 的 page 数量，用于后续交错填充的循环控制
	unsigned int block_index[PG_ROUND] = {0};
	// struct page_info结构体指针，用于封装 page 并挂载到对应 Bank 的链表
	struct page_info *info;
	// 8 个 Bank 中 page 数量的最大值，决定后续交错填充的总轮数
	unsigned int maximum;

	for (i = 0; i < PG_ROUND; i++)
		INIT_LIST_HEAD(&lists[i]);

	/*
		从 GEM 对象绑定的匿名 tmpfs 文件中，分配并锁定 buffer 所需的全部物理页，返回 page 指针数组；
		物理页的分配严格遵守rockchip_gem_alloc_object中设置的gfp_mask（GFP_HIGHUSER 
			| __GFP_RECLAIMABLE | __GFP_DMA32），确保分配到外设可访问的内存区域；
		分配的物理页无需物理连续，因为平台开启了 IOMMU，可通过地址翻译给外设提供连续的 IOVA 地址
	*/
	pages = drm_gem_get_pages(&rk_obj->base);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	/*
		rk_obj->pages = pages：把原生 page 数组暂存到 GEM 对象，用于后续错误处理的资源释放；
		rk_obj->num_pages = rk_obj->base.size >> PAGE_SHIFT：计算 buffer 的总物理页数；
		PAGE_SHIFT是页大小的偏移量（4KB 页对应 12 位），buffer 大小已提前完成页对齐，右移后无余数；
		n_pages = rk_obj->num_pages：给总页数起别名，简化后续代码书写。
	*/
	rk_obj->pages = pages;
	rk_obj->num_pages = rk_obj->base.size >> PAGE_SHIFT;
	n_pages = rk_obj->num_pages;

	// 分配目标 page 数组dst_pages，用于存放经过 DDR Bank 优化后的 page 指针，最终会替换原生pages数组；
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
	// cur_page=0从原生数组第 0 页开始处理，remain=n_pages剩余未处理页数为总页数；
	while (remain) {
		// 查找从cur_page开始的最大连续物理页块
		for (j = cur_page + 1; j < n_pages; ++j) {
			if (page_to_pfn(pages[j]) !=
				page_to_pfn(pages[j - 1]) + 1)
				break;
		}

		// 连续块分支处理：大段保留，小段打散优化
		chunk_pages = j - cur_page;
		if (chunk_pages >= PG_ROUND) {
			// 分支 1：大连续块（≥8 页）直接保留
			for (i = 0; i < chunk_pages; i++)
				dst_pages[end + i] = pages[cur_page + i];
			end += chunk_pages;
		} else {
			// 小连续块（<8 页）按 Bank 分类打散
			for (i = 0; i < chunk_pages; i++) {
				// 分配page_info结构体：封装当前 page 指针和链表节点，用于后续分类挂载；
				info = kmalloc(sizeof(*info), GFP_KERNEL);
				if (!info) {
					ret = -ENOMEM;
					goto err_put_list;
				}

				INIT_LIST_HEAD(&info->list);
				// info->page = pages[cur_page + i]：保存当前 page 的指针
				info->page = pages[cur_page + i];
				// phys = page_to_phys(info->page)：获取该 page 的 CPU 物理起始地址；
				phys = page_to_phys(info->page);
				/*
					逻辑：物理地址右移bank_bit_first（默认 12 位，跳过页内偏移），
						和bank_bit_mask（默认 0x7）做与运算，得到该页对应的硬件 
						Bank 号，再对 8 取模确保索引在 0~7 范围内；
					目的：精准识别每个物理页属于哪个 DDR Bank，为后续交错优化做准备。
				*/
				bit_index = ((phys >> bank_bit_first) & bank_bit_mask) % PG_ROUND;
				// list_add_tail(&info->list, &lists[bit_index])：把封装好的 page 节点，挂载到对应 Bank 的链表尾部；
				list_add_tail(&info->list, &lists[bit_index]);
				// block_index[bit_index]++：对应 Bank 的 page 计数 + 1，统计每个 Bank 的总 page 数。
				block_index[bit_index]++;
			}
		}

		cur_page = j;
		remain -= chunk_pages;
	}

	/*
		作用：找到 8 个 Bank 中 page 数量的最大值，作为后续交错填充的总轮数；
		示例：Bank0 有 100 个 page，其他 Bank 均少于 100，则maximum=100，需要循环 100 轮完成交错填充。
	*/
	maximum = block_index[0];
	for (i = 1; i < PG_ROUND; i++)
		maximum = max(maximum, block_index[i]);

	/*
		DDR 内存的 Bank 是并行工作的，同一个 Bank 的连续访问需要等待预充电、激活等操作，会产生访问延迟；
			而不同 Bank 的访问可并行执行，实现流水线式访问。
		优化前：连续的逻辑地址可能集中在同一个 Bank，导致 Bank 冲突，带宽利用率低；
		优化后：连续的逻辑地址会依次访问不同的 Bank，完美实现 Bank 交错，大幅提升内存有效带宽，
			降低访问延迟，尤其适配显示、GPU 这种大带宽连续访问的场景。

		表格
		Bank0	Bank1	Bank2	Bank3	Bank4	Bank5	Bank6	Bank7
		pageA	pageC	pageE	pageG	pageI	pageK	pageM	pageO
		pageB	pageD	pageF	pageH	pageJ	pageL	pageN	pageP
		优化后dst_pages的顺序：pageA → pageC → pageE → pageG → pageI → pageK → 
			pageM → pageO → pageB → pageD → pageF → ...，完美实现跨 Bank 交错。

	*/
	for (i = 0; i < maximum; i++) {
		for (j = 0; j < PG_ROUND; j++) {
			if (!list_empty(&lists[j])) {
				/*
					内层循环：每一轮循环，按 Bank0 到 Bank7 的顺序，
					从每个 Bank 的链表中取出一个 page，依次放入dst_pages；

					取当前 Bank 链表的第一个节点，把 page 指针放入dst_pages，end索引同步递增；
					从链表中删除节点，释放page_info结构体，避免内存泄漏
				*/ 
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
	/*
		1. 用优化后的dst_pages数组，调用 DRM 标准 API 生成sg_table散列表；
		2. drm_prime_pages_to_sg的核心特性：自动合并物理上连续的 page，把多个连续页合并为 
			1 个 sg 条目，大幅减少 sg 条目数量，降低后续 IOMMU 映射的开销；
		3. 生成的sgt会永久绑定到 GEM 对象，后续 IOMMU 映射、dma-buf 跨进程共享、缓存同步全
			链路都会复用这个 sg_table，和 CMA/SECURE 路径的逻辑完全对齐，实现代码复用；
	*/
	rk_obj->sgt = drm_prime_pages_to_sg(rk_obj->base.dev,
					    dst_pages, rk_obj->num_pages);
	if (IS_ERR(rk_obj->sgt)) {
		ret = PTR_ERR(rk_obj->sgt);
		goto err_put_list;
	}

	/*
		作用：把 GEM 对象的 page 指针，从原生pages数组，替换为经过 DDR Bank 优化后的dst_pages数组；
		核心目的：后续所有上层操作（用户态 mmap、内核态 vmap、dma-buf 导出）都会使用优化后的 page 顺序，
			确保 CPU 和外设访问都能享受到 Bank 交错的性能提升。
	*/
	rk_obj->pages = dst_pages;

	/*
	 * Fake up the SG table so that dma_sync_sg_for_device() can be used
	 * to flush the pages associated with it.
	 *
	 * TODO: Replace this by drm_clflush_sg() once it can be implemented
	 * without relying on symbols that are not exported.
	 * 遍历 sg_table 的所有有效条目，给每个 sg 条目的dma_address字段赋值为该段内存的物理起始地址；
	 * 核心原因：drm_prime_pages_to_sg生成的 sg_table，dma_address默认值为 0，而 DMA 缓存同步 
	 * 	API dma_sync_sg_for_device依赖该字段，必须手动填充才能正常工作；
	 * 和 CMA/SECURE 路径的逻辑完全对齐，确保全场景的缓存同步逻辑统一。
	 */
	for_each_sgtable_sg(rk_obj->sgt, s, i)
		sg_dma_address(s) = sg_phys(s);

	/*
		作用：执行 DMA 缓存同步，把 CPU 缓存中的数据刷到物理内存，
			让外设（VOP 显示控制器 / GPU）能读到最新的完整数据；
		DMA_TO_DEVICE：数据方向为 CPU 写、外设读，完美匹配显示帧缓
			冲的典型场景（CPU 填充画面数据，VOP 读取并输出到屏幕）
	*/
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

/*
	这个函数是Rockchip DRM GEM 驱动 CMA 连续物理内存缓冲区的核心分配接口，对应ROCKCHIP_GEM_BUF_TYPE_CMA类型，
	专为无 IOMMU 的 VOP 显示、外设 32 位寻址限制、需要物理连续内存的场景设计。
	buffer 类型		pages 数组来源						物理页特性					上层逻辑兼容性
	CMA（当前函数）		从 dma_alloc_attrs 分配的连续内存 sg_table 中提取	物理连续，内核常规内存，有完整 struct page	100% 兼容所有 DRM 标准能力
	SHMEM（离散页）		从 tmpfs 文件分配，drm_gem_get_pages 获取		物理离散，内核常规内存，有完整 struct page	100% 兼容，和 CMA 共用一套上层代码
	SECURE（安全内存）	从 gen_pool 预留内存循环 phys_to_page 生成		物理连续，no-map 安全内存，无内核映射		兼容大部分能力，禁止用户态 mmap
*/
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

	/*
		CMA 内存通过dma_alloc_attrs分配后，已经拿到了内核虚拟地址kvaddr和物理 / 总线地址dma_handle，
			但依然必须生成 pages 数组，核心原因是：
		DRM GEM 框架的所有标准能力（用户态 mmap、dma-buf 零拷贝共享、IOMMU 映射、内核 vmap、缓存同步）
			，都基于struct page数组设计。
		没有 pages 数组，CMA 类型 buffer 就无法和 SHMEM（离散页）、SECURE（安全内存）类型 buffer 
			共用一套上层逻辑，会导致代码冗余、兼容性差，这是 Rockchip 驱动的核心抽象设计。

		这是 Linux DMA 框架的标准连续物理内存分配 API，专门为 DMA 外设分配满足寻址约束的、物理连续的内存，
			对应ROCKCHIP_BO_CONTIG连续内存场景，是无 IOMMU 时显示控制器能正常工作的前提。
		&rk_obj->dma_handle 输出参数，返回外设可直接访问的 DMA 总线地址：无 IOMMU 时 = CPU 物理地址；
				有 IOMMU 时 = IOMMU 映射后的 IOVA 虚拟地址
		内存属性控制，提前配置了两个核心标志：
		1. DMA_ATTR_WRITE_COMBINE：写合并属性，关闭 CPU 写缓存的读分配，
			大幅提升帧缓冲这类「CPU 写、外设读」场景的性能，减少缓存刷新开销
		2. 可选DMA_ATTR_NO_KERNEL_MAPPING：alloc_kmap=false时添加，
			不创建内核线性映射，节省内核虚拟地址空间
		无DMA_ATTR_NO_KERNEL_MAPPING：合法的内核虚拟地址，驱动可直接读写这段内存；
		有DMA_ATTR_NO_KERNEL_MAPPING：不透明的分配 cookie，仅用于后续dma_free_attrs
			/dma_get_sgtable_attrs等配套 API，绝对不能直接解引用访问，否则会触发内核缺页 Oops。
	*/
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

	/*
		Linux DMA 框架标准 API，从 dma_alloc_attrs 分配的连续内存中提取物理内存信息，
			构建符合 DMA 框架规范的 scatterlist 散列表，是把裸 CMA 内存接入 DRM 标准框架的核心桥梁。
		入参强制要求：dev/kvaddr/dma_handle/size/dma_attrs必须和dma_alloc_attrs分配时的参数完全一致，
			尤其是DMA_ATTR_NO_KERNEL_MAPPING属性，否则会直接函数失败甚至内核 Oops。
		因为 CMA 分配的内存是 100% 物理连续的，最终生成的 sg_table 只有1 个有效 scatterlist 条目
			（sgt->nents=1），对应整块连续内存；
		填充该条目的核心字段：page_link（指向内存起始物理页的struct page）、offset=0、
			length=buffer总大小；
		⚠️ 关键特性：该函数不会自动填充sg->dma_address字段，该字段初始值为 0，
			这也是后续必须手动循环赋值的核心原因。

		没有这个 sg_table，CMA 分配的只是一块裸物理内存，完全无法接入 DRM GEM 的标准生态，
			它支撑了后续所有核心能力：
		后续通过drm_prime_sg_to_page_addr_arrays提取pages数组，实现用户态 mmap、dma-buf 导出；
		无 IOMMU 时，基于 sg_table 完成 DMA 缓存一致性同步；
		有 IOMMU 时，通过iommu_map_sgtable完成 IOVA 地址映射；
		跨驱动 dma-buf 零拷贝共享的唯一标准载体。
	*/
	ret = dma_get_sgtable_attrs(drm->dev, sgt, rk_obj->kvaddr,
				    rk_obj->dma_handle, obj->size,
				    rk_obj->dma_attrs);
	if (ret) {
		DRM_ERROR("failed to allocate sgt, %d\n", ret);
		goto err_sgt_free;
	}

	/*
		内核标准遍历宏，循环遍历 sg_table 里的所有有效 scatterlist 条目；
			CMA 连续内存场景下只会循环 1 次
		无 IOMMU 场景下，DMA 总线地址和 CPU 物理地址完全相等，sg_phys(s)的结果和分配时
			的rk_obj->dma_handle完全一致；
		这个写法和 SHMEM 离散页、SECURE 安全内存场景的逻辑 100% 对齐，后续的 IOMMU 映射、
			缓存同步、dma-buf 共享代码完全不用区分内存类型，一套逻辑适配所有场景，和之
			前pages数组的抽象设计思想完全统一。
	*/
	for_each_sg(sgt->sgl, s, sgt->nents, i)
		sg_dma_address(s) = sg_phys(s);

	/*
		核心逻辑：buffer 总大小已经在rockchip_gem_alloc_object中完成了PAGE_SIZE页对齐，
			因此右移PAGE_SHIFT（4KB 页对应 12）即可得到准确的物理页总数，无余数。
		关键作用：为后续 pages 数组的内存分配、元素填充提供严格的长度基准，避免数组越界。
	*/
	rk_obj->num_pages = rk_obj->base.size >> PAGE_SHIFT;

	/*
		这是 DRM 框架封装的大内存适配分配函数，而非普通的kcalloc：
		若总分配大小（num_pages * sizeof(struct page*)）≤ 1 个页大小，用kcalloc从 slab 分配；
		若超过 1 个页大小，用__vmalloc从高端内存分配，避免耗尽内核稀缺的低端线性映射区，减少内存碎片化。
		典型场景：4K 分辨率的 32 位 ARGB 帧缓冲大小为 32MB，对应 8192 个物理页，64 位系统下 pages 数组大
			小为8192*8=64KB，超过 4KB 页大小，自动走 vmalloc 分配。
		分配内容：申请一块连续内存，用于存放num_pages个struct page *指针，每个指针后续会指向对应物理页的
			struct page描述符。
	*/
	rk_obj->pages = drm_calloc_large(rk_obj->num_pages,
					 sizeof(*rk_obj->pages));
	if (!rk_obj->pages) {
		ret = -ENOMEM;
		DRM_ERROR("failed to allocate pages.\n");
		goto err_sg_table_free;
	}

	/*
		函数作用：DRM 框架标准辅助函数，从 sg_table 的 scatterlist 条目中，
			批量提取每个物理页的struct page指针，按顺序填充到 pages 数组。
		哪怕 CMA 内存是物理连续的，也会按页拆分成num_pages个 page 指针，最终生成的 pages 数组格式，
			和 SHMEM 离散页 buffer、SECURE 安全 buffer 完全一致。上层的 mmap、dma-buf、IOMMU
			 映射逻辑，完全不用区分内存类型，一套代码适配所有场景，实现了极致的代码复用
		drm_prime_sg_to_page_addr_arrays 这个函数的核心作用，就是抹平 CMA 连续内存和 SHMEM 离散内存
			的底层差异：不管输入的 sg_table 是 CMA 的单段连续物理内存，还是 SHMEM 的多段离散物理块
			，最终都会把所有物理页的struct page*，按 buffer 的线性逻辑顺序，平铺填充成格式 100% 
			一致的rk_obj->pages数组。上层代码完全不用关心底层内存是连续还是离散，只用一套逻辑就能适
			配所有场景。
	*/
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

// 这个函数是瑞芯微 DRM 驱动安全缓冲区（Secure Buffer）的核心分配接口，专为 TrustZone 安全世界
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

	/*
		将分配到的原始起始物理地址永久备份到 rk_obj->dma_handle，作为整个 buffer 生命周期内的唯一合法地址基准。
		安全 buffer 场景下，dma_handle 完全等于物理起始地址，和普通 buffer 有本质区别：
		表格
		buffer 类型		dma_handle 含义
		安全 buffer		纯物理起始地址（paddr 初始值）
		CMA 普通 buffer		DMA 映射后的总线地址
		SHMEM 普通 buffer	IOMMU 映射后的 IOVA 虚拟地址
	*/
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
		// phys_to_page(paddr)：内核标准函数，将物理地址转换为对应的 struct page 结构体指针，是内核管理物理页的标准载体。
		rk_obj->pages[i] = phys_to_page(paddr);
		paddr += PAGE_SIZE;
		i++;
	}

	/*
		paddr 已经完成了它的核心使命：通过填充 pages 数组，为生成 sg_table（散列表）提供了基础。
		sg_table 是 Linux 内核 DMA 传输、IOMMU 映射、dma-buf 零拷贝共享的核心结构，是安全 buffer 适配 DRM 标准框架的必要载体。
		没有 paddr 填充的合法 pages 数组，就无法生成合规的 sg_table，安全 buffer 无法被 DRM 框架识别和使用。
	*/

	/*
		它把离散 / 连续的物理页，封装成了外设、IOMMU、DMA、dma-buf 跨进程共享全链路都能统一识别的结构，
		linux 内核 DMA 框架的标准最小单元，描述一段物理上连续的内存块，包含物理页指针、页内偏移、块长度、外设可访问的 DMA 总线地址。
		struct sg_table：scatterlist 的集合容器，管理物理上可能离散、逻辑上连续的整块内存，比如 GEM buffer 的所有物理页。
	
		输入：rk_obj->pages 物理页指针数组（可能是离散的 SHMEM 页、连续的 CMA / 安全内存页）、
			总页数、DRM 设备；
		核心处理：自动合并物理上连续的页，比如 8 个连续的 4KB 页会被合并成 1 个 sg 条目，而非 8 个，
			大幅减少 IOMMU/DMA 映射开销；

		有 IOMMU/SMMU 场景：代码中rockchip_gem_iommu_map直接把sgt传给内核 API iommu_map_sgtable，
			IOMMU 驱动遍历 sg 条目，批量建立连续 IOVA 虚拟地址到离散物理页的页表映射，最终生成外设可访问的rk_obj->dma_addr。
		无 IOMMU 场景：代码中rockchip_gem_dma_map_sg调用dma_map_sgtable，把 sg 里的物理地址转换成外设可访问的总线地址，
			填充到sg->dma_address，让 DMA 控制器直接访问物理连续内存。

		代码中rockchip_gem_prime_get_sg_table，就是在用户态导出 dma-buf fd 时，把这个sgt返回给 dma-buf 框架；
		其他驱动（GPU / 摄像头）拿到这个 sg_table 后，无需拷贝数据，直接就能获取 buffer 的全部物理内存信息，完成映射访问，实现真正的零拷贝。


	*/
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
		/*
			vunmap(rk_obj->kvaddr)：解除alloc_kmap=true时创建的内核连续虚拟地址映射，释放内核虚拟地址空间；
			rockchip_gem_put_pages：SHMEM buffer 的专属释放函数，完成：
			释放 sg_table 散列表；
			调用drm_gem_put_pages解锁并释放 tmpfs 文件的物理页，让内核可回收该内存；
			释放 page 指针数组，完整回收 SHMEM 路径的所有资源。
		*/
		vunmap(rk_obj->kvaddr);
		rockchip_gem_put_pages(rk_obj);
	} else if (rk_obj->buf_type == ROCKCHIP_GEM_BUF_TYPE_SECURE) {
		/*
			释放 page 指针数组、sg_table 散列表；
			调用gen_pool_free把安全内存归还给安全内存池，供后续重新分配；
			严格遵循 TrustZone 安全内存的释放规则，不泄露安全内存的物理地址信息。
		*/
		rockchip_gem_free_secure(rk_obj);
	} else {
		/*
			释放 page 指针数组、sg_table 散列表；
		调用dma_free_attrs释放通过dma_alloc_attrs分配的 CMA 连续物理内存，归还给内核 CMA 区域；
		释放内核虚拟地址映射，完整回收 CMA 路径的所有资源
		*/
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

	/*
		核心调用，Linux 内核标准 API，核心作用：
		1. 把rk_obj->pages数组里的所有物理页，按顺序映射到用户态 vma 的连续虚拟地址空间；
		2. 无论物理页是否连续，用户态看到的都是连续的虚拟地址，完美适配 SHMEM 离散页的 DDR Bank 优化场景；
		3. 自动处理页属性、引用计数、缺页处理，是内核推荐的多页映射方式；
		4. 关键闭环：这里用的pages数组是rockchip_gem_get_pages中优化后的 DDR Bank 交错数组，
			用户态访问连续虚拟地址时，对应的物理页跨 Bank 分布，CPU 访问也能享受到 Bank 交错的带宽提升
	*/
	return vm_map_pages(vma, rk_obj->pages, count);
}

static int rockchip_drm_gem_object_mmap_dma(struct drm_gem_object *obj,
					    struct vm_area_struct *vma)
{
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);
	struct drm_device *drm = obj->dev;

	/*
		核心调用，Linux DMA 框架标准 API，参数与执行逻辑详解：
		入参严格匹配要求：所有参数必须和rockchip_gem_alloc_dma中dma_alloc_attrs的入参完全一致，否则会映射失败
		1. drm->dev：分配这段内存的原始设备；
		2. vma：用户态虚拟地址空间描述符；
		3. rk_obj->kvaddr：dma_alloc_attrs返回的内核虚拟地址（或分配 cookie，设置了
			DMA_ATTR_NO_KERNEL_MAPPING时）；
		4. rk_obj->dma_addr：dma_alloc_attrs返回的物理总线地址；
		5. obj->size：映射的内存大小，和分配时完全一致；
		6. rk_obj->dma_attrs：分配时的 DMA 属性（写合并、无内核映射等）；
		核心作用：为dma_alloc_attrs分配的连续物理内存建立用户态虚拟地址映射，自动继承分配时的内存属性，
			完美适配无内核映射的连续内存场景，是 DMA 连续内存映射的标准方式，避免手动设置页表导致的兼容性问题
	*/
	return dma_mmap_attrs(drm->dev, vma, rk_obj->kvaddr, rk_obj->dma_addr,
			      obj->size, rk_obj->dma_attrs);
}

static int rockchip_drm_gem_object_mmap(struct drm_gem_object *obj,
					struct vm_area_struct *vma)
{
	int ret;
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);

	/* default is wc. */
	/*
		页属性配置逻辑：
		1. 若 GEM 对象设置了ROCKCHIP_BO_CACHABLE可缓存标志，调用vm_get_page_prot获取和 vma 
			权限匹配的标准可缓存页属性，适合 CPU 频繁读写的软件渲染场景；
		2. 不设置该标志时，默认使用写合并属性：关闭 CPU 写缓存的读分配，减少缓存刷新开销，极致
			适配「CPU 写、外设读」的帧缓冲场景，大幅提升写入性能
	*/
	if (rk_obj->flags & ROCKCHIP_BO_CACHABLE)
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

	/*
	 * We allocated a struct page table for rk_obj, so clear
	 * VM_PFNMAP flag that was set by drm_gem_mmap_obj()/drm_gem_mmap().
	 * 核心设计细节，必须重点理解：
	1. VM_PFNMAP标志的含义：标记 vma 是「纯 PFN 物理地址映射」，
		没有对应的struct page结构体，内核不会管理页的引用计数、缺页处理；
	2. 之前的drm_gem_mmap/drm_gem_mmap_obj默认设置了该标志，
		但 Rockchip 的 GEM 对象（SHMEM/CMA/ 导入 buffer）
		都有完整的struct page数组，内核可正常管理；
	3. 清除该标志的目的：让内核使用常规页映射机制，支持正常的缺页
		处理、引用计数管理、vma 操作，避免内存泄漏、内核 Oops
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

/*
	内核态封装的 GEM 对象映射通用接口，用于将指定 GEM 对象映射到用户态虚拟地址空间，
		是 dma-buf mmap、驱动内部映射场景的核心入口，封装了 DRM 标准校验与 Rockchip 定制映射逻辑。
*/
int rockchip_gem_mmap_buf(struct drm_gem_object *obj,
			  struct vm_area_struct *vma)
{
	int ret;
	/*
		调用 DRM 框架标准 API，完成映射前的核心校验与初始化：
		1. 校验 vma 映射大小不超过 GEM 对象总大小；
		2. 校验 vma 的读写权限与 GEM 对象的访问权限匹配；
		3. 给 GEM 对象的引用计数 + 1，防止映射过程中对象被意外释放；
		4. 将 GEM 对象指针存入vma->vm_private_data，供后续回调使用；
		5. 给 vma 设置默认标志位（含VM_PFNMAP、VM_IO）；
		6. 成功返回 0，失败返回负数错误码
	*/
	ret = drm_gem_mmap_obj(obj, obj->size, vma);
	if (ret)
		return ret;

	return rockchip_drm_gem_object_mmap(obj, vma);
}

/* drm driver mmap file operations 用户态 mmap 系统调用入口 
	这是DRM 设备文件的 mmap 系统调用入口，注册到 DRM 驱动的file_operations->mmap字段，
	用户态对/dev/dri/card0执行 mmap 时，内核会直接调用该函数，是用户态映射 GEM 缓冲区的最核心入口。
*/
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
	/*
		DRM 框架标准 API，释放 GEM 基类对象的所有标准资源：
		释放 GEM 对象绑定的匿名 tmpfs 文件；
		清理 GEM 对象的链表节点、锁、引用计数相关字段；
		解除和 DRM 设备的绑定，完成 DRM 框架层面的收尾工作。
	*/
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
	/*
		调用 dma-buf 标准 API，解除导入时通过dma_buf_map_attachment创建的 sg_table 映射，和导入流程完全配对；
		保证源驱动能正确回收该 buffer 的相关资源，避免引用计数泄漏。
	*/
	if (sg)
		dma_buf_unmap_attachment(attach, sg, DMA_BIDIRECTIONAL);
	dma_buf = attach->dmabuf;
	// 解除 Rockchip 驱动和源 dma-buf 的附着关系，销毁attach实例，彻底断开和源 dma-buf 的关联。
	dma_buf_detach(attach->dmabuf, attach);
	/* remove the reference */
	// 归还导入时持有的 dma-buf 引用计数，当引用计数归 0 时，
	// 源驱动会自动释放该 buffer 的物理内存，完全符合 dma-buf 的所有权规则。
	dma_buf_put(dma_buf);
}

/*
 * rockchip_gem_free_object - (struct drm_driver)->gem_free_object_unlocked
 * callback function
 * 当 GEM 对象的引用计数被 DRM 核心减到 0 时（用户态关闭 handle、mmap 映射解除、dma-buf 共享释放等所有引用全部释放后），
 * 	DRM 框架会自动调用该函数，完成硬件映射、内核内存、物理缓冲区、dma-buf 绑定的全链路资源释放，是驱动内存安全、无
 * 	泄漏、无硬件资源残留的核心保障。
函数严格区分 **「外部导入的 dma-buf 对象」和「驱动原生创建的 GEM 对象」** 两大场景，采用差异化释放流程，
	完全符合 Linux dma-buf 跨驱动共享的内存所有权规则。

	维度		导入 dma-buf 对象						      原生创建 GEM 对象
	内存所有权	归属源导出驱动，仅释放自身创建的映射 / 元数据				  归属 Rockchip DRM 驱动，完整释放物理内存 + 所有元数据
	释放核心	解除 dma-buf 绑定，归还引用计数						按内存类型释放物理内存，回收所有内核资源
	禁止操作	绝对不能释放物理内存，否则会导致源驱动访问野指针，触发内核 Oops		   必须 100% 释放所有物理内存和映射，避免内存泄漏
 */
void rockchip_gem_free_object(struct drm_gem_object *obj)
{
	struct drm_device *drm = obj->dev;
	struct rockchip_drm_private *private = drm->dev_private;
	struct rockchip_gem_object *rk_obj = to_rockchip_obj(obj);

	/*
		import_attach是 DRM GEM 对象的标准成员，类型为struct dma_buf_attachment *。
			当且仅当这个 GEM 对象是通过 dma-buf 从其他驱动（摄像头 ISP、视频解码器、
			GPU 等）导入时，该指针才非空。
		内存所有权规则：导入的 buffer，物理内存的所有者是导出驱动（如 ISP），Rockchip DRM 
			驱动仅持有临时访问权限，绝对不能释放物理内存本身，只能释放自己为访问该 buffer 
			创建的映射、元数据等资源。
		典型场景：Android 相机预览链路，ISP 驱动分配图像 buffer，通过 dma-buf 传给显示驱动直
			接上屏，显示驱动的 GEM 对象就是导入类型。
	*/
	if (obj->import_attach) {
		if (private->domain) {
			rockchip_gem_iommu_unmap(rk_obj);
		} else {
			/*
				无 IOMMU 场景：调用内核标准 API dma_unmap_sgtable，解除 DMA 总线地址映射：
				失效该 buffer 的 DMA 缓存同步状态，保证 CPU 和外设的内存一致性；
				清除 sg_table 中每个条目的dma_address总线地址，避免野指针访问；
				DMA_BIDIRECTIONAL和映射时的方向保持一致，保证双向读写的缓存都能正确失效。
			*/
			dma_unmap_sgtable(drm->dev, rk_obj->sgt,
					  DMA_BIDIRECTIONAL, 0);
		}
		/*
			释放导入流程中通过drm_calloc_large分配的pages数组，该数组仅用
				来存放从导入的 sg_table 中提取的物理页指针，是 Rockch
				ip 驱动为适配自身框架创建的元数据，而非物理内存本身。
			必须用drm_free_large释放，和分配时的drm_calloc_large对应，该函数会自动识
				别数组是 kmalloc 还是 vmalloc 分配，调用对应的释放函数，避免内存泄漏。
		*/
		drm_free_large(rk_obj->pages);
		// 调用该函数完成 dma-buf 导入相关的收尾工作
		rockchip_gem_destroy(obj, rk_obj->sgt);
	} else {
		/*
			obj->import_attach为 NULL，代表这个 GEM 对象是 Rockchip DRM 驱动原生创建的（
				用户态通过ROCKCHIP_GEM_CREATE或dumb_create ioctl 创建），物理内存的
				所有权完全归属 Rockchip DRM 驱动，需要完整释放从物理内存到内核元数据的所有资源。
		*/
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
	/*
		alloc_kmap：是否需要内核虚拟地址映射
		为 true：内核可直接访问缓冲区（用于驱动内部操作）
		为 false：不映射，节省内核地址空间
	*/
	bool alloc_kmap = flags & ROCKCHIP_BO_ALLOC_KMAP ? true : false;

	rk_obj = rockchip_gem_create_object(drm, size, alloc_kmap, flags);
	if (IS_ERR(rk_obj))
		return ERR_CAST(rk_obj);

	obj = &rk_obj->base;

	/*
	 * allocate a id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 * 在 file_priv 的 IDR 表中分配一个唯一 ID
		把 GEM 对象与该 ID 绑定
		将 ID 写入 *handle 返回给用户态
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
	/*
		dma_map_sgtable：为 sg_table 执行标准 DMA 总线地址映射，填充每个 sg 条目的 
			dma_address（外设可直接访问的物理总线地址），同时完成 DMA 缓存同步；
		强制连续性校验：无 IOMMU 时，VOP 显示控制器只能访问物理连续的内存，因此必须校
			验 sg_table 对应的物理内存是否 100% 连续，若不连续直接报错回滚，避免显示黑屏、硬件访问错误；
		rk_obj->dma_addr = sg_dma_address(sg->sgl)：把连续物理内存的起始总线地址赋
			值给 rk_obj->dma_addr，供外设寄存器配置使用；
		rk_obj->sgt = sg：绑定 sg_table 到 GEM 对象，供后续缓存同步、释放操作使用。
	*/
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

/*
	将其他驱动（ISP 摄像头、视频编解码器、GPU、V4L2 等）通过 dma-buf 共享过来的物理内存，
		封装成 Rockchip DRM 驱动可识别、可操作的标准 GEM 图形对象，让 Rockchip 显示
		控制器（VOP）、GPU 等硬件能直接访问外部驱动分配的内存，全程零内存拷贝。
*/
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

	/*
		调用 DRM 框架封装的大内存分配函数，和你之前解析的 CMA 分配路径完全一致：
		数组大小 ≤ 1 页时，用 kcalloc 从 slab 分配；
		数组大小 > 1 页时，用 __vmalloc 从高端内存分配，避免耗尽内核稀缺的低端线性映射区；
		数组的作用：存放从 sg_table 中提取的每个物理页的 struct page * 指针，是适配 DRM 框架标准能力的核心。

		没有 pages 数组，导入的 GEM 对象无法支持用户态 mmap、内核态 vmap、
			dma-buf 二次导出、缓存同步等 DRM 标准能力；
		有了 pages 数组，上层的显示、合成、共享逻辑完全不用区分「原生创建的 
			GEM」和「外部导入的 GEM」，一套代码适配所有场景，实现极致的代码复用。
	*/
	rk_obj->num_pages = rk_obj->base.size >> PAGE_SHIFT;
	rk_obj->pages = drm_calloc_large(rk_obj->num_pages, sizeof(*rk_obj->pages));
	if (!rk_obj->pages) {
		DRM_ERROR("failed to allocate pages.\n");
		ret = -ENOMEM;
		goto err_free_rk_obj;
	}

	/*
		遍历 sg_table 中的所有 scatterlist 条目；
		按 buffer 的线性逻辑顺序，提取每个物理页对应的 struct page * 指针，依次填充到 rk_obj->pages 数组中；
		无论源内存是物理连续还是离散，最终都会生成格式 100% 统一的 page 数组，完全抹平不同驱动内存分配的底层差异。
	*/
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

	/*
		struct drm_rockchip_gem_map_off {
			uint32_t handle;  // 输入：GEM缓冲区句柄（创建接口返回）
			uint32_t pad;     // 填充对齐
			uint64_t offset;  // 输出：mmap用的文件偏移量
		};
		通过 handle 从进程的 DRM 文件句柄表中，找到对应的 GEM 对象；
		调用 DRM 标准 API drm_gem_dumb_map_offset，为 GEM 对象分配一个唯一的、
			全局的虚拟文件偏移量（fake offset）；
		把偏移量返回给用户态，作为mmap的offset入参

		用户态拿到 handle 和 offset 后，执行 mmap 即可直接读写缓冲区：
		// 示例：用户态映射代码
		void *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, offset);
		// 直接填充画面数据，无需内核交互
		memcpy(buf, frame_data, size);
	*/
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

	/*
		struct drm_rockchip_gem_phys {
			uint32_t handle;    // 输入：GEM缓冲区句柄
			uint32_t pad;       // 填充对齐
			uint64_t phy_addr;  // 输出：缓冲区起始物理地址
		};
	*/
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

	// 通过page_to_phys(rk_obj->pages[0])获取缓冲区的起始物理地址，返回给用户态。
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
