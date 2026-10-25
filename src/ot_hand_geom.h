// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The image-side geometry around the two hand models: palm detector
 *         anchors + decoding + NMS, and the rotated region of interest the
 *         hand-landmark model runs on (from a palm detection, or from the
 *         previous frame's landmarks). Pure C, no OS calls: unit-tested.
 *
 * Written from the public MediaPipe hand model card and solution
 * documentation (anchor layout, ROI rules), not from any other implementation.
 * Coordinates are the face role's (ot_face_geom.h): continuous pixels of the
 * logical, un-mirrored image, +x right, +y down.
 */

#pragma once

#include "ot_face_geom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * The 21 hand landmarks (MediaPipe order).
 *
 */

#define OT_HAND_LM_COUNT 21

enum ot_hand_lm
{
	OT_HL_WRIST = 0,
	OT_HL_THUMB_CMC = 1,
	OT_HL_THUMB_MCP = 2,
	OT_HL_THUMB_IP = 3,
	OT_HL_THUMB_TIP = 4,
	OT_HL_INDEX_MCP = 5,
	OT_HL_INDEX_PIP = 6,
	OT_HL_INDEX_DIP = 7,
	OT_HL_INDEX_TIP = 8,
	OT_HL_MIDDLE_MCP = 9,
	OT_HL_MIDDLE_PIP = 10,
	OT_HL_MIDDLE_DIP = 11,
	OT_HL_MIDDLE_TIP = 12,
	OT_HL_RING_MCP = 13,
	OT_HL_RING_PIP = 14,
	OT_HL_RING_DIP = 15,
	OT_HL_RING_TIP = 16,
	OT_HL_PINKY_MCP = 17,
	OT_HL_PINKY_PIP = 18,
	OT_HL_PINKY_DIP = 19,
	OT_HL_PINKY_TIP = 20,
};


/*
 *
 * Palm detector (full, 192 × 192): 2016 SSD anchors, 18 regressors each
 * (box centre + size, then 7 keypoints).
 *
 */

#define OT_HAND_DET_SIZE 192
#define OT_HAND_DET_ANCHORS 2016
#define OT_HAND_DET_REGRESSORS 18
#define OT_HAND_DET_KEYPOINTS 7
#define OT_HAND_DET_MAX 8

//! The two palm-detection keypoints the ROI uses: the wrist centre and the middle finger's knuckle.
enum ot_hand_det_kp
{
	OT_HAND_KP_WRIST = 0,
	OT_HAND_KP_MIDDLE_MCP = 2,
};

struct ot_hand_det
{
	float score;
	float box[4];                       //!< xmin, ymin, xmax, ymax (detector input px, 0..192)
	float kp[OT_HAND_DET_KEYPOINTS][2]; //!< detector input px
};

//! Anchor centres (normalised 0..1): a 24×24 grid (stride 8) with 2 per cell, then a 12×12 grid with 6 per cell.
void
ot_hand_anchors(float out[OT_HAND_DET_ANCHORS][2]);

/*!
 * Decode the raw outputs (regressors [2016][18], logits [2016]) into at most
 * @p max palms with score >= @p min_score, after non-maximum suppression at
 * IoU @p nms_iou; sorted by score. Returns the count.
 */
uint32_t
ot_hand_decode(const float *regressors,
               const float *logits,
               const float anchors[OT_HAND_DET_ANCHORS][2],
               float min_score,
               float nms_iou,
               struct ot_hand_det *out,
               uint32_t max);

/*!
 * The landmark model's ROI from a palm detection, in image px (MediaPipe's
 * palm-to-hand rule): rotation from the wrist to the middle-finger knuckle
 * keypoint, so that line points UP in the crop; the box centre moved by half
 * the box height toward the fingers; a square of the box's long side × 2.6.
 * @p det_to_img maps detector input px to image px (the letterbox).
 */
void
ot_hand_roi_from_detection(const struct ot_hand_det *det, const struct ot_affine *det_to_img, struct ot_roi *out);

/*!
 * The next frame's ROI from this frame's 21 landmarks (tracking mode):
 * rotation from the wrist to the middle of the index / middle / ring
 * knuckles (fingers up in the crop), the bounding box of the palm and lower
 * finger landmarks in that rotated frame, moved by 0.1 × its height toward the
 * fingers, square of its long side × 2.
 */
void
ot_hand_roi_from_landmarks(const float (*pts)[3], struct ot_roi *out);

//! Overlap of two ROIs (IoU of their axis-aligned bounding boxes), 0..1: "the same hand".
float
ot_roi_overlap(const struct ot_roi *a, const struct ot_roi *b);

#ifdef __cplusplus
}
#endif
