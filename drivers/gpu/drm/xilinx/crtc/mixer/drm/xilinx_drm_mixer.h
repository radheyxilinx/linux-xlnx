/*
 * (C) Copyright 2016 - 2017, Xilinx, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef __XLNX_DRM_MIXER__
#define __XLNX_DRM_MIXER__
#include "crtc/mixer/hw/xilinx_mixer_data.h"
#include "xilinx_drm_plane.h"


/**
 * @struct xilinx_drm_mixer
 * Container for interfacing DRM driver to mixer hardware IP driver layer.
 * Contains pointers to logical constructions such as the DRM plane
 * manager as well as pointers to distinquish the mixer layer serving
 * as the DRM "primary" plane from the actual mixer layer which serves
 * as the background layer in hardware.
 */
struct xilinx_drm_mixer {
	struct xv_mixer mixer_hw;
	struct xilinx_drm_plane_manager *plane_manager;
	struct xv_mixer_layer_data *drm_primary_layer;
	struct xv_mixer_layer_data *hw_master_layer;
	struct xv_mixer_layer_data *hw_logo_layer;
	struct drm_property *alpha_prop;
	struct drm_property *scale_prop;
	struct drm_property *bg_color;
};
#define get_mixer_max_height(m)      mixer_layer_height(m->hw_master_layer)
#define get_mixer_max_width(m)       mixer_layer_width(m->hw_master_layer)
#define get_mixer_max_logo_height(m) mixer_layer_height(m->hw_logo_layer)
#define get_mixer_max_logo_width(m)  mixer_layer_width(m->hw_logo_layer)
#define get_num_mixer_planes(m)      m->mixer_hw.layer_cnt
#define get_mixer_vid_out_fmt(m)     mixer_video_fmt(&m->mixer_hw)

#define to_xv_mixer_hw(p) (&(p->manager->mixer->mixer_hw))

#define get_xilinx_mixer_mem_align(m)  \
	sizeof(m->mixer_hw.layer_data[0].layer_regs.buff_addr)

/**
 * Used to parse device tree for mixer node and initialize the mixer IP core
 * to a default state wherein a background color is generated and all layers
 * are initially disabled.
 *
 * @param[in] dev Device member of drm device
 * @param[in] node Open firmware(of) device tree node describing the mixer IP
 * @param[in] manager Plane manager object to bind to mixer instance
 *
 * @returns reference to drm mixer instance struct; err pointer otherwise
 *
 */
struct xilinx_drm_mixer * xilinx_drm_mixer_probe(struct device *dev,
				struct device_node *node,
				struct xilinx_drm_plane_manager *manager);

/**
 * Mixer-specific implementation for xilinx_drm_plane_mode_set() call.
 * Configures a mixer layer to comply with userspace SET_PLANE icotl
 * call.
 *
 * @param[in] plane Xilinx_drm_plane object containing references to
 *		the base plane and mixer
 * @param[in] fb Framebuffer descriptor
 * @param[in] crtc_x X position of layer on crtc.  Note, if the plane
 *		represents either the master hardware layer (video0) or
 *		the layer representing the DRM primary layer, the crtc
 *		x/y coordinates are either ignored and/or set to 0/0
 *		respectively.
 * @param[in] crtc_y Y position of layer.  See description of crtc_x handling
 *		for more inforation.
 * @param[in] src_x x-offset in memory buffer from which to start reading
 * @param[in] src_y y-offset in memory buffer from which to start reading
 * @param[in] src_w Number of horizontal pixels to read from memory per row
 * @param[in] src_h Number of rows of video data to read from memory
 *
 * @returns 0 on success.  Non-zero linux error code otherwise.
 */
int xilinx_drm_mixer_set_plane(struct xilinx_drm_plane *plane,
			struct drm_framebuffer *fb,
			int crtc_x, int crtc_y,
			uint32_t src_x, uint32_t src_y,
			uint32_t src_w, uint32_t src_h);

/**
 * Create Mixer-specific drm property objects to track mixer layer
 * settings for alpha and scale
 *
 * @param[in] mixer drm mixer object
 */
void xilinx_drm_create_mixer_plane_properties(struct xilinx_drm_mixer *mixer);

/**
 * Used to set the current value for a particular plane property in the
 * corresponding mixer layer hardware
 *
 * @param[in] plane Xilinx drm plane object containing references to the mixer
 * @param[in] property drm property passed in by userspace for update
 * @param[in] value new value used to set mixer layer hardware for register
 *		mapped to the drm property
 *
 * @returns 0 on success; EINVAL otherwise
 */
int xilinx_drm_mixer_set_plane_property(struct xilinx_drm_plane *plane,
				struct drm_property *property,
				uint64_t value);

/**
 * Links the xilinx plane object to a mixer layer object
 *
 * @param[in] manager Xilinx drm plane manager object with references to all
 *		of the xilinx planes and the mixer
 * @param[in] plane The specific plane object to link a layer to
 * @param[in] node Device tree open firmware node object containing the mixer
 *		layer information from the device tree
 *
 * @returns 0 on success; -EINVAL if dts properties are missing/invalid; -ENODEV
 *		if no layer object has been create for the referenced layer node
 *		(this may indicate an out-of-memory condition or failed mixer
 *		probe)
 */
int xilinx_drm_create_mixer_layer_plane(
				struct xilinx_drm_plane_manager *manager,
				struct xilinx_drm_plane *plane,
				struct device_node *node);

/**
 * Attaches mixer-specific drm properties to the given plane if it is linked
 * to a mixer layer and the layer supports those properites.  The linked
 * mixer layer will be inspected to see what capabilities it offers (e.g.
 * global layer alpha; scaling) and drm property objects that indicate those
 * capabilities will then be attached and initialized to default values.
 *
 * @param[in] plane Xilinx drm plane object to inspect and attach appropriate
 *		properties to
 */
void xilinx_drm_mixer_attach_plane_prop(struct xilinx_drm_plane *plane);

/**
 * Hold the reset line for the IP core low for 300 nano seconds and then
 * brings line high to pull out of reset.  The core can then be reprogrammed
 * with new mode settings and subsequently started to begin generating video
 *
 * @param[in] mixer IP core instance to reset
 *
 */
void xilinx_drm_mixer_reset(struct xilinx_drm_mixer *mixer);

/**
 * Start generation of video stream from mixer
 *
 * @param[in] mixer IP core instance to reset
 *
 * @note sets the mixer to auto-restart so that video will be streamed
 *       continuously
 */
void xilinx_drm_mixer_start(struct xv_mixer *mixer);

/**
 * Internal method used to look-up color format index based on device tree
 * string.
 *
 * @param[in] color_fmt String value representing color format found in device
 *                      tree (e.g. "rgb", "yuv422", "yuv444")
 * @param[out] output Enum value of video format id
 *
 * @returns 0 on success; -EINVAL if no entry was found in table
 *
 * @note Should not be used outside of DRM driver.
 */
int xilinx_drm_mixer_string_to_fmt(const char *color_fmt, u32 *output);

/**
 * Internal method used to use Xilinx color id and match to DRM-based fourcc
 * color code.
 *
 * @param[in] id Xilinx enum value for a color space type (e.g. YUV422)
 * @param[out] output DRM fourcc value for corresponding Xilinx color space id
 *
 * @returns 0 on success; -EINVAL if no matching entry found
 *
 * @note Should not be used outside of DRM driver.
 */
int xilinx_drm_mixer_fmt_to_drm_fmt(enum xv_comm_color_fmt_id id, u32 *output);


/**
 * Change video scale factor for video plane
 *
 * @param[in] plane Drm plane object describing layer to be modified
 * @param[in] val Index of scale factor to use:
 *		0 = 1x
 *		1 = 2x
 *		2 = 4x
 *
 * @returns 0 on success; either -EINVAL if scale value is illegal or
 *          -ENODEV if layer does not exist (null)
 */
int xilinx_drm_mixer_set_layer_scale(struct xilinx_drm_plane *plane,
				uint64_t val);

/**
 * Change the transparency of an entire plane
 *
 * @param[in] plane Video layer affected by new alpha setting
 * @param[in] val Value of transparency setting (0-255) with 255 being opaque
 *		0 being fully transparent
 *
 * @returns 0 on success; -EINVAL on failure
 */
int xilinx_drm_mixer_set_layer_alpha(struct xilinx_drm_plane *plane,
				uint64_t val);

/**
 * Disables video output represented by the plane object
 *
 * @param[in] plane Drm plane object describing video layer to disable
 *
 */
void xilinx_drm_mixer_layer_disable(struct xilinx_drm_plane *plane);

/**
 * Enables video output represented by the plane object
 *
 * @param[in] plane Drm plane object describing video layer to enable
 *
 */
void xilinx_drm_mixer_layer_enable(struct xilinx_drm_plane *plane);


/**
 * Enables video output represented by the plane object
 *
 * @param[in] plane Drm plane object describing video layer to mark
 *		as active.  Only layers marked 'active' will be
 *		enabled when size or scale registeres are update.
 *		In-active layers can be updated but will not be
 *		enabled in hardware.
 *
 * @returns 0 on success; -ENODEV if mixer layer does not exist
 */
int xilinx_drm_mixer_mark_layer_active(struct xilinx_drm_plane *plane);

/**
 * Enables video output represented by the plane object
 *
 * @param[in] plane Drm plane object describing video layer to mark
 *		as inactive.  Only layers marked 'active' will be
 *		enabled when size or scale registeres are update.
 *		In-active layers can be updated but will not be
 *		enabled in hardware.
 *
 * @returns 0 on success; -ENODEV if mixer layer does not exist
 */
int xilinx_drm_mixer_mark_layer_inactive(struct xilinx_drm_plane *plane);

/**
 * Establishes new coordinates and dimensions for a video plane layer
 *
 * @param[in] plane Drm plane object desribing video layer to reposition
 * @param[in] crtc_x New horizontal anchor postion from which to begin rendering
 * @param[in] crtc_y New vertical anchor position from which to begin rendering
 * @param[in] width Width, in pixels, to render from stream or memory buffer
 * @param[in] height Height, in pixels, to render from stream or memory buffer
 * @param[in] stride Width, in bytes, of a memory buffer.  Used only for
 *		memory layers.  Use 0 for streaming layers.
 *
 * @returns 0 if successful; Either -EINVAL if coordindate data is invalid
 *	      or -ENODEV if layer data not present
 *
 * @note New size and coordinates of window must fit within the currently active
 * area of the crtc (e.g. the background resolution)
 */
int xilinx_drm_mixer_set_layer_dimensions(struct xilinx_drm_plane *plane,
				u32 crtc_x, u32 crtc_y,
				u32 width, u32 height, u32 stride);

/**
 * Obtains a pointer to a struct containing layer-specific data for the mixer IP
 *
 * @param[in] mixer Instance of mixer for which to obtain layer data
 * @param[in] id logical layer id (e.g. 0=background, 1=overlay) for which to
 *		obtain layer information
 *
 * @returns pointer to struct xv_mixer_layer_data for layer specified by id;
 *	NULL on failure.
 *
 * @note Does not apply to logo layer.  Logo layer data is contained within the
 *	struct xv_mixer instance.
 */
struct xv_mixer_layer_data * xilinx_drm_mixer_get_layer(struct xv_mixer *mixer,
						enum xv_mixer_layer_id id);


/**
 * Sets and interrupt handler function to run when the mixer generates and
 * ap_done interrupt event (when frame processing has completed)
 *
 * @param[in] mixer Mixer object upon which to run handler function when mixer
 *		generates an "done" interrupt for a frame
 * @param[in] intr_handler_fn Function pointer for interrupt handler.  Typically
 *		a drm vertical blank event generation function.
 * @param[in] data Pointer to crtc object
 */
void xilinx_drm_mixer_set_intr_handler(struct xilinx_drm_mixer *mixer,
				void (*intr_handler_fn)(void *),
				void *data);

/**
 * Implementation of display power management system call (dpms).  Designed
 * to disable and turn off a plane and restore all attached drm properities to
 * their initial values.  Alterntively, if dpms is "on", will enable a layer.
 *
 * param[in] plane Plane/mixer layer to enable/disable (based on dpms value)
 * param[in] dpms  Display power management state to act upon
 */
void xilinx_drm_mixer_plane_dpms(struct xilinx_drm_plane *plane, int dpms);


/**
 * Implement drm dpms semantics for video mixer IP
 *
 * param[in] mixer Device instance representing mixer IP
 * param[in] dpms  Display power management state to act upon
 */
void xilinx_drm_mixer_dpms(struct xilinx_drm_mixer *mixer, int dpms);

/**
 * Updates internal R, G and B buffer array of mixer from kernel framebuffer
 * which is expected to be arranged as RGB888 (fourcc 'RG24') packed 24 bit data
 *
 * @param[in] plane Xilinx drm plane object with current video format info
 * @param[in] fb Framebuffer with which to obtain reference to backing storage
 * @param[in] src_w  Width of buffer to read RGB888 data
 * @param[in] src_h  Height of buffer to read RGB888 data
 *
 * @returns 0 on success; -EINVAL if format and/or size of buffer is invalid
 *
 * @note Initial call caches buffer kernel virtual address.  Subsequent calls
 *       will only re-load buffer if virtual address and/or size changes.
 */
int xilinx_drm_mixer_update_logo_img(struct xilinx_drm_plane *plane,
				struct drm_gem_cma_object *buffer,
				uint32_t src_w, uint32_t src_h);
#endif /* end __XLNX_DRM_MIXER__ */
