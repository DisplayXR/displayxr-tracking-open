// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo on a RECTIFIED pair (pure, unit-tested): the disparity of a
 *         few palm points, and from it the hand-size factor that makes the
 *         monocular depth agree with the pair.
 *
 * Why a scale and not a per-frame stereo depth: the hands role places the
 * landmark model's metric hand along the pixel rays, so its depth is exact up
 * to one unknown, the user's hand size (`HAND_SCALE`, a population prior that
 * is ~±6–10 % off for most adults: 2.5–4 cm at 40 cm). A rectified pair
 * measures that depth directly, but a per-landmark disparity on a hand is noisy
 * (fingers against the background, motion blur, a monochrome texture). So the
 * pair measures the RATIO of the stereo depth to the placed depth on the
 * palm's well-textured interior points, the median over them, and the provider
 * folds it slowly into the hand-size factor (ot_stereo_scale_update). The
 * frame-to-frame motion stays the monocular fit's (smooth, exact in 2D); its
 * metric scale comes from the pair.
 *
 * Rectified means: same focal length and principal point in both eyes, rows
 * aligned, the right eye translated by the baseline along the left eye's image
 * +x. A point at depth Z (along the optical axis) is seen at u_left in the left
 * image and at u_right = u_left - f·B/Z in the right one, on the same row.
 */

#pragma once

#include "ot_eyes.h"
#include "ot_face_geom.h"
#include "ot_hand_geom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A match needs this normalised cross-correlation (of the best disparity).
#define OT_STEREO_MIN_NCC 0.8f
//! ...and a patch with at least this grey-level standard deviation (texture).
#define OT_STEREO_MIN_STD 3.0f
//! The ratio is reported only from this many matched palm points.
#define OT_STEREO_MIN_POINTS 3u
//! The hand-size factor the pair may settle on (a hand 40 % smaller to 60 % larger than the model's average).
#define OT_STEREO_SCALE_MIN 0.6f
#define OT_STEREO_SCALE_MAX 1.6f

/*!
 * The disparity d (px, u_left - u_right) of left-image pixel (@p u, @p v):
 * normalised cross-correlation of a (2 @p radius + 1)² patch along the same
 * row of @p right, every integer d in [@p d_min, @p d_max], then a parabola
 * through the best one and its neighbours. Any pixel format (grey is read
 * directly, colour as luma). The range is cut to the disparities whose patch
 * lies inside the right image. False when the patch leaves the left image, has
 * no texture, the best score is under OT_STEREO_MIN_NCC, or the best d is at an
 * end of the (cut) range (the true one may lie outside it).
 */
bool
ot_stereo_disparity(const struct ot_image *left,
                    const struct ot_image *right,
                    float u,
                    float v,
                    float d_min,
                    float d_max,
                    int radius,
                    float *out_d,
                    float *out_ncc);

/*!
 * One hand: the median, over its palm points that match (wrist, the thumb's
 * base and the four knuckles), of stereo depth / placed depth.
 *
 * @p lm: the 21 landmarks in left-image px (x, y). @p cam: the same 21 placed
 * in the camera frame (ot_hands_place_camera; +Z along the optical axis), so
 * cam[i][2] is each one's monocular depth. @p focal_px: the rectified focal
 * length; @p baseline_m: the pair's baseline. The search covers the depths the
 * clamp OT_STEREO_SCALE_MIN..MAX allows around each placed one. Returns false
 * with fewer than OT_STEREO_MIN_POINTS matches; *@p out_points = how many matched.
 */
bool
ot_stereo_hand_ratio(const struct ot_image *left,
                     const struct ot_image *right,
                     float focal_px,
                     float baseline_m,
                     const float (*lm)[3],
                     const float (*cam)[3],
                     float *out_ratio,
                     uint32_t *out_points);

/*!
 * Fold one measured @p ratio into the hand-size factor @p scale: a step of
 * @p gain (0..1) toward scale · ratio in log space, clamped to
 * OT_STEREO_SCALE_MIN..MAX. With gain 0.1 at 30 analysed frames per second
 * the factor settles in about a second and a single bad frame moves it by a
 * fraction of its error.
 */
float
ot_stereo_scale_update(float scale, float ratio, float gain);

#ifdef __cplusplus
}
#endif
