// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Viewer role, see ot_viewer.h.
 */

#include "ot_viewer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void
ot_viewer_reset(struct ot_viewer *v)
{
	memset(v, 0, sizeof(*v));
}

void
ot_face_obs_from_landmarks(const float (*lm)[3], struct ot_face_obs *obs)
{
	memset(obs, 0, sizeof(*obs));
	const int centre[2] = {OT_LM_LEFT_IRIS_CENTRE, OT_LM_RIGHT_IRIS_CENTRE};
	for (int e = 0; e < 2; e++) {
		memcpy(obs->iris[e], lm[centre[e]], sizeof(obs->iris[e]));
		for (int i = 0; i < 4; i++) {
			obs->iris_ring[e][i][0] = lm[centre[e] + 1 + i][0];
			obs->iris_ring[e][i][1] = lm[centre[e] + 1 + i][1];
		}
	}
	memcpy(obs->eye_outer[0], lm[OT_LM_LEFT_EYE_OUTER], sizeof(obs->eye_outer[0]));
	memcpy(obs->eye_outer[1], lm[OT_LM_RIGHT_EYE_OUTER], sizeof(obs->eye_outer[1]));
	memcpy(obs->forehead, lm[OT_LM_FOREHEAD], sizeof(obs->forehead));
	memcpy(obs->chin, lm[OT_LM_CHIN], sizeof(obs->chin));
}

static void
to_display(const struct ot_pose *pose, const float dir[3], float dist, float out[3])
{
	float d[3] = {dir[0], dir[1], pose->focal_scale * dir[2]};
	const float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	float p[3];
	for (int i = 0; i < 3; i++) {
		p[i] = dist * d[i] / n;
	}
	float r[3];
	ot_quat_rotate(pose->q, p, r);
	for (int i = 0; i < 3; i++) {
		out[i] = pose->t[i] + r[i];
	}
}

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
                 struct ot_eye_estimate *est_out)
{
	struct ot_eye_estimate est;
	memset(&est, 0, sizeof(est));
	bool ok = false;
	if (face != NULL && face->found) {
		struct ot_face_obs obs;
		ot_face_obs_from_landmarks((const float(*)[3])face->lm, &obs);
		ok = ot_estimate_eyes(k, ep, &obs, &est);
	}
	if (est_out != NULL) {
		if (ok) {
			*est_out = est;
		} else {
			memset(est_out, 0, sizeof(*est_out));
		}
	}
	if (!ok) {
		if (!v->have_last) {
			return false;
		}
		// Last known, not tracking. The filters restart on the next face (a jump, not a slide).
		*out = v->last;
		out->capture_ns = capture_ns;
		out->flags &= ~(uint32_t)XRT_TRACKING_SAMPLE_TRACKING;
		out->flags &= ~(uint32_t)XRT_TRACKING_SAMPLE_CAMERA_VALID;
		memset(&out->camera, 0, sizeof(out->camera));
		out->confidence = 0.0f;
		for (int i = 0; i < 6; i++) {
			ot_one_euro_reset(&v->f[i]);
		}
		return true;
	}

	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->capture_ns = capture_ns;
	const float scale = pose->iris_scale > 0.0f ? pose->iris_scale : 1.0f;
	float cam[2][3];
	for (int e = 0; e < 2; e++) {
		// Filter the camera-space point (metres), then map. The filter sees what the user
		// does (head motion), not the pose.
		float p[3];
		for (int i = 0; i < 3; i++) {
			p[i] = scale * est.distance_m[e] * est.dir[e][i];
			cam[e][i] = ot_one_euro(&v->f[e * 3 + i], p[i], capture_ns, fp->min_cutoff_hz, fp->beta,
			                        fp->d_cutoff_hz);
		}
		const float n = sqrtf(cam[e][0] * cam[e][0] + cam[e][1] * cam[e][1] + cam[e][2] * cam[e][2]);
		float dir[3] = {cam[e][0] / n, cam[e][1] / n, cam[e][2] / n};
		to_display(pose, dir, n, e == 0 ? out->left_eye : out->right_eye);
	}
	for (int i = 0; i < 3; i++) {
		out->head_position[i] = 0.5f * (out->left_eye[i] + out->right_eye[i]);
	}
	ot_quat_mul(pose->q, est.head_rotation, out->head_orientation);
	out->confidence = face->presence;
	out->flags = XRT_TRACKING_SAMPLE_EYES_VALID | XRT_TRACKING_SAMPLE_TRACKING | XRT_TRACKING_SAMPLE_HEAD_POSE_VALID;

	struct xrt_tracking_camera_sample *c = &out->camera;
	snprintf(c->camera_key, sizeof(c->camera_key), "%s", camera_key != NULL ? camera_key : "");
	memcpy(c->left_dir, est.dir[0], sizeof(c->left_dir));
	memcpy(c->right_dir, est.dir[1], sizeof(c->right_dir));
	c->left_distance_m = est.distance_m[0];
	c->right_distance_m = est.distance_m[1];
	out->flags |= XRT_TRACKING_SAMPLE_CAMERA_VALID;

	v->last = *out;
	v->have_last = true;
	v->last_face_ns = capture_ns;
	return true;
}
