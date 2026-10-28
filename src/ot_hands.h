// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The hands role: one hand-tracker result -> the left and right hands
 *         in display space (pure, unit-tested; the provider adds threads and
 *         the runtime's device objects).
 *
 * Per analysed frame:
 *  1. which observed hand is the user's left / right (the model's handedness
 *     averaged over the track; with two hands the more "right" one is right);
 *  2. each hand's 21 points in the camera frame: the model's metric hand
 *     (× the user's hand-size factor) placed along the pixel rays
 *     (ot_hand_place) — the depth is an estimate from hand size;
 *  3. display = T + R · p, the camera's pose as the runtime answers it
 *     (`get_camera_pose`; its focal scale multiplies the focal length);
 *  4. a One Euro filter per coordinate (display space), reset on a new track;
 *  5. the 26 XR_EXT_hand_tracking joints, the palm's velocity, the pinch.
 * Display space: origin at the display centre, +X right, +Y up, +Z toward the
 * viewer, metres — the runtime's RIG_LOCAL frame (input-provider spec §4b.4).
 */

#pragma once

#include "ot_eyes.h"
#include "ot_hand_pose.h"
#include "ot_hand_tracker.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ot_side
{
	OT_SIDE_LEFT = 0,
	OT_SIDE_RIGHT = 1,
};

//! The camera's pose in display space (the runtime's answer, or the top-centre prior).
struct ot_cam_pose
{
	float t[3];
	float q[4];        //!< x, y, z, w
	float focal_scale; //!< 1 = none
	uint32_t source;   //!< XRT_CAMERA_POSE_SOURCE_*; 0 = the provider's own fallback
	uint64_t generation;
};

struct ot_hands_params
{
	float hand_scale;    //!< the user's hand size relative to the model's average (1)
	float pinch_on_m;    //!< 0.025
	float pinch_off_m;   //!< 0.035
	float min_cutoff_hz; //!< One Euro, 1.0
	float beta;          //!< per m/s, 3.0
	float d_cutoff_hz;   //!< 1.0
	float max_distance_m; //!< hands whose wrist is farther from the camera are not the user's (0.8; 0 = any)
};

void
ot_hands_params_defaults(struct ot_hands_params *p);

//! One hand, as published.
struct ot_hand_state
{
	bool active;
	int64_t capture_ns;
	uint32_t track_id;
	float confidence;             //!< the landmark presence
	float right_mean;             //!< the handedness evidence it was assigned by
	struct ot_hand_joints joints; //!< display space
	float palm_velocity[3];       //!< m/s, display space (0 on the first frame of a track)
	bool pinching;
	float pinch_m;
	float depth_m;                     //!< camera -> wrist distance (the monocular estimate)
	float points[OT_HAND_LM_COUNT][3]; //!< the 21 filtered points, display space
};

struct ot_hands
{
	struct ot_one_euro f[2][OT_HAND_LM_COUNT * 3];
	uint32_t track[2];
	bool have[2];
	struct ot_hand_state last[2];
};

void
ot_hands_reset(struct ot_hands *h);

/*!
 * Which of @p r's hands is the left and which the right: @p out[OT_SIDE_*] =
 * the index into r->hands, or -1. Two hands: the larger right_mean is right
 * (each side once). One hand: right when right_mean >= 0.5.
 */
void
ot_hands_assign(const struct ot_hand_result *r, int out[2]);

/*!
 * One analysed frame -> both sides. A side without a hand this frame comes
 * out inactive (and its filter restarts with the next track).
 */
void
ot_hands_update(struct ot_hands *h,
                const struct ot_intrinsics *k,
                const struct ot_hands_params *p,
                const struct ot_cam_pose *pose,
                const struct ot_hand_result *r,
                int64_t capture_ns,
                struct ot_hand_state out[2]);

/*!
 * The camera -> wrist distance of @p o, placed as ot_hands_update places it.
 * False when it cannot be placed.
 */
bool
ot_hands_distance(const struct ot_intrinsics *k,
                  const struct ot_hands_params *p,
                  const struct ot_cam_pose *pose,
                  const struct ot_hand_obs *o,
                  float *out_m);

//! What the tracker's admit callback needs (ot_hands_admit).
struct ot_hands_admit
{
	const struct ot_intrinsics *k;
	const struct ot_hands_params *p;
	const struct ot_cam_pose *pose;
	float last_rejected_m; //!< the distance of the newest hand turned away
};

/*!
 * The tracker's admit callback (@p user = struct ot_hands_admit): a hand is
 * the user's unless it is placed farther than p->max_distance_m — a person
 * walking past behind the user, a hand on a poster. A hand that cannot be
 * placed is admitted (ot_hands_update drops it).
 */
bool
ot_hands_admit(void *user, const struct ot_hand_obs *o);

//! Camera -> display: T + R · p.
void
ot_cam_to_display(const struct ot_cam_pose *pose, const float p[3], float out[3]);

#ifdef __cplusplus
}
#endif
