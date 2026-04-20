// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author:Mark Yao <mark.yao@rock-chips.com>
 */

#include <linux/kernel.h>
#include <linux/devfreq.h>

#include <drm/drm.h>
#include <drm/drm_atomic.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>
#include <soc/rockchip/rockchip_dmc.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_fb.h"
#include "rockchip_drm_gem.h"
#include "rockchip_drm_logo.h"

static bool is_rockchip_logo_fb(struct drm_framebuffer *fb)
{
	return fb->flags & ROCKCHIP_DRM_MODE_LOGO_FB ? true : false;
}

static void __rockchip_drm_fb_destroy(struct drm_framebuffer *fb)
{
	int i = 0;

	drm_framebuffer_cleanup(fb);

	if (is_rockchip_logo_fb(fb)) {
		struct rockchip_drm_logo_fb *rockchip_logo_fb = to_rockchip_logo_fb(fb);

#ifndef MODULE
		rockchip_free_loader_memory(fb->dev);
#endif
		drm_gem_object_release(rockchip_logo_fb->fb.obj[0]);
		kfree(rockchip_logo_fb);
	} else {
		for (i = 0; i < 4; i++) {
			if (fb->obj[i])
				drm_gem_object_put(fb->obj[i]);
		}

		kfree(fb);
	}
}

static void rockchip_drm_fb_destroy_work(struct work_struct *work)
{
	struct rockchip_drm_logo_fb *fb;

	fb = container_of(to_delayed_work(work), struct rockchip_drm_logo_fb, destroy_work);

	__rockchip_drm_fb_destroy(&fb->fb);
}

static void rockchip_drm_fb_destroy(struct drm_framebuffer *fb)
{

	if (is_rockchip_logo_fb(fb)) {
		struct rockchip_drm_logo_fb *rockchip_logo_fb = to_rockchip_logo_fb(fb);

		/* 
			schedule_delayed_work(..., HZ)：不立即销毁 logo 帧缓冲，
			而是延迟 1 秒（HZ为 Linux 内核时钟频率，1HZ 对应 1 秒）执行销毁工作。 
		*/
		schedule_delayed_work(&rockchip_logo_fb->destroy_work, HZ);
	} else {
		__rockchip_drm_fb_destroy(fb);
	}
}

/*
	DRM 框架调用时机：用户态进程通过DRM_IOCTL_MODE_GETFB2 ioctl，
		请求获取帧缓冲对应的 GEM 对象句柄时，DRM 核心自动触发该回调。
*/
static int rockchip_drm_gem_fb_create_handle(struct drm_framebuffer *fb,
					     struct drm_file *file,
					     unsigned int *handle)
{
	if (is_rockchip_logo_fb(fb))
		return -EOPNOTSUPP;

	/*
		1.为当前用户态进程（struct drm_file *file），在其 DRM 文件上下文
			的 IDR 基数树中，为帧缓冲绑定的 GEM 对象分配唯一的 32 位句柄；
		2.递增 GEM 对象的引用计数，保证用户态持有句柄期间，GEM 对象和对应内存不会被释放；
		3.将创建的句柄通过handle指针返回给用户态，供后续 mmap、PRIME 导出等操作使用。
	*/
	return drm_gem_fb_create_handle(fb, file, handle);
}

/*
	struct drm_framebuffer_funcs 是 Linux DRM 内核框架强制要求为
		每个帧缓冲（struct drm_framebuffer）绑定的标准操作回调集
	对普通用户态创建的帧缓冲：完全遵循 DRM GEM 标准框架的默认行为；
	LOGO 专用帧缓冲（标记了ROCKCHIP_DRM_MODE_LOGO_FB flag）：实现定制化的生命周期管控、
		内存释放逻辑、用户态访问拦截，核心保障开机 logo 无缝显示的稳定性，避免用户态
		操作或时序问题破坏内核态 logo 显示。
*/
static const struct drm_framebuffer_funcs rockchip_drm_fb_funcs = {
	/*
		DRM 框架调用时机：当帧缓冲的引用计数降为 0 时，DRM 核心自动触发该回调，
			是帧缓冲生命周期的终点，负责完成所有资源释放、内存回收、框架状
			态清理，避免内存泄漏和内核状态异常。
		核心设计亮点：区分 logo 帧缓冲与普通帧缓冲，为 logo 帧缓冲实现延迟销毁
			机制，这是瑞芯微开机 logo 无闪屏无缝切换的核心关键设计之一。
	*/
	.destroy       = rockchip_drm_fb_destroy,
	.create_handle = rockchip_drm_gem_fb_create_handle,
};

struct drm_framebuffer *
rockchip_fb_alloc(struct drm_device *dev, const struct drm_mode_fb_cmd2 *mode_cmd,
		  struct drm_gem_object **obj, unsigned int num_planes)
{
	struct drm_framebuffer *fb;
	int ret;
	int i;

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb)
		return ERR_PTR(-ENOMEM);

	drm_helper_mode_fill_fb_struct(dev, fb, mode_cmd);

	for (i = 0; i < num_planes; i++)
		fb->obj[i] = obj[i];

	ret = drm_framebuffer_init(dev, fb, &rockchip_drm_fb_funcs);
	if (ret) {
		DRM_DEV_ERROR(dev->dev,
			      "Failed to initialize framebuffer: %d\n",
			      ret);
		kfree(fb);
		return ERR_PTR(ret);
	}

	return fb;
}

struct drm_framebuffer *
rockchip_drm_logo_fb_alloc(struct drm_device *dev, const struct drm_mode_fb_cmd2 *mode_cmd,
			   struct rockchip_logo *logo)
{
	int ret = 0;
	struct rockchip_drm_logo_fb *rockchip_logo_fb;
	struct drm_framebuffer *fb;

	rockchip_logo_fb = kzalloc(sizeof(*rockchip_logo_fb), GFP_KERNEL);
	if (!rockchip_logo_fb)
		return ERR_PTR(-ENOMEM);
	fb = &rockchip_logo_fb->fb;

	/* 
		这是 DRM 框架提供的辅助函数，将 mode_cmd 中的显示参数（宽、高、像素格式、行间距 pitch、偏移量等）
		自动填充到 fb 结构体的对应字段，完成标准帧缓冲的基础初始化，避免手动赋值出错。
	*/
	drm_helper_mode_fill_fb_struct(dev, fb, mode_cmd);

	/*
		核心初始化：调用 DRM 核心函数 drm_framebuffer_init，将 fb 注册到 DRM 子系统，
			并绑定自定义的 rockchip_drm_fb_funcs 回调函数集（包含 destroy、create_handle 等）。
		回调绑定的意义：通过 rockchip_drm_fb_funcs，Logo 帧缓冲的销毁、Handle 
			创建等操作会走 Rockchip 定制的流程（如延迟销毁、禁止用户态映射），区别于普通帧缓冲。
	*/
	ret = drm_framebuffer_init(dev, fb, &rockchip_drm_fb_funcs);
	if (ret) {
		DRM_DEV_ERROR(dev->dev,
			      "Failed to initialize rockchip logo fb: %d\n",
			      ret);
		kfree(rockchip_logo_fb);
		return ERR_PTR(ret);
	}

	/*
		GEM 对象的作用：DRM 框架通过 GEM（Graphics Execution Manager）对象管理图形内存，
			即使 Logo 内存是预留在 reserved-memory 中的，也需要封装成 GEM 对象才能被 DRM 框架识别。
		关联 GEM 对象：fb.obj[0] 指向自定义 GEM 对象 rk_obj 的基类（struct drm_gem_object），
			因为 Logo 是单平面显示，所以只需要 obj[0]。
		初始化 GEM 对象：drm_gem_object_init 初始化 GEM 对象的核心字段，
			设置大小为页对齐后的 Logo 大小，符合 DRM GEM 框架的内存管理要求。
		绑定内存地址：
		rk_obj.dma_addr = logo->dma_addr：设置 DMA/IOMMU 地址（VOP 显示硬件访问的地址）。
		rk_obj.kvaddr = logo->kvaddr：设置内核虚拟地址（CPU 读写 Logo 数据的地址）。
		至此，GEM 对象完全关联到了之前预留的 Logo 物理内存，DRM 框架可以通过 GEM 对象访问 Logo 数据。
	*/
	fb->flags |= ROCKCHIP_DRM_MODE_LOGO_FB;
	rockchip_logo_fb->logo = logo;
	rockchip_logo_fb->fb.obj[0] = &rockchip_logo_fb->rk_obj.base;
	drm_gem_object_init(dev, rockchip_logo_fb->fb.obj[0], PAGE_ALIGN(logo->size));
	rockchip_logo_fb->rk_obj.dma_addr = logo->dma_addr;
	rockchip_logo_fb->rk_obj.kvaddr = logo->kvaddr;
	logo->count++;
	INIT_DELAYED_WORK(&rockchip_logo_fb->destroy_work, rockchip_drm_fb_destroy_work);
	return &rockchip_logo_fb->fb;
}

static int rockchip_drm_bandwidth_atomic_check(struct drm_device *dev,
					       struct drm_atomic_state *state,
					       struct dmcfreq_vop_info *vop_bw_info)
{
	struct rockchip_drm_private *priv = dev->dev_private;
	struct drm_crtc_state *old_crtc_state;
	const struct rockchip_crtc_funcs *funcs;
	struct drm_crtc *crtc;
	int i;

	vop_bw_info->line_bw_mbyte = 0;
	vop_bw_info->frame_bw_mbyte = 0;
	vop_bw_info->plane_num = 0;

	for_each_old_crtc_in_state(state, crtc, old_crtc_state, i) {
		funcs = priv->crtc_funcs[drm_crtc_index(crtc)];

		if (funcs && funcs->bandwidth)
			funcs->bandwidth(crtc, old_crtc_state, vop_bw_info);
	}

	return 0;
}

static void drm_atomic_helper_connector_commit(struct drm_device *dev,
					       struct drm_atomic_state *old_state)
{
	struct drm_connector *connector;
	struct drm_connector_state *new_conn_state;
	int i;

	for_each_new_connector_in_state(old_state, connector, new_conn_state, i) {
		const struct drm_connector_helper_funcs *funcs;

		funcs = connector->helper_private;
		if (!funcs->atomic_commit)
			continue;

		funcs->atomic_commit(connector, new_conn_state);
	}
}

/**
 * rockchip_drm_atomic_helper_commit_tail_rpm - commit atomic update to hardware
 * @old_state: new modeset state to be committed
 *
 * This is an alternative implementation for the
 * &drm_mode_config_helper_funcs.atomic_commit_tail hook, for drivers
 * that support runtime_pm or need the CRTC to be enabled to perform a
 * commit. Otherwise, one should use the default implementation
 * drm_atomic_helper_commit_tail().
 */
static void rockchip_drm_atomic_helper_commit_tail_rpm(struct drm_atomic_state *old_state)
{
	struct drm_device *dev = old_state->dev;
	struct rockchip_drm_private *prv = dev->dev_private;
	struct dmcfreq_vop_info vop_bw_info;

	drm_atomic_helper_commit_modeset_disables(dev, old_state);

	drm_atomic_helper_commit_modeset_enables(dev, old_state);

	rockchip_drm_bandwidth_atomic_check(dev, old_state, &vop_bw_info);

	rockchip_dmcfreq_vop_bandwidth_update(&vop_bw_info);

	mutex_lock(&prv->ovl_lock);
	drm_atomic_helper_commit_planes(dev, old_state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
	mutex_unlock(&prv->ovl_lock);

	drm_atomic_helper_fake_vblank(old_state);

	drm_atomic_helper_connector_commit(dev, old_state);

	drm_atomic_helper_commit_hw_done(old_state);

	drm_atomic_helper_wait_for_vblanks(dev, old_state);

	drm_atomic_helper_cleanup_planes(dev, old_state);
}

static const struct drm_mode_config_helper_funcs rockchip_mode_config_helpers = {
	.atomic_commit_tail = rockchip_drm_atomic_helper_commit_tail_rpm,
};

static struct drm_framebuffer *
rockchip_fb_create(struct drm_device *dev, struct drm_file *file,
		   const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_afbc_framebuffer *afbc_fb;
	const struct drm_format_info *info;
	int ret, i;

	info = drm_get_format_info(dev, mode_cmd);
	if (!info)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < info->num_planes; ++i) {
		if (mode_cmd->pitches[i] % 4) {
			DRM_DEV_ERROR_RATELIMITED(dev->dev,
				"fb pitch[%d] must be 4 byte aligned: %d\n", i, mode_cmd->pitches[i]);
			return ERR_PTR(-EINVAL);
		}
	}

	afbc_fb = kzalloc(sizeof(*afbc_fb), GFP_KERNEL);
	if (!afbc_fb)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_fb_init_with_funcs(dev, &afbc_fb->base, file, mode_cmd,
					 &rockchip_drm_fb_funcs);
	if (ret) {
		kfree(afbc_fb);
		return ERR_PTR(ret);
	}

	if (drm_is_afbc(mode_cmd->modifier[0])) {
		ret = drm_gem_fb_afbc_init(dev, mode_cmd, afbc_fb);
		if (ret) {
			struct drm_gem_object **obj = afbc_fb->base.obj;

			for (i = 0; i < info->num_planes; ++i)
				drm_gem_object_put(obj[i]);

			kfree(afbc_fb);
			return ERR_PTR(ret);
		}
	}

	return &afbc_fb->base;
}

static void rockchip_drm_output_poll_changed(struct drm_device *dev)
{
	struct rockchip_drm_private *private = dev->dev_private;
	struct drm_fb_helper *fb_helper = private->fbdev_helper;

	if (fb_helper && dev->mode_config.poll_enabled && !private->loader_protect)
		drm_fb_helper_hotplug_event(fb_helper);
}

static const struct drm_mode_config_funcs rockchip_drm_mode_config_funcs = {
	.fb_create = rockchip_fb_create,
	.output_poll_changed = rockchip_drm_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

struct drm_framebuffer *
rockchip_drm_framebuffer_init(struct drm_device *dev,
			      const struct drm_mode_fb_cmd2 *mode_cmd,
			      struct drm_gem_object *obj)
{
	struct drm_framebuffer *fb;

	fb = rockchip_fb_alloc(dev, mode_cmd, &obj, 1);
	if (IS_ERR(fb))
		return ERR_CAST(fb);

	return fb;
}

void rockchip_drm_mode_config_init(struct drm_device *dev)
{
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;

	/*
	 * set max width and height as default value(16384x16384).
	 * this value would be used to check framebuffer size limitation
	 * at drm_mode_addfb().
	 */
	dev->mode_config.max_width = 16384;
	dev->mode_config.max_height = 16384;
	dev->mode_config.async_page_flip = true;

	dev->mode_config.funcs = &rockchip_drm_mode_config_funcs;
	dev->mode_config.helper_private = &rockchip_mode_config_helpers;
}
