/*
 * Xilinx Video HDMI RX Subsystem driver implementing a V4L2 subdevice
 *
 * Copyright (C) 2016-2017 Leon Woestenberg <leon@sidebranch.com>
 * Copyright (C) 2016-2017 Xilinx, Inc.
 *
 * Authors: Leon Woestenberg <leon@sidebranch.com>
 *          Rohit Consul <rohitco@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* if both both DEBUG and DEBUG_TRACE are defined, trace_printk() is used */
#define DEBUG
//#define DEBUG_TRACE

#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/xilinx-v4l2-controls.h>
#include <linux/v4l2-dv-timings.h>
#include <linux/firmware.h>
#include <linux/clk.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-dv-timings.h>

#include <linux/phy/phy-vphy.h>

#include "xilinx-vip.h"

/* baseline driver includes */
#include "xilinx-hdmi-rx/xv_hdmirxss.h"
#include "xilinx-hdmi-rx/xil_printf.h"
#include "xilinx-hdmi-rx/xstatus.h"

/* select either trace or printk logging */
#ifdef DEBUG_TRACE
#define do_hdmi_dbg(format, ...) do { \
  trace_printk("xlnx-hdmi-rxss: " format, ##__VA_ARGS__); \
} while(0)
#else
#define do_hdmi_dbg(format, ...) do { \
  printk(KERN_DEBUG "xlnx-hdmi-rxss: " format, ##__VA_ARGS__); \
} while(0)
#endif

/* either enable or disable debugging */
#ifdef DEBUG
#  define hdmi_dbg(x...) do_hdmi_dbg(x)
#else
#  define hdmi_dbg(x...)
#endif

#define HDMI_MAX_LANES	4

#define EDID_BLOCKS_MAX 2

struct xhdmirx_device {
	struct device xvip;
	struct device *dev;
	void __iomem *iomem;
	struct clk *clk;
	/* interrupt number */
	int irq;
	bool teardown;
	struct phy *phy[HDMI_MAX_LANES];

	/* mutex to prevent concurrent access to this structure */
	struct mutex xhdmirx_mutex;

	/* protects concurrent access from interrupt context */
	spinlock_t irq_lock;

	/* schedule (future) work */
	struct workqueue_struct *work_queue;
	struct delayed_work delayed_work_enable_hotplug;

	struct v4l2_subdev subdev;

	/* V4L media output pad to construct the video pipeline */
	struct media_pad pad;

	/* https://linuxtv.org/downloads/v4l-dvb-apis/subdev.html#v4l2-mbus-framefmt */
	struct v4l2_mbus_framefmt detected_format;

	struct v4l2_dv_timings detected_timings;
#if 0
	struct v4l2_mbus_framefmt default_format;
#endif
	const struct xvip_video_format *vip_format;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *pattern;
	bool cable_connected;
	bool hdmi_stream;

	/* NI-DRU clock input */
	struct clk *clkp;

	u8 edid_user[EDID_BLOCKS_MAX*128];
	int edid_user_blocks;

	/* configuration for the baseline subsystem driver instance */
	XV_HdmiRxSs_Config config;
	/* bookkeeping for the baseline subsystem driver instance */
	XV_HdmiRxSs xv_hdmirxss;
	/* pointer to xvphy */
	XVphy *xvphy;
	/* sub core interrupt status registers */
	u32 IntrStatus[7];
};

// Xilinx EDID
static const u8 xilinx_edid[] = {
	0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x61, 0x98, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12,
	0x1F, 0x19, 0x01, 0x03, 0x80, 0x59, 0x32, 0x78, 0x0A, 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26,
	0x0F, 0x50, 0x54, 0x21, 0x08, 0x00, 0x71, 0x4F, 0x81, 0xC0, 0x81, 0x00, 0x81, 0x80, 0x95, 0x00,
	0xA9, 0xC0, 0xB3, 0x00, 0x01, 0x01, 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C,
	0x45, 0x00, 0x20, 0xC2, 0x31, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x58, 0x49, 0x4C,
	0x49, 0x4E, 0x58, 0x20, 0x48, 0x44, 0x4D, 0x49, 0x0A, 0x20, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0C,
	0x02, 0x03, 0x34, 0x71, 0x57, 0x61, 0x10, 0x1F, 0x04, 0x13, 0x05, 0x14, 0x20, 0x21, 0x22, 0x5D,
	0x5E, 0x5F, 0x60, 0x65, 0x66, 0x62, 0x63, 0x64, 0x07, 0x16, 0x03, 0x12, 0x23, 0x09, 0x07, 0x07,
	0x67, 0x03, 0x0C, 0x00, 0x10, 0x00, 0x78, 0x3C, 0xE3, 0x0F, 0x01, 0xE0, 0x67, 0xD8, 0x5D, 0xC4,
	0x01, 0x78, 0x80, 0x07, 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00,
	0x20, 0xC2, 0x31, 0x00, 0x00, 0x1E, 0x08, 0xE8, 0x00, 0x30, 0xF2, 0x70, 0x5A, 0x80, 0xB0, 0x58,
	0x8A, 0x00, 0x20, 0xC2, 0x31, 0x00, 0x00, 0x1E, 0x04, 0x74, 0x00, 0x30, 0xF2, 0x70, 0x5A, 0x80,
	0xB0, 0x58, 0x8A, 0x00, 0x20, 0x52, 0x31, 0x00, 0x00, 0x1E, 0x66, 0x21, 0x56, 0xAA, 0x51, 0x00,
	0x1E, 0x30, 0x46, 0x8F, 0x33, 0x00, 0x50, 0x1D, 0x74, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x2E
};

static inline struct xhdmirx_device *to_xhdmirx(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xhdmirx_device, subdev);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Video Operations
 */

static int xhdmirx_s_stream(struct v4l2_subdev *subdev, int enable)
{
	printk(KERN_INFO "xhdmirx_s_stream enable = %d\n", enable);
	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

 /* https://linuxtv.org/downloads/v4l-dvb-apis/vidioc-dv-timings-cap.html */

/* https://linuxtv.org/downloads/v4l-dvb-apis/vidioc-subdev-g-fmt.html */
static struct v4l2_mbus_framefmt *
__xhdmirx_get_pad_format_ptr(struct xhdmirx_device *xhdmirx,
		struct v4l2_subdev_pad_config *cfg,
		unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		hdmi_dbg("__xhdmirx_get_pad_format(): V4L2_SUBDEV_FORMAT_TRY\n");
		return v4l2_subdev_get_try_format(&xhdmirx->subdev, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		hdmi_dbg("__xhdmirx_get_pad_format(): V4L2_SUBDEV_FORMAT_ACTIVE\n");
		hdmi_dbg("detected_format->width = %u\n", xhdmirx->detected_format.width);
		return &xhdmirx->detected_format;
	default:
		return NULL;
	}
}

static int xhdmirx_get_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
{
	struct xhdmirx_device *xhdmirx = to_xhdmirx(subdev);
	hdmi_dbg("xhdmirx_get_format\n");

	if (fmt->pad > 0)
		return -EINVAL;

	/* copy either try or currently-active (i.e. detected) format to caller */
	fmt->format = *__xhdmirx_get_pad_format_ptr(xhdmirx, cfg, fmt->pad, fmt->which);

	hdmi_dbg("xhdmirx_get_format, height = %u\n", fmt->format.height);

	return 0;
}

/* we must modify the requested format to match what the hardware can provide */
static int xhdmirx_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
{
	struct xhdmirx_device *xhdmirx = to_xhdmirx(subdev);
	hdmi_dbg("xhdmirx_set_format\n");
	if (fmt->pad > 0)
		return -EINVAL;
	/* there is nothing we can take from the format requested by the caller, we must return the active format */
	fmt->format = xhdmirx->detected_format;
	return 0;
}

/* https://linuxtv.org/downloads/v4l-dvb-apis-new/media/kapi/v4l2-subdev.html#v4l2-sub-device-functions-and-data-structures
 * https://linuxtv.org/downloads/v4l-dvb-apis/vidioc-g-edid.html
 */
static int xhdmirx_get_edid(struct v4l2_subdev *subdev, struct v4l2_edid *edid) {
	struct xhdmirx_device *xhdmirx = to_xhdmirx(subdev);
	int do_copy = 1;
	if (edid->pad > 0)
		return -EINVAL;
	if (edid->start_block != 0)
		return -EINVAL;
	/* caller is only interested in the size of the EDID? */
	if ((edid->start_block == 0) && (edid->blocks == 0)) do_copy = 0;
	mutex_lock(&xhdmirx->xhdmirx_mutex);
	/* user EDID active? */
	if (xhdmirx->edid_user_blocks) {
		if (do_copy)
			memcpy(edid->edid, xhdmirx->edid_user, 128 * xhdmirx->edid_user_blocks);
		edid->blocks = xhdmirx->edid_user_blocks;
	} else {
		if (do_copy)
			memcpy(edid->edid, &xilinx_edid[0], sizeof(xilinx_edid));
		edid->blocks = sizeof(xilinx_edid) / 128;
	}
	mutex_unlock(&xhdmirx->xhdmirx_mutex);
	return 0;
}

static void xhdmirx_set_hpd(struct xhdmirx_device *xhdmirx, int enable)
{
	XV_HdmiRxSs *HdmiRxSsPtr;
	BUG_ON(!xhdmirx);
	HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;
	XV_HdmiRx_SetHpd(HdmiRxSsPtr->HdmiRxPtr, enable);
}

static void xhdmirx_delayed_work_enable_hotplug(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct xhdmirx_device *xhdmirx = container_of(dwork, struct xhdmirx_device,
						delayed_work_enable_hotplug);
#if 0
	struct v4l2_subdev *subdev = &xhdmirx->subdev;
	v4l2_dbg(2, debug, subdev, "%s: enable hotplug\n", __func__);
#endif
	xhdmirx_set_hpd(xhdmirx, 1 /*xhdmirx->edid.present*/);
}

static int xhdmirx_set_edid(struct v4l2_subdev *subdev, struct v4l2_edid *edid) {
	struct xhdmirx_device *xhdmirx = to_xhdmirx(subdev);
	XV_HdmiRxSs *HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;
	if (edid->pad > 0)
		return -EINVAL;
	if (edid->start_block != 0)
		return -EINVAL;
	if (edid->blocks > EDID_BLOCKS_MAX) {
		/* notify caller of how many EDID blocks this driver supports */
		edid->blocks = EDID_BLOCKS_MAX;
		return -E2BIG;
	}
	mutex_lock(&xhdmirx->xhdmirx_mutex);
	xhdmirx->edid_user_blocks = edid->blocks;

	/* Disable hotplug and I2C access to EDID RAM from DDC port */
	cancel_delayed_work_sync(&xhdmirx->delayed_work_enable_hotplug);
	xhdmirx_set_hpd(xhdmirx, 0);

	if (edid->blocks) {
		memcpy(xhdmirx->edid_user, edid->edid, 128 * edid->blocks);
		XV_HdmiRxSs_LoadEdid(HdmiRxSsPtr, (u8 *)&xhdmirx->edid_user, 128 * xhdmirx->edid_user_blocks);
		/* enable hotplug after 100 ms */
		queue_delayed_work(xhdmirx->work_queue,
				&xhdmirx->delayed_work_enable_hotplug, HZ / 10);
		}
	mutex_unlock(&xhdmirx->xhdmirx_mutex);
	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

static int xhdmirx_enum_frame_size(struct v4l2_subdev *subdev,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad > 0)
		return -EINVAL;
#if 0
	/* @TODO we want to provide all HDMI frame sizes, not that they are
	 * settable... */
	printk(KERN_INFO "xhdmirx_enum_frame_size\n");
#endif
	return 0;
}

#if 0
static const struct v4l2_dv_timings_cap xhdmirx_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	/* keep this initialization for compatibility with GCC < 4.4.6 */
	.reserved = { 0 },
	/* Pixel clock from REF_01 p. 20. Min/max height/width are unknown */
	V4L2_INIT_BT_TIMINGS(1, 10000, 1, 10000, 0, 165000000,
			V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT |
			V4L2_DV_BT_STD_GTF | V4L2_DV_BT_STD_CVT,
			V4L2_DV_BT_CAP_PROGRESSIVE |
			V4L2_DV_BT_CAP_REDUCED_BLANKING |
			V4L2_DV_BT_CAP_CUSTOM)
};

/* https://en.wikipedia.org/wiki/Extended_Display_Identification_Data#CEA-861-F */
static const struct v4l2_dv_timings xhdmirx_timings[] = {
	V4L2_DV_BT_CEA_1920X1080P6,0
};

static int xhdmirx_enum_dv_timings(struct v4l2_subdev *sd,
				    struct v4l2_enum_dv_timings *timings)
{
	if (timings->pad != 0)
		return -EINVAL;

	return v4l2_enum_dv_timings_cap(timings,
			&xhdmirx_timings_cap, NULL, NULL);
}
#endif

static int xhdmirx_dv_timings_cap(struct v4l2_subdev *subdev,
		struct v4l2_dv_timings_cap *cap)
{
	if (cap->pad != 0)
		return -EINVAL;
#if 0
	*cap = xhdmirx_timings_cap;
#else
	cap->type = V4L2_DV_BT_656_1120;
	cap->bt.max_width = 4096;
	cap->bt.max_height = 2160;
	cap->bt.min_pixelclock = 25000000;
	cap->bt.max_pixelclock = 297000000;
	cap->bt.standards = V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT |
			 V4L2_DV_BT_STD_GTF | V4L2_DV_BT_STD_CVT;
	cap->bt.capabilities = V4L2_DV_BT_CAP_PROGRESSIVE |
		V4L2_DV_BT_CAP_REDUCED_BLANKING | V4L2_DV_BT_CAP_CUSTOM;
#endif
	return 0;
}

#if 0 /* replaced by full timing - might be needed again later for VIC mapping */
/* Supported CEA and DMT timings */
static const struct v4l2_dv_timings supported_timings[] = {
	V4L2_DV_BT_CEA_1280X720P50,
	V4L2_DV_BT_CEA_1920X1080P50,
	V4L2_DV_BT_CEA_3840X2160P50,
};
#endif

static int xhdmirx_query_dv_timings(struct v4l2_subdev *subdev,
			struct v4l2_dv_timings *timings)
{
	struct xhdmirx_device *xhdmirx = to_xhdmirx(subdev);
	struct v4l2_bt_timings *bt = &timings->bt;

	if (!timings)
		return -EINVAL;

	if (!xhdmirx->hdmi_stream)
		return -ENOLINK;

	memset(timings, 0, sizeof(struct v4l2_dv_timings));

	*timings = xhdmirx->detected_timings;
	return 0;
}

/* struct v4l2_subdev_internal_ops.open */
static int xhdmirx_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct xhdmirx_device *xhdmirx = to_xhdmirx(subdev);
	struct v4l2_mbus_framefmt *format;
	(void)xhdmirx;
	(void)format;
	hdmi_dbg("xhdmirx_open\n");

#if 0
	format = v4l2_subdev_get_try_format(subdev, fh->pad, 0);
	*format = xhdmirx->default_format;
#endif
	return 0;
}

/* struct v4l2_subdev_internal_ops.close */
static int xhdmirx_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	hdmi_dbg("xhdmirx_close\n");
	return 0;
}

static int xhdmirx_s_ctrl(struct v4l2_ctrl *ctrl)
{
	hdmi_dbg("xhdmirx_s_ctrl\n");
	return 0;
}

static const struct v4l2_ctrl_ops xhdmirx_ctrl_ops = {
	.s_ctrl	= xhdmirx_s_ctrl,
};

static struct v4l2_subdev_core_ops xhdmirx_core_ops = {
};

static struct v4l2_subdev_video_ops xhdmirx_video_ops = {
	.s_stream = xhdmirx_s_stream,
	.query_dv_timings = xhdmirx_query_dv_timings,
};

/* If the subdev driver intends to process video and integrate with the media framework,
 * it must implement format related functionality using v4l2_subdev_pad_ops instead of
 * v4l2_subdev_video_ops. */
static struct v4l2_subdev_pad_ops xhdmirx_pad_ops = {
	.enum_mbus_code		= xvip_enum_mbus_code,
	.enum_frame_size	= xhdmirx_enum_frame_size,
	.get_fmt			= xhdmirx_get_format,
	.set_fmt			= xhdmirx_set_format,
	.get_edid			= xhdmirx_get_edid,
	.set_edid			= xhdmirx_set_edid,
#if 0 /* @TODO not sure if we need this */
	.enum_dv_timings	= xhdmirx_enum_dv_timings,
#endif
	.dv_timings_cap		= xhdmirx_dv_timings_cap,
};

static struct v4l2_subdev_ops xhdmirx_ops = {
	.core   = &xhdmirx_core_ops,
	.video  = &xhdmirx_video_ops,
	.pad    = &xhdmirx_pad_ops,
};

static const struct v4l2_subdev_internal_ops xhdmirx_internal_ops = {
	.open	= xhdmirx_open,
	.close	= xhdmirx_close,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xhdmirx_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * Power Management
 */

static int __maybe_unused xhdmirx_pm_suspend(struct device *dev)
{
	struct xhdmirx_device *xhdmirx = dev_get_drvdata(dev);
	//xvip_suspend(&xhdmirx->xvip);
	return 0;
}

static int __maybe_unused xhdmirx_pm_resume(struct device *dev)
{
	struct xhdmirx_device *xhdmirx = dev_get_drvdata(dev);
	//xvip_resume(&xhdmirx->xvip);
	return 0;
}

void HdmiRx_PioIntrHandler(XV_HdmiRx *InstancePtr);
void HdmiRx_TmrIntrHandler(XV_HdmiRx *InstancePtr);
void HdmiRx_VtdIntrHandler(XV_HdmiRx *InstancePtr);
void HdmiRx_DdcIntrHandler(XV_HdmiRx *InstancePtr);
void HdmiRx_AuxIntrHandler(XV_HdmiRx *InstancePtr);
void HdmiRx_AudIntrHandler(XV_HdmiRx *InstancePtr);
void HdmiRx_LinkStatusIntrHandler(XV_HdmiRx *InstancePtr);

void XV_HdmiRxSs_IntrEnable(XV_HdmiRxSs *HdmiRxSsPtr)
{
	XV_HdmiRx_PioIntrEnable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_TmrIntrEnable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_VtdIntrEnable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_DdcIntrEnable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_AuxIntrEnable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_AudioIntrEnable(HdmiRxSsPtr->HdmiRxPtr);
	//XV_HdmiRx_LinkIntrEnable(HdmiRxSsPtr->HdmiRxPtr);
}

void XV_HdmiRxSs_IntrDisable(XV_HdmiRxSs *HdmiRxSsPtr)
{
	XV_HdmiRx_PioIntrDisable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_TmrIntrDisable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_VtdIntrDisable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_DdcIntrDisable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_AuxIntrDisable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_AudioIntrDisable(HdmiRxSsPtr->HdmiRxPtr);
	XV_HdmiRx_LinkIntrDisable(HdmiRxSsPtr->HdmiRxPtr);
}

static irqreturn_t hdmirx_irq_handler(int irq, void *dev_id)
{
	struct xhdmirx_device *xhdmirx;
	XV_HdmiRxSs *HdmiRxSsPtr;
	unsigned long flags;
	BUG_ON(!dev_id);
	xhdmirx = (struct xhdmirx_device *)dev_id;
	//printk(KERN_INFO "hdmirx_irq_handler()\n");
	if (!xhdmirx) {
		static int fault_count = 0;
		fault_count++;
		if (fault_count)
		dev_err(xhdmirx->dev, "irq_handler: !dev_id\n");
		spin_lock_irqsave(&xhdmirx->irq_lock, flags);
		/* mask interrupt request */
		XV_HdmiRxSs_IntrDisable(HdmiRxSsPtr);
		spin_unlock_irqrestore(&xhdmirx->irq_lock, flags);
		return IRQ_HANDLED;
	}
	HdmiRxSsPtr = (XV_HdmiRxSs *)&xhdmirx->xv_hdmirxss;
	BUG_ON(!HdmiRxSsPtr->HdmiRxPtr);

	if (HdmiRxSsPtr->IsReady != XIL_COMPONENT_IS_READY) {
		printk(KERN_INFO "hdmirx_irq_handler(): HDMI RX SS is not initialized?!\n");
	}

	/* read status registers */
	xhdmirx->IntrStatus[0] = XV_HdmiRx_ReadReg(HdmiRxSsPtr->Config.BaseAddress, (XV_HDMIRX_PIO_STA_OFFSET)) & (XV_HDMIRX_PIO_STA_IRQ_MASK);
	xhdmirx->IntrStatus[1] = XV_HdmiRx_ReadReg(HdmiRxSsPtr->Config.BaseAddress, (XV_HDMIRX_TMR_STA_OFFSET)) & (XV_HDMIRX_TMR_STA_IRQ_MASK);
	xhdmirx->IntrStatus[2] = XV_HdmiRx_ReadReg(HdmiRxSsPtr->Config.BaseAddress, (XV_HDMIRX_VTD_STA_OFFSET)) & (XV_HDMIRX_VTD_STA_IRQ_MASK);
	xhdmirx->IntrStatus[3] = XV_HdmiRx_ReadReg(HdmiRxSsPtr->Config.BaseAddress, (XV_HDMIRX_DDC_STA_OFFSET)) & (XV_HDMIRX_DDC_STA_IRQ_MASK);
	xhdmirx->IntrStatus[4] = XV_HdmiRx_ReadReg(HdmiRxSsPtr->Config.BaseAddress, (XV_HDMIRX_AUX_STA_OFFSET)) & (XV_HDMIRX_AUX_STA_IRQ_MASK);
	xhdmirx->IntrStatus[5] = XV_HdmiRx_ReadReg(HdmiRxSsPtr->Config.BaseAddress, (XV_HDMIRX_AUD_STA_OFFSET)) & (XV_HDMIRX_AUD_STA_IRQ_MASK);
	xhdmirx->IntrStatus[6] = XV_HdmiRx_ReadReg(HdmiRxSsPtr->Config.BaseAddress, (XV_HDMIRX_LNKSTA_STA_OFFSET)) & (XV_HDMIRX_LNKSTA_STA_IRQ_MASK);

	spin_lock_irqsave(&xhdmirx->irq_lock, flags);
	/* mask interrupt request */
	XV_HdmiRxSs_IntrDisable(HdmiRxSsPtr);
	spin_unlock_irqrestore(&xhdmirx->irq_lock, flags);

	/* call bottom-half */
	return IRQ_WAKE_THREAD;
}

static irqreturn_t hdmirx_irq_thread(int irq, void *dev_id)
{
	static int irq_count = 0;
	struct xhdmirx_device *xhdmirx;
	XV_HdmiRxSs *HdmiRxSsPtr;
	unsigned long flags;
	int i;
	char which[8] = "01234567";
	int which_mask = 0;

	BUG_ON(!dev_id);
	xhdmirx = (struct xhdmirx_device *)dev_id;
	if (!xhdmirx) {
		printk(KERN_INFO "irq_thread: !dev_id\n");
		return IRQ_HANDLED;
	}
	if (xhdmirx->teardown) {
		printk(KERN_INFO "irq_thread: teardown\n");
		return IRQ_HANDLED;
	}
	HdmiRxSsPtr = (XV_HdmiRxSs *)&xhdmirx->xv_hdmirxss;
	BUG_ON(!HdmiRxSsPtr->HdmiRxPtr);

	mutex_lock(&xhdmirx->xhdmirx_mutex);
	/* call baremetal interrupt handler, this in turn will
	 * call the registed callbacks functions */

	for (i = 0; i < 7; i++) {
		which[i] = xhdmirx->IntrStatus[i]? '0' + i: '.';
		which_mask |= (xhdmirx->IntrStatus[i]? 1: 0) << i;
	}
	which[7] = 0;

#if 1
	if (xhdmirx->IntrStatus[0]) HdmiRx_PioIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	if (xhdmirx->IntrStatus[1]) HdmiRx_TmrIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	if (xhdmirx->IntrStatus[2]) HdmiRx_VtdIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	if (xhdmirx->IntrStatus[3]) HdmiRx_DdcIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	if (xhdmirx->IntrStatus[4]) HdmiRx_AuxIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	if (xhdmirx->IntrStatus[5]) HdmiRx_AudIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	if (xhdmirx->IntrStatus[6]) HdmiRx_LinkStatusIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
#else
	HdmiRx_PioIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	HdmiRx_TmrIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	HdmiRx_VtdIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	HdmiRx_DdcIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	HdmiRx_AuxIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	HdmiRx_AudIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
	HdmiRx_LinkStatusIntrHandler(HdmiRxSsPtr->HdmiRxPtr);
#endif
	//printk(KERN_INFO "hdmirx_irq_thread() %s 0x%08x\n", which, (int)which_mask);

	mutex_unlock(&xhdmirx->xhdmirx_mutex);
	spin_lock_irqsave(&xhdmirx->irq_lock, flags);
	/* unmask interrupt request */
	XV_HdmiRxSs_IntrEnable(HdmiRxSsPtr);
	spin_unlock_irqrestore(&xhdmirx->irq_lock, flags);
	//printk(KERN_INFO "hdmirx_irq_thread() %s 0x%08x done\n", which, (int)which_mask);

	return IRQ_HANDLED;
}

/* callbacks from HDMI RX SS interrupt handler
 * these are called with the xhdmirx->mutex locked and the xvphy_mutex non-locked
 * to prevent mutex deadlock, always lock the xhdmirx first, then the xvphy mutex */
static void RxConnectCallback(void *CallbackRef)
{
	struct xhdmirx_device *xhdmirx = (struct xhdmirx_device *)CallbackRef;
	XV_HdmiRxSs *HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;
	XVphy *VphyPtr = xhdmirx->xvphy;
	BUG_ON(!xhdmirx);
	BUG_ON(!HdmiRxSsPtr);
	if (!xhdmirx || !HdmiRxSsPtr || !VphyPtr) return;

	xhdmirx->cable_connected = !!HdmiRxSsPtr->IsStreamConnected;
	hdmi_dbg("RxConnectCallback()\n");
	hdmi_dbg("cable is %sconnected.\n", xhdmirx->cable_connected? "": "dis");

	xvphy_mutex_lock(xhdmirx->phy[0]);
	/* RX cable is connected? */
	if (HdmiRxSsPtr->IsStreamConnected)
	{
		XVphy_IBufDsEnable(VphyPtr, 0, XVPHY_DIR_RX, (TRUE));
	}
	else
	{
		/* clear GT RX TMDS clock ratio */
		VphyPtr->HdmiRxTmdsClockRatio = 0;
		XVphy_IBufDsEnable(VphyPtr, 0, XVPHY_DIR_RX, (FALSE));
	}
	xvphy_mutex_unlock(xhdmirx->phy[0]);
}

static void RxAuxCallback(void *CallbackRef)
{
	struct xhdmirx_device *xhdmirx = (struct xhdmirx_device *)CallbackRef;
	XV_HdmiRxSs *HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;
	u8 AuxBuffer[36];
	BUG_ON(!xhdmirx);
	BUG_ON(!HdmiRxSsPtr);
	if (!xhdmirx || !HdmiRxSsPtr) return;
	// Copy the RX packet into the local buffer
	memcpy(AuxBuffer, XV_HdmiRxSs_GetAuxiliary(HdmiRxSsPtr), sizeof(AuxBuffer));
}

static void RxAudCallback(void *CallbackRef)
{
	struct xhdmirx_device *xhdmirx = (struct xhdmirx_device *)CallbackRef;
	XV_HdmiRxSs *HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;\
	BUG_ON(!xhdmirx);
	BUG_ON(!HdmiRxSsPtr);
	if (!xhdmirx || !HdmiRxSsPtr) return;
	hdmi_dbg("RxAudCallback()\n");
	(void)HdmiRxSsPtr;
}

static void RxLnkStaCallback(void *CallbackRef)
{
	struct xhdmirx_device *xhdmirx = (struct xhdmirx_device *)CallbackRef;
	XV_HdmiRxSs *HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;
	BUG_ON(!xhdmirx);
	BUG_ON(!HdmiRxSsPtr);
	if (!xhdmirx || !HdmiRxSsPtr) return;
	(void)HdmiRxSsPtr;
}

static void RxStreamDownCallback(void *CallbackRef)
{
	struct xhdmirx_device *xhdmirx = (struct xhdmirx_device *)CallbackRef;
	XV_HdmiRxSs *HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;
	BUG_ON(!xhdmirx);
	BUG_ON(!HdmiRxSsPtr);
	if (!xhdmirx || !HdmiRxSsPtr) return;
	(void)HdmiRxSsPtr;
	hdmi_dbg("RxStreamDownCallback()\n");
	xhdmirx->hdmi_stream = 0;
}

static void RxStreamInitCallback(void *CallbackRef)
{
	struct xhdmirx_device *xhdmirx = (struct xhdmirx_device *)CallbackRef;
	XV_HdmiRxSs *HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;
	XVphy *VphyPtr = xhdmirx->xvphy;
	XVidC_VideoStream *HdmiRxSsVidStreamPtr;
	u32 Status;
	BUG_ON(!xhdmirx);
	BUG_ON(!HdmiRxSsPtr);
	BUG_ON(!VphyPtr);
	if (!xhdmirx || !HdmiRxSsPtr || !VphyPtr) return;
	hdmi_dbg("RxStreamInitCallback\r\n");
	// Calculate RX MMCM parameters
	// In the application the YUV422 colordepth is 12 bits
	// However the HDMI transports YUV422 in 8 bits.
	// Therefore force the colordepth to 8 bits when the colorspace is YUV422

	HdmiRxSsVidStreamPtr = XV_HdmiRxSs_GetVideoStream(HdmiRxSsPtr);

	xvphy_mutex_lock(xhdmirx->phy[0]);

	if (HdmiRxSsVidStreamPtr->ColorFormatId == XVIDC_CSF_YCRCB_422) {
		Status = XVphy_HdmiCfgCalcMmcmParam(VphyPtr, 0, XVPHY_CHANNEL_ID_CH1,
				XVPHY_DIR_RX,
				HdmiRxSsVidStreamPtr->PixPerClk,
				XVIDC_BPC_8);
	// Other colorspaces
	} else {
		Status = XVphy_HdmiCfgCalcMmcmParam(VphyPtr, 0, XVPHY_CHANNEL_ID_CH1,
				XVPHY_DIR_RX,
				HdmiRxSsVidStreamPtr->PixPerClk,
				HdmiRxSsVidStreamPtr->ColorDepth);
	}

	if (Status == XST_FAILURE) {
		xvphy_mutex_unlock(xhdmirx->phy[0]);
		return;
	}

	// Enable and configure RX MMCM
	XVphy_MmcmStart(VphyPtr, 0, XVPHY_DIR_RX);
	xvphy_mutex_unlock(xhdmirx->phy[0]);
}

/* @TODO Once this upstream V4L2 patch lands, consider VIC support: https://patchwork.linuxtv.org/patch/37137/ */
static void RxStreamUpCallback(void *CallbackRef)
{
	struct xhdmirx_device *xhdmirx = (struct xhdmirx_device *)CallbackRef;
	XV_HdmiRxSs *HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;
	XVidC_VideoStream *Stream;
	BUG_ON(!xhdmirx);
	BUG_ON(!HdmiRxSsPtr);
	BUG_ON(!HdmiRxSsPtr->HdmiRxPtr);
	if (!xhdmirx || !HdmiRxSsPtr || !HdmiRxSsPtr->HdmiRxPtr) return;
	hdmi_dbg("RxStreamUpCallback((; stream is up.\n");
	Stream = &HdmiRxSsPtr->HdmiRxPtr->Stream.Video;
#if 1
	XVidC_ReportStreamInfo(Stream);
#endif
#if 1
	XV_HdmiRx_DebugInfo(HdmiRxSsPtr->HdmiRxPtr);
#endif
	/* http://lxr.free-electrons.com/source/include/uapi/linux/videodev2.h#L1229 */
	xhdmirx->detected_format.width = Stream->Timing.HActive;
	xhdmirx->detected_format.height = Stream->Timing.VActive;

	/* @TODO correct mapping, see https://linuxtv.org/downloads/v4l-dvb-apis/field-order.html#v4l2-field */
	xhdmirx->detected_format.field = Stream->IsInterlaced? V4L2_FIELD_INTERLACED: V4L2_FIELD_NONE;
	/* https://linuxtv.org/downloads/v4l-dvb-apis/ch02s05.html#v4l2-colorspace */
	if (Stream->ColorFormatId == XVIDC_CSF_RGB) {
		hdmi_dbg("xhdmirx->detected_format.colorspace = V4L2_COLORSPACE_SRGB\n");
		xhdmirx->detected_format.colorspace = V4L2_COLORSPACE_SRGB;
	} else {
		hdmi_dbg("xhdmirx->detected_format.colorspace = V4L2_COLORSPACE_REC709\n");
		xhdmirx->detected_format.colorspace = V4L2_COLORSPACE_REC709;
	}

	/* https://linuxtv.org/downloads/v4l-dvb-apis/subdev.html#v4l2-mbus-framefmt */
	/* see UG934 page 8 */
	if (Stream->ColorFormatId == XVIDC_CSF_RGB) {
		/* red blue green */
		xhdmirx->detected_format.code = MEDIA_BUS_FMT_RBG888_1X24;
		hdmi_dbg("XVIDC_CSF_RGB -> MEDIA_BUS_FMT_RBG888_1X24\n");
	} else if (Stream->ColorFormatId == XVIDC_CSF_YCRCB_444) {
		xhdmirx->detected_format.code = MEDIA_BUS_FMT_VUY8_1X24;
		hdmi_dbg("XVIDC_CSF_YCRCB_444 -> MEDIA_BUS_FMT_VUY8_1X24\n");
	} else if (Stream->ColorFormatId == XVIDC_CSF_YCRCB_422) {
		xhdmirx->detected_format.code = MEDIA_BUS_FMT_UYVY8_1X16;
		hdmi_dbg("XVIDC_CSF_YCRCB_422 -> MEDIA_BUS_FMT_UYVY8_1X16\n");
	} else if (Stream->ColorFormatId == XVIDC_CSF_YCRCB_420) {
		/* similar mapping as 4:2:2 w/ omitted chroma every other line */
		xhdmirx->detected_format.code = MEDIA_BUS_FMT_UYVY8_1X16;
		hdmi_dbg("XVIDC_CSF_YCRCB_420 -> MEDIA_BUS_FMT_UYVY8_1X16\n");
	}

	xhdmirx->detected_format.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	xhdmirx->detected_format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	xhdmirx->detected_format.quantization = V4L2_QUANTIZATION_DEFAULT;

	/* map to v4l2_dv_timings */
	xhdmirx->detected_timings.type =  V4L2_DV_BT_656_1120;

	/* Read Active Pixels */
	xhdmirx->detected_timings.bt.width = Stream->Timing.HActive;
	/* Active lines field 1 */
	xhdmirx->detected_timings.bt.height = Stream->Timing.VActive;
	/* Interlaced */
	xhdmirx->detected_timings.bt.interlaced = !!Stream->IsInterlaced;
	xhdmirx->detected_timings.bt.polarities =
	/* Vsync polarity, Positive == 1 */
		(Stream->Timing.VSyncPolarity? V4L2_DV_VSYNC_POS_POL: 0) |
	/* Hsync polarity, Positive == 1 */
		(Stream->Timing.HSyncPolarity? V4L2_DV_HSYNC_POS_POL: 0);
	xhdmirx->detected_timings.bt.pixelclock =
#if 1
		XVidC_GetPixelClockHzByHVFr(
			Stream->Timing.HTotal,
			Stream->Timing.F0PVTotal,
			Stream->FrameRate);
#else
		HdmiRxSsPtr->HdmiRxPtr->Stream.PixelClk;
#endif
	hdmi_dbg("HdmiRxSsPtr->HdmiRxPtr->Stream.PixelClk = %d\n", HdmiRxSsPtr->HdmiRxPtr->Stream.PixelClk);
	/* Read HFront Porch */
	xhdmirx->detected_timings.bt.hfrontporch = Stream->Timing.HFrontPorch;
	/* Read Hsync Width */
	xhdmirx->detected_timings.bt.hsync = Stream->Timing.HSyncWidth;
	/* Read HBack Porch */
	xhdmirx->detected_timings.bt.hbackporch = Stream->Timing.HBackPorch;
	/* Read VFront Porch field 1*/
	xhdmirx->detected_timings.bt.vfrontporch = Stream->Timing.F0PVFrontPorch;
	/* Read VSync Width field 1*/
	xhdmirx->detected_timings.bt.vsync = Stream->Timing.F0PVSyncWidth;
	/* Read VBack Porch field 1 */
	xhdmirx->detected_timings.bt.vbackporch = Stream->Timing.F0PVBackPorch;
	/* Read VFront Porch field 2*/
	xhdmirx->detected_timings.bt.il_vfrontporch = Stream->Timing.F1VFrontPorch;
	/* Read VSync Width field 2*/
	xhdmirx->detected_timings.bt.il_vsync = Stream->Timing.F1VSyncWidth;
	/* Read VBack Porch field 2 */
	xhdmirx->detected_timings.bt.il_vbackporch = Stream->Timing.F1VBackPorch;
	xhdmirx->detected_timings.bt.standards = V4L2_DV_BT_STD_CEA861;
	xhdmirx->detected_timings.bt.flags = V4L2_DV_FL_IS_CE_VIDEO;

	/* Read Total Pixels */
	(void)Stream->Timing.HTotal;
	/* Total lines field 1 */
	(void)Stream->Timing.F0PVTotal;
	/* Total lines field 2 */
	(void)Stream->Timing.F1VTotal;

	// Calculate and set the frame rate field
	(void)Stream->FrameRate;
	// Lookup the video mode id (Xilinx), XVIDC_VM_CUSTOM
	(void)Stream->VmId;

	xhdmirx->hdmi_stream = 1;
	v4l2_print_dv_timings("xilinx-hdmi-rx", "", & xhdmirx->detected_timings, 1);
}

/* Called from non-interrupt context with xvphy mutex locked
 */
static void VphyHdmiRxInitCallback(void *CallbackRef)
{
	struct xhdmirx_device *xhdmirx = (struct xhdmirx_device *)CallbackRef;
	XV_HdmiRxSs *HdmiRxSsPtr = &xhdmirx->xv_hdmirxss;
	XVphy *VphyPtr = xhdmirx->xvphy;
	BUG_ON(!xhdmirx);
	BUG_ON(!VphyPtr);
	BUG_ON(!xhdmirx->phy[0]);
	if (!xhdmirx || !VphyPtr) return;
	hdmi_dbg("VphyHdmiRxInitCallback()\n");

	/* a pair of mutexes must be locked in fixed order to prevent deadlock,
	 * and the order is RX SS then XVPHY, so first unlock XVPHY then lock both */
	xvphy_mutex_unlock(xhdmirx->phy[0]);
	mutex_lock(&xhdmirx->xhdmirx_mutex);
	xvphy_mutex_lock(xhdmirx->phy[0]);

	XV_HdmiRxSs_RefClockChangeInit(HdmiRxSsPtr);
	/* @NOTE maybe implement xvphy_set_hdmirx_tmds_clockratio(); */
	VphyPtr->HdmiRxTmdsClockRatio = HdmiRxSsPtr->TMDSClockRatio;
	/* unlock RX SS but keep XVPHY locked */
	mutex_unlock(&xhdmirx->xhdmirx_mutex);
}

/* Called from non-interrupt context with xvphy mutex locked
 */
static void VphyHdmiRxReadyCallback(void *CallbackRef)
{
	struct xhdmirx_device *xhdmirx = (struct xhdmirx_device *)CallbackRef;
	XVphy *VphyPtr = xhdmirx->xvphy;
	XVphy_PllType RxPllType;
	BUG_ON(!xhdmirx);
	BUG_ON(!VphyPtr);
	BUG_ON(!xhdmirx->phy[0]);
	if (!xhdmirx || !VphyPtr) return;
	hdmi_dbg("VphyHdmiRxReadyCallback()\n");

#if 0
	// Enable pass-through
#if(LOOPBACK_MODE_EN != 1)
	IsPassThrough = (TRUE);
#endif
#endif

	/* a pair of mutexes must be locked in fixed order to prevent deadlock,
	 * and the order is RX SS then XVPHY, so first unlock XVPHY then lock both */
	xvphy_mutex_unlock(xhdmirx->phy[0]);
	mutex_lock(&xhdmirx->xhdmirx_mutex);
	xvphy_mutex_lock(xhdmirx->phy[0]);
	/* @NOTE too much peeking around in Vphy */
	RxPllType = XVphy_GetPllType(VphyPtr, 0, XVPHY_DIR_RX,
		XVPHY_CHANNEL_ID_CH1);
	if (!(RxPllType == XVPHY_PLL_TYPE_CPLL)) {
		XV_HdmiRxSs_SetStream(&xhdmirx->xv_hdmirxss, VphyPtr->HdmiRxRefClkHz,
				(XVphy_GetLineRateHz(VphyPtr, 0, XVPHY_CHANNEL_ID_CMN0)/1000000));
	}
	else {
		XV_HdmiRxSs_SetStream(&xhdmirx->xv_hdmirxss, VphyPtr->HdmiRxRefClkHz,
				(XVphy_GetLineRateHz(VphyPtr, 0, XVPHY_CHANNEL_ID_CH1)/1000000));
	}
	mutex_unlock(&xhdmirx->xhdmirx_mutex);
}

static XV_HdmiRxSs_Config config = {
	.DeviceId = 0,
	.BaseAddress = 0,
	.HighAddress = 0,
	.Ppc = 2,
	.MaxBitsPerPixel = 8,

	.HdcpTimer = {
		.IsPresent = 0,
		.DeviceId = 255,
		.AbsAddr = 0,
	},
	.Hdcp14 = {
		.IsPresent = 0,
		.DeviceId = 0,
		.AbsAddr = 0,
	},
	.Hdcp22 = {
		.IsPresent = 0,
		.DeviceId  = 0,
		.AbsAddr = 0,
	},
	.HdmiRx = {
		.IsPresent = 1,
		.DeviceId = 0,
		.AbsAddr = 0,
	}
};

static XV_HdmiRx_Config XV_HdmiRx_FixedConfig =
{
	0,
	0
};

XV_HdmiRx_Config *XV_HdmiRx_LookupConfig(u16 DeviceId)
{
	return (XV_HdmiRx_Config *)&XV_HdmiRx_FixedConfig;
}

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xhdmirx_parse_of(struct xhdmirx_device *xhdmirx, XV_HdmiRxSs_Config *config)
{
	struct device *dev = xhdmirx->dev;
	struct device_node *node = dev->of_node;
	int rc;
	u32 val;

	rc = of_property_read_u32(node, "xlnx,input-pixels-per-clock", &val);
	if (rc < 0)
		goto error_dt;
	config->Ppc = val;

	rc = of_property_read_u32(node, "xlnx,max-bits-per-component", &val);
	if (rc < 0)
		goto error_dt;
	config->MaxBitsPerPixel = val;
	
	rc = of_property_read_u32(node, "xlnx,hdmi-rx-offset", &val);	 
 	if (rc == 0) { 
 		config->HdmiRx.DeviceId = 0; 
 		config->HdmiRx.IsPresent = 1; 
 		config->HdmiRx.AbsAddr = val; 
 	} else { 
 		goto error_dt; 
 	} 

	return 0;

error_dt:
		dev_err(xhdmirx->dev, "Error parsing device tree");
		return rc;
}

static int xhdmirx_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct xhdmirx_device *xhdmirx;
	int ret;
	unsigned int index;
	struct resource *res;

	const struct firmware *fw_edid;
	const char *fw_edid_name = "xilinx/xilinx-hdmi-rx-edid.bin";
	unsigned long flags;
	unsigned long dru_clk_rate;

	XV_HdmiRxSs *HdmiRxSsPtr;
	u32 Status;

	hdmi_dbg("hdmi-rx probed\n");
	/* allocate zeroed HDMI RX device structure */
	xhdmirx = devm_kzalloc(&pdev->dev, sizeof(*xhdmirx), GFP_KERNEL);
	if (!xhdmirx)
		return -ENOMEM;
	/* store pointer of the real device inside platform device */
	xhdmirx->dev = &pdev->dev;

	/* mutex that protects against concurrent access */
	mutex_init(&xhdmirx->xhdmirx_mutex);
	spin_lock_init(&xhdmirx->irq_lock);
	/* work queues */
	xhdmirx->work_queue = create_singlethread_workqueue("xilinx-hdmi-rx");
	if (!xhdmirx->work_queue) {
		dev_info(xhdmirx->dev, "Could not create work queue\n");
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&xhdmirx->delayed_work_enable_hotplug,
		xhdmirx_delayed_work_enable_hotplug);


	hdmi_dbg("xhdmirx_probe DT parse start\n");
	/* parse open firmware device tree data */
	ret = xhdmirx_parse_of(xhdmirx, &config);
	if (ret < 0)
		return ret;
	hdmi_dbg("xhdmirx_probe DT parse done\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	xhdmirx->iomem = devm_ioremap_resource(xhdmirx->dev, res);
	if (IS_ERR(xhdmirx->iomem)) {
		ret = PTR_ERR(xhdmirx->iomem);
		goto error_resource;
	}
	config.BaseAddress = (uintptr_t)xhdmirx->iomem;
	config.HighAddress = config.BaseAddress + resource_size(res);

	/* Compute AbsAddress for sub-cores - Add subsystem base address to sub-core offset */
	config.HdmiRx.AbsAddr += config.BaseAddress;
	if(config.HdmiRx.AbsAddr > config.HighAddress) {
	   hdmi_dbg("hdmirx sub-core address out-of range\n");
	   return -EFAULT;
	}
	
	/* video streaming bus clock */
	xhdmirx->clk = devm_clk_get(xhdmirx->dev, NULL);
	if (IS_ERR(xhdmirx->clk))
		return PTR_ERR(xhdmirx->clk);

	clk_prepare_enable(xhdmirx->clk);
#if 0
	ret = xvip_init_resources(&xhdmirx->xvip);
	if (ret < 0)
		return ret;
#endif
	/* get irq */
	xhdmirx->irq = platform_get_irq(pdev, 0);
	if (xhdmirx->irq <= 0) {
		dev_err(&pdev->dev, "platform_get_irq() failed\n");
		return xhdmirx->irq;
	}

	if (!xhdmirx->clkp) {
		xhdmirx->clkp = devm_clk_get(&pdev->dev, "dru-clk");
		if (IS_ERR(xhdmirx->clkp)) {
			ret = PTR_ERR(xhdmirx->clkp);
			xhdmirx->clkp = NULL;
			if (ret != -EPROBE_DEFER)
				dev_err(&pdev->dev, "failed to get the dru-clk.\n");
			hdmi_dbg("dru-clk not ready - probe deferred\n");
			return ret;
		}
	}

	ret = clk_prepare_enable(xhdmirx->clkp);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable dru-clk\n");
		return ret;
	}
	dru_clk_rate = clk_get_rate(xhdmirx->clkp);
	hdmi_dbg("dru-clk rate = %lu\n", dru_clk_rate);

	/* @TODO compiler issue, or am I just staring blind? if change to "index < 3", the case for index == 3 is also run.
	 * Possibly a compiler issue with aggressive loop unrolling optimization.
	 * We only need to "init" one vphy lane, as the VPHY will initialize all other lanes, so this is not an issue now.
	 * -- Leon Woestenberg <leon@sidebranch.com>
	 */
	for (index = 0; index < 1; index++) {
		hdmi_dbg("entering loop with index = %d\n",index);
		char phy_name[32];

		snprintf(phy_name, sizeof(phy_name), "hdmi-phy%d", index);
		dev_info(xhdmirx->dev, "%s\n", phy_name);
		xhdmirx->phy[index] = devm_phy_get(xhdmirx->dev, phy_name);
		if (IS_ERR(xhdmirx->phy[index])) {
			ret = PTR_ERR(xhdmirx->phy[index]);
			xhdmirx->phy[index] = NULL;
			if (ret != -EPROBE_DEFER)
				dev_err(xhdmirx->dev, "failed to get phy lane %s, error %d\n", phy_name, ret);
			hdmi_dbg("xvphy not ready - probe deferred\n");
			goto error_phy;
		}

		ret = phy_init(xhdmirx->phy[index]);
		if (ret) {
			dev_err(xhdmirx->dev,
				"failed to init phy lane %d\n", index);
			goto error_phy;
		}
		hdmi_dbg("exiting loop with index = %d\n",index);
	}

	HdmiRxSsPtr = (XV_HdmiRxSs *)&xhdmirx->xv_hdmirxss;

	mutex_lock(&xhdmirx->xhdmirx_mutex);

	/* sets pointer to the EDID used by XV_HdmiRxSs_LoadDefaultEdid() */
	XV_HdmiRxSs_SetEdidParam(HdmiRxSsPtr, (u8 *)&xilinx_edid[0], sizeof(xilinx_edid));

	// Initialize top level and all included sub-cores
	Status = XV_HdmiRxSs_CfgInitialize(HdmiRxSsPtr, &config,
		(uintptr_t)xhdmirx->iomem);
	if (Status != XST_SUCCESS)
	{
		dev_err(xhdmirx->dev, "initialization failed with error %d\r\n", Status);
		return -EINVAL;
	}

	/* retrieve EDID */
	if (request_firmware(&fw_edid, fw_edid_name, xhdmirx->dev) == 0) {
		int blocks = fw_edid->size / 128;
		if ((blocks == 0) || (blocks > EDID_BLOCKS_MAX) || (fw_edid->size % 128)) {
			dev_err(xhdmirx->dev, "%s must be n * 128 bytes, with 1 <= n <= %d, using Xilinx built-in EDID instead.\n",
				fw_edid_name, EDID_BLOCKS_MAX);
		} else {
			memcpy(xhdmirx->edid_user, fw_edid->data, 128 * blocks);
			xhdmirx->edid_user_blocks = blocks;
		}
	}
#if 0 /* kernel already prints something similar */
	else {
		dev_info(xhdmirx->dev, "%s EDID firmware could not be loaded, using Xilinx built-in EDID.\n", fw_edid_name);
	}
#endif
	release_firmware(fw_edid);
	if (xhdmirx->edid_user_blocks) {
		dev_info(xhdmirx->dev, "Using %d EDID block%s (%d bytes) from '%s'.\n",
			xhdmirx->edid_user_blocks, xhdmirx->edid_user_blocks > 1? "s":"", 128 * xhdmirx->edid_user_blocks, fw_edid_name);
		XV_HdmiRxSs_LoadEdid(HdmiRxSsPtr, (u8 *)&xhdmirx->edid_user, 128 * xhdmirx->edid_user_blocks);
	} else {
		dev_info(xhdmirx->dev, "Using Xilinx built-in EDID.\n");
		XV_HdmiRxSs_LoadDefaultEdid(HdmiRxSsPtr);
	}

	spin_lock_irqsave(&xhdmirx->irq_lock, flags);
	XV_HdmiRxSs_IntrDisable(HdmiRxSsPtr);
	spin_unlock_irqrestore(&xhdmirx->irq_lock, flags);

	/* RX SS callback setup (from xapp1287/xhdmi_example.c:2146) */
	XV_HdmiRxSs_SetCallback(HdmiRxSsPtr, XV_HDMIRXSS_HANDLER_CONNECT,
		RxConnectCallback, (void *)xhdmirx);
	XV_HdmiRxSs_SetCallback(HdmiRxSsPtr,XV_HDMIRXSS_HANDLER_AUX,
		RxAuxCallback,(void *)xhdmirx);
	XV_HdmiRxSs_SetCallback(HdmiRxSsPtr,XV_HDMIRXSS_HANDLER_AUD,
		RxAudCallback, (void *)xhdmirx);
	XV_HdmiRxSs_SetCallback(HdmiRxSsPtr, XV_HDMIRXSS_HANDLER_LNKSTA,
		RxLnkStaCallback, (void *)xhdmirx);
	XV_HdmiRxSs_SetCallback(HdmiRxSsPtr, XV_HDMIRXSS_HANDLER_STREAM_DOWN,
		RxStreamDownCallback, (void *)xhdmirx);
	XV_HdmiRxSs_SetCallback(HdmiRxSsPtr, XV_HDMIRXSS_HANDLER_STREAM_INIT,
		RxStreamInitCallback, (void *)xhdmirx);
	XV_HdmiRxSs_SetCallback(HdmiRxSsPtr, XV_HDMIRXSS_HANDLER_STREAM_UP,
		RxStreamUpCallback, (void *)xhdmirx);

	/* get a reference to the XVphy data structure */
	xhdmirx->xvphy = xvphy_get_xvphy(xhdmirx->phy[0]);

	BUG_ON(!xhdmirx->xvphy);

	//dev_info(xhdmirx->dev, "trying to lock xpvhy mutex\n");
	xvphy_mutex_lock(xhdmirx->phy[0]);
	//dev_info(xhdmirx->dev, "got xpvhy mutex\n");
	/* the callback is not specific to a single lane, but we need to
	 * provide one of the phy's as reference */
	XVphy_SetHdmiCallback(xhdmirx->xvphy, XVPHY_HDMI_HANDLER_RXINIT,
		VphyHdmiRxInitCallback, (void *)xhdmirx);

	XVphy_SetHdmiCallback(xhdmirx->xvphy, XVPHY_HDMI_HANDLER_RXREADY,
		VphyHdmiRxReadyCallback, (void *)xhdmirx);
	xvphy_mutex_unlock(xhdmirx->phy[0]);
	//dev_info(xhdmirx->dev, "unlock xpvhy mutex\n");

	platform_set_drvdata(pdev, xhdmirx);

	ret = devm_request_threaded_irq(&pdev->dev, xhdmirx->irq, hdmirx_irq_handler, hdmirx_irq_thread,
		IRQF_TRIGGER_HIGH, "xilinx-hdmi-rx", xhdmirx/*dev_id*/);

	if (ret) {
		dev_err(&pdev->dev, "unable to request IRQ %d\n", xhdmirx->irq);
		mutex_unlock(&xhdmirx->xhdmirx_mutex);
		goto error_phy;
	}

	/* Initialize V4L2 subdevice */
	subdev = &xhdmirx->subdev;
	v4l2_subdev_init(subdev, &xhdmirx_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &xhdmirx_internal_ops;
	strlcpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xhdmirx);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE /* | V4L2_SUBDEV_FL_HAS_EVENTS*/;

	/* Initialize V4L2 media entity */
	xhdmirx->pad.flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.ops = &xhdmirx_media_ops;
	ret = media_entity_pads_init(&subdev->entity, 1/*npads*/, &xhdmirx->pad);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to init media entity\n");
		mutex_unlock(&xhdmirx->xhdmirx_mutex);
		goto error_irq;
	}

	v4l2_ctrl_handler_init(&xhdmirx->ctrl_handler, 0/*controls*/);
	subdev->ctrl_handler = &xhdmirx->ctrl_handler;
	ret = v4l2_ctrl_handler_setup(&xhdmirx->ctrl_handler);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to set controls\n");
		mutex_unlock(&xhdmirx->xhdmirx_mutex);
		goto error_irq;
	}

	//xvip_print_version(&xhdmirx->xvip);

	/* assume detected format */
	xhdmirx->detected_format.width = 1280;
	xhdmirx->detected_format.height = 720;
	xhdmirx->detected_format.field = V4L2_FIELD_NONE;
	xhdmirx->detected_format.colorspace = V4L2_COLORSPACE_REC709;
	xhdmirx->detected_format.code = MEDIA_BUS_FMT_RBG888_1X24;
	xhdmirx->detected_format.colorspace = V4L2_COLORSPACE_SRGB;
	xhdmirx->detected_format.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	xhdmirx->detected_format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	xhdmirx->detected_format.quantization = V4L2_QUANTIZATION_DEFAULT;

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		mutex_unlock(&xhdmirx->xhdmirx_mutex);
		goto error;
	}

	mutex_unlock(&xhdmirx->xhdmirx_mutex);

	spin_lock_irqsave(&xhdmirx->irq_lock, flags);
	XV_HdmiRxSs_IntrEnable(HdmiRxSsPtr);
	spin_unlock_irqrestore(&xhdmirx->irq_lock, flags);
    hdmi_dbg("hdmi-rx probe successful\n");
	return 0;

error:
	v4l2_ctrl_handler_free(&xhdmirx->ctrl_handler);
	media_entity_cleanup(&subdev->entity);
error_irq:

error_phy:
	printk(KERN_INFO "xhdmirx_probe() error_phy:\n");
	/* release the lanes that we did get, if we did not get all lanes */
	for (index = 0; index < 2; index++) {
		if (xhdmirx->phy[index]) {
			printk(KERN_INFO "phy_exit() xhdmirx->phy[%d] = %p\n", index, xhdmirx->phy[index]);
			phy_exit(xhdmirx->phy[index]);
			xhdmirx->phy[index] = NULL;
		}
	}
error_resource:
	printk(KERN_INFO "xhdmirx_probe() error_resource:\n");
	return ret;
}

static int xhdmirx_remove(struct platform_device *pdev)
{
	struct xhdmirx_device *xhdmirx = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xhdmirx->subdev;
	unsigned long flags;

	spin_lock_irqsave(&xhdmirx->irq_lock, flags);
	XV_HdmiRxSs_IntrDisable(&xhdmirx->xv_hdmirxss);
	xhdmirx->teardown = 1;
	spin_unlock_irqrestore(&xhdmirx->irq_lock, flags);

	cancel_delayed_work(&xhdmirx->delayed_work_enable_hotplug);
	destroy_workqueue(xhdmirx->work_queue);
#if 0 // @TODO mutex can not be acquired
	mutex_lock(&xhdmirx->xhdmirx_mutex);
#endif

#if 0
	mutex_unlock(&xhdmirx->xhdmirx_mutex);
#endif

	v4l2_async_unregister_subdev(subdev);
	v4l2_ctrl_handler_free(&xhdmirx->ctrl_handler);
	media_entity_cleanup(&subdev->entity);
	clk_disable_unprepare(xhdmirx->clk);
	hdmi_dbg("removed.\n");
	return 0;
}

static SIMPLE_DEV_PM_OPS(xhdmirx_pm_ops, xhdmirx_pm_suspend, xhdmirx_pm_resume);

static const struct of_device_id xhdmirx_of_id_table[] = {
	{ .compatible = "xlnx,v-hdmi-rx-ss-2.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, xhdmirx_of_id_table);

static struct platform_driver xhdmirx_driver = {
	.driver = {
		.name		= "xilinx-hdmi-rx",
		.pm		= &xhdmirx_pm_ops,
		.of_match_table	= xhdmirx_of_id_table,
	},
	.probe			= xhdmirx_probe,
	.remove			= xhdmirx_remove,
};

module_platform_driver(xhdmirx_driver);

MODULE_DESCRIPTION("Xilinx HDMI RXSS V4L2 driver");
MODULE_AUTHOR("Leon Woestenberg <leon@sidebranch.com>");
MODULE_LICENSE("GPL v2");
