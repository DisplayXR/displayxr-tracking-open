// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The hand's 3D side (pure, unit-tested): the landmark model's metric
 *         hand placed along the observed pixel rays (a monocular depth
 *         estimate from hand size), handedness from the 3D shape, and the 21
 *         MediaPipe landmarks -> the 26 joints of XR_EXT_hand_tracking.
 *
 * Frames: the camera frame is the face role's (ot_eyes.h): +X the image's
 * left (the user's right for a camera facing the user), +Y up, +Z out of the
 * camera toward the user, metres. Display space has the same axes (the
 * camera's pose maps one to the other). Every function here works in any
 * right-handed frame unless it says otherwise.
 */

#pragma once

#include "ot_eyes.h"
#include "ot_hand_geom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The model's "world" landmarks (metres, origin near the hand's centre, axes
 * of the CROP: +x right, +y down, +z away from the camera) -> the camera
 * frame, undoing the crop's rotation @p roi_rot (ot_roi::rot).
 */
void
ot_hand_world_to_camera(const float (*world)[3], float roi_rot, float (*out)[3]);

/*!
 * Place the metric hand @p shape (camera frame, from ot_hand_world_to_camera,
 * already scaled by the user's hand-size factor) in front of the camera: the
 * translation T that puts every shape point + T closest to its landmark's
 * pixel ray (linear least squares), then each landmark = the point of its own
 * ray at the depth of shape + T, so the result reprojects exactly onto the
 * 2D landmarks. The depth is only as good as the hand-size prior: a hand 10 %
 * larger than the model's average reads 10 % farther.
 *
 * @p px: the 21 landmarks in image px (x, y; z ignored). False when the rays
 * do not determine a depth (degenerate) or the hand lands behind the camera.
 */
bool
ot_hand_place(
    const struct ot_intrinsics *k, const float (*px)[3], const float (*shape)[3], float (*out)[3], float out_t[3]);

/*!
 * Handedness from a 3D hand (any right-handed frame): +1 right, -1 left, 0
 * when the shape cannot tell (a flat hand). The palm's normal from the
 * knuckles (index × little, about the wrist) points to the palm side for a
 * right hand and to the back for a left one; the palm side is where the
 * fingers bend (their joints sit off the knuckle plane toward the palm).
 * A cross-check of the model's handedness output, not the decision.
 */
int
ot_hand_chirality(const float (*p)[3]);


/*
 *
 * XR_EXT_hand_tracking's 26 joints.
 *
 */

//! XrHandJointEXT's order (identical to the runtime's `enum xrt_hand_joint`).
enum ot_hand_joint
{
	OT_HJ_PALM = 0,
	OT_HJ_WRIST = 1,
	OT_HJ_THUMB_METACARPAL = 2,
	OT_HJ_THUMB_PROXIMAL = 3,
	OT_HJ_THUMB_DISTAL = 4,
	OT_HJ_THUMB_TIP = 5,
	OT_HJ_INDEX_METACARPAL = 6, //!< then PROXIMAL, INTERMEDIATE, DISTAL, TIP; MIDDLE, RING, LITTLE likewise
	OT_HJ_MIDDLE_METACARPAL = 11,
	OT_HJ_RING_METACARPAL = 16,
	OT_HJ_LITTLE_METACARPAL = 21,
	OT_HJ_LITTLE_TIP = 25,
	OT_HJ_COUNT = 26,
};

/*!
 * Where a finger's metacarpal joint (its carpometacarpal joint, which the
 * 21-point model does not output) sits on the wrist -> knuckle line: about a
 * quarter of the way from the wrist landmark (metacarpal ≈ 65 mm of ≈ 85 mm).
 */
#define OT_HAND_METACARPAL_FRACTION 0.25f

//! The nominal wrist -> middle-knuckle length the joint radii are scaled from.
#define OT_HAND_NOMINAL_SIZE_M 0.095f

struct ot_hand_joints
{
	float pos[OT_HJ_COUNT][3];
	float rot[OT_HJ_COUNT][4]; //!< x, y, z, w
	float radius[OT_HJ_COUNT]; //!< metres, joint centre to skin
};

//! Wrist -> middle knuckle distance, metres.
float
ot_hand_size_m(const float (*p)[3]);

/*!
 * The 26 joints from the 21 landmarks @p p (display space, metres).
 *
 * Positions: the 20 measured joints as they are (thumb CMC / MCP / IP / tip
 * -> thumb metacarpal / proximal / distal / tip; each finger's MCP / PIP / DIP
 * / tip -> proximal / intermediate / distal / tip, wrist -> wrist). Synthesised:
 * each finger's metacarpal at @ref OT_HAND_METACARPAL_FRACTION of wrist ->
 * knuckle, the palm at the middle of the middle finger's metacarpal bone.
 *
 * Orientation, per the extension: -Z along the bone toward the fingertip (a
 * joint takes the bone that starts at it; a tip takes the last bone; wrist and
 * palm point at the middle knuckle), +Y out of the back of the hand
 * (orthogonalised against Z), X = Y × Z. The back of the hand comes from the
 * knuckle plane and @p is_left. The thumb uses the hand's back as its up
 * vector too, an approximation (its nail faces partly sideways).
 *
 * Radii: a nominal adult hand's, scaled by ot_hand_size_m / nominal (clamped
 * 0.7..1.4).
 */
void
ot_hand_joints_from_landmarks(const float (*p)[3], bool is_left, struct ot_hand_joints *out);

/*!
 * Thumb-tip to index-tip distance with hysteresis: pinching starts below
 * @p on_m and ends above @p off_m. Returns the new state.
 */
bool
ot_hand_pinch(const float (*p)[3], bool was_pinching, float on_m, float off_m, float *out_distance_m);

#ifdef __cplusplus
}
#endif
