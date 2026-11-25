// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The display's shared stereo camera as a hands source: a thin wrapper
 *         over the runtime's public input-host slots `open_shared_camera`,
 *         `camera_acquire`, `camera_release` and `close_shared_camera`
 *         (input-provider spec §4d, ADR-043 Amendment 6).
 *
 * The runtime opens the camera in its own process, applies its consent and
 * sharing policy, and hands out RECTIFIED side-by-side frames (left eye = left
 * half, never mirrored), usually GRAY8, with their rectified calibration: one
 * focal length and principal point for both eyes, the right eye translated by
 * the baseline. This file only turns those into ot_image views and pinhole
 * intrinsics; it knows nothing about the display behind them.
 */

#pragma once

#include "xrt/xrt_input_plugin.h"

#include "ot_eyes.h"
#include "ot_face_geom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OT_SHCAM_KEY_MAX 128
#define OT_SHCAM_NAME_MAX 128

struct ot_shcam;

//! What the open handed back.
struct ot_shcam_info
{
	char key[OT_SHCAM_KEY_MAX];   //!< the camera's persistent id ("stereo:<id>" when it has none)
	char name[OT_SHCAM_NAME_MAX]; //!< the camera's display name
	uint32_t eye_width, eye_height;
	struct ot_intrinsics k; //!< the LEFT eye's rectified pinhole (focal 0 = no calibration)
	float baseline_m;       //!< 0 = unknown (no stereo)
	float max_frame_rate;
	uint32_t frame_orientation_deg; //!< as the runtime reports it (0 = upright; 0xFFFFFFFF unknown)
};

//! One acquired frame. The images point into the runtime's ring until ot_shcam_release.
struct ot_shcam_frame
{
	struct ot_image left, right;
	int64_t capture_ns; //!< the runtime's monotonic clock (= ot_monotonic_ns)
	bool time_is_exposure;
	uint64_t index;
	uint32_t calibration_generation;
};

enum ot_shcam_take
{
	OT_SHCAM_FRAME = 0,   //!< a frame: release it
	OT_SHCAM_TIMEOUT = 1, //!< none yet; *state says why
	OT_SHCAM_ENDED = 2,   //!< the stream ended (or failed): close, and open again later
};

//! The runtime offers the four slots (cover-checked against host->struct_size, all non-NULL).
bool
ot_shcam_host_supports(const struct xrt_input_plugin_host_iface *host);

/*!
 * Open the active display's shared camera, RECTIFIED, for provider @p id. May
 * block (a consent prompt): call it from the provider's thread. Returns the
 * runtime's result; *@p out is set only on XRT_SUCCESS.
 */
xrt_result_t
ot_shcam_open(const struct xrt_input_plugin_host_iface *host,
              const char *id,
              float max_frame_rate,
              struct ot_shcam **out,
              struct ot_shcam_info *info);

/*!
 * The newest frame, waiting up to @p timeout_ms. @p state receives the camera's
 * state (enum xrt_stereo_camera_state) on every call.
 */
enum ot_shcam_take
ot_shcam_take(struct ot_shcam *c, uint32_t timeout_ms, struct ot_shcam_frame *out, uint32_t *state);

void
ot_shcam_release(struct ot_shcam *c, const struct ot_shcam_frame *f);

//! NULL is ignored.
void
ot_shcam_close(struct ot_shcam *c);

//! A short human reason for an open / acquire result.
const char *
ot_shcam_result_str(xrt_result_t r);

/*!
 * The camera's rectified calibration -> the left eye's pinhole and the baseline,
 * for frames of @p eye_w x @p eye_h (the calibration's own size may differ: it is
 * rescaled). False when the calibration carries no focal length.
 */
bool
ot_shcam_intrinsics(const struct xrt_stereo_camera_calibration *calib,
                    uint32_t eye_w,
                    uint32_t eye_h,
                    struct ot_intrinsics *k,
                    float *baseline_m);

#ifdef __cplusplus
}
#endif
