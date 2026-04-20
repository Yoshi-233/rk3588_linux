// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author:Mark Yao <mark.yao@rock-chips.com>
 *
 * based on exynos_drm_drv.c
 */

#include <linux/dma-buf-cache.h>
#include <linux/dma-mapping.h>
#include <linux/dma-iommu.h>
#include <linux/genalloc.h>
#include <linux/pm_runtime.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/console.h>
#include <linux/iommu.h>
#include <linux/of_reserved_mem.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_displayid.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_fb.h"
#include "rockchip_drm_fbdev.h"
#include "rockchip_drm_gem.h"
#include "rockchip_drm_logo.h"

#include "../drm_crtc_internal.h"
#include "../drivers/clk/rockchip/clk.h"

#define DRIVER_NAME	"rockchip"
#define DRIVER_DESC	"RockChip Soc DRM"
#define DRIVER_DATE	"20140818"
#define DRIVER_MAJOR	3
#define DRIVER_MINOR	0

#if IS_ENABLED(CONFIG_DRM_ROCKCHIP_VVOP)
static bool is_support_iommu = false;
#else
static bool is_support_iommu = true;
#endif
static bool iommu_reserve_map;

static struct drm_driver rockchip_drm_driver;

static unsigned int drm_debug;
module_param_named(debug, drm_debug, int, 0600);

static inline bool rockchip_drm_debug_enabled(enum rockchip_drm_debug_category category)
{
	return unlikely(drm_debug & category);
}

__printf(3, 4)
void rockchip_drm_dbg(const struct device *dev, enum rockchip_drm_debug_category category,
		      const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (!rockchip_drm_debug_enabled(category))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	if (dev)
		dev_printk(KERN_DEBUG, dev, "%pV", &vaf);
	else
		printk(KERN_DEBUG "%pV", &vaf);

	va_end(args);
}

/**
 * rockchip_drm_wait_vact_end
 * @crtc: CRTC to enable line flag
 * @mstimeout: millisecond for timeout
 *
 * Wait for vact_end line flag irq or timeout.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int rockchip_drm_wait_vact_end(struct drm_crtc *crtc, unsigned int mstimeout)
{
	struct rockchip_drm_private *priv;
	int pipe, ret = 0;

	if (!crtc)
		return -ENODEV;

	if (mstimeout <= 0)
		return -EINVAL;

	priv = crtc->dev->dev_private;
	pipe = drm_crtc_index(crtc);

	if (priv->crtc_funcs[pipe] && priv->crtc_funcs[pipe]->wait_vact_end)
		ret = priv->crtc_funcs[pipe]->wait_vact_end(crtc, mstimeout);

	return ret;
}
EXPORT_SYMBOL(rockchip_drm_wait_vact_end);

void drm_mode_convert_to_split_mode(struct drm_display_mode *mode)
{
	u16 hactive, hfp, hsync, hbp;

	hactive = mode->hdisplay;
	hfp = mode->hsync_start - mode->hdisplay;
	hsync = mode->hsync_end - mode->hsync_start;
	hbp = mode->htotal - mode->hsync_end;

	mode->clock *= 2;
	mode->hdisplay = hactive * 2;
	mode->hsync_start = mode->hdisplay + hfp * 2;
	mode->hsync_end = mode->hsync_start + hsync * 2;
	mode->htotal = mode->hsync_end + hbp * 2;
	drm_mode_set_name(mode);
}
EXPORT_SYMBOL(drm_mode_convert_to_split_mode);

void drm_mode_convert_to_origin_mode(struct drm_display_mode *mode)
{
	u16 hactive, hfp, hsync, hbp;

	hactive = mode->hdisplay;
	hfp = mode->hsync_start - mode->hdisplay;
	hsync = mode->hsync_end - mode->hsync_start;
	hbp = mode->htotal - mode->hsync_end;

	mode->clock /= 2;
	mode->hdisplay = hactive / 2;
	mode->hsync_start = mode->hdisplay + hfp / 2;
	mode->hsync_end = mode->hsync_start + hsync / 2;
	mode->htotal = mode->hsync_end + hbp / 2;
}
EXPORT_SYMBOL(drm_mode_convert_to_origin_mode);

/**
 * drm_connector_oob_hotplug_event - Report out-of-band hotplug event to connector
 * @connector: connector to report the event on
 *
 * On some hardware a hotplug event notification may come from outside the display
 * driver / device. An example of this is some USB Type-C setups where the hardware
 * muxes the DisplayPort data and aux-lines but does not pass the altmode HPD
 * status bit to the GPU's DP HPD pin.
 *
 * This function can be used to report these out-of-band events after obtaining
 * a drm_connector reference through calling drm_connector_find_by_fwnode().
 */
void drm_connector_oob_hotplug_event(struct fwnode_handle *connector_fwnode)
{
	struct rockchip_drm_sub_dev *sub_dev;

	if (!connector_fwnode || !connector_fwnode->dev)
		return;

	sub_dev = rockchip_drm_get_sub_dev(dev_of_node(connector_fwnode->dev));

	if (sub_dev && sub_dev->connector && sub_dev->oob_hotplug_event)
		sub_dev->oob_hotplug_event(sub_dev->connector);
}
EXPORT_SYMBOL(drm_connector_oob_hotplug_event);

uint32_t rockchip_drm_get_bpp(const struct drm_format_info *info)
{
	/* use whatever a driver has set */
	if (info->cpp[0])
		return info->cpp[0] * 8;

	switch (info->format) {
	case DRM_FORMAT_YUV420_8BIT:
		return 12;
	case DRM_FORMAT_YUV420_10BIT:
		return 15;
	case DRM_FORMAT_VUY101010:
		return 30;
	default:
		break;
	}

	/* all attempts failed */
	return 0;
}
EXPORT_SYMBOL(rockchip_drm_get_bpp);

/**
 * rockchip_drm_of_find_possible_crtcs - find the possible CRTCs for an active
 * encoder port
 * @dev: DRM device
 * @port: encoder port to scan for endpoints
 *
 * Scan all active endpoints attached to a port, locate their attached CRTCs,
 * and generate the DRM mask of CRTCs which may be attached to this
 * encoder.
 *
 * See Documentation/devicetree/bindings/graph.txt for the bindings.
 */
uint32_t rockchip_drm_of_find_possible_crtcs(struct drm_device *dev,
					     struct device_node *port)
{
	struct device_node *remote_port, *ep;
	uint32_t possible_crtcs = 0;

	for_each_endpoint_of_node(port, ep) {
		if (!of_device_is_available(ep))
			continue;

		remote_port = of_graph_get_remote_port(ep);
		if (!remote_port) {
			of_node_put(ep);
			continue;
		}

		possible_crtcs |= drm_of_crtc_port_mask(dev, remote_port);

		of_node_put(remote_port);
	}

	return possible_crtcs;
}
EXPORT_SYMBOL(rockchip_drm_of_find_possible_crtcs);

static DEFINE_MUTEX(rockchip_drm_sub_dev_lock);
static LIST_HEAD(rockchip_drm_sub_dev_list);

void rockchip_connector_update_vfp_for_vrr(struct drm_crtc *crtc, struct drm_display_mode *mode,
					   int vfp)
{
	struct rockchip_drm_sub_dev *sub_dev;

	mutex_lock(&rockchip_drm_sub_dev_lock);
	list_for_each_entry(sub_dev, &rockchip_drm_sub_dev_list, list) {
		if (sub_dev->connector->state->crtc == crtc) {
			if (sub_dev->update_vfp_for_vrr)
				sub_dev->update_vfp_for_vrr(sub_dev->connector, mode, vfp);
		}
	}
	mutex_unlock(&rockchip_drm_sub_dev_lock);
}
EXPORT_SYMBOL(rockchip_connector_update_vfp_for_vrr);

void rockchip_drm_register_sub_dev(struct rockchip_drm_sub_dev *sub_dev)
{
	mutex_lock(&rockchip_drm_sub_dev_lock);
	list_add_tail(&sub_dev->list, &rockchip_drm_sub_dev_list);
	mutex_unlock(&rockchip_drm_sub_dev_lock);
}
EXPORT_SYMBOL(rockchip_drm_register_sub_dev);

void rockchip_drm_unregister_sub_dev(struct rockchip_drm_sub_dev *sub_dev)
{
	mutex_lock(&rockchip_drm_sub_dev_lock);
	list_del(&sub_dev->list);
	mutex_unlock(&rockchip_drm_sub_dev_lock);
}
EXPORT_SYMBOL(rockchip_drm_unregister_sub_dev);

struct rockchip_drm_sub_dev *rockchip_drm_get_sub_dev(struct device_node *node)
{
	struct rockchip_drm_sub_dev *sub_dev = NULL;
	bool found = false;

	mutex_lock(&rockchip_drm_sub_dev_lock);
	list_for_each_entry(sub_dev, &rockchip_drm_sub_dev_list, list) {
		if (sub_dev->of_node == node) {
			found = true;
			break;
		}
	}
	mutex_unlock(&rockchip_drm_sub_dev_lock);

	return found ? sub_dev : NULL;
}
EXPORT_SYMBOL(rockchip_drm_get_sub_dev);

int rockchip_drm_get_sub_dev_type(void)
{
	int connector_type = DRM_MODE_CONNECTOR_Unknown;
	struct rockchip_drm_sub_dev *sub_dev = NULL;

	mutex_lock(&rockchip_drm_sub_dev_lock);
	list_for_each_entry(sub_dev, &rockchip_drm_sub_dev_list, list) {
		if (sub_dev->connector->encoder) {
			connector_type = sub_dev->connector->connector_type;
			break;
		}
	}
	mutex_unlock(&rockchip_drm_sub_dev_lock);

	return connector_type;
}
EXPORT_SYMBOL(rockchip_drm_get_sub_dev_type);

u32 rockchip_drm_get_scan_line_time_ns(void)
{
	struct rockchip_drm_sub_dev *sub_dev = NULL;
	struct drm_display_mode *mode;
	int linedur_ns = 0;

	mutex_lock(&rockchip_drm_sub_dev_lock);
	list_for_each_entry(sub_dev, &rockchip_drm_sub_dev_list, list) {
		if (sub_dev->connector->encoder && sub_dev->connector->state->crtc) {
			mode = &sub_dev->connector->state->crtc->state->adjusted_mode;
			linedur_ns  = div_u64((u64) mode->crtc_htotal * 1000000, mode->crtc_clock);
			break;
		}
	}
	mutex_unlock(&rockchip_drm_sub_dev_lock);

	return linedur_ns;
}
EXPORT_SYMBOL(rockchip_drm_get_scan_line_time_ns);

void rockchip_drm_te_handle(struct drm_crtc *crtc)
{
	struct rockchip_drm_private *priv = crtc->dev->dev_private;
	int pipe = drm_crtc_index(crtc);

	if (priv->crtc_funcs[pipe] && priv->crtc_funcs[pipe]->te_handler)
		priv->crtc_funcs[pipe]->te_handler(crtc);
}
EXPORT_SYMBOL(rockchip_drm_te_handle);

static const struct drm_display_mode rockchip_drm_default_modes[] = {
	/* 4 - 1280x720@60Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390,
		   1430, 1650, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9, },
	/* 16 - 1920x1080@60Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9, },
	/* 31 - 1920x1080@50Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2448,
		   2492, 2640, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9, },
	/* 19 - 1280x720@50Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1720,
		   1760, 1980, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9, },
	/* 0x10 - 1024x768@60Hz */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 65000, 1024, 1048,
		   1184, 1344, 0,  768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 17 - 720x576@50Hz 4:3 */
	{ DRM_MODE("720x576", DRM_MODE_TYPE_DRIVER, 27000, 720, 732,
		   796, 864, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_4_3, },
	/* 2 - 720x480@60Hz 4:3 */
	{ DRM_MODE("720x480", DRM_MODE_TYPE_DRIVER, 27000, 720, 736,
		   798, 858, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_4_3, },
};

int rockchip_drm_add_modes_noedid(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;
	int i, count, num_modes = 0;

	mutex_lock(&rockchip_drm_sub_dev_lock);
	count = ARRAY_SIZE(rockchip_drm_default_modes);

	for (i = 0; i < count; i++) {
		const struct drm_display_mode *ptr = &rockchip_drm_default_modes[i];

		mode = drm_mode_duplicate(dev, ptr);
		if (mode) {
			if (!i)
				mode->type = DRM_MODE_TYPE_PREFERRED;
			drm_mode_probed_add(connector, mode);
			num_modes++;
		}
	}
	mutex_unlock(&rockchip_drm_sub_dev_lock);

	return num_modes;
}
EXPORT_SYMBOL(rockchip_drm_add_modes_noedid);

static const struct rockchip_drm_width_dclk {
	int width;
	u32 dclk_khz;
} rockchip_drm_dclk[] = {
	{1920, 148500},
	{2048, 200000},
	{2560, 280000},
	{3840, 594000},
	{4096, 594000},
	{7680, 2376000},
};

u32 rockchip_drm_get_dclk_by_width(int width)
{
	int i = 0;
	u32 dclk_khz;

	for (i = 0; i < ARRAY_SIZE(rockchip_drm_dclk); i++) {
		if (width == rockchip_drm_dclk[i].width) {
			dclk_khz = rockchip_drm_dclk[i].dclk_khz;
			break;
		}
	}

	if (i == ARRAY_SIZE(rockchip_drm_dclk)) {
		DRM_ERROR("Can't not find %d width solution and use 148500 khz as max dclk\n", width);

		dclk_khz = 148500;
	}

	return dclk_khz;
}
EXPORT_SYMBOL(rockchip_drm_get_dclk_by_width);

static int
cea_db_tag(const u8 *db)
{
	return db[0] >> 5;
}

static int
cea_db_payload_len(const u8 *db)
{
	return db[0] & 0x1f;
}

#define for_each_cea_db(cea, i, start, end) \
	for ((i) = (start); \
	     (i) < (end) && (i) + cea_db_payload_len(&(cea)[(i)]) < (end); \
	     (i) += cea_db_payload_len(&(cea)[(i)]) + 1)

#define HDMI_NEXT_HDR_VSDB_OUI 0xd04601

static bool cea_db_is_hdmi_next_hdr_block(const u8 *db)
{
	unsigned int oui;

	if (cea_db_tag(db) != 0x07)
		return false;

	if (cea_db_payload_len(db) < 11)
		return false;

	oui = db[3] << 16 | db[2] << 8 | db[1];

	return oui == HDMI_NEXT_HDR_VSDB_OUI;
}

static bool cea_db_is_hdmi_forum_vsdb(const u8 *db)
{
	unsigned int oui;

	if (cea_db_tag(db) != 0x03)
		return false;

	if (cea_db_payload_len(db) < 7)
		return false;

	oui = db[3] << 16 | db[2] << 8 | db[1];

	return oui == HDMI_FORUM_IEEE_OUI;
}

static int
cea_db_offsets(const u8 *cea, int *start, int *end)
{
	/* DisplayID CTA extension blocks and top-level CEA EDID
	 * block header definitions differ in the following bytes:
	 *   1) Byte 2 of the header specifies length differently,
	 *   2) Byte 3 is only present in the CEA top level block.
	 *
	 * The different definitions for byte 2 follow.
	 *
	 * DisplayID CTA extension block defines byte 2 as:
	 *   Number of payload bytes
	 *
	 * CEA EDID block defines byte 2 as:
	 *   Byte number (decimal) within this block where the 18-byte
	 *   DTDs begin. If no non-DTD data is present in this extension
	 *   block, the value should be set to 04h (the byte after next).
	 *   If set to 00h, there are no DTDs present in this block and
	 *   no non-DTD data.
	 */
	if (cea[0] == 0x81) {
		/*
		 * for_each_displayid_db() has already verified
		 * that these stay within expected bounds.
		 */
		*start = 3;
		*end = *start + cea[2];
	} else if (cea[0] == 0x02) {
		/* Data block offset in CEA extension block */
		*start = 4;
		*end = cea[2];
		if (*end == 0)
			*end = 127;
		if (*end < 4 || *end > 127)
			return -ERANGE;
	} else {
		return -EOPNOTSUPP;
	}

	return 0;
}

static u8 *find_edid_extension(const struct edid *edid,
			       int ext_id, int *ext_index)
{
	u8 *edid_ext = NULL;
	int i;

	/* No EDID or EDID extensions */
	if (edid == NULL || edid->extensions == 0)
		return NULL;

	/* Find CEA extension */
	for (i = *ext_index; i < edid->extensions; i++) {
		edid_ext = (u8 *)edid + EDID_LENGTH * (i + 1);
		if (edid_ext[0] == ext_id)
			break;
	}

	if (i >= edid->extensions)
		return NULL;

	*ext_index = i + 1;

	return edid_ext;
}

static int validate_displayid(u8 *displayid, int length, int idx)
{
	int i, dispid_length;
	u8 csum = 0;
	struct displayid_hdr *base;

	base = (struct displayid_hdr *)&displayid[idx];

	DRM_DEBUG_KMS("base revision 0x%x, length %d, %d %d\n",
		      base->rev, base->bytes, base->prod_id, base->ext_count);

	/* +1 for DispID checksum */
	dispid_length = sizeof(*base) + base->bytes + 1;
	if (dispid_length > length - idx)
		return -EINVAL;

	for (i = 0; i < dispid_length; i++)
		csum += displayid[idx + i];
	if (csum) {
		DRM_NOTE("DisplayID checksum invalid, remainder is %d\n", csum);
		return -EINVAL;
	}

	return 0;
}

static u8 *find_displayid_extension(const struct edid *edid,
				    int *length, int *idx,
				    int *ext_index)
{
	u8 *displayid = find_edid_extension(edid, 0x70, ext_index);
	struct displayid_hdr *base;
	int ret;

	if (!displayid)
		return NULL;

	/* EDID extensions block checksum isn't for us */
	*length = EDID_LENGTH - 1;
	*idx = 1;

	ret = validate_displayid(displayid, *length, *idx);
	if (ret)
		return NULL;

	base = (struct displayid_hdr *)&displayid[*idx];
	*length = *idx + sizeof(*base) + base->bytes;

	return displayid;
}

static u8 *find_cea_extension(const struct edid *edid)
{
	int length, idx;
	struct displayid_block *block;
	u8 *cea;
	u8 *displayid;
	int ext_index;

	/* Look for a top level CEA extension block */
	/* FIXME: make callers iterate through multiple CEA ext blocks? */
	ext_index = 0;
	cea = find_edid_extension(edid, 0x02, &ext_index);
	if (cea)
		return cea;

	/* CEA blocks can also be found embedded in a DisplayID block */
	ext_index = 0;
	for (;;) {
		displayid = find_displayid_extension(edid, &length, &idx,
						     &ext_index);
		if (!displayid)
			return NULL;

		idx += sizeof(struct displayid_hdr);
		for_each_displayid_db(displayid, block, idx, length) {
			if (block->tag == 0x81)
				return (u8 *)block;
		}
	}

	return NULL;
}

#define EDID_CEA_YCRCB422	(1 << 4)

int rockchip_drm_get_yuv422_format(struct drm_connector *connector,
				   struct edid *edid)
{
	struct drm_display_info *info;
	const u8 *edid_ext;

	if (!connector || !edid)
		return -EINVAL;

	info = &connector->display_info;

	edid_ext = find_cea_extension(edid);
	if (!edid_ext)
		return -EINVAL;

	if (edid_ext[3] & EDID_CEA_YCRCB422)
		info->color_formats |= DRM_COLOR_FORMAT_YCRCB422;

	return 0;
}
EXPORT_SYMBOL(rockchip_drm_get_yuv422_format);

static
void get_max_frl_rate(int max_frl_rate, u8 *max_lanes, u8 *max_rate_per_lane)
{
	switch (max_frl_rate) {
	case 1:
		*max_lanes = 3;
		*max_rate_per_lane = 3;
		break;
	case 2:
		*max_lanes = 3;
		*max_rate_per_lane = 6;
		break;
	case 3:
		*max_lanes = 4;
		*max_rate_per_lane = 6;
		break;
	case 4:
		*max_lanes = 4;
		*max_rate_per_lane = 8;
		break;
	case 5:
		*max_lanes = 4;
		*max_rate_per_lane = 10;
		break;
	case 6:
		*max_lanes = 4;
		*max_rate_per_lane = 12;
		break;
	case 0:
	default:
		*max_lanes = 0;
		*max_rate_per_lane = 0;
	}
}

#define EDID_DSC_10BPC			(1 << 0)
#define EDID_DSC_12BPC			(1 << 1)
#define EDID_DSC_16BPC			(1 << 2)
#define EDID_DSC_ALL_BPP		(1 << 3)
#define EDID_DSC_NATIVE_420		(1 << 6)
#define EDID_DSC_1P2			(1 << 7)
#define EDID_DSC_MAX_FRL_RATE_MASK	0xf0
#define EDID_DSC_MAX_SLICES		0xf
#define EDID_DSC_TOTAL_CHUNK_KBYTES	0x3f
#define EDID_MAX_FRL_RATE_MASK		0xf0

static
void parse_edid_forum_vsdb(struct rockchip_drm_dsc_cap *dsc_cap,
			   u8 *max_frl_rate_per_lane, u8 *max_lanes, u8 *add_func,
			   const u8 *hf_vsdb)
{
	u8 max_frl_rate;
	u8 dsc_max_frl_rate;
	u8 dsc_max_slices;

	if (!hf_vsdb[7])
		return;

	DRM_DEBUG_KMS("hdmi_21 sink detected. parsing edid\n");
	max_frl_rate = (hf_vsdb[7] & EDID_MAX_FRL_RATE_MASK) >> 4;
	get_max_frl_rate(max_frl_rate, max_lanes,
			 max_frl_rate_per_lane);

	*add_func = hf_vsdb[8];

	if (cea_db_payload_len(hf_vsdb) < 13)
		return;

	dsc_cap->v_1p2 = hf_vsdb[11] & EDID_DSC_1P2;

	if (!dsc_cap->v_1p2)
		return;

	dsc_cap->native_420 = hf_vsdb[11] & EDID_DSC_NATIVE_420;
	dsc_cap->all_bpp = hf_vsdb[11] & EDID_DSC_ALL_BPP;

	if (hf_vsdb[11] & EDID_DSC_16BPC)
		dsc_cap->bpc_supported = 16;
	else if (hf_vsdb[11] & EDID_DSC_12BPC)
		dsc_cap->bpc_supported = 12;
	else if (hf_vsdb[11] & EDID_DSC_10BPC)
		dsc_cap->bpc_supported = 10;
	else
		dsc_cap->bpc_supported = 0;

	dsc_max_frl_rate = (hf_vsdb[12] & EDID_DSC_MAX_FRL_RATE_MASK) >> 4;
	get_max_frl_rate(dsc_max_frl_rate, &dsc_cap->max_lanes,
			 &dsc_cap->max_frl_rate_per_lane);
	dsc_cap->total_chunk_kbytes = hf_vsdb[13] & EDID_DSC_TOTAL_CHUNK_KBYTES;

	dsc_max_slices = hf_vsdb[12] & EDID_DSC_MAX_SLICES;
	switch (dsc_max_slices) {
	case 1:
		dsc_cap->max_slices = 1;
		dsc_cap->clk_per_slice = 340;
		break;
	case 2:
		dsc_cap->max_slices = 2;
		dsc_cap->clk_per_slice = 340;
		break;
	case 3:
		dsc_cap->max_slices = 4;
		dsc_cap->clk_per_slice = 340;
		break;
	case 4:
		dsc_cap->max_slices = 8;
		dsc_cap->clk_per_slice = 340;
		break;
	case 5:
		dsc_cap->max_slices = 8;
		dsc_cap->clk_per_slice = 400;
		break;
	case 6:
		dsc_cap->max_slices = 12;
		dsc_cap->clk_per_slice = 400;
		break;
	case 7:
		dsc_cap->max_slices = 16;
		dsc_cap->clk_per_slice = 400;
		break;
	case 0:
	default:
		dsc_cap->max_slices = 0;
		dsc_cap->clk_per_slice = 0;
	}
}

enum {
	VER_26_BYTE_V0,
	VER_15_BYTE_V1,
	VER_12_BYTE_V1,
	VER_12_BYTE_V2,
};

static int check_next_hdr_version(const u8 *next_hdr_db)
{
	u16 ver;

	ver = (next_hdr_db[5] & 0xf0) << 8 | next_hdr_db[0];

	switch (ver) {
	case 0x00f9:
		return VER_26_BYTE_V0;
	case 0x20ee:
		return VER_15_BYTE_V1;
	case 0x20eb:
		return VER_12_BYTE_V1;
	case 0x40eb:
		return VER_12_BYTE_V2;
	default:
		return -ENOENT;
	}
}

static void parse_ver_26_v0_data(struct ver_26_v0 *hdr, const u8 *data)
{
	hdr->yuv422_12bit = data[5] & BIT(0);
	hdr->support_2160p_60 = (data[5] & BIT(1)) >> 1;
	hdr->global_dimming = (data[5] & BIT(2)) >> 2;

	hdr->dm_major_ver = (data[21] & 0xf0) >> 4;
	hdr->dm_minor_ver = data[21] & 0xf;

	hdr->t_min_pq = (data[19] << 4) | ((data[18] & 0xf0) >> 4);
	hdr->t_max_pq = (data[20] << 4) | (data[18] & 0xf);

	hdr->rx = (data[7] << 4) | ((data[6] & 0xf0) >> 4);
	hdr->ry = (data[8] << 4) | (data[6] & 0xf);
	hdr->gx = (data[10] << 4) | ((data[9] & 0xf0) >> 4);
	hdr->gy = (data[11] << 4) | (data[9] & 0xf);
	hdr->bx = (data[13] << 4) | ((data[12] & 0xf0) >> 4);
	hdr->by = (data[14] << 4) | (data[12] & 0xf);
	hdr->wx = (data[16] << 4) | ((data[15] & 0xf0) >> 4);
	hdr->wy = (data[17] << 4) | (data[15] & 0xf);
}

static void parse_ver_15_v1_data(struct ver_15_v1 *hdr, const u8 *data)
{
	hdr->yuv422_12bit = data[5] & BIT(0);
	hdr->support_2160p_60 = (data[5] & BIT(1)) >> 1;
	hdr->global_dimming = data[6] & BIT(0);

	hdr->dm_version = (data[5] & 0x1c) >> 2;

	hdr->colorimetry = data[7] & BIT(0);

	hdr->t_max_lum = (data[6] & 0xfe) >> 1;
	hdr->t_min_lum = (data[7] & 0xfe) >> 1;

	hdr->rx = data[9];
	hdr->ry = data[10];
	hdr->gx = data[11];
	hdr->gy = data[12];
	hdr->bx = data[13];
	hdr->by = data[14];
}

static void parse_ver_12_v1_data(struct ver_12_v1 *hdr, const u8 *data)
{
	hdr->yuv422_12bit = data[5] & BIT(0);
	hdr->support_2160p_60 = (data[5] & BIT(1)) >> 1;
	hdr->global_dimming = data[6] & BIT(0);

	hdr->dm_version = (data[5] & 0x1c) >> 2;

	hdr->colorimetry = data[7] & BIT(0);

	hdr->t_max_lum = (data[6] & 0xfe) >> 1;
	hdr->t_min_lum = (data[7] & 0xfe) >> 1;

	hdr->low_latency = data[8] & 0x3;

	hdr->unique_rx = (data[11] & 0xf8) >> 3;
	hdr->unique_ry = (data[11] & 0x7) << 2 | (data[10] & BIT(0)) << 1 |
		(data[9] & BIT(0));
	hdr->unique_gx = (data[9] & 0xfe) >> 1;
	hdr->unique_gy = (data[10] & 0xfe) >> 1;
	hdr->unique_bx = (data[8] & 0xe0) >> 5;
	hdr->unique_by = (data[8] & 0x1c) >> 2;
}

static void parse_ver_12_v2_data(struct ver_12_v2 *hdr, const u8 *data)
{
	hdr->yuv422_12bit = data[5] & BIT(0);
	hdr->backlt_ctrl = (data[5] & BIT(1)) >> 1;
	hdr->global_dimming = (data[6] & BIT(2)) >> 2;

	hdr->dm_version = (data[5] & 0x1c) >> 2;
	hdr->backlt_min_luma = data[6] & 0x3;
	hdr->interface = data[7] & 0x3;
	hdr->yuv444_10b_12b = (data[8] & BIT(0)) << 1 | (data[9] & BIT(0));

	hdr->t_min_pq_v2 = (data[6] & 0xf8) >> 3;
	hdr->t_max_pq_v2 = (data[7] & 0xf8) >> 3;

	hdr->unique_rx = (data[10] & 0xf8) >> 3;
	hdr->unique_ry = (data[11] & 0xf8) >> 3;
	hdr->unique_gx = (data[8] & 0xfe) >> 1;
	hdr->unique_gy = (data[9] & 0xfe) >> 1;
	hdr->unique_bx = data[10] & 0x7;
	hdr->unique_by = data[11] & 0x7;
}

static
void parse_next_hdr_block(struct next_hdr_sink_data *sink_data,
			  const u8 *next_hdr_db)
{
	int version;

	version = check_next_hdr_version(next_hdr_db);
	if (version < 0)
		return;

	sink_data->version = version;

	switch (version) {
	case VER_26_BYTE_V0:
		parse_ver_26_v0_data(&sink_data->ver_26_v0, next_hdr_db);
		break;
	case VER_15_BYTE_V1:
		parse_ver_15_v1_data(&sink_data->ver_15_v1, next_hdr_db);
		break;
	case VER_12_BYTE_V1:
		parse_ver_12_v1_data(&sink_data->ver_12_v1, next_hdr_db);
		break;
	case VER_12_BYTE_V2:
		parse_ver_12_v2_data(&sink_data->ver_12_v2, next_hdr_db);
		break;
	default:
		break;
	}
}

int rockchip_drm_parse_cea_ext(struct rockchip_drm_dsc_cap *dsc_cap,
			       u8 *max_frl_rate_per_lane, u8 *max_lanes, u8 *add_func,
			       const struct edid *edid)
{
	const u8 *edid_ext;
	int i, start, end;

	if (!dsc_cap || !max_frl_rate_per_lane || !max_lanes || !edid || !add_func)
		return -EINVAL;

	edid_ext = find_cea_extension(edid);
	if (!edid_ext)
		return -EINVAL;

	if (cea_db_offsets(edid_ext, &start, &end))
		return -EINVAL;

	for_each_cea_db(edid_ext, i, start, end) {
		const u8 *db = &edid_ext[i];

		if (cea_db_is_hdmi_forum_vsdb(db))
			parse_edid_forum_vsdb(dsc_cap, max_frl_rate_per_lane,
					      max_lanes, add_func, db);
	}

	return 0;
}
EXPORT_SYMBOL(rockchip_drm_parse_cea_ext);

int rockchip_drm_parse_next_hdr(struct next_hdr_sink_data *sink_data,
				const struct edid *edid)
{
	const u8 *edid_ext;
	int i, start, end;

	if (!sink_data || !edid)
		return -EINVAL;

	memset(sink_data, 0, sizeof(struct next_hdr_sink_data));

	edid_ext = find_cea_extension(edid);
	if (!edid_ext)
		return -EINVAL;

	if (cea_db_offsets(edid_ext, &start, &end))
		return -EINVAL;

	for_each_cea_db(edid_ext, i, start, end) {
		const u8 *db = &edid_ext[i];

		if (cea_db_is_hdmi_next_hdr_block(db))
			parse_next_hdr_block(sink_data, db);
	}

	return 0;
}
EXPORT_SYMBOL(rockchip_drm_parse_next_hdr);

#define COLORIMETRY_DATA_BLOCK		0x5
#define USE_EXTENDED_TAG		0x07

static bool cea_db_is_hdmi_colorimetry_data_block(const u8 *db)
{
	if (cea_db_tag(db) != USE_EXTENDED_TAG)
		return false;

	if (db[1] != COLORIMETRY_DATA_BLOCK)
		return false;

	return true;
}

int
rockchip_drm_parse_colorimetry_data_block(u8 *colorimetry, const struct edid *edid)
{
	const u8 *edid_ext;
	int i, start, end;

	if (!colorimetry || !edid)
		return -EINVAL;

	*colorimetry = 0;

	edid_ext = find_cea_extension(edid);
	if (!edid_ext)
		return -EINVAL;

	if (cea_db_offsets(edid_ext, &start, &end))
		return -EINVAL;

	for_each_cea_db(edid_ext, i, start, end) {
		const u8 *db = &edid_ext[i];

		if (cea_db_is_hdmi_colorimetry_data_block(db))
			/* As per CEA 861-G spec */
			*colorimetry = ((db[3] & (0x1 << 7)) << 1) | db[2];
	}

	return 0;
}
EXPORT_SYMBOL(rockchip_drm_parse_colorimetry_data_block);

/*
 * Attach a (component) device to the shared drm dma mapping from master drm
 * device.  This is used by the VOPs to map GEM buffers to a common DMA
 * mapping.
 */
int rockchip_drm_dma_attach_device(struct drm_device *drm_dev,
				   struct device *dev)
{
	struct rockchip_drm_private *private = drm_dev->dev_private;
	int ret;

	if (!is_support_iommu)
		return 0;

	ret = iommu_attach_device(private->domain, dev);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to attach iommu device\n");
		return ret;
	}

	return 0;
}

void rockchip_drm_dma_detach_device(struct drm_device *drm_dev,
				    struct device *dev)
{
	struct rockchip_drm_private *private = drm_dev->dev_private;
	struct iommu_domain *domain = private->domain;

	if (!is_support_iommu)
		return;

	iommu_detach_device(domain, dev);
}

void rockchip_drm_crtc_standby(struct drm_crtc *crtc, bool standby)
{
	struct rockchip_drm_private *priv = crtc->dev->dev_private;
	int pipe = drm_crtc_index(crtc);

	if (pipe < ROCKCHIP_MAX_CRTC &&
	    priv->crtc_funcs[pipe] &&
	    priv->crtc_funcs[pipe]->crtc_standby)
		priv->crtc_funcs[pipe]->crtc_standby(crtc, standby);
}

int rockchip_register_crtc_funcs(struct drm_crtc *crtc,
				 const struct rockchip_crtc_funcs *crtc_funcs)
{
	int pipe = drm_crtc_index(crtc);
	struct rockchip_drm_private *priv = crtc->dev->dev_private;

	if (pipe >= ROCKCHIP_MAX_CRTC)
		return -EINVAL;

	priv->crtc_funcs[pipe] = crtc_funcs;

	return 0;
}

void rockchip_unregister_crtc_funcs(struct drm_crtc *crtc)
{
	int pipe = drm_crtc_index(crtc);
	struct rockchip_drm_private *priv = crtc->dev->dev_private;

	if (pipe >= ROCKCHIP_MAX_CRTC)
		return;

	priv->crtc_funcs[pipe] = NULL;
}

static int rockchip_drm_fault_handler(struct iommu_domain *iommu,
				      struct device *dev,
				      unsigned long iova, int flags, void *arg)
{
	struct drm_device *drm_dev = arg;
	struct rockchip_drm_private *priv = drm_dev->dev_private;
	struct drm_crtc *crtc;

	DRM_ERROR("iommu fault handler flags: 0x%x\n", flags);
	drm_for_each_crtc(crtc, drm_dev) {
		int pipe = drm_crtc_index(crtc);

		if (priv->crtc_funcs[pipe] &&
		    priv->crtc_funcs[pipe]->regs_dump)
			priv->crtc_funcs[pipe]->regs_dump(crtc, NULL);

		if (priv->crtc_funcs[pipe] &&
		    priv->crtc_funcs[pipe]->debugfs_dump)
			priv->crtc_funcs[pipe]->debugfs_dump(crtc, NULL);
	}

	return 0;
}

/*
	该函数是 Rockchip DRM 驱动中 IOMMU 子系统的核心初始化接口，专为显示场景设计，
	负责搭建「虚拟地址→物理地址」的转换环境、内存隔离与故障处理机制，
	确保 VOP、HDMI 等显示硬件能安全、高效地访问显存（GEM 对象）

	IOMMU 的核心用途：不是单纯给显示 buffer 分配内存，而是为显示硬件（VOP、HDMI 等）提供「地址转换 + 内存隔离 + 故障保护」，解决显示链路的内存安全、地址冲突、平台兼容问题；
	是否给显示 buffer 用：是，但仅负责「显示 buffer 的虚拟地址（IOVA）→ 物理地址（PA）映射」，不直接分配 buffer 大小，buffer 大小由用户态（如 APP、 compositor）按分辨率需求申请；
	大小确定逻辑：IOMMU 自身的地址空间大小（start~end）由硬件规格 + 设备树配置决定；显示 buffer 的大小由分辨率、像素格式计算（如 1080p@32bit 约 8MB），且不能超过 IOMMU 地址空间上限。
*/
static int rockchip_drm_init_iommu(struct drm_device *drm_dev)
{
	struct rockchip_drm_private *private = drm_dev->dev_private;
	struct iommu_domain_geometry *geometry;
	u64 start, end;
	int ret = 0;

	if (!is_support_iommu)
		return 0;

	/*
		iommu_domain_alloc：Linux IOMMU 子系统标准 API，为「platform 总线设备」分配独立的 IOMMU 域；
		关键概念：IOMMU 域是「地址转换的隔离单元」，每个域有独立的页表，
		显示硬件的地址转换仅在该域内生效，避免与其他设备（如 GPU、DMA）的地址冲突；
	*/
	private->domain = iommu_domain_alloc(&platform_bus_type);
	if (!private->domain)
		return -ENOMEM;

	geometry = &private->domain->geometry;
	start = geometry->aperture_start;
	end = geometry->aperture_end;

	DRM_DEBUG("IOMMU context initialized (aperture: %#llx-%#llx)\n",
		  start, end);
	/*
		drm_mm_init：DRM 框架的内存管理器初始化函数，用于管理 IOMMU 虚拟地址空间的分配与释放；
		第一个参数：private->mm 是 DRM 内存管理器实例；
		第二个参数：地址空间起始地址（即 aperture_start）；
		第三个参数：地址空间总大小（end - start + 1）；
	*/ 
	drm_mm_init(&private->mm, start, end - start + 1);
	mutex_init(&private->mm_lock);
	
	/*
		iommu_set_fault_handler：注册 IOMMU 访问故障回调函数，当显示硬件访问非法虚拟地址（如越界、未映射）时，触发该回调；
		回调函数 rockchip_drm_fault_handler 功能（在文档代码中实现）：
		打印故障标志（flags），提示内存访问异常；
		遍历所有 CRTC，调用 regs_dump/debugfs_dump 打印硬件寄存器状态，辅助定位故障原因（如哪个 Plane、哪个虚拟地址触发异常）；
		核心价值：增强驱动的容错性和可调试性，避免硬件访问异常导致系统崩溃。
	*/
	iommu_set_fault_handler(private->domain, rockchip_drm_fault_handler,
				drm_dev);

	if (iommu_reserve_map) {
		/*
		 * At 32 bit platform size_t maximum value is 0xffffffff, SZ_4G(0x100000000) will be
		 * cliped to 0, so we split into two mapping
		 */
		ret = iommu_map(private->domain, 0, 0, (size_t)SZ_2G,
				IOMMU_WRITE | IOMMU_READ | IOMMU_PRIV);
		if (ret)
			dev_err(drm_dev->dev, "failed to create 0-2G pre mapping\n");

		ret = iommu_map(private->domain, SZ_2G, SZ_2G, (size_t)SZ_2G,
				IOMMU_WRITE | IOMMU_READ | IOMMU_PRIV);
		if (ret)
			dev_err(drm_dev->dev, "failed to create 2G-4G pre mapping\n");
	}

	return ret;
}

static void rockchip_iommu_cleanup(struct drm_device *drm_dev)
{
	struct rockchip_drm_private *private = drm_dev->dev_private;

	if (!is_support_iommu)
		return;

	if (iommu_reserve_map) {
		iommu_unmap(private->domain, 0, (size_t)SZ_2G);
		iommu_unmap(private->domain, SZ_2G, (size_t)SZ_2G);
	}
	drm_mm_takedown(&private->mm);
	iommu_domain_free(private->domain);
}

#ifdef CONFIG_DEBUG_FS
static int rockchip_drm_mm_dump(struct seq_file *s, void *data)
{
	struct drm_info_node *node = s->private;
	struct drm_minor *minor = node->minor;
	struct drm_device *drm_dev = minor->dev;
	struct rockchip_drm_private *priv = drm_dev->dev_private;
	struct drm_printer p = drm_seq_file_printer(s);

	if (!priv->domain)
		return 0;
	mutex_lock(&priv->mm_lock);
	drm_mm_print(&priv->mm, &p);
	mutex_unlock(&priv->mm_lock);

	return 0;
}

static int rockchip_drm_summary_show(struct seq_file *s, void *data)
{
	struct drm_info_node *node = s->private;
	struct drm_minor *minor = node->minor;
	struct drm_device *drm_dev = minor->dev;
	struct rockchip_drm_private *priv = drm_dev->dev_private;
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, drm_dev) {
		int pipe = drm_crtc_index(crtc);

		if (priv->crtc_funcs[pipe] &&
		    priv->crtc_funcs[pipe]->debugfs_dump)
			priv->crtc_funcs[pipe]->debugfs_dump(crtc, s);
	}

	return 0;
}

static int rockchip_drm_regs_dump(struct seq_file *s, void *data)
{
	struct drm_info_node *node = s->private;
	struct drm_minor *minor = node->minor;
	struct drm_device *drm_dev = minor->dev;
	struct rockchip_drm_private *priv = drm_dev->dev_private;
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, drm_dev) {
		int pipe = drm_crtc_index(crtc);

		if (priv->crtc_funcs[pipe] &&
		    priv->crtc_funcs[pipe]->regs_dump)
			priv->crtc_funcs[pipe]->regs_dump(crtc, s);
	}

	return 0;
}

static int rockchip_drm_active_regs_dump(struct seq_file *s, void *data)
{
	struct drm_info_node *node = s->private;
	struct drm_minor *minor = node->minor;
	struct drm_device *drm_dev = minor->dev;
	struct rockchip_drm_private *priv = drm_dev->dev_private;
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, drm_dev) {
		int pipe = drm_crtc_index(crtc);

		if (priv->crtc_funcs[pipe] &&
		    priv->crtc_funcs[pipe]->active_regs_dump)
			priv->crtc_funcs[pipe]->active_regs_dump(crtc, s);
	}

	return 0;
}

static struct drm_info_list rockchip_debugfs_files[] = {
	{ "active_regs", rockchip_drm_active_regs_dump, 0, NULL },
	{ "regs", rockchip_drm_regs_dump, 0, NULL },
	{ "summary", rockchip_drm_summary_show, 0, NULL },
	{ "mm_dump", rockchip_drm_mm_dump, 0, NULL },
};

static void rockchip_drm_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct rockchip_drm_private *priv = dev->dev_private;
	struct drm_crtc *crtc;

	drm_debugfs_create_files(rockchip_debugfs_files,
				 ARRAY_SIZE(rockchip_debugfs_files),
				 minor->debugfs_root, minor);

	drm_for_each_crtc(crtc, dev) {
		int pipe = drm_crtc_index(crtc);

		if (priv->crtc_funcs[pipe] &&
		    priv->crtc_funcs[pipe]->debugfs_init)
			priv->crtc_funcs[pipe]->debugfs_init(minor, crtc);
	}
}
#endif

static int rockchip_drm_create_properties(struct drm_device *dev)
{
	struct drm_property *prop;
	struct rockchip_drm_private *private = dev->dev_private;

	/*
		EOTF 属性
		创建接口：drm_property_create_range，取值范围 0~5
		权限：原子可写（DRM_MODE_PROP_ATOMIC）
		全称：Electro-Optical Transfer Function（电光转换函数）
		核心作用：控制显示链路的光电转换曲线，匹配片源的 HDR/SDR 格式，是 HDR 显示的核心参数。
		取值对应标准：覆盖主流的 HDR/SDR 格式，例如 0=传统伽马(SDR)、1=BT.1886、2=SMPTE ST 2084(HDR10)、3=HLG 等。
		典型场景：播放 HDR 视频时，用户空间通过原子提交设置对应 EOTF，驱动配置 VOP/HDMI 输出匹配的 HDR 信号，适配显示设备。
	*/
	prop = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC,
					 "EOTF", 0, 5);
	if (!prop)
		return -ENOMEM;
	private->eotf_prop = prop;
	
	/*
		COLOR_SPACE 属性
		创建接口：drm_property_create_range，取值范围 0~12
		权限：原子可写（DRM_MODE_PROP_ATOMIC）
		核心作用：设置显示链路的色彩空间 / 色域标准，匹配片源和显示设备的色域，避免色偏。
		取值对应标准：覆盖全量主流色域，例如 BT.601（标清）、BT.709（高清SDR）、BT.2020（超高清HDR）、DCI-P3（电影广色域） 等。
		典型场景：播放广色域电影、专业校色场景，设置对应色彩空间，驱动配置 VOP 硬件色彩转换模块，保证色彩还原准确。
	*/
	prop = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC,
					 "COLOR_SPACE", 0, 12);
	if (!prop)
		return -ENOMEM;
	private->color_space_prop = prop;

	/*
		创建接口：drm_property_create_range，取值范围 0~1（布尔型）
		权限：原子可写（DRM_MODE_PROP_ATOMIC）
		核心作用：控制 DRM 原子提交的执行模式，是嵌入式场景优化显示流畅度的关键参数。
		取值含义：0=同步提交（默认，等待硬件配置完成再返回，安全但延迟高）；1=异步提交（提交后立即返回，内核后台完成硬件配置，降低渲染延迟）。
		典型场景：Android UI、嵌入式交互界面，异步提交可降低画面渲染延迟，避免 vsync 等待导致的卡顿、掉帧。
		补充：Rockchip 驱动仅支持平面地址更新等轻量操作的异步提交，分辨率 / 刷新率切换等重配置只能用同步模式。
	*/
	prop = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC,
					 "ASYNC_COMMIT", 0, 1);
	if (!prop)
		return -ENOMEM;
	private->async_commit_prop = prop;

	/*
		SHARE_ID 属性
		创建接口：drm_property_create_range，取值范围 0~UINT_MAX
		权限：原子可写（DRM_MODE_PROP_ATOMIC）
		核心作用：Rockchip 平台特有的资源共享标识，用于多进程、多 CRTC、GPU 与显示模块之间的 DMA-BUF / 帧缓冲共享，避免内存重复映射。
		典型场景：双屏异显、多 VOP 联动、GPU 渲染与显示输出的零拷贝 buffer 共享。
	*/
	prop = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC,
					 "SHARE_ID", 0, UINT_MAX);
	if (!prop)
		return -ENOMEM;
	private->share_id_prop = prop;

	/*
		CONNECTOR_ID 属性
		创建接口：drm_property_create_range，取值范围 0~0xf（4bit，最多支持 16 个显示接口）
		权限：原子只读（DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_IMMUTABLE）
		核心作用：给每个物理显示接口（HDMI、MIPI、DP、LVDS 等）分配唯一的硬件 ID，用户空间可通过该 ID 精准识别对应的物理接口。
		典型场景：双 HDMI、HDMI+MIPI 等多接口设备，用户空间通过 ID 区分不同显示接口，实现针对性的显示策略。
	*/
	prop = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_IMMUTABLE,
					 "CONNECTOR_ID", 0, 0xf);
	if (!prop)
		return -ENOMEM;
	private->connector_id_prop = prop;

	/*
		SOC_ID 属性
		创建接口：drm_property_create_object，绑定对象类型 DRM_MODE_OBJECT_CRTC
		权限：原子只读（DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_IMMUTABLE）
		核心作用：标识当前 CRTC 对应的 SoC 型号，Rockchip 全系列 SoC 共用一套 DRM 驱动，通过该 ID 给用户空间暴露芯片型号，适配不同硬件的能力差异。
		典型场景：通用固件 / 系统，用户空间通过该 ID 识别是 RK3588/RK3568/RK3399 等芯片，加载对应配置、适配对应的显示能力（如 8K/4K 支持）。
	*/
	prop = drm_property_create_object(dev,
					  DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_IMMUTABLE,
					  "SOC_ID", DRM_MODE_OBJECT_CRTC);
	private->soc_id_prop = prop;

	/*
		PORT_ID 属性
		创建接口：drm_property_create_object，绑定对象类型 DRM_MODE_OBJECT_CRTC
		权限：原子只读（DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_IMMUTABLE）
		核心作用：标识 CRTC 对应的硬件视频输出端口 ID，RK3588 这类多 VOP / 多 CRTC 芯片，每个 CRTC 对应一个独立硬件端口，通过该 ID 区分。
		典型场景：RK3588 支持 3 屏异显，用户空间通过 PORT_ID 识别每个 CRTC 对应的硬件端口，匹配端口的能力上限（如部分端口支持 8K，部分仅支持 4K）。
	*/
	prop = drm_property_create_object(dev,
					  DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_IMMUTABLE,
					  "PORT_ID", DRM_MODE_OBJECT_CRTC);
	private->port_id_prop = prop;
	
	/*
		ACLK 属性
		创建接口：drm_property_create_range，取值范围 0~UINT_MAX
		权限：普通可读写
		核心作用：设置 / 读取 VOP 的 AXI 总线时钟（ACLK）频率，ACLK 是 VOP 访问内存的核心总线时钟，直接决定显示带宽上限。
		典型场景：8K/4K@120Hz 等高分辨率场景，提升 ACLK 频率满足带宽需求；调试时排查带宽不足导致的花屏、卡顿、画面撕裂问题。
	*/
	private->aclk_prop = drm_property_create_range(dev, 0, "ACLK", 0, UINT_MAX);
	/*
		BACKGROUND 属性
		创建接口：drm_property_create_range，取值范围 0~UINT_MAX
		权限：普通可读写
		核心作用：设置 VOP 硬件背景层的颜色值，当所有图层都无内容 / 关闭时，VOP 输出该背景色。
		取值格式：ARGB8888 颜色值，0 为全黑，UINT_MAX 为全白。
		典型场景：系统启动、界面切换时设置默认黑底，避免出现花屏；调试时验证背景层硬件是否正常工作。
	*/
	private->bg_prop = drm_property_create_range(dev, 0, "BACKGROUND", 0, UINT_MAX);
	/*
		LINE_FLAG1 属性
		创建接口：drm_property_create_range，取值范围 0~UINT_MAX
		权限：普通可读写
		核心作用：Rockchip 特有的行中断配置属性，设置 VOP 行中断的触发位置，用于 TE 同步、帧率统计、垂直消隐期的精准时序控制。
		典型场景：VRR 可变刷新率、画面合成精准时序控制、调试显示撕裂 / 不同步问题。
	*/
	private->line_flag_prop = drm_property_create_range(dev, 0, "LINE_FLAG1", 0, UINT_MAX);
	/*
		CUBIC_LUT 属性
		创建接口：drm_property_create，类型 DRM_MODE_PROP_BLOB
		权限：普通可读写
		核心作用：传递三维查找表（3D LUT）的二进制数据，实现硬件级的色彩校正、色域映射、HDR→SDR 色调映射、伽马校准。
		典型场景：专业显示设备校色、广色域内容适配、电影级画质调色，用户空间生成校色后的 3D LUT 表，通过该属性传给内核并配置到 VOP 硬件。
	*/
	private->cubic_lut_prop = drm_property_create(dev, DRM_MODE_PROP_BLOB, "CUBIC_LUT", 0);
	/*
		CUBIC_LUT_SIZE 属性
		创建接口：drm_property_create_range，取值范围 0~UINT_MAX
		权限：只读（DRM_MODE_PROP_IMMUTABLE）
		核心作用：标识硬件支持的 3D LUT 表最大尺寸（如 17x17x17、33x33x33），用户空间通过该值生成匹配尺寸的 LUT 数据，避免硬件不兼容。
		典型场景：校色工具、显示服务读取该值，生成对应规格的 3D LUT 校准数据。
	*/
	private->cubic_lut_size_prop = drm_property_create_range(dev, DRM_MODE_PROP_IMMUTABLE,
								 "CUBIC_LUT_SIZE", 0, UINT_MAX);

	return drm_mode_create_tv_properties(dev, 0, NULL);
}

static void rockchip_attach_connector_property(struct drm_device *drm)
{
	struct drm_connector *connector;
	struct drm_mode_config *conf = &drm->mode_config;
	struct drm_connector_list_iter conn_iter;

	mutex_lock(&drm->mode_config.mutex);

#define ROCKCHIP_PROP_ATTACH(prop, v) \
		drm_object_attach_property(&connector->base, prop, v)

	drm_connector_list_iter_begin(drm, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		ROCKCHIP_PROP_ATTACH(conf->tv_brightness_property, 50);
		ROCKCHIP_PROP_ATTACH(conf->tv_contrast_property, 50);
		ROCKCHIP_PROP_ATTACH(conf->tv_saturation_property, 50);
		ROCKCHIP_PROP_ATTACH(conf->tv_hue_property, 50);
	}
	drm_connector_list_iter_end(&conn_iter);
#undef ROCKCHIP_PROP_ATTACH

	mutex_unlock(&drm->mode_config.mutex);
}

/*
	acquire_ctx 是 DRM (Direct Rendering Manager) 中 原子模式设置（Atomic Mode Setting） 
	机制的核心上下文对象，全称为 struct drm_modeset_acquire_ctx

	drm_modeset_acquire_ctx 是 DRM 用于 管理模式设置锁的生命周期、避免死锁、追踪锁依赖 的上下文对象，核心解决以下问题：
	原子提交（atomic commit）过程中需要加锁多个 DRM 对象（CRTC/connector/encoder），acquire_ctx 追踪已获取的锁，确保解锁顺序与加锁顺序相反，避免死锁；
	统一管理锁的 “获取 - 重试 - 释放” 逻辑，简化原子操作的锁管理；
	绑定到 drm_mode_config 的 acquire_ctx 是全局默认上下文，供整个 DRM 设备的原子操作复用。
*/
static void rockchip_drm_set_property_default(struct drm_device *drm)
{
	struct drm_connector *connector;
	struct drm_mode_config *conf = &drm->mode_config;
	struct drm_atomic_state *state;
	int ret;
	struct drm_connector_list_iter conn_iter;

	drm_modeset_lock_all(drm);

	/*
		conf->acquire_ctx：drm_mode_config 初始化时会创建一个默认的 acquire_ctx，作为整个 DRM 设备原子操作的全局锁上下文；
		drm_atomic_helper_duplicate_state：基于传入的 acquire_ctx 复制一份当前的原子状态（包含所有 CRTC/connector/encoder 的状态），
		同时将 acquire_ctx 关联到新的 atomic_state，用于后续原子操作的锁管理。
	*/
	state = drm_atomic_helper_duplicate_state(drm, conf->acquire_ctx);
	if (IS_ERR(state)) {
		DRM_ERROR("failed to alloc atomic state\n");
		goto err_unlock;
	}
	/*
		显式将全局 acquire_ctx 赋值给新创建的 atomic_state，确保后续对 connector_state 的修改、原子提交过程中，锁的获取 / 释放由该上下文管理；
		若不绑定 acquire_ctx，原子提交时会因无锁上下文导致死锁或锁管理混乱。
	*/
	state->acquire_ctx = conf->acquire_ctx;

	drm_connector_list_iter_begin(drm, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		struct drm_connector_state *connector_state;

		connector_state = drm_atomic_get_connector_state(state,
								 connector);
		if (IS_ERR(connector_state)) {
			DRM_ERROR("Connector[%d]: Failed to get state\n", connector->base.id);
			continue;
		}

		// connector_state->tv 及其包含的亮度 / 对比度 / 饱和度 / 色调，是框架为 TV 类接口设计的标准属性，并非 Rockchip 自定义；
		connector_state->tv.brightness = 50;
		connector_state->tv.contrast = 50;
		connector_state->tv.saturation = 50;
		connector_state->tv.hue = 50;
	}
	drm_connector_list_iter_end(&conn_iter);

	/*
		drm_atomic_commit(state)：负责将 state 中记录的所有显示状态变更（如亮度 / 对比度修改、分辨率切换、图层移动等）原子化地应用到硬件，调用成功即表示状态已生效（硬件已按新配置工作）；
		drm_atomic_state_put(state)：在提交完成后，释放 state 结构体占用的内核内存及关联资源（如锁上下文、状态副本），是 DRM 状态管理的 “收尾清理” 操作。
	*/
	ret = drm_atomic_commit(state);
	WARN_ON(ret == -EDEADLK);
	if (ret)
		DRM_ERROR("Failed to update properties\n");
	/*
		严格成对的 API 是 drm_atomic_state_create ↔ drm_atomic_state_put（创建空状态→释放）；
		drm_atomic_helper_duplicate_state 是 “复制已有状态” 的变体，其创建的状态容器，
		最终仍需通过 drm_atomic_state_put 释放，因此是 “逻辑配对” 而非 “API 设计成对
	*/
	drm_atomic_state_put(state);

err_unlock:
	drm_modeset_unlock_all(drm);
}

/*
	该函数是 Rockchip DRM 驱动中安全显存池的初始化接口，核心作用是从设备树解析预分配的 “安全内存区域”，
	并通过 Linux 内核的 gen_pool（通用内存池）机制封装成可动态分配的显存池，专供显示驱动的 GEM buffer 使用。
*/
static int rockchip_gem_pool_init(struct drm_device *drm)
{
	struct rockchip_drm_private *private = drm->dev_private;
	struct device_node *np = drm->dev->of_node;
	struct device_node *node;
	phys_addr_t start, size;
	struct resource res;
	int ret;

	node = of_parse_phandle(np, "secure-memory-region", 0);
	if (!node)
		return -ENXIO;

	ret = of_address_to_resource(node, 0, &res);
	if (ret)
		return ret;
	start = res.start;
	size = resource_size(&res);
	if (!size)
		return -ENOMEM;

	// 参数1：PAGE_SHIFT（页偏移，通常为12，对应4KB页大小），表示内存池的最小分配粒度为4KB
    	// 参数2：-1（CPU 节点，-1 表示不绑定特定CPU）
	private->secure_buffer_pool = gen_pool_create(PAGE_SHIFT, -1);
	if (!private->secure_buffer_pool)
		return -ENOMEM;

	 // 参数1：创建好的内存池
	// 参数2：内存池的物理起始地址（start）
	// 参数3：内存池的大小（size）
	// 参数4：-1（分配器标识，-1 表示默认）
	gen_pool_add(private->secure_buffer_pool, start, size, -1);

	return 0;
}

static void rockchip_gem_pool_destroy(struct drm_device *drm)
{
	struct rockchip_drm_private *private = drm->dev_private;

	if (!private->secure_buffer_pool)
		return;

	gen_pool_destroy(private->secure_buffer_pool);
}

static int rockchip_drm_bind(struct device *dev)
{
	struct drm_device *drm_dev;
	struct rockchip_drm_private *private;
	int ret;

	drm_dev = drm_dev_alloc(&rockchip_drm_driver, dev);
	if (IS_ERR(drm_dev))
		return PTR_ERR(drm_dev);

	dev_set_drvdata(dev, drm_dev);

	private = devm_kzalloc(drm_dev->dev, sizeof(*private), GFP_KERNEL);
	if (!private) {
		ret = -ENOMEM;
		goto err_free;
	}

	mutex_init(&private->ovl_lock);

	drm_dev->dev_private = private;

	INIT_LIST_HEAD(&private->psr_list);
	mutex_init(&private->psr_list_lock);
	mutex_init(&private->commit_lock);

	private->hdmi_pll.pll = devm_clk_get_optional(dev, "hdmi-tmds-pll");
	if (PTR_ERR(private->hdmi_pll.pll) == -EPROBE_DEFER) {
		ret = -EPROBE_DEFER;
		goto err_free;
	} else if (IS_ERR(private->hdmi_pll.pll)) {
		dev_err(dev, "failed to get hdmi-tmds-pll\n");
		ret = PTR_ERR(private->hdmi_pll.pll);
		goto err_free;
	}
	private->default_pll.pll = devm_clk_get_optional(dev, "default-vop-pll");
	if (PTR_ERR(private->default_pll.pll) == -EPROBE_DEFER) {
		ret = -EPROBE_DEFER;
		goto err_free;
	} else if (IS_ERR(private->default_pll.pll)) {
		dev_err(dev, "failed to get default vop pll\n");
		ret = PTR_ERR(private->default_pll.pll);
		goto err_free;
	}

	// 所有mode config属性设置
	ret = drmm_mode_config_init(drm_dev);
	if (ret)
		goto err_free;

	rockchip_drm_mode_config_init(drm_dev);
	rockchip_drm_create_properties(drm_dev);
	/* Try to bind all sub drivers. */
	ret = component_bind_all(dev, drm_dev);
	if (ret)
		goto err_mode_config_cleanup;

	rockchip_attach_connector_property(drm_dev);
	// 通过drm_crtc_init_with_planes()注册crtc，并初始化属性，num_crtc有值
	ret = drm_vblank_init(drm_dev, drm_dev->mode_config.num_crtc);
	if (ret)
		goto err_unbind_all;

	drm_mode_config_reset(drm_dev);
	rockchip_drm_set_property_default(drm_dev);

	/*
	 * enable drm irq mode.
	 * - with irq_enabled = true, we can use the vblank feature.
	 */
	drm_dev->irq_enabled = true;

	/* init kms poll for handling hpd */
	/*
		drm_kms_helper_poll_init(drm_dev) 是 Linux DRM KMS（Kernel Mode Setting）辅助层的 热插拔（HPD）轮询机制初始化函数，
		核心作用是为显示设备（如 HDMI、MIPI DSI、DP）初始化「软件轮询检测逻辑」，用于感知显示器的插拔状态
		创建轮询工作队列：初始化 struct drm_kms_helper_poll 结构体，关联到 DRM 设备（drm_dev），用于管理轮询任务；
		注册轮询回调函数：绑定默认的轮询处理函数 drm_kms_helper_poll_work，该函数会遍历所有连接器（Connector），调用 connector->funcs->detect 检测设备状态；
		启动定时轮询：默认设置 1 秒为轮询周期（可通过 drm_kms_helper_poll_enable/disable 调整或启停），内核会周期性执行轮询工作队列。
		void drm_kms_helper_poll_init(struct drm_device *dev)
		{
			INIT_DELAYED_WORK(&dev->mode_config.output_poll_work, output_poll_execute);
			dev->mode_config.poll_enabled = true;

			drm_kms_helper_poll_enable(dev);
		}
	*/
	drm_kms_helper_poll_init(drm_dev);

	ret = rockchip_drm_init_iommu(drm_dev);
	if (ret)
		goto err_unbind_all;

	// 3588已经不用
	rockchip_gem_pool_init(drm_dev);
	// reserved-memory
	/*
		阶段	                操作	                                    类比	                                                    驱动 / 用户行为
		1. 系统启动	        设备树定义 reserved-memory	             开发商划定 “DRM 专属仓库”（物理地址 0x70000000，大小 256MB）	内核锁定该内存，禁止普通进程使用
		2. DRM 初始化	        调用 of_reserved_mem_device_init	    DRM 驱动去 “物业认领仓库产权”，拿到仓库的「地址、大小、钥匙」	 驱动将预留内存信息存入 drm_dev->dev->reserved_mem
		3. 驱动管控	        驱动划分内存区域	                     驱动把仓库分成 “显存区”“帧缓冲区”“硬件缓存区”	                 开发者自主关联物理地址到不同模块：
		                       ・显存区：0x70000000 - 0x78000000
		        	       ・帧缓冲区：0x78000000 - 0x7F000000
		4. 用户申请 buffer	调用 DRM 接口（如 DRM_IOCTL_MODE_CREATE_DUMB）	用户向驱动 “申请仓库货架”	                                驱动从预留内存的对应区域分配物理地址，返回给用户
	*/
	ret = of_reserved_mem_device_init(drm_dev->dev);
	if (ret)
		DRM_DEBUG_KMS("No reserved memory region assign to drm\n");

	rockchip_drm_show_logo(drm_dev);

	// 兼容fb驱动，可以不用
	ret = rockchip_drm_fbdev_init(drm_dev);
	if (ret)
		goto err_iommu_cleanup;

	drm_dev->mode_config.allow_fb_modifiers = true;

	ret = drm_dev_register(drm_dev, 0);
	if (ret)
		goto err_kms_helper_poll_fini;

	/*
		该函数是 Rockchip 芯片 DRM / 显示驱动中时钟保护机制的 “收尾清理函数”，核心作用是：释放驱动初始化阶段为了 “保护关键显示时钟不被意外关闭”
		 而临时持有的时钟资源，完成时钟的 “解锁 / 释放”，让系统可以正常管理这些时钟的启停和功耗。
	*/
	rockchip_clk_unprotect();

	return 0;
err_kms_helper_poll_fini:
	rockchip_gem_pool_destroy(drm_dev);
	drm_kms_helper_poll_fini(drm_dev);
	rockchip_drm_fbdev_fini(drm_dev);
err_iommu_cleanup:
	rockchip_iommu_cleanup(drm_dev);
err_unbind_all:
	component_unbind_all(dev, drm_dev);
err_mode_config_cleanup:
	drm_mode_config_cleanup(drm_dev);
err_free:
	drm_dev->dev_private = NULL;
	dev_set_drvdata(dev, NULL);
	drm_dev_put(drm_dev);
	return ret;
}

static void rockchip_drm_unbind(struct device *dev)
{
	struct drm_device *drm_dev = dev_get_drvdata(dev);

	drm_dev_unregister(drm_dev);

	rockchip_drm_fbdev_fini(drm_dev);
	rockchip_gem_pool_destroy(drm_dev);
	drm_kms_helper_poll_fini(drm_dev);

	drm_atomic_helper_shutdown(drm_dev);
	component_unbind_all(dev, drm_dev);
	drm_mode_config_cleanup(drm_dev);
	rockchip_iommu_cleanup(drm_dev);

	drm_dev->dev_private = NULL;
	dev_set_drvdata(dev, NULL);
	drm_dev_put(drm_dev);
}

static void rockchip_drm_crtc_cancel_pending_vblank(struct drm_crtc *crtc,
						    struct drm_file *file_priv)
{
	struct rockchip_drm_private *priv = crtc->dev->dev_private;
	int pipe = drm_crtc_index(crtc);

	if (pipe < ROCKCHIP_MAX_CRTC &&
	    priv->crtc_funcs[pipe] &&
	    priv->crtc_funcs[pipe]->cancel_pending_vblank)
		priv->crtc_funcs[pipe]->cancel_pending_vblank(crtc, file_priv);
}

static int rockchip_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, dev)
		crtc->primary->fb = NULL;

	return 0;
}

static void rockchip_drm_postclose(struct drm_device *dev,
				   struct drm_file *file_priv)
{
	struct drm_crtc *crtc;

	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head)
		rockchip_drm_crtc_cancel_pending_vblank(crtc, file_priv);
}

static void rockchip_drm_lastclose(struct drm_device *dev)
{
	struct rockchip_drm_private *priv = dev->dev_private;

	if (!priv->logo)
		drm_fb_helper_restore_fbdev_mode_unlocked(priv->fbdev_helper);
}

static struct drm_pending_vblank_event *
rockchip_drm_add_vcnt_event(struct drm_crtc *crtc, union drm_wait_vblank *vblwait,
			    struct drm_file *file_priv)
{
	struct drm_pending_vblank_event *e;
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return NULL;

	e->pipe = drm_crtc_index(crtc);
	e->event.base.type = DRM_EVENT_ROCKCHIP_CRTC_VCNT;
	e->event.base.length = sizeof(e->event.vbl);
	e->event.vbl.crtc_id = crtc->base.id;
	e->event.vbl.user_data = vblwait->request.signal;

	spin_lock_irqsave(&dev->event_lock, flags);
	drm_event_reserve_init_locked(dev, file_priv, &e->base, &e->event.base);
	spin_unlock_irqrestore(&dev->event_lock, flags);

	return e;
}

static int rockchip_drm_get_vcnt_event_ioctl(struct drm_device *dev, void *data,
					     struct drm_file *file_priv)
{
	struct rockchip_drm_private *priv = dev->dev_private;
	union drm_wait_vblank *vblwait = data;
	struct drm_pending_vblank_event *e;
	struct drm_crtc *crtc;
	unsigned int flags, pipe;

	flags = vblwait->request.type & (_DRM_VBLANK_FLAGS_MASK | _DRM_ROCKCHIP_VCNT_EVENT);
	pipe = (vblwait->request.type & _DRM_VBLANK_HIGH_CRTC_MASK);
	if (pipe)
		pipe = pipe >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
	else
		pipe = flags & _DRM_VBLANK_SECONDARY ? 1 : 0;

	crtc = drm_crtc_from_index(dev, pipe);

	if (flags & _DRM_ROCKCHIP_VCNT_EVENT) {
		e = rockchip_drm_add_vcnt_event(crtc, vblwait, file_priv);
		priv->vcnt[pipe].event = e;
	}

	return 0;
}

static const struct drm_ioctl_desc rockchip_ioctls[] = {
	DRM_IOCTL_DEF_DRV(ROCKCHIP_GEM_CREATE, rockchip_gem_create_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	/*
		GEM 缓冲区用户态 mmap 映射的前置必备接口，用来获取 GEM 对象对应的 mmap 文件偏移量，
			用户态才能通过mmap把内核的图形缓冲区映射到用户态虚拟地址空间，实现 CPU 直接读写。
	*/
	DRM_IOCTL_DEF_DRV(ROCKCHIP_GEM_MAP_OFFSET,
			  rockchip_gem_map_offset_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	// Rockchip 定制的连续物理缓冲区起始地址获取接口，专门给无 IOMMU 场景、自定义外设驱动提供物理地址访问能力。
	DRM_IOCTL_DEF_DRV(ROCKCHIP_GEM_GET_PHYS, rockchip_gem_get_phys_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	// Rockchip 定制的垂直同步（VSYNC）事件与帧计数获取接口，vcnt全称 vblank count（垂直消隐计数），是显示合成、帧率控制、防画面撕裂的核心接口。
	DRM_IOCTL_DEF_DRV(ROCKCHIP_GET_VCNT_EVENT, rockchip_drm_get_vcnt_event_ioctl,
			  DRM_UNLOCKED),
};

static const struct file_operations rockchip_drm_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.mmap = rockchip_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.release = drm_release,
};

static int rockchip_drm_gem_dmabuf_begin_cpu_access(struct dma_buf *dma_buf,
						    enum dma_data_direction dir)
{
	struct drm_gem_object *obj = dma_buf->priv;

	return rockchip_gem_prime_begin_cpu_access(obj, dir);
}

static int rockchip_drm_gem_dmabuf_end_cpu_access(struct dma_buf *dma_buf,
						  enum dma_data_direction dir)
{
	struct drm_gem_object *obj = dma_buf->priv;

	return rockchip_gem_prime_end_cpu_access(obj, dir);
}

/*
	这个结构体是Rockchip DRM GEM 驱动对接 Linux dma-buf 框架的标准操作方法集，
		是实现跨进程 / 跨设备零拷贝内存共享的核心载体，完全遵循 Linux 内核 
		dma-buf 框架规范，也是 DRM Prime 机制的核心实现。
	简单类比：它就像 Linux 驱动里的 file_operations 结构体，file_operations 
		定义了用户态操作文件时内核的回调逻辑；而这个 dma_buf_ops 定义了内核 
		/ 用户态操作共享缓冲区时，dma-buf 框架要执行的回调逻辑，是 Rockchip 
		私有 GEM 内存和内核通用 dma-buf 框架之间的适配层

	标准化适配：把 Rockchip 私有定制的 GEM 内存（SHMEM/CMA/ 安全内存），封装成内核
		 dma-buf 框架能识别的标准共享缓冲区，实现和其他驱动的无差别兼容；
	零拷贝共享：避免 GPU 渲染、相机预览、视频解码等场景下的内存拷贝，大幅提升多媒体 / 显示链路性能；
	生命周期统一管理：通过 dma-buf 的引用计数机制，统一管理跨进程 / 跨设备共享的 GEM 内存，避免内存泄漏、野指针访问；
	缓存一致性保证：通过自定义的 begin_cpu_access/end_cpu_access 回调，保证 CPU 和外设访问共享内存
		时的缓存一致性，避免画面花屏、数据错乱。
*/
static const struct dma_buf_ops rockchip_drm_gem_prime_dmabuf_ops = {
	/*
		这是 dma-buf 框架的性能优化开关，而非回调函数，开启后内核会缓存 
		map_dma_buf 生成的 sg_table 映射结果，避免重复的 map/unmap 操作。
		关闭时（默认 false）：每次外设访问 dma-buf 时，都要重新调用 map_dma_buf 
			生成 sg_table、做 DMA 映射，访问结束后立即 unmap，高频场景（如
			相机预览 60fps 上屏）会产生大量重复开销；
		开启时（Rockchip 设置 true）：第一次 map 后，sg_table 会被缓存，后续所有
			访问都复用这个映射结果，直到 dma-buf 释放才会 unmap，大幅降低 CPU
			 开销，是嵌入式多媒体场景的标准优化手段。
		适配场景：完美匹配 Android 显示链路、相机预览、视频播放等高频 dma-buf 共享
			场景，是 Rockchip 驱动的性能优化关键点。
	*/
	.cache_sgt_mapping = true,
	/*
		dma-buf 框架标准回调，当其他设备驱动要访问这个 dma-buf 时，
			会调用该函数完成设备和 dma-buf 的绑定（附着），是跨设备共享的第一步。
		其他驱动（如 ISP、GPU、编解码器）调用 dma_buf_attach() 把自己的设备绑定到这个 dma-buf 时，内核自动调用该回调。
		1.检查绑定设备的 DMA 寻址能力，确保设备能访问这个 GEM 缓冲区的物理内存；
		2.为该设备创建专属的附着上下文，记录设备和 dma-buf 的绑定关系；
		3.处理 IOMMU 域的适配，确保后续 DMA 映射能正常完成。
	*/
	.attach = drm_gem_map_attach,
	/*
		和 attach 完全配对的回调，当其他设备不再访问这个 dma-buf 时，解除设备和 dma-buf 的绑定，完成资源清理。
		触发时机
		其他驱动调用 dma_buf_detach() 解绑设备时，内核自动调用该回调。
		实现逻辑
		使用 DRM 标准函数 drm_gem_map_detach，释放 attach 时创建的附着上下文，清理绑定关系，确保无资源泄漏。
	*/
	.detach = drm_gem_map_detach,
	/*
		核心作用
		dma-buf 跨设备共享的核心回调，为访问设备生成 DMA 可用的 sg_table 散列表，并完成 DMA 地址映射，让外设能直接访问这个 GEM 缓冲区的物理内存。
		触发时机
		其他驱动调用 dma_buf_map_attachment() 获取缓冲区的 DMA 映射时，内核自动调用该回调。
		使用 DRM 标准函数 drm_gem_map_dma_buf，核心动作：
		调用 Rockchip 驱动的 rockchip_gem_prime_get_sg_table（你之前解析的接口），获取 GEM 对象的 sg_table 散列表；
		为访问设备执行 DMA 映射，填充 sg_table 每个条目的 dma_address（外设可访问的总线地址 / IOVA 地址）；
		执行 DMA 缓存同步，保证外设能读到最新的内存数据。
	*/
	.map_dma_buf = drm_gem_map_dma_buf,
	// 和 map_dma_buf 配对的回调，解除 DMA 映射，释放 sg_table 相关资源。
	.unmap_dma_buf = drm_gem_unmap_dma_buf,
	// dma-buf 生命周期的最终销毁回调，当 dma-buf 的引用计数归 0 时，调用该函数释放绑定的 GEM 对象
	.release = drm_gem_dmabuf_release,
	/*
		触发时机
		持有 dma-buf fd 的用户态进程调用 mmap() 系统调用，把 fd 映射到自己的虚拟地址空间时，内核自动调用该回调。
		实现逻辑
		使用 DRM 标准函数，把 GEM 对象的物理页映射到用户态虚拟地址空间，和你之前解析的 
			ROCKCHIP_GEM_MAP_OFFSET ioctl + mmap 逻辑完全一致，保证跨进程映射的兼容性。
	*/
	.mmap = drm_gem_dmabuf_mmap,
	// 内核态驱动的虚拟地址映射回调，把 dma-buf 对应的物理页，映射成内核虚拟地址空间的连续地址，供内核态驱动直接线性访问。
	.vmap = drm_gem_dmabuf_vmap,
	.vunmap = drm_gem_dmabuf_vunmap,
	// 获取 dma-buf 绑定的唯一 UUID，用于跨进程 / 跨设备的缓冲区唯一标识，实现缓冲区的全局追踪、权限校验。
	.get_uuid = drm_gem_dmabuf_get_uuid,
	// 用户态 / 内核态调用 dma_buf_begin_cpu_access() 时，内核自动调用该回调。
	/*
		根据数据方向 dir，执行 DMA 缓存同步：
		DMA_FROM_DEVICE：外设写入数据，CPU 要读取 → 失效 CPU 缓存，避免读到旧的缓存数据；
		DMA_TO_DEVICE：CPU 写入数据，外设要读取 → 把 CPU 缓存刷到物理内存；
		保证 CPU 和外设看到的内存数据完全一致，是避免显示花屏、视频数据错乱的核心保障。
	*/
	.begin_cpu_access = rockchip_drm_gem_dmabuf_begin_cpu_access,
	// 和 begin_cpu_access 配对的回调，CPU 完成对 dma-buf 的访问后调用，完成收尾缓存同步，保证外设能读到 CPU 写入的最新数据。
	.end_cpu_access = rockchip_drm_gem_dmabuf_end_cpu_access,
};

/*
	将一个外部（或同驱动）的 dma-buf 共享缓冲区，转换成 Rockchip DRM 驱动可识别、可操作的标准 GEM 图形对象，让 VOP 显示控制器、
	GPU 等硬件能直接访问其他驱动（ISP 相机、视频解码器、GPU）分配的内存，全程零内存拷贝
	1.用户态调用 DRM_IOCTL_PRIME_FD_TO_HANDLE ioctl，把 dma-buf fd 转换成 DRM GEM handle 时，DRM 框架最终会调用该函数；
	2.典型业务场景：相机预览、视频解码画面上屏、GPU 渲染画面输出，实现跨驱动零拷贝；
	3.它是上层 rockchip_drm_gem_prime_import 的底层实现，上层默认将 attach_dev 设为 DRM 设备本身。
*/
static struct drm_gem_object *rockchip_drm_gem_prime_import_dev(struct drm_device *dev,
								struct dma_buf *dma_buf,
								struct device *attach_dev)
{
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct drm_gem_object *obj;
	int ret;

	if (dma_buf->ops == &rockchip_drm_gem_prime_dmabuf_ops) {
		/*
			判断待导入的 dma-buf，是否是 Rockchip DRM 驱动自己导出的：只有 Rockchip 
				驱动导出的 dma-buf，才会绑定你之前解析的专属 rockchip_drm_gem_prime_dmabuf_ops 方法集；
			这是快速路径的准入条件，不满足则直接进入通用跨驱动导入流程。
		*/
		obj = dma_buf->priv;
		if (obj->dev == dev) {
			// 校验原始 GEM 对象是否属于当前的 DRM 设备，避免跨设备、跨芯片的非法导入，防止内核 Oops。
			/*
			 * Importing dmabuf exported from out own gem increases
			 * refcount on gem itself instead of f_count of dmabuf.
			 */
			drm_gem_object_get(obj);
			return obj;
		}
	}

	if (!dev->driver->gem_prime_import_sg_table)
		return ERR_PTR(-EINVAL);

	/*
		dma_buf_attach 是 Linux dma-buf 框架标准 API，核心作用是把要访问 buffer 的硬件设备（attach_dev）
			与 dma-buf 进行绑定（附着），创建专属的 dma_buf_attachment 附着实例；
		底层执行逻辑：
		校验硬件设备的 DMA 寻址能力，确保设备能访问该 dma-buf 的物理内存；
		调用 dma-buf ops 中的 .attach 回调（Rockchip 驱动对应 drm_gem_map_attach），完成设备相关的初始化；
		返回附着实例，后续所有映射、同步操作都基于该实例执行；
	*/
	attach = dma_buf_attach(dma_buf, attach_dev);
	if (IS_ERR(attach))
		return ERR_CAST(attach);

	get_dma_buf(dma_buf);
	
	/*
		dma_buf_map_attachment 是 dma-buf 框架核心标准 API，作用是为附着的硬件设备，
			生成可直接用于 DMA 传输的 sg_table 散列表，并完成 DMA 总线地址映射；
		参数 DMA_BIDIRECTIONAL：指定数据传输方向为双向，既支持 CPU 写、外设读（显示场景），
			也支持外设写、CPU 读（相机采集场景），适配全业务场景；
		调用 dma-buf ops 中的 .map_dma_buf 回调，获取 buffer 对应的物理内存信息；
		为 attach_dev 完成 DMA 地址映射，填充 sg_table 每个条目的 dma_address（外设可直接访问的总线地址 / IOVA 地址）；
		执行 DMA 缓存同步，保证硬件能读到内存中的最新数据；
	*/
	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto fail_detach;
	}

	/*
		分配 rockchip_gem_object 定制结构体，初始化标准 DRM GEM 基类；
		保存 sg_table 到 GEM 对象，从 sg_table 中提取物理页指针数组 pages；
		开启 IOMMU 的场景下，自动完成 IOVA 虚拟地址映射，生成外设可访问的 dma_addr；
		返回标准 drm_gem_object 基类指针；
	*/
	obj = dev->driver->gem_prime_import_sg_table(dev, attach, sgt);
	if (IS_ERR(obj)) {
		ret = PTR_ERR(obj);
		goto fail_unmap;
	}

	obj->import_attach = attach;
	obj->resv = dma_buf->resv;

	return obj;

fail_unmap:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
fail_detach:
	dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);

	return ERR_PTR(ret);
}

static struct drm_gem_object *rockchip_drm_gem_prime_import(struct drm_device *dev,
							    struct dma_buf *dma_buf)
{
	return rockchip_drm_gem_prime_import_dev(dev, dma_buf, dev->dev);
}

static struct dma_buf *rockchip_drm_gem_prime_export(struct drm_gem_object *obj,
						     int flags)
{
	struct drm_device *dev = obj->dev;
	struct dma_buf_export_info exp_info = {
		.exp_name = KBUILD_MODNAME, /* white lie for debug */
		.owner = dev->driver->fops->owner,
		.ops = &rockchip_drm_gem_prime_dmabuf_ops,
		.size = obj->size,
		.flags = flags,
		.priv = obj,
		.resv = obj->resv,
	};

	return drm_gem_dmabuf_export(dev, &exp_info);
}

static struct drm_driver rockchip_drm_driver = {
	/*
		DRIVER_MODESET：支持模式设置（Mode Setting），即可以修改分辨率、刷新率、显示时序等。
		DRIVER_GEM：支持GEM（Graphics Execution Manager），即内核级的图形内存管理（分配、释放、映射显存）。
		DRIVER_ATOMIC：支持原子模式设置（Atomic Mode Setting），即 “要么所有配置同时生效，
			要么都不生效”，避免显示撕裂 / 闪烁。
		DRIVER_RENDER：支持渲染节点（Render Node），允许用户空间直接访问 GPU 进行渲染
			（Rockchip 这里主要用于 GPU 与显示的内存共享）。
	*/
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC | DRIVER_RENDER,
	/* 清理未完成的 VBlank（垂直消隐）事件，防止资源泄漏。 */
	.postclose		= rockchip_drm_postclose,
	/* 如果没有显示 Logo，恢复 fbdev（Framebuffer Device）控制台（让内核可以继续显示日志）。 */
	.lastclose		= rockchip_drm_lastclose,
	/* 初始化打开上下文，清空主图层的帧缓冲（避免遗留垃圾数据） */
	.open			= rockchip_drm_open,
	/* 使用 CMA（Contiguous Memory Allocator） 管理显存，保证物理地址连续（Rockchip 显示硬件需要连续内存） */
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	/* 释放 GEM 对象对应的物理内存和虚拟地址映射。 */
	.gem_free_object_unlocked = rockchip_gem_free_object,
	/* 创建 “哑缓冲”（Dumb Buffer）—— 一种简单的、不需要 GPU 参与的帧缓冲，常用于显示桌面或 Logo。 */
	.dumb_create		= rockchip_gem_dumb_create,
	/* GEM 句柄转 DMA-BUF 文件描述符 */
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	/* DMA-BUF 文件描述符转 GEM 句柄 */
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	/* 导入 DMA-BUF 为 GEM 对象 */
	.gem_prime_import	= rockchip_drm_gem_prime_import,
	/* 导出 GEM 对象为 DMA-BUF */
	.gem_prime_export	= rockchip_drm_gem_prime_export,
	/* 获取 GEM 的散列表（用于 DMA 映射） */
	.gem_prime_get_sg_table	= rockchip_gem_prime_get_sg_table,
	/* 从散列表导入 GEM */
	.gem_prime_import_sg_table	= rockchip_gem_prime_import_sg_table,
	/* 映射 GEM 到内核虚拟地址 */
	.gem_prime_vmap		= rockchip_gem_prime_vmap,
	/* 取消内核虚拟地址映射 */
	.gem_prime_vunmap	= rockchip_gem_prime_vunmap,
	/* 映射 GEM 到用户虚拟地址 */
	.gem_prime_mmap		= rockchip_gem_mmap_buf,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init		= rockchip_drm_debugfs_init,
#endif
	.ioctls			= rockchip_ioctls,
	.num_ioctls		= ARRAY_SIZE(rockchip_ioctls),
	.fops			= &rockchip_drm_driver_fops,
	.name	= DRIVER_NAME,
	.desc	= DRIVER_DESC,
	.date	= DRIVER_DATE,
	.major	= DRIVER_MAJOR,
	.minor	= DRIVER_MINOR,
};

#ifdef CONFIG_PM_SLEEP
static int rockchip_drm_sys_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(drm);
}

static int rockchip_drm_sys_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	return drm_mode_config_helper_resume(drm);
}
#endif

static const struct dev_pm_ops rockchip_drm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(rockchip_drm_sys_suspend,
				rockchip_drm_sys_resume)
};

#define MAX_ROCKCHIP_SUB_DRIVERS 16
static struct platform_driver *rockchip_sub_drivers[MAX_ROCKCHIP_SUB_DRIVERS];
static int num_rockchip_sub_drivers;

/*
 * Check if a vop endpoint is leading to a rockchip subdriver or bridge.
 * Should be called from the component bind stage of the drivers
 * to ensure that all subdrivers are probed.
 *
 * @ep: endpoint of a rockchip vop
 *
 * returns true if subdriver, false if external bridge and -ENODEV
 * if remote port does not contain a device.
 */
int rockchip_drm_endpoint_is_subdriver(struct device_node *ep)
{
	struct device_node *node = of_graph_get_remote_port_parent(ep);
	struct platform_device *pdev;
	struct device_driver *drv;
	int i;

	if (!node)
		return -ENODEV;

	/* status disabled will prevent creation of platform-devices */
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -ENODEV;

	/*
	 * All rockchip subdrivers have probed at this point, so
	 * any device not having a driver now is an external bridge.
	 */
	drv = pdev->dev.driver;
	if (!drv) {
		platform_device_put(pdev);
		return false;
	}

	for (i = 0; i < num_rockchip_sub_drivers; i++) {
		if (rockchip_sub_drivers[i] == to_platform_driver(drv)) {
			platform_device_put(pdev);
			return true;
		}
	}

	platform_device_put(pdev);
	return false;
}

static int compare_dev(struct device *dev, void *data)
{
	return dev == (struct device *)data;
}

static void rockchip_drm_match_remove(struct device *dev)
{
	struct device_link *link;

	list_for_each_entry(link, &dev->links.consumers, s_node)
		device_link_del(link);
}

static struct component_match *rockchip_drm_match_add(struct device *dev)
{
	struct component_match *match = NULL;
	int i;

	for (i = 0; i < num_rockchip_sub_drivers; i++) {
		struct platform_driver *drv = rockchip_sub_drivers[i];
		struct device *p = NULL, *d;

		do {
			d = platform_find_device_by_driver(p, &drv->driver);
			put_device(p);
			p = d;

			if (!d)
				break;

			device_link_add(dev, d, DL_FLAG_STATELESS);
			component_match_add(dev, &match, compare_dev, d);
		} while (true);
	}

	if (IS_ERR(match))
		rockchip_drm_match_remove(dev);

	return match ?: ERR_PTR(-ENODEV);
}

static const struct component_master_ops rockchip_drm_ops = {
	.bind = rockchip_drm_bind,
	.unbind = rockchip_drm_unbind,
};

static int rockchip_drm_platform_of_probe(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct device_node *port;
	bool found = false;
	int i;

	if (!np)
		return -ENODEV;

	for (i = 0;; i++) {
		struct device_node *iommu;

		port = of_parse_phandle(np, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		iommu = of_parse_phandle(port->parent, "iommus", 0);
		if (!iommu || !of_device_is_available(iommu)) {
			DRM_DEV_DEBUG(dev,
				      "no iommu attached for %pOF, using non-iommu buffers\n",
				      port->parent);
			/*
			 * if there is a crtc not support iommu, force set all
			 * crtc use non-iommu buffer.
			 */
			is_support_iommu = false;
		}

		found = true;

		iommu_reserve_map |= of_property_read_bool(iommu, "rockchip,reserve-map");
		of_node_put(iommu);
		of_node_put(port);
	}

	if (i == 0) {
		DRM_DEV_ERROR(dev, "missing 'ports' property\n");
		return -ENODEV;
	}

	if (!found) {
		DRM_DEV_ERROR(dev,
			      "No available vop found for display-subsystem.\n");
		return -ENODEV;
	}

	return 0;
}

static int rockchip_drm_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	int ret;

	ret = rockchip_drm_platform_of_probe(dev);
#if !IS_ENABLED(CONFIG_DRM_ROCKCHIP_VVOP)
	if (ret)
		return ret;
#endif

	match = rockchip_drm_match_add(dev);
	if (IS_ERR(match))
		return PTR_ERR(match);

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		goto err;

	ret = component_master_add_with_match(dev, &rockchip_drm_ops, match);
	if (ret < 0)
		goto err;

	return 0;
err:
	rockchip_drm_match_remove(dev);

	return ret;
}

static int rockchip_drm_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &rockchip_drm_ops);

	rockchip_drm_match_remove(&pdev->dev);

	return 0;
}

static void rockchip_drm_platform_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	if (drm)
		drm_atomic_helper_shutdown(drm);
}

static const struct of_device_id rockchip_drm_dt_ids[] = {
	{ .compatible = "rockchip,display-subsystem", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rockchip_drm_dt_ids);

static struct platform_driver rockchip_drm_platform_driver = {
	.probe = rockchip_drm_platform_probe,
	.remove = rockchip_drm_platform_remove,
	.shutdown = rockchip_drm_platform_shutdown,
	.driver = {
		.name = "rockchip-drm",
		.of_match_table = rockchip_drm_dt_ids,
		.pm = &rockchip_drm_pm_ops,
	},
};

#define ADD_ROCKCHIP_SUB_DRIVER(drv, cond) { \
	if (IS_ENABLED(cond) && \
	    !WARN_ON(num_rockchip_sub_drivers >= MAX_ROCKCHIP_SUB_DRIVERS)) \
		rockchip_sub_drivers[num_rockchip_sub_drivers++] = &drv; \
}

static int __init rockchip_drm_init(void)
{
	int ret;

	num_rockchip_sub_drivers = 0;
#if IS_ENABLED(CONFIG_DRM_ROCKCHIP_VVOP)
	ADD_ROCKCHIP_SUB_DRIVER(vvop_platform_driver, CONFIG_DRM_ROCKCHIP_VVOP);
#else
	ADD_ROCKCHIP_SUB_DRIVER(vop_platform_driver, CONFIG_ROCKCHIP_VOP);
	ADD_ROCKCHIP_SUB_DRIVER(vop2_platform_driver, CONFIG_ROCKCHIP_VOP2);
	ADD_ROCKCHIP_SUB_DRIVER(vconn_platform_driver, CONFIG_ROCKCHIP_VCONN);
	ADD_ROCKCHIP_SUB_DRIVER(rockchip_lvds_driver,
				CONFIG_ROCKCHIP_LVDS);
	ADD_ROCKCHIP_SUB_DRIVER(rockchip_dp_driver,
				CONFIG_ROCKCHIP_ANALOGIX_DP);
	ADD_ROCKCHIP_SUB_DRIVER(cdn_dp_driver, CONFIG_ROCKCHIP_CDN_DP);
	ADD_ROCKCHIP_SUB_DRIVER(dw_hdmi_rockchip_pltfm_driver,
				CONFIG_ROCKCHIP_DW_HDMI);
	ADD_ROCKCHIP_SUB_DRIVER(dw_mipi_dsi_rockchip_driver,
				CONFIG_ROCKCHIP_DW_MIPI_DSI);
	ADD_ROCKCHIP_SUB_DRIVER(dw_mipi_dsi2_rockchip_driver,
				CONFIG_ROCKCHIP_DW_MIPI_DSI);
	ADD_ROCKCHIP_SUB_DRIVER(inno_hdmi_driver, CONFIG_ROCKCHIP_INNO_HDMI);
	ADD_ROCKCHIP_SUB_DRIVER(rk3066_hdmi_driver,
				CONFIG_ROCKCHIP_RK3066_HDMI);
	ADD_ROCKCHIP_SUB_DRIVER(rockchip_rgb_driver, CONFIG_ROCKCHIP_RGB);
	ADD_ROCKCHIP_SUB_DRIVER(rockchip_tve_driver, CONFIG_ROCKCHIP_DRM_TVE);
	ADD_ROCKCHIP_SUB_DRIVER(dw_dp_driver, CONFIG_ROCKCHIP_DW_DP);

#endif
	ret = platform_register_drivers(rockchip_sub_drivers,
					num_rockchip_sub_drivers);
	if (ret)
		return ret;

	ret = platform_driver_register(&rockchip_drm_platform_driver);
	if (ret)
		goto err_unreg_drivers;

	rockchip_gem_get_ddr_info();

	return 0;

err_unreg_drivers:
	platform_unregister_drivers(rockchip_sub_drivers,
				    num_rockchip_sub_drivers);
	return ret;
}

static void __exit rockchip_drm_fini(void)
{
	platform_driver_unregister(&rockchip_drm_platform_driver);

	platform_unregister_drivers(rockchip_sub_drivers,
				    num_rockchip_sub_drivers);
}

#ifdef CONFIG_VIDEO_REVERSE_IMAGE
fs_initcall(rockchip_drm_init);
#else
module_init(rockchip_drm_init);
#endif
module_exit(rockchip_drm_fini);

MODULE_AUTHOR("Mark Yao <mark.yao@rock-chips.com>");
MODULE_DESCRIPTION("ROCKCHIP DRM Driver");
MODULE_LICENSE("GPL v2");
