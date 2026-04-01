/*
 *
 * Copyright (c) Fuzhou Rockchip Electronics Co.Ltd
 * Authors:
 *       Mark Yao <yzq@rock-chips.com>
 *
 * base on exynos_drm.h
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef _UAPI_ROCKCHIP_DRM_H
#define _UAPI_ROCKCHIP_DRM_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include <drm/drm.h>

/*
 * Send vcnt event instead of blocking,
 * like _DRM_VBLANK_EVENT
 */
#define _DRM_ROCKCHIP_VCNT_EVENT 0x80000000
#define DRM_EVENT_ROCKCHIP_CRTC_VCNT   0xf

/* memory type definitions. */
enum drm_rockchip_gem_mem_type {
	/* Physically Continuous memory. */
	/*
		核心定义：强制分配物理地址连续的内存
		底层逻辑：内核会通过 CMA（连续内存分配器）分配一段物理地址完全连续的内存，而非 Linux 默认的离散物理页。
		典型场景：给不带 MMU 的硬件外设使用，比如瑞芯微的 VOP 显示控制器、ISP 图像处理器、VDU 视频处理单元。这类外设无法做虚拟地址到离散物理页的映射，只能访问物理连续的内存。
		注意事项：CMA 区域空间有限，超大尺寸 buffer 分配可能失败；是显示帧缓存的必选 flag
	*/
	ROCKCHIP_BO_CONTIG	= 1 << 0,
	/* cachable mapping. */
	/*
		核心定义：内存映射为CPU 可缓存模式
		底层逻辑：内核在建立页表时，会把这段内存标记为 CPU L1/L2 缓存可缓存，CPU 读写这段内存会走缓存，读写性能提升 10~100 倍。
		典型场景：CPU 需要频繁读写、做图像数据运算 / 软件渲染的 buffer，比如 CPU 端做图像缩放、格式转换的场景。
		注意事项：会带来缓存一致性问题——CPU 修改的数据会存在缓存里，外设（显示 / GPU）看不到；外设修改的数据，
			CPU 也会读到缓存里的旧值。必须配合头文件里的 CPU_ACQUIRE/CPU_RELEASE IOCTL 做缓存同步。
	*/
	ROCKCHIP_BO_CACHABLE	= 1 << 1,
	/* write-combine mapping. */
	/*
		核心定义：内存映射为Write-Combine（写合并）模式
		底层逻辑：不经过 CPU 的读缓存，CPU 的写操作会被合并成大的突发批量写入内存；读操作直接访问物理内存，无缓存。性能介于「可缓存」和「不可缓存」之间，同时规避了缓存一致性的麻烦。
		典型场景：瑞芯微平台显示帧缓存的默认配置，CPU 单向推帧给显示控制器、GPU 单向写数据、CPU 极少读回的场景，兼顾性能和易用性。
		注意事项：和 ROCKCHIP_BO_CACHABLE 互斥，不能同时设置，否则内核会按非法参数处理。
	*/
	ROCKCHIP_BO_WC		= 1 << 2,
	/*
		核心定义：分配安全世界内存
		底层逻辑：基于 ARM TrustZone 安全扩展，这段内存会被划分到安全地址空间，
			普通世界（Linux 内核、用户态应用）无法直接读写，只有安全世界的 
			OP-TEE 固件、安全外设（安全 ISP、安全显示路径）能访问。
		典型场景：DRM 数字版权保护、高清付费内容的安全播放、指纹 / 人脸等安全生物识别的显示场景。
		注意事项：普通应用无法使用，必须有配套的安全固件和签名，否则分配直接失败。
	*/
	ROCKCHIP_BO_SECURE	= 1 << 3,
	/* keep kmap for cma buffer or alloc kmap for other type memory */
	/*
		核心定义：提前为内存分配并保留内核态虚拟地址映射（kmap）
		底层逻辑：kmap 是内核把物理页映射到内核虚拟地址空间的操作。这个 flag 会让内核在分配内存时，
			就提前完成内核态的虚拟地址映射并长期保留，避免每次访问都临时做 kmap/unmap，减少性能开销。
		典型场景：内核驱动需要频繁在内核态读写这块 GEM buffer 的场景，比如瑞芯微自研的显示驱动、
			GPU 内核驱动配套的内存，用户态普通应用极少用到。
		注意事项：滥用会占用内核虚拟地址空间，仅内核配套组件使用。
	*/
	ROCKCHIP_BO_ALLOC_KMAP	= 1 << 4,
	/* alloc page with gfp_dma32 */
	/*
		核心定义：强制从 **32 位物理地址空间（4GB 以内）** 分配内存
		底层逻辑：ARM64 平台物理地址可以超过 4GB，但部分老外设的 DMA 控制器只支持 32 位地址寻址，
			无法访问 4GB 以上的内存。这个 flag 会强制内核从 ZONE_DMA32 区域分配内存，
			保证物理地址在 4GB 以内。
		典型场景：瑞芯微老款芯片的外设、PCIe 设备、部分第三方 IP 核，这类硬件有 32 位地址寻址限制。
		注意事项：仅 ARM64 平台有意义，ARM32 平台整个物理地址都在 32 位以内，无需设置。
	*/
	ROCKCHIP_BO_DMA32	= 1 << 5,
	ROCKCHIP_BO_MASK	= ROCKCHIP_BO_CONTIG | ROCKCHIP_BO_CACHABLE |
				ROCKCHIP_BO_WC | ROCKCHIP_BO_SECURE | ROCKCHIP_BO_ALLOC_KMAP |
				ROCKCHIP_BO_DMA32,
};

/**
 * User-desired buffer creation information structure.
 *
 * @size: user-desired memory allocation size.
 * @flags: user request for setting memory type or cache attributes.
 * @handle: returned a handle to created gem object.
 *     - this handle will be set by gem module of kernel side.
 */
struct drm_rockchip_gem_create {
	uint64_t size;
	uint32_t flags;
	uint32_t handle;
};

struct drm_rockchip_gem_phys {
	uint32_t handle;
	uint32_t phy_addr;
};

/**
 * A structure for getting buffer offset.
 *
 * @handle: a pointer to gem object created.
 * @pad: just padding to be 64-bit aligned.
 * @offset: relatived offset value of the memory region allocated.
 *     - this value should be set by user.
 */
struct drm_rockchip_gem_map_off {
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;
};

/* acquire type definitions. */
enum drm_rockchip_gem_cpu_acquire_type {
	DRM_ROCKCHIP_GEM_CPU_ACQUIRE_SHARED = 0x0,
	DRM_ROCKCHIP_GEM_CPU_ACQUIRE_EXCLUSIVE = 0x1,
};

enum rockchip_crtc_feture {
	ROCKCHIP_DRM_CRTC_FEATURE_ALPHA_SCALE,
	ROCKCHIP_DRM_CRTC_FEATURE_HDR10,
	ROCKCHIP_DRM_CRTC_FEATURE_NEXT_HDR,
};

enum rockchip_plane_feture {
	ROCKCHIP_DRM_PLANE_FEATURE_SCALE,
	ROCKCHIP_DRM_PLANE_FEATURE_ALPHA,
	ROCKCHIP_DRM_PLANE_FEATURE_HDR2SDR,
	ROCKCHIP_DRM_PLANE_FEATURE_SDR2HDR,
	ROCKCHIP_DRM_PLANE_FEATURE_AFBDC,
	ROCKCHIP_DRM_PLANE_FEATURE_PDAF_POS,
	ROCKCHIP_DRM_PLANE_FEATURE_MAX,
};

enum rockchip_cabc_mode {
	ROCKCHIP_DRM_CABC_MODE_DISABLE,
	ROCKCHIP_DRM_CABC_MODE_NORMAL,
	ROCKCHIP_DRM_CABC_MODE_LOWPOWER,
	ROCKCHIP_DRM_CABC_MODE_USERSPACE,
};

#define DRM_ROCKCHIP_GEM_CREATE		0x00
#define DRM_ROCKCHIP_GEM_MAP_OFFSET	0x01
#define DRM_ROCKCHIP_GEM_CPU_ACQUIRE	0x02
#define DRM_ROCKCHIP_GEM_CPU_RELEASE	0x03
#define DRM_ROCKCHIP_GEM_GET_PHYS	0x04
#define DRM_ROCKCHIP_GET_VCNT_EVENT	0x05

#define DRM_IOCTL_ROCKCHIP_GEM_CREATE	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ROCKCHIP_GEM_CREATE, struct drm_rockchip_gem_create)

#define DRM_IOCTL_ROCKCHIP_GEM_MAP_OFFSET	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ROCKCHIP_GEM_MAP_OFFSET, struct drm_rockchip_gem_map_off)

#define DRM_IOCTL_ROCKCHIP_GEM_CPU_ACQUIRE	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ROCKCHIP_GEM_CPU_ACQUIRE, struct drm_rockchip_gem_cpu_acquire)

#define DRM_IOCTL_ROCKCHIP_GEM_CPU_RELEASE	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ROCKCHIP_GEM_CPU_RELEASE, struct drm_rockchip_gem_cpu_release)

#define DRM_IOCTL_ROCKCHIP_GEM_GET_PHYS		DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ROCKCHIP_GEM_GET_PHYS, struct drm_rockchip_gem_phys)

#define DRM_IOCTL_ROCKCHIP_GET_VCNT_EVENT	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ROCKCHIP_GET_VCNT_EVENT, union drm_wait_vblank)

#endif /* _UAPI_ROCKCHIP_DRM_H */
