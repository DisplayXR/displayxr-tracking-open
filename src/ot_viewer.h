// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The viewer role: one face result -> one `xrt_tracking_viewer_sample`.
 *
 * Per analysed frame with a face:
 *  1. camera-space eye points from the landmarks (ot_estimate_eyes), the
 *     depth scaled by the viewer's per-user iris scale;
 *  2. a One Euro filter per coordinate of the two camera-space eye points;
 *  3. display = T + R · p (the camera's pose as the runtime answers it, with
 *     its focal scale applied to the direction: normalize(u_x, u_y, k·u_z));
 *  4. the camera-space block: the RAW measurement of this frame (unfiltered,
 *     NOT scaled by the iris scale), which is what the runtime's calibration
 *     solves against.
 * Without a face: the last known eyes with TRACKING clear (the runtime drops a
 * sample without EYES_VALID, so nothing is pushed before the first face).
 */

#pragma once

#include "xrt/xrt_tracking_plugin.h"

#include "ot_eyes.h"
#include "ot_face_tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

//! The camera -> display pose the samples are mapped through.
struct ot_pose
{
	float t[3];
	float q[4];
	float focal_scale; //!< 1 = none
	float iris_scale;  //!< the viewer's per-user depth scale, 1 = none
	uint32_t source;   //!< XRT_CAMERA_POSE_SOURCE_*; 0 = the provider's own fallback
	uint64_t generation;
};

struct ot_filter_params
{
	float min_cutoff_hz; //!< 1.0
	float beta;          //!< per m/s, 3.0
	float d_cutoff_hz;   //!< 1.0
};

struct ot_viewer
{
	struct ot_one_euro f[6];
	bool have_last;
	struct xrt_tracking_viewer_sample last;
	int64_t last_face_ns;
};

void
ot_viewer_reset(struct ot_viewer *v);

/*!
 * Build the sample for one analysed frame. Returns false when there is
 * nothing to push (no face yet). @p est (optional) receives the raw estimate.
 */
bool
ot_viewer_update(struct ot_viewer *v,
                 const struct ot_intrinsics *k,
                 const struct ot_eye_params *ep,
                 const struct ot_filter_params *fp,
                 const struct ot_pose *pose,
                 const struct ot_face_result *face,
                 int64_t capture_ns,
                 const char *camera_key,
                 struct xrt_tracking_viewer_sample *out,
                 struct ot_eye_estimate *est);

//! The face result's landmarks -> the estimator's observation.
void
ot_face_obs_from_landmarks(const float (*lm)[3], struct ot_face_obs *obs);

#ifdef __cplusplus
}
#endif
