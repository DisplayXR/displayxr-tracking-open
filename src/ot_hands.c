// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The hands role's per-frame mapping, see ot_hands.h.
 */

#include "ot_hands.h"

#include <math.h>
#include <string.h>

void
ot_hands_params_defaults(struct ot_hands_params *p)
{
	p->hand_scale = 1.0f;
	p->pinch_on_m = 0.025f;
	p->pinch_off_m = 0.035f;
	p->min_cutoff_hz = 1.0f;
	p->beta = 3.0f;
	p->d_cutoff_hz = 1.0f;
	p->max_distance_m = 0.8f;
}

void
ot_hands_reset(struct ot_hands *h)
{
	memset(h, 0, sizeof(*h));
}

void
ot_hands_assign(const struct ot_hand_result *r, int out[2])
{
	out[OT_SIDE_LEFT] = -1;
	out[OT_SIDE_RIGHT] = -1;
	if (r->count >= 2) {
		const int right = r->hands[0].right_mean >= r->hands[1].right_mean ? 0 : 1;
		out[OT_SIDE_RIGHT] = right;
		out[OT_SIDE_LEFT] = 1 - right;
	} else if (r->count == 1) {
		out[r->hands[0].right_mean >= 0.5f ? OT_SIDE_RIGHT : OT_SIDE_LEFT] = 0;
	}
}

void
ot_cam_to_display(const struct ot_cam_pose *pose, const float p[3], float out[3])
{
	float r[3];
	ot_quat_rotate(pose->q, p, r);
	out[0] = pose->t[0] + r[0];
	out[1] = pose->t[1] + r[1];
	out[2] = pose->t[2] + r[2];
}

//! The 21 points of one observed hand in the camera frame. False when it cannot be placed.
static bool
place_camera(const struct ot_intrinsics *k,
             const struct ot_hands_params *p,
             const struct ot_cam_pose *pose,
             const struct ot_hand_obs *o,
             float (*cam)[3],
             float *depth_m)
{
	struct ot_intrinsics kk = *k;
	kk.f *= pose->focal_scale > 0.0f ? pose->focal_scale : 1.0f;
	float shape[OT_HAND_LM_COUNT][3], t[3] = {0, 0, 0};
	ot_hand_world_to_camera((const float(*)[3])o->world, o->roi.rot, shape);
	const float s = p->hand_scale > 0.0f ? p->hand_scale : 1.0f;
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		shape[i][0] *= s;
		shape[i][1] *= s;
		shape[i][2] *= s;
	}
	if (!ot_hand_place(&kk, (const float(*)[3])o->lm, (const float(*)[3])shape, cam, t)) {
		return false;
	}
	*depth_m = sqrtf(cam[OT_HL_WRIST][0] * cam[OT_HL_WRIST][0] + cam[OT_HL_WRIST][1] * cam[OT_HL_WRIST][1] +
	                 cam[OT_HL_WRIST][2] * cam[OT_HL_WRIST][2]);
	return true;
}

//! The 21 points of one observed hand in display space (unfiltered). False when it cannot be placed.
static bool
place(const struct ot_intrinsics *k,
      const struct ot_hands_params *p,
      const struct ot_cam_pose *pose,
      const struct ot_hand_obs *o,
      float (*disp)[3],
      float *depth_m)
{
	float cam[OT_HAND_LM_COUNT][3];
	if (!place_camera(k, p, pose, o, cam, depth_m)) {
		return false;
	}
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		ot_cam_to_display(pose, cam[i], disp[i]);
	}
	return true;
}

bool
ot_hands_distance(const struct ot_intrinsics *k,
                  const struct ot_hands_params *p,
                  const struct ot_cam_pose *pose,
                  const struct ot_hand_obs *o,
                  float *out_m)
{
	float cam[OT_HAND_LM_COUNT][3];
	return place_camera(k, p, pose, o, cam, out_m);
}

bool
ot_hands_admit(void *user, const struct ot_hand_obs *o)
{
	struct ot_hands_admit *a = (struct ot_hands_admit *)user;
	float d;
	if (a->p->max_distance_m <= 0.0f || a->k->f <= 0.0f || !ot_hands_distance(a->k, a->p, a->pose, o, &d) ||
	    d <= a->p->max_distance_m) {
		return true;
	}
	a->last_rejected_m = d;
	return false;
}

void
ot_hands_update(struct ot_hands *h,
                const struct ot_intrinsics *k,
                const struct ot_hands_params *p,
                const struct ot_cam_pose *pose,
                const struct ot_hand_result *r,
                int64_t capture_ns,
                struct ot_hand_state out[2])
{
	int idx[2];
	ot_hands_assign(r, idx);
	for (int side = 0; side < 2; side++) {
		struct ot_hand_state *st = &out[side];
		memset(st, 0, sizeof(*st));
		st->capture_ns = capture_ns;
		float disp[OT_HAND_LM_COUNT][3];
		const struct ot_hand_obs *o = idx[side] >= 0 ? &r->hands[idx[side]] : NULL;
		if (o == NULL || !place(k, p, pose, o, disp, &st->depth_m)) {
			h->have[side] = false;
			continue;
		}
		// A new track, a hand that changed sides, or a gap: the filter starts over.
		const bool restart = !h->have[side] || h->track[side] != o->track_id ||
		                     capture_ns - h->last[side].capture_ns > 300000000LL;
		if (restart) {
			for (int i = 0; i < OT_HAND_LM_COUNT * 3; i++) {
				ot_one_euro_reset(&h->f[side][i]);
			}
		}
		for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
			for (int c = 0; c < 3; c++) {
				st->points[i][c] = ot_one_euro(&h->f[side][i * 3 + c], disp[i][c], capture_ns,
				                               p->min_cutoff_hz, p->beta, p->d_cutoff_hz);
			}
		}
		st->active = true;
		st->track_id = o->track_id;
		st->confidence = o->presence;
		st->right_mean = o->right_mean;
		ot_hand_joints_from_landmarks((const float(*)[3])st->points, side == OT_SIDE_LEFT, &st->joints);
		st->pinching = ot_hand_pinch((const float(*)[3])st->points, !restart && h->last[side].pinching,
		                             p->pinch_on_m, p->pinch_off_m, &st->pinch_m);
		if (!restart) {
			const double dt = (double)(capture_ns - h->last[side].capture_ns) * 1e-9;
			if (dt > 1e-4) {
				for (int c = 0; c < 3; c++) {
					const float v = (float)((st->joints.pos[OT_HJ_PALM][c] -
					                         h->last[side].joints.pos[OT_HJ_PALM][c]) /
					                        dt);
					st->palm_velocity[c] = 0.5f * v + 0.5f * h->last[side].palm_velocity[c];
				}
			}
		}
		h->have[side] = true;
		h->track[side] = o->track_id;
		h->last[side] = *st;
	}
}
