// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2021 Rockchip Electronics Co., Ltd.
 * Author: Sandy Huang <hjc@rock-chips.com>
 */
#include <linux/memblock.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/iommu.h>

#include <drm/drm_atomic_uapi.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_fb.h"
#include "rockchip_drm_logo.h"

static bool is_support_hotplug(uint32_t output_type)
{
	switch (output_type) {
	case DRM_MODE_CONNECTOR_DVII:
	case DRM_MODE_CONNECTOR_DVID:
	case DRM_MODE_CONNECTOR_DVIA:
	case DRM_MODE_CONNECTOR_DisplayPort:
	case DRM_MODE_CONNECTOR_HDMIA:
	case DRM_MODE_CONNECTOR_HDMIB:
	case DRM_MODE_CONNECTOR_TV:
		return true;
	default:
		return false;
	}
}

static struct drm_crtc *
find_crtc_by_node(struct drm_device *drm_dev, struct device_node *node)
{
	struct device_node *np_crtc;
	struct drm_crtc *crtc;

	np_crtc = of_get_parent(node);
	if (!np_crtc || !of_device_is_available(np_crtc))
		return NULL;

	drm_for_each_crtc(crtc, drm_dev) {
		if (crtc->port == np_crtc)
			return crtc;
	}

	return NULL;
}

static struct rockchip_drm_sub_dev *
find_sub_dev_by_node(struct drm_device *drm_dev, struct device_node *node)
{
	struct device_node *np_connector;
	struct rockchip_drm_sub_dev *sub_dev;

	np_connector = of_graph_get_remote_port_parent(node);
	if (!np_connector || !of_device_is_available(np_connector))
		return NULL;

	sub_dev = rockchip_drm_get_sub_dev(np_connector);
	if (!sub_dev)
		return NULL;

	return sub_dev;
}

static struct rockchip_drm_sub_dev *
find_sub_dev_by_bridge(struct drm_device *drm_dev, struct device_node *node)
{
	struct device_node *np_encoder, *np_connector = NULL;
	struct rockchip_drm_sub_dev *sub_dev = NULL;
	struct device_node *port, *endpoint;

	np_encoder = of_graph_get_remote_port_parent(node);
	if (!np_encoder || !of_device_is_available(np_encoder))
		goto err_put_encoder;

	port = of_graph_get_port_by_id(np_encoder, 1);
	if (!port) {
		dev_err(drm_dev->dev, "can't found port point!\n");
		goto err_put_encoder;
	}

	for_each_child_of_node(port, endpoint) {
		np_connector = of_graph_get_remote_port_parent(endpoint);
		if (!np_connector) {
			dev_err(drm_dev->dev,
				"can't found connector node, please init!\n");
			goto err_put_port;
		}
		if (!of_device_is_available(np_connector)) {
			of_node_put(np_connector);
			np_connector = NULL;
			continue;
		} else {
			break;
		}
	}
	if (!np_connector) {
		dev_err(drm_dev->dev, "can't found available connector node!\n");
		goto err_put_port;
	}

sub_dev = rockchip_drm_get_sub_dev(np_connector);
	if (!sub_dev)
		goto err_put_port;

	of_node_put(np_connector);
err_put_port:
	of_node_put(port);
err_put_encoder:
	of_node_put(np_encoder);

	return sub_dev;
}

static void rockchip_drm_release_reserve_vm(struct drm_device *drm, struct drm_mm_node *node)
{
	struct rockchip_drm_private *private = drm->dev_private;

	mutex_lock(&private->mm_lock);
	if (drm_mm_node_allocated(node))
		drm_mm_remove_node(node);
	mutex_unlock(&private->mm_lock);
}

static int rockchip_drm_reserve_vm(struct drm_device *drm, struct drm_mm *mm,
				   struct drm_mm_node *node, u64 size, u64 offset)
{
	struct rockchip_drm_private *private = drm->dev_private;
	int ret;

	node->size = size;
	node->start = offset;
	node->color = 0;
	mutex_lock(&private->mm_lock);
	ret = drm_mm_reserve_node(mm, node);
	mutex_unlock(&private->mm_lock);

	return ret;
}

static unsigned long
rockchip_drm_free_reserved_area(phys_addr_t start, phys_addr_t end, int poison, const char *s)
{
	unsigned long pages = 0;

	start = ALIGN_DOWN(start, PAGE_SIZE);
	end = PAGE_ALIGN(end);
	for (; start < end; start += PAGE_SIZE) {
		struct page *page = phys_to_page(start);
		void *direct_map_addr;

		if (!pfn_valid(__phys_to_pfn(start)))
			continue;

		/*
		 * 'direct_map_addr' might be different from 'pos'
		 * because some architectures' virt_to_page()
		 * work with aliases.  Getting the direct map
		 * address ensures that we get a _writeable_
		 * alias for the memset().
		 */
		direct_map_addr = page_address(page);
		/*
		 * Perform a kasan-unchecked memset() since this memory
		 * has not been initialized.
		 */
		direct_map_addr = kasan_reset_tag(direct_map_addr);
		if ((unsigned int)poison <= 0xFF)
			memset(direct_map_addr, poison, PAGE_SIZE);

		free_reserved_page(page);
		pages++;
	}

	if (pages && s)
		pr_info("Freeing %s memory: %ldK\n", s, pages << (PAGE_SHIFT - 10));

	return pages;
}

void rockchip_free_loader_memory(struct drm_device *drm)
{
	struct rockchip_drm_private *private = drm->dev_private;
	struct rockchip_logo *logo;

	if (!private || !private->logo || --private->logo->count)
		return;

	logo = private->logo;

	if (private->domain) {
		u32 pg_size = 1UL << __ffs(private->domain->pgsize_bitmap);

		iommu_unmap(private->domain, logo->dma_addr, ALIGN(logo->size, pg_size));
		rockchip_drm_release_reserve_vm(drm, &logo->logo_reserved_node);
	}

	memblock_free(logo->start, logo->size);
	rockchip_drm_free_reserved_area(logo->start, logo->start + logo->size,
					-1, "drm_logo");
	kfree(logo);
	private->logo = NULL;
	private->loader_protect = false;
}

static int init_loader_memory(struct drm_device *drm_dev)
{
	struct rockchip_drm_private *private = drm_dev->dev_private;
	struct rockchip_logo *logo;
	struct device_node *np = drm_dev->dev->of_node;
	struct device_node *node;
	phys_addr_t start, size;
	u32 pg_size = PAGE_SIZE;
	struct resource res;
	int ret, idx;

	idx = of_property_match_string(np, "memory-region-names", "drm-logo");
	if (idx >= 0)
		node = of_parse_phandle(np, "memory-region", idx);
	else
		node = of_parse_phandle(np, "logo-memory-region", 0);
	if (!node)
		return -ENOMEM;

	ret = of_address_to_resource(node, 0, &res);
	if (ret)
		return ret;
	if (private->domain)
		pg_size = 1UL << __ffs(private->domain->pgsize_bitmap);
	start = ALIGN_DOWN(res.start, pg_size);
	size = resource_size(&res);
	if (!size)
		return -ENOMEM;
	if (!IS_ALIGNED(res.start, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
		DRM_ERROR("Reserved logo memory should be aligned as:0x%lx, cureent is:start[%pad] size[%pad]\n",
			  PAGE_SIZE, &res.start, &size);
	if (pg_size != PAGE_SIZE)
		DRM_WARN("iommu page size[0x%x] isn't equal to OS page size[0x%lx]\n", pg_size, PAGE_SIZE);

	logo = kmalloc(sizeof(*logo), GFP_KERNEL);
	if (!logo)
		return -ENOMEM;

	logo->kvaddr = phys_to_virt(start);

	if (private->domain) {
		ret = rockchip_drm_reserve_vm(drm_dev, &private->mm, &logo->logo_reserved_node, size, start);
		if (ret)
			dev_err(drm_dev->dev, "failed to reserve vm for logo memory\n");
		ret = iommu_map(private->domain, start, start, ALIGN(size, pg_size),
				IOMMU_WRITE | IOMMU_READ);
		if (ret) {
			dev_err(drm_dev->dev, "failed to create 1v1 mapping\n");
			goto err_free_logo;
		}
	}

	logo->dma_addr = start;
	logo->start = res.start;
	logo->size = size;
	logo->count = 1;
	private->logo = logo;

	idx = of_property_match_string(np, "memory-region-names", "drm-cubic-lut");
	if (idx < 0)
		return 0;

	node = of_parse_phandle(np, "memory-region", idx);
	if (!node)
		return -ENOMEM;

	ret = of_address_to_resource(node, 0, &res);
	if (ret)
		return ret;
	start = ALIGN_DOWN(res.start, pg_size);
	size = resource_size(&res);
	if (!size)
		return 0;
	if (!IS_ALIGNED(res.start, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
		DRM_ERROR("Reserved drm cubic memory should be aligned as:0x%lx, cureent is:start[%pad] size[%pad]\n",
			  PAGE_SIZE, &res.start, &size);

	private->cubic_lut_kvaddr = phys_to_virt(start);
	if (private->domain) {
		private->clut_reserved_node = kmalloc(sizeof(struct drm_mm_node), GFP_KERNEL);
		if (!private->clut_reserved_node)
			return -ENOMEM;

		ret = rockchip_drm_reserve_vm(drm_dev, &private->mm, private->clut_reserved_node, size, start);
		if (ret)
			dev_err(drm_dev->dev, "failed to reserve vm for clut memory\n");

		ret = iommu_map(private->domain, start, start, ALIGN(size, pg_size),
				IOMMU_WRITE | IOMMU_READ);
		if (ret) {
			dev_err(drm_dev->dev, "failed to create 1v1 mapping for cubic lut\n");
			goto err_free_clut;
		}
	}
	private->cubic_lut_dma_addr = start;

	return 0;

err_free_clut:
	rockchip_drm_release_reserve_vm(drm_dev, private->clut_reserved_node);
	kfree(private->clut_reserved_node);
	private->clut_reserved_node = NULL;
err_free_logo:
	rockchip_drm_release_reserve_vm(drm_dev, &logo->logo_reserved_node);
	kfree(logo);

	return ret;
}

static struct drm_framebuffer *
get_framebuffer_by_node(struct drm_device *drm_dev, struct device_node *node)
{
	struct rockchip_drm_private *private = drm_dev->dev_private;
	struct drm_mode_fb_cmd2 mode_cmd = { 0 };
	u32 val;
	int bpp;

	if (WARN_ON(!private->logo))
		return NULL;

	if (of_property_read_u32(node, "logo,offset", &val)) {
		dev_err(drm_dev->dev, "%s: failed to get logo,offset\n", node->full_name);
		return NULL;
	}
	mode_cmd.offsets[0] = val;

	if (of_property_read_u32(node, "logo,width", &val)) {
		dev_err(drm_dev->dev, "%s: failed to get logo,width\n", node->full_name);
		return NULL;
	}
	mode_cmd.width = val;

	if (of_property_read_u32(node, "logo,height", &val)) {
		dev_err(drm_dev->dev, "%s: failed to get logo,height\n", node->full_name);
		return NULL;
	}
	mode_cmd.height = val;

	if (of_property_read_u32(node, "logo,bpp", &val)) {
		dev_err(drm_dev->dev, "%s: failed to get logo,bpp\n", node->full_name);
		return NULL;
	}
	bpp = val;

	mode_cmd.pitches[0] = ALIGN(mode_cmd.width * bpp, 32) / 8;

	switch (bpp) {
	case 16:
		mode_cmd.pixel_format = DRM_FORMAT_RGB565;
		break;
	case 24:
		mode_cmd.pixel_format = DRM_FORMAT_RGB888;
		break;
	case 32:
		mode_cmd.pixel_format = DRM_FORMAT_XRGB8888;
		break;
	default:
		dev_err(drm_dev->dev, "%s: unsupported to logo bpp %d\n", node->full_name, bpp);
		return NULL;
	}

	return rockchip_drm_logo_fb_alloc(drm_dev, &mode_cmd, private->logo);
}

static struct rockchip_drm_mode_set *
of_parse_display_resource(struct drm_device *drm_dev, struct device_node *route)
{
	struct rockchip_drm_private *private = drm_dev->dev_private;
	struct rockchip_drm_mode_set *set;
	struct device_node *connect;
	struct drm_framebuffer *fb;
	struct rockchip_drm_sub_dev *sub_dev;
	struct drm_crtc *crtc;
	const char *string;
	u32 val;

	connect = of_parse_phandle(route, "connect", 0);
	if (!connect)
		return NULL;

	fb = get_framebuffer_by_node(drm_dev, route);
	if (IS_ERR_OR_NULL(fb))
		return NULL;

	crtc = find_crtc_by_node(drm_dev, connect);

	sub_dev = find_sub_dev_by_node(drm_dev, connect);

	if (!sub_dev)
		sub_dev = find_sub_dev_by_bridge(drm_dev, connect);

	if (!crtc || !sub_dev) {
		dev_warn(drm_dev->dev,
			 "No available crtc or connector for display");
		drm_framebuffer_put(fb);
		return NULL;
	}

	set = kzalloc(sizeof(*set), GFP_KERNEL);
	if (!set)
		return NULL;

	if (!of_property_read_u32(route, "video,clock", &val))
		set->clock = val;

	if (!of_property_read_u32(route, "video,hdisplay", &val))
		set->hdisplay = val;

	if (!of_property_read_u32(route, "video,vdisplay", &val))
		set->vdisplay = val;

	if (!of_property_read_u32(route, "video,crtc_hsync_end", &val))
		set->crtc_hsync_end = val;

	if (!of_property_read_u32(route, "video,crtc_vsync_end", &val))
		set->crtc_vsync_end = val;

	if (!of_property_read_u32(route, "video,vrefresh", &val))
		set->vrefresh = val;

	if (!of_property_read_u32(route, "video,flags", &val))
		set->flags = val;

	if (!of_property_read_u32(route, "video,aspect_ratio", &val))
		set->picture_aspect_ratio = val;

	if (!of_property_read_u32(route, "overscan,left_margin", &val))
		set->left_margin = val;

	if (!of_property_read_u32(route, "overscan,right_margin", &val))
		set->right_margin = val;

	if (!of_property_read_u32(route, "overscan,top_margin", &val))
		set->top_margin = val;

	if (!of_property_read_u32(route, "overscan,bottom_margin", &val))
		set->bottom_margin = val;

	if (!of_property_read_u32(route, "bcsh,brightness", &val))
		set->brightness = val;
	else
		set->brightness = 50;

	if (!of_property_read_u32(route, "bcsh,contrast", &val))
		set->contrast = val;
	else
		set->contrast = 50;

	if (!of_property_read_u32(route, "bcsh,saturation", &val))
		set->saturation = val;
	else
		set->saturation = 50;

	if (!of_property_read_u32(route, "bcsh,hue", &val))
		set->hue = val;
	else
		set->hue = 50;

	set->force_output = of_property_read_bool(route, "force-output");

	if (!of_property_read_u32(route, "cubic_lut,offset", &val)) {
		private->cubic_lut[crtc->index].enable = true;
		private->cubic_lut[crtc->index].offset = val;
	}

	set->ratio = 1;
	if (!of_property_read_string(route, "logo,mode", &string) &&
	    !strcmp(string, "fullscreen"))
		set->ratio = 0;

	set->fb = fb;
	set->crtc = crtc;
	set->sub_dev = sub_dev;

	return set;
}

static int rockchip_drm_fill_connector_modes(struct drm_connector *connector,
					     uint32_t maxX, uint32_t maxY,
					     bool force_output)
{
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;
	const struct drm_connector_helper_funcs *connector_funcs =
		connector->helper_private;
	int count = 0;
	bool verbose_prune = true;
	enum drm_connector_status old_status;

	WARN_ON(!mutex_is_locked(&dev->mode_config.mutex));

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n", connector->base.id,
		      connector->name);
	/* set all modes to the unverified state */
	list_for_each_entry(mode, &connector->modes, head)
		mode->status = MODE_STALE;

	if (force_output)
		connector->force = DRM_FORCE_ON;
	if (connector->force) {
		if (connector->force == DRM_FORCE_ON ||
		    connector->force == DRM_FORCE_ON_DIGITAL)
			connector->status = connector_status_connected;
		else
			connector->status = connector_status_disconnected;
		if (connector->funcs->force)
			connector->funcs->force(connector);
	} else {
		old_status = connector->status;

		if (connector->funcs->detect)
			connector->status = connector->funcs->detect(connector, true);
		else
			connector->status  = connector_status_connected;
		/*
		 * Normally either the driver's hpd code or the poll loop should
		 * pick up any changes and fire the hotplug event. But if
		 * userspace sneaks in a probe, we might miss a change. Hence
		 * check here, and if anything changed start the hotplug code.
		 */
		if (old_status != connector->status) {
			DRM_DEBUG_KMS("[CONNECTOR:%d:%s] status updated from %d to %d\n",
				      connector->base.id,
				      connector->name,
				      old_status, connector->status);

			/*
			 * The hotplug event code might call into the fb
			 * helpers, and so expects that we do not hold any
			 * locks. Fire up the poll struct instead, it will
			 * disable itself again.
			 */
			dev->mode_config.delayed_event = true;
			if (dev->mode_config.poll_enabled)
				schedule_delayed_work(&dev->mode_config.output_poll_work,
						      0);
		}
	}

	/* Re-enable polling in case the global poll config changed. */
	if (!dev->mode_config.poll_running)
		drm_kms_helper_poll_enable(dev);

	dev->mode_config.poll_running = true;

	if (connector->status == connector_status_disconnected) {
		DRM_DEBUG_KMS("[CONNECTOR:%d:%s] disconnected\n",
			      connector->base.id, connector->name);
		drm_connector_update_edid_property(connector, NULL);
		verbose_prune = false;
		goto prune;
	}

	if (!force_output)
		count = (*connector_funcs->get_modes)(connector);

	if (count == 0 && connector->status == connector_status_connected)
		count = drm_add_modes_noedid(connector, 4096, 4096);
	if (force_output)
		count += rockchip_drm_add_modes_noedid(connector);
	if (count == 0)
		goto prune;

	drm_connector_list_update(connector);

	list_for_each_entry(mode, &connector->modes, head) {
		if (mode->status == MODE_OK)
			mode->status = drm_mode_validate_driver(dev, mode);

		if (mode->status == MODE_OK)
			mode->status = drm_mode_validate_size(mode, maxX, maxY);

		/**
		 * if (mode->status == MODE_OK)
		 *	mode->status = drm_mode_validate_flag(mode, mode_flags);
		 */
		if (mode->status == MODE_OK && connector_funcs->mode_valid)
			mode->status = connector_funcs->mode_valid(connector,
								   mode);
		if (mode->status == MODE_OK)
			mode->status = drm_mode_validate_ycbcr420(mode,
								  connector);
	}

prune:
	drm_mode_prune_invalid(dev, &connector->modes, verbose_prune);

	if (list_empty(&connector->modes))
		return 0;

	drm_mode_sort(&connector->modes);

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s] probed modes :\n", connector->base.id,
		      connector->name);
	list_for_each_entry(mode, &connector->modes, head) {
		drm_mode_set_crtcinfo(mode, CRTC_INTERLACE_HALVE_V);
		drm_mode_debug_printmodeline(mode);
	}

	return count;
}

/*
 * For connectors that support multiple encoders, either the
 * .atomic_best_encoder() or .best_encoder() operation must be implemented.
 */
static struct drm_encoder *
rockchip_drm_connector_get_single_encoder(struct drm_connector *connector)
{
	struct drm_encoder *encoder;

	WARN_ON(hweight32(connector->possible_encoders) > 1);
	drm_connector_for_each_possible_encoder(connector, encoder)
		return encoder;

	return NULL;
}

static int setup_initial_state(struct drm_device *drm_dev,
			       struct drm_atomic_state *state,
			       struct rockchip_drm_mode_set *set)
{
	struct rockchip_drm_private *priv = drm_dev->dev_private;
	struct drm_connector *connector = set->sub_dev->connector;
	struct drm_crtc *crtc = set->crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	struct drm_plane_state *primary_state;
	struct drm_display_mode *mode = NULL;
	const struct drm_connector_helper_funcs *funcs;
	int pipe = drm_crtc_index(crtc);
	bool is_crtc_enabled = true;
	int hdisplay, vdisplay;
	int fb_width, fb_height;
	int found = 0, match = 0;
	int num_modes;
	int ret = 0;
	struct rockchip_crtc_state *s = NULL;

	if (!set->hdisplay || !set->vdisplay || !set->vrefresh)
		is_crtc_enabled = false;

	crtc->state->state = state;

	conn_state = drm_atomic_get_connector_state(state, connector);
	if (IS_ERR(conn_state))
		return PTR_ERR(conn_state);

	funcs = connector->helper_private;

	if (funcs->best_encoder)
		conn_state->best_encoder = funcs->best_encoder(connector);
	else
		conn_state->best_encoder = rockchip_drm_connector_get_single_encoder(connector);

	if (set->sub_dev->loader_protect) {
		ret = set->sub_dev->loader_protect(conn_state->best_encoder, true);
		if (ret) {
			dev_err(drm_dev->dev,
				"connector[%s] loader protect failed\n",
				connector->name);
			return ret;
		}
	}

	num_modes = rockchip_drm_fill_connector_modes(connector, 7680, 7680, set->force_output);
	if (!num_modes) {
		dev_err(drm_dev->dev, "connector[%s] can't found any modes\n",
			connector->name);
		ret = -EINVAL;
		goto error_conn;
	}

	list_for_each_entry(mode, &connector->modes, head) {
		if (mode->clock == set->clock &&
		    mode->hdisplay == set->hdisplay &&
		    mode->vdisplay == set->vdisplay &&
		    mode->crtc_hsync_end == set->crtc_hsync_end &&
		    mode->crtc_vsync_end == set->crtc_vsync_end &&
		    drm_mode_vrefresh(mode) == set->vrefresh &&
		    /* we just need to focus on DRM_MODE_FLAG_ALL flag, so here
		     * we compare mode->flags with set->flags & DRM_MODE_FLAG_ALL.
		     */
		    mode->flags == (set->flags & DRM_MODE_FLAG_ALL) &&
		    mode->picture_aspect_ratio == set->picture_aspect_ratio) {
			found = 1;
			match = 1;
			break;
		}
	}

	if (!found) {
		ret = -EINVAL;
		connector->status = connector_status_disconnected;
		dev_err(drm_dev->dev, "connector[%s] can't found any match mode\n",
			connector->name);
		DRM_INFO("%s support modes:\n\n", connector->name);
		list_for_each_entry(mode, &connector->modes, head) {
			DRM_INFO(DRM_MODE_FMT "\n", DRM_MODE_ARG(mode));
		}
		DRM_INFO("uboot set mode: h/v display[%d,%d] h/v sync_end[%d,%d] vfresh[%d], flags[0x%x], aspect_ratio[%d]\n",
			 set->hdisplay, set->vdisplay, set->crtc_hsync_end, set->crtc_vsync_end,
			 set->vrefresh, set->flags, set->picture_aspect_ratio);
		goto error_conn;
	}

	conn_state->tv.brightness = set->brightness;
	conn_state->tv.contrast = set->contrast;
	conn_state->tv.saturation = set->saturation;
	conn_state->tv.hue = set->hue;
	set->mode = mode;
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto error_conn;
	}

	drm_mode_copy(&crtc_state->adjusted_mode, mode);
	if (!match || !is_crtc_enabled) {
		set->mode_changed = true;
	} else {
		ret = drm_atomic_set_crtc_for_connector(conn_state, crtc);
		if (ret)
			goto error_conn;

		mode->picture_aspect_ratio = HDMI_PICTURE_ASPECT_NONE;
		ret = drm_atomic_set_mode_for_crtc(crtc_state, mode);
		if (ret)
			goto error_conn;

		crtc_state->active = true;

		if (priv->crtc_funcs[pipe] &&
		    priv->crtc_funcs[pipe]->loader_protect)
			priv->crtc_funcs[pipe]->loader_protect(crtc, true);
	}

	if (!set->fb) {
		ret = 0;
		goto error_crtc;
	}
	primary_state = drm_atomic_get_plane_state(state, crtc->primary);
	if (IS_ERR(primary_state)) {
		ret = PTR_ERR(primary_state);
		goto error_crtc;
	}

	hdisplay = mode->hdisplay;
	vdisplay = mode->vdisplay;
	fb_width = set->fb->width;
	fb_height = set->fb->height;

	primary_state->crtc = crtc;
	primary_state->src_x = 0;
	primary_state->src_y = 0;
	primary_state->src_w = fb_width << 16;
	primary_state->src_h = fb_height << 16;
	if (set->ratio) {
		if (set->fb->width >= hdisplay) {
			primary_state->crtc_x = 0;
			primary_state->crtc_w = hdisplay;
		} else {
			primary_state->crtc_x = (hdisplay - fb_width) / 2;
			primary_state->crtc_w = set->fb->width;
		}

		if (set->fb->height >= vdisplay) {
			primary_state->crtc_y = 0;
			primary_state->crtc_h = vdisplay;
		} else {
			primary_state->crtc_y = (vdisplay - fb_height) / 2;
			primary_state->crtc_h = fb_height;
		}
	} else {
		primary_state->crtc_x = 0;
		primary_state->crtc_y = 0;
		primary_state->crtc_w = hdisplay;
		primary_state->crtc_h = vdisplay;
	}
	s = to_rockchip_crtc_state(crtc->state);
	s->output_type = connector->connector_type;

	return 0;

error_crtc:
	if (priv->crtc_funcs[pipe] && priv->crtc_funcs[pipe]->loader_protect)
		priv->crtc_funcs[pipe]->loader_protect(crtc, false);
error_conn:
	if (set->sub_dev->loader_protect)
		set->sub_dev->loader_protect(conn_state->best_encoder, false);

	return ret;
}

static int update_state(struct drm_device *drm_dev,
			struct drm_atomic_state *state,
			struct rockchip_drm_mode_set *set,
			unsigned int *plane_mask)
{
	struct drm_crtc *crtc = set->crtc;
	struct drm_connector *connector = set->sub_dev->connector;
	struct drm_display_mode *mode = set->mode;
	struct drm_plane_state *primary_state;
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	int ret;
	struct rockchip_crtc_state *s;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);
	conn_state = drm_atomic_get_connector_state(state, connector);
	if (IS_ERR(conn_state))
		return PTR_ERR(conn_state);
	s = to_rockchip_crtc_state(crtc_state);
	s->left_margin = set->left_margin;
	s->right_margin = set->right_margin;
	s->top_margin = set->top_margin;
	s->bottom_margin = set->bottom_margin;

	if (set->mode_changed) {
		ret = drm_atomic_set_crtc_for_connector(conn_state, crtc);
		if (ret)
			return ret;

		ret = drm_atomic_set_mode_for_crtc(crtc_state, mode);
		if (ret)
			return ret;

		crtc_state->active = true;
	} else {
		const struct drm_encoder_helper_funcs *encoder_helper_funcs;
		const struct drm_connector_helper_funcs *connector_helper_funcs;
		struct drm_encoder *encoder;
		struct drm_bridge *bridge;

		connector_helper_funcs = connector->helper_private;
		if (!connector_helper_funcs)
			return -ENXIO;
		if (connector_helper_funcs->best_encoder)
			encoder = connector_helper_funcs->best_encoder(connector);
		else
			encoder = rockchip_drm_connector_get_single_encoder(connector);
		if (!encoder)
			return -ENXIO;
		encoder_helper_funcs = encoder->helper_private;
		if (!encoder_helper_funcs->atomic_check)
			return -ENXIO;
		ret = encoder_helper_funcs->atomic_check(encoder, crtc->state,
							 conn_state);
		if (ret)
			return ret;

		if (encoder_helper_funcs->atomic_mode_set)
			encoder_helper_funcs->atomic_mode_set(encoder,
							      crtc_state,
							      conn_state);
		else if (encoder_helper_funcs->mode_set)
			encoder_helper_funcs->mode_set(encoder, mode, mode);

		bridge = drm_bridge_chain_get_first_bridge(encoder);
		drm_bridge_chain_mode_set(bridge, mode, mode);
	}

	primary_state = drm_atomic_get_plane_state(state, crtc->primary);
	if (IS_ERR(primary_state))
		return PTR_ERR(primary_state);

	crtc_state->plane_mask = 1 << drm_plane_index(crtc->primary);
	*plane_mask |= crtc_state->plane_mask;


	drm_atomic_set_fb_for_plane(primary_state, set->fb);
	drm_framebuffer_put(set->fb);
	ret = drm_atomic_set_crtc_for_plane(primary_state, crtc);

	return ret;
}

static void rockchip_drm_copy_mode_from_mode_set(struct drm_display_mode *mode,
						 struct rockchip_drm_mode_set *set)
{
	mode->clock = set->clock;
	mode->hdisplay = set->hdisplay;
	mode->vdisplay = set->vdisplay;
	mode->crtc_hsync_end = set->crtc_hsync_end;
	mode->crtc_vsync_end = set->crtc_vsync_end;
	mode->flags = set->flags & DRM_MODE_FLAG_ALL;
	mode->picture_aspect_ratio = set->picture_aspect_ratio;
}

/*
	该函数是 Rockchip DRM 驱动实现「uboot→kernel 无缝开机 logo 显示」的核心接口，核心目标是：
	在 DRM 驱动初始化阶段，解析设备树中预定义的显示配置，直接复用 uboot 
	预加载到预留内存的开机 logo 图片，通过 DRM 原子模式设置框架渲染到屏幕，全程不重置显示硬件、
	不黑屏闪屏，实现开机画面的无缝衔接。
*/
void rockchip_drm_show_logo(struct drm_device *drm_dev)
{
	/*
		DRM 原子状态对象：state 用于存储本次 logo 显示的硬件配置；
		old_state 用于备份初始硬件状态，显示失败时回滚，避免屏幕异常
	*/
	struct drm_atomic_state *state, *old_state;
	struct device_node *np = drm_dev->dev->of_node;
	struct drm_mode_config *mode_config = &drm_dev->mode_config;
	struct rockchip_drm_private *private = drm_dev->dev_private;
	struct device_node *root, *route;
	/*
		Rockchip 自定义显示模式集：set 存储有效显示通路的完整配置；unset 存储无效 / 未连接的显示通路配置
	*/
	struct rockchip_drm_mode_set *set, *tmp, *unset;
	/* 内核链表头：分别存储有效、无效的显示模式集，后续批量处理 */
	struct list_head mode_set_list;
	struct list_head mode_unset_list;
	unsigned int plane_mask = 0;
	struct drm_crtc *crtc;
	int ret, i;

	/* 
		从 DRM 设备的设备树节点中，查找名为route的子节点，
		该节点是 Rockchip 定义的显示路由配置根节点，内部包含每个屏幕的 logo 显示参数、硬件通路配置。 
	*/
	root = of_get_child_by_name(np, "route");
	if (!root) {
		dev_warn(drm_dev->dev, "failed to parse resources for logo display\n");
		return;
	}

	/*
		调用init_loader_memory函数（同文件内实现），这是 logo 显示的核心前置步骤：
		从设备树解析logo-memory-region/drm-logo预留内存区域（uboot 启动时已把 logo 图片加载到这段物理内存中）；
		若启用了 IOMMU，会为这段内存做 1:1 的 IOVA→物理地址映射，让 VOP 硬件能直接访问 logo 数据；
		把内存的物理地址、虚拟地址、大小等信息存入private->logo结构体，供后续创建帧缓冲使用。
	*/
	if (init_loader_memory(drm_dev)) {
		dev_warn(drm_dev->dev, "failed to parse loader memory\n");
		return;
	}

	INIT_LIST_HEAD(&mode_set_list);
	INIT_LIST_HEAD(&mode_unset_list);
	drm_modeset_lock_all(drm_dev);
	/* 申请 DRM 原子状态对象，这是 DRM 原子模式设置的核心 
	—— 所有硬件配置修改都先写入这个状态对象，验证无误后一次性提交给硬件，避免中途修改导致的屏幕异常 */
	state = drm_atomic_state_alloc(drm_dev);
	if (!state) {
		dev_err(drm_dev->dev, "failed to alloc atomic state for logo display\n");
		ret = -ENOMEM;
		goto err_unlock;
	}

	state->acquire_ctx = mode_config->acquire_ctx;

	// 遍历route根节点下的所有子节点，每个子节点对应一个屏幕的显示通路（如 HDMI0、MIPI-DSI0）。
	for_each_child_of_node(root, route) {
		if (!of_device_is_available(route))
			continue;

		/*
			1.解析该显示通路的硬件连接：找到对应的 CRTC（VOP）、Connector（HDMI/MIPI）；
			2.解析 logo 参数：宽高、色深、内存偏移、显示模式（全屏 / 居中）；
			3.基于 uboot 预留的 logo 内存，创建 DRM 帧缓冲对象set->fb，供后续显示使用。
		*/
		set = of_parse_display_resource(drm_dev, route);
		if (!set)
			continue;

		/*
			1.检测 Connector 的连接状态、获取支持的显示模式；
			2.匹配设备树中配置的显示时序（分辨率、刷新率）；
			3.配置 CRTC、Encoder、Connector 的绑定关系；
			4.设置亮度、对比度、饱和度、过扫描等显示参数
			释放帧缓冲，把该配置加入无效链表mode_unset_list；初始化成功：加入有效链表mode_set_list。
		*/
		if (setup_initial_state(drm_dev, state, set)) {
			drm_framebuffer_put(set->fb);
			INIT_LIST_HEAD(&set->head);
			list_add_tail(&set->head, &mode_unset_list);
			continue;
		}

		INIT_LIST_HEAD(&set->head);
		list_add_tail(&set->head, &mode_set_list);
	}

	/*
	 * the mode_unset_list store the unconnected route, if route's crtc
	 * isn't used, we should close it.
	 */
	/*
		这段代码的作用是清理无效显示通路，关闭未使用的 CRTC（VOP）硬件，
		避免闲置硬件占用时钟、功耗，同时保护 uboot 的硬件状态。
	*/
	list_for_each_entry_safe(unset, tmp, &mode_unset_list, head) {
		struct rockchip_drm_mode_set *tmp_set;
		int find_used_crtc = 0;

		// 若 CRTC 被有效通路占用（find_used_crtc=1）：不做处理，避免影响正常显示；
		list_for_each_entry_safe(set, tmp_set, &mode_set_list, head) {
			if (set->crtc == unset->crtc) {
				find_used_crtc = 1;
				continue;
			}
		}

		if (!find_used_crtc) {
			struct drm_crtc *crtc = unset->crtc;
			struct drm_crtc_state *crtc_state;
			int pipe = drm_crtc_index(crtc);
			struct rockchip_drm_private *priv =
							drm_dev->dev_private;

			/*
			 * The display timing information of mode_set is parsed from dts, which
			 * written in uboot. If the mode_set is added into mode_unset_list, it
			 * should be converted to crtc_state->adjusted_mode, in order to check
			 * splice_mode flag in loader_protect().
			 */
			if (unset->hdisplay && unset->vdisplay) {
				// 获取 CRTC 的原子状态，把设备树中的显示时序写入adjusted_mode
				crtc_state = drm_atomic_get_crtc_state(state, crtc);
				// 锁定 CRTC 的硬件状态，关闭过程中不重置 uboot 设置的时钟、电源域，避免闪屏；
				if (crtc_state)
					rockchip_drm_copy_mode_from_mode_set(&crtc_state->adjusted_mode,
									     unset);
				
				if (priv->crtc_funcs[pipe] &&
				    priv->crtc_funcs[pipe]->loader_protect)
					priv->crtc_funcs[pipe]->loader_protect(crtc, true);
				// 调用 Rockchip CRTC 的关闭接口，关闭闲置的 VOP 硬件
				priv->crtc_funcs[pipe]->crtc_close(crtc);
				// 解除锁定。
				if (priv->crtc_funcs[pipe] &&
				    priv->crtc_funcs[pipe]->loader_protect)
					priv->crtc_funcs[pipe]->loader_protect(crtc, false);
			}
		}

		// 最后从链表中删除该无效配置，释放内存。
		list_del(&unset->head);
		kfree(unset);
	}

	if (list_empty(&mode_set_list)) {
		dev_warn(drm_dev->dev, "can't not find any logo display\n");
		ret = -ENXIO;
		goto err_free_state;
	}

	/*
	 * The state save initial devices status, swap the state into
	 * drm devices as old state, so if new state come, can compare
	 * with this state to judge which status need to update.
	 */
	/* 
		把之前初始化的原子状态，交换为 DRM 设备的当前硬件状态，让内核识别当前硬件的配置（继承 uboot 设置的状态）。
	*/
	WARN_ON(drm_atomic_helper_swap_state(state, false));
	// 释放交换后的旧状态对象，减少内存占用。
	drm_atomic_state_put(state);
	// 复制一份当前硬件的原子状态，作为备份 —— 如果后续 logo 显示失败，直接恢复这个状态，避免屏幕异常。
	old_state = drm_atomic_helper_duplicate_state(drm_dev,
						      mode_config->acquire_ctx);
	if (IS_ERR(old_state)) {
		dev_err(drm_dev->dev, "failed to duplicate atomic state for logo display\n");
		ret = PTR_ERR_OR_ZERO(old_state);
		goto err_free_state;
	}

	// 再复制一份新的原子状态，用于后续 logo 显示的配置修改，避免直接修改当前硬件状态。
	state = drm_atomic_helper_duplicate_state(drm_dev,
						  mode_config->acquire_ctx);
	if (IS_ERR(state)) {
		dev_err(drm_dev->dev, "failed to duplicate atomic state for logo display\n");
		ret = PTR_ERR_OR_ZERO(state);
		goto err_free_old_state;
	}
	state->acquire_ctx = mode_config->acquire_ctx;

	/*
		1. 遍历所有有效显示通路，调用update_state更新原子状态，完成 logo 显示的核心配置：
		把 logo 帧缓冲set->fb绑定到 CRTC 的主图层（Primary Plane）；
		配置图层的显示位置、宽高（全屏 / 居中）；
		设置过扫描、色彩参数，更新 CRTC/Connector 的绑定关系；
		把用到的图层标记到plane_mask，供后续资源清理。
		2. 遍历所有 Connector，把未连接的屏幕的编码器置空，避免无效的硬件配置导致原子提交失败
	*/
	list_for_each_entry(set, &mode_set_list, head)
		/*
		 * We don't want to see any fail on update_state.
		 */
		WARN_ON(update_state(drm_dev, state, set, &plane_mask));

	for (i = 0; i < state->num_connector; i++) {
		if (state->connectors[i].new_state->connector->status !=
		    connector_status_connected)
			state->connectors[i].new_state->best_encoder = NULL;
	}

	/*
		先验证原子状态中的所有硬件配置是否合法、无冲突；
		验证通过后，一次性把所有配置提交给硬件，点亮屏幕显示 logo；
		提交是原子性的：要么全部配置生效，要么全部不生效，不会出现中途异常导致的花屏、闪屏
	*/
	ret = drm_atomic_commit(state);
	/**
	 * todo
	 * drm_atomic_clean_old_fb(drm_dev, plane_mask, ret);
	 */

	 /*
	 	若配置了force-output强制输出，恢复 Connector 的强制输出状态为默认值，避免影响后续热插拔检测；
		从链表中删除节点，释放rockchip_drm_mode_set结构体的内存
	 */
	list_for_each_entry_safe(set, tmp, &mode_set_list, head) {
		if (set->force_output)
			set->sub_dev->connector->force = DRM_FORCE_UNSPECIFIED;
		list_del(&set->head);
		kfree(set);
	}

	/*
	 * Is possible get deadlock here?
	 */
	WARN_ON(ret == -EDEADLK);

	if (ret) {
		/*
		 * restore display status if atomic commit failed.
		 */
		WARN_ON(drm_atomic_helper_swap_state(old_state, false));
		goto err_free_state;
	}

	/* 
		rockchip_free_loader_memory(drm_dev)：释放 uboot 预留的 logo 内存，包括解除 IOMMU 映射、释放物理内存、释放private->logo结构体，避免内存泄漏。
		drm_atomic_state_put(old_state/state)：释放备份的和当前的原子状态对象，完成内存清理。
		private->loader_protect = true：设置加载器保护标志为 true，告知驱动其他模块：当前显示状态继承自 uboot，不要随意重置硬件、时钟，保证后续 FBDEV / 应用显示的无缝衔接。
		drm_modeset_unlock_all(drm_dev)：释放 DRM 模式设置的全局锁，开放硬件资源给后续模块使用。
	*/
	rockchip_free_loader_memory(drm_dev);
	drm_atomic_state_put(old_state);
	drm_atomic_state_put(state);

	private->loader_protect = true;
	drm_modeset_unlock_all(drm_dev);

	/*
		检查 FBDEV 辅助器和帧缓冲是否已初始化；
		遍历所有 CRTC，检查对应输出接口是否支持热插拔（HDMI/DP 等）；
		对支持热插拔的接口，调用drm_framebuffer_get增加 FBDEV 帧缓冲的引用计数，保证 FBDEV 初始化时，
		直接复用当前的显示状态，不会重置屏幕，实现 logo 到控制台的无缝切换。
	*/
	if (private->fbdev_helper && private->fbdev_helper->fb) {
		drm_for_each_crtc(crtc, drm_dev) {
			struct rockchip_crtc_state *s = NULL;

			s = to_rockchip_crtc_state(crtc->state);
			if (is_support_hotplug(s->output_type))
				drm_framebuffer_get(private->fbdev_helper->fb);
		}
	}

	return;
err_free_old_state:
	drm_atomic_state_put(old_state);
err_free_state:
	drm_atomic_state_put(state);
err_unlock:
	drm_modeset_unlock_all(drm_dev);
	if (ret)
		dev_err(drm_dev->dev, "failed to show kernel logo\n");
}

#ifndef MODULE
static const char *const loader_protect_clocks[] __initconst = {
	"hclk_vio",
	"hclk_vop",
	"hclk_vopb",
	"hclk_vopl",
	"aclk_vio",
	"aclk_vio0",
	"aclk_vio1",
	"aclk_vop",
	"aclk_vopb",
	"aclk_vopl",
	"aclk_vo_pre",
	"aclk_vio_pre",
	"dclk_vop",
	"dclk_vop0",
	"dclk_vop1",
	"dclk_vopb",
	"dclk_vopl",
};

static struct clk **loader_clocks __initdata;
static int __init rockchip_clocks_loader_protect(void)
{
	int nclocks = ARRAY_SIZE(loader_protect_clocks);
	struct clk *clk;
	int i;

	loader_clocks = kcalloc(nclocks, sizeof(void *), GFP_KERNEL);
	if (!loader_clocks)
		return -ENOMEM;

	for (i = 0; i < nclocks; i++) {
		clk = __clk_lookup(loader_protect_clocks[i]);

		if (clk) {
			loader_clocks[i] = clk;
			clk_prepare_enable(clk);
		}
	}

	return 0;
}
arch_initcall_sync(rockchip_clocks_loader_protect);

static int __init rockchip_clocks_loader_unprotect(void)
{
	int i;

	if (!loader_clocks)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(loader_protect_clocks); i++) {
		struct clk *clk = loader_clocks[i];

		if (clk)
			clk_disable_unprepare(clk);
	}
	kfree(loader_clocks);

	return 0;
}
late_initcall_sync(rockchip_clocks_loader_unprotect);
#endif
