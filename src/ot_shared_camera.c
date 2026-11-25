// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The display's shared stereo camera, see ot_shared_camera.h.
 */

#include "ot_shared_camera.h"
#include "ot_platform.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ot_shcam
{
	const struct xrt_input_plugin_host_iface *host;
	struct xrt_input_host_camera *cam;
	bool holding;
	uint64_t held_index;
};

bool
ot_shcam_host_supports(const struct xrt_input_plugin_host_iface *h)
{
	return h != NULL &&
	       h->struct_size >=
	           offsetof(struct xrt_input_plugin_host_iface, close_shared_camera) + sizeof(h->close_shared_camera) &&
	       h->open_shared_camera != NULL && h->camera_acquire != NULL && h->camera_release != NULL &&
	       h->close_shared_camera != NULL;
}

const char *
ot_shcam_result_str(xrt_result_t r)
{
	switch (r) {
	case XRT_SUCCESS: return "ok";
	case XRT_ERROR_FEATURE_NOT_SUPPORTED:
		return "no shared camera here (an in-process runtime, no rectifiable camera, or the service is "
		       "starting)";
	case XRT_ERROR_STEREO_CAMERA_DISABLED: return "camera sharing is off";
	case XRT_ERROR_STEREO_CAMERA_CONSENT_REFUSED: return "consent refused";
	case XRT_ERROR_NOT_AUTHORIZED: return "not authorized";
	case XRT_ERROR_CLIENT_LIMIT_REACHED: return "the camera-consumer quota is exhausted";
	case XRT_ERROR_STEREO_CAMERA_BUSY: return "the camera is busy";
	case XRT_ERROR_STEREO_CAMERA_STREAM_ENDED: return "the stream ended";
	default: return "failed";
	}
}

bool
ot_shcam_intrinsics(const struct xrt_stereo_camera_calibration *calib,
                    uint32_t eye_w,
                    uint32_t eye_h,
                    struct ot_intrinsics *k,
                    float *baseline_m)
{
	const struct xrt_stereo_camera_intrinsics *e = &calib->eye[0];
	if (!(e->fx > 0.0f) || eye_w == 0 || eye_h == 0) {
		return false;
	}
	// Calibration given at another size than the frames: scale (pixel-centre aware: continuous coordinates).
	const float sx = e->width > 0 ? (float)eye_w / (float)e->width : 1.0f;
	const float sy = e->height > 0 ? (float)eye_h / (float)e->height : 1.0f;
	k->width = eye_w;
	k->height = eye_h;
	k->f = e->fx * sx; // rectified: fx = fy
	k->cx = e->cx * sx;
	k->cy = e->cy * sy;
	float b = calib->baseline_mm > 0.0f ? calib->baseline_mm * 0.001f : fabsf(calib->position[0]);
	*baseline_m = b > 0.0f && b < 1.0f ? b : 0.0f;
	return true;
}

xrt_result_t
ot_shcam_open(const struct xrt_input_plugin_host_iface *host,
              const char *id,
              float max_frame_rate,
              struct ot_shcam **out,
              struct ot_shcam_info *info)
{
	*out = NULL;
	memset(info, 0, sizeof(*info));
	if (!ot_shcam_host_supports(host)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_input_host_camera_open req;
	memset(&req, 0, sizeof(req));
	req.struct_size = (uint32_t)sizeof(req);
	req.output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
	req.camera_id = 0; // the active display's own camera
	req.max_frame_rate = max_frame_rate;
	struct xrt_input_host_camera *cam = NULL;
	const xrt_result_t r = host->open_shared_camera(id, &req, &cam);
	if (r != XRT_SUCCESS || cam == NULL) {
		return r != XRT_SUCCESS ? r : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct ot_shcam *c = (struct ot_shcam *)calloc(1, sizeof(*c));
	if (c == NULL) {
		host->close_shared_camera(cam);
		return XRT_ERROR_ALLOCATION;
	}
	c->host = host;
	c->cam = cam;
	if (req.props.persistent_id[0] != '\0') {
		ot_copy_str(info->key, sizeof(info->key), req.props.persistent_id);
	} else {
		snprintf(info->key, sizeof(info->key), "stereo:%llu", (unsigned long long)req.props.camera_id);
	}
	ot_copy_str(info->name, sizeof(info->name),
	            req.props.display_name[0] != '\0' ? req.props.display_name : "shared stereo camera");
	info->eye_width = req.layout.width / 2u;
	info->eye_height = req.layout.height;
	info->max_frame_rate = req.layout.max_frame_rate;
	info->frame_orientation_deg = req.props.frame_orientation_deg;
	if (!ot_shcam_intrinsics(&req.calib, info->eye_width, info->eye_height, &info->k, &info->baseline_m)) {
		memset(&info->k, 0, sizeof(info->k));
		info->baseline_m = 0.0f;
	}
	*out = c;
	return XRT_SUCCESS;
}

//! The planes of one frame -> left / right views. False for a format the samplers cannot read.
static bool
views(const struct xrt_input_host_camera_frame *f, struct ot_image *l, struct ot_image *r)
{
	enum ot_pixel_format fmt;
	switch (f->format) {
	case XRT_STEREO_CAMERA_FORMAT_GRAY8: fmt = OT_FMT_GRAY8; break;
	case XRT_STEREO_CAMERA_FORMAT_NV12: fmt = OT_FMT_GRAY8; break; // plane 0 is the luma: a grey image
	case XRT_STEREO_CAMERA_FORMAT_BGRA8: fmt = OT_FMT_BGRA; break;
	default: return false;
	}
	if (f->plane[0] == NULL || f->width < 2 || f->height == 0) {
		return false;
	}
	const uint32_t half = f->width / 2u;
	l->data = f->plane[0];
	l->width = half;
	l->height = f->height;
	l->stride = f->row_pitch[0];
	l->format = fmt;
	l->mirrored = false; // the runtime never mirrors these frames
	*r = *l;
	r->data = f->plane[0] + (size_t)half * ot_pixel_bytes(fmt);
	return true;
}

enum ot_shcam_take
ot_shcam_take(struct ot_shcam *c, uint32_t timeout_ms, struct ot_shcam_frame *out, uint32_t *state)
{
	*state = 0;
	if (c->holding) {
		c->host->camera_release(c->cam, c->held_index);
		c->holding = false;
	}
	struct xrt_input_host_camera_frame f;
	memset(&f, 0, sizeof(f));
	f.struct_size = (uint32_t)sizeof(f);
	const xrt_result_t r = c->host->camera_acquire(c->cam, timeout_ms, &f, state);
	if (r == XRT_TIMEOUT) {
		return OT_SHCAM_TIMEOUT;
	}
	if (r != XRT_SUCCESS) {
		return OT_SHCAM_ENDED;
	}
	c->holding = true;
	c->held_index = f.frame_index;
	memset(out, 0, sizeof(*out));
	if (!views(&f, &out->left, &out->right)) {
		c->host->camera_release(c->cam, f.frame_index);
		c->holding = false;
		return OT_SHCAM_TIMEOUT; // nothing usable in this frame
	}
	out->capture_ns = f.timestamp_ns;
	out->time_is_exposure = f.time_is_exposure;
	out->index = f.frame_index;
	out->calibration_generation = f.calibration_generation;
	return OT_SHCAM_FRAME;
}

void
ot_shcam_release(struct ot_shcam *c, const struct ot_shcam_frame *f)
{
	if (c->holding && c->held_index == f->index) {
		c->host->camera_release(c->cam, f->index);
		c->holding = false;
	}
}

void
ot_shcam_close(struct ot_shcam *c)
{
	if (c == NULL) {
		return;
	}
	if (c->holding) {
		c->host->camera_release(c->cam, c->held_index);
	}
	c->host->close_shared_camera(c->cam);
	free(c);
}
