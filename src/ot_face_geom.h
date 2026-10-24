// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The image-side geometry around the two face models: images and
 *         affine crops, BlazeFace anchors + decoding + NMS, and the rotated
 *         region of interest (from a detection, or from the previous frame's
 *         landmarks). Pure C, no OS calls: unit-tested.
 *
 * Written from the public MediaPipe model cards and solution documentation
 * (anchor layout, ROI rules), not from any other implementation.
 *
 * Coordinates: continuous pixel coordinates of the LOGICAL image — the
 * camera's own, un-mirrored view — with pixel i covering [i, i+1), +x right,
 * +y down. A mirrored source is un-mirrored by the sampler
 * (@ref ot_image::mirrored), so everything above it works in one convention.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ot_pixel_format
{
	OT_FMT_BGRA = 0, //!< 4 bytes per pixel, B G R A (AVFoundation's 32BGRA)
	OT_FMT_RGB = 1,  //!< 3 bytes per pixel
};

struct ot_image
{
	const uint8_t *data;
	uint32_t width, height;
	uint32_t stride; //!< bytes per row
	enum ot_pixel_format format;
	bool mirrored; //!< the source delivers a mirrored (selfie) view; samplers un-mirror it
};

/*!
 * Maps crop coordinates (u, v) to image coordinates (x, y):
 * x = a u + b v + c, y = d u + e v + f.
 */
struct ot_affine
{
	float a, b, c, d, e, f;
};

/*! A rotated square region of the image. */
struct ot_roi
{
	float cx, cy; //!< centre, image px
	float side;   //!< side length, image px
	float rot;    //!< radians; the crop's +u axis is the image's +x rotated by rot (clockwise on screen, +y down)
};

void
ot_affine_apply(const struct ot_affine *m, float u, float v, float *x, float *y);

//! The crop of @p roi resampled to @p size × @p size.
void
ot_roi_to_affine(const struct ot_roi *roi, uint32_t size, struct ot_affine *out);

//! The whole image letterboxed (aspect kept, centred, zero padding) into @p size × @p size.
void
ot_letterbox_affine(uint32_t img_w, uint32_t img_h, uint32_t size, struct ot_affine *out);

/*!
 * Resample @p img through @p m into an NHWC float tensor of @p w × @p h × 3
 * (R, G, B), value = byte / 255 × @p scale + @p bias. Bilinear. Outside the
 * image: @p zero_pad ? black : the nearest edge pixel.
 */
void
ot_sample_affine(const struct ot_image *img,
                 const struct ot_affine *m,
                 uint32_t w,
                 uint32_t h,
                 float scale,
                 float bias,
                 bool zero_pad,
                 float *nhwc);


/*
 *
 * BlazeFace (short range, 128 × 128): 896 SSD anchors, 16 regressors each.
 *
 */

#define OT_FACE_DET_SIZE 128
#define OT_FACE_DET_ANCHORS 896
#define OT_FACE_DET_KEYPOINTS 6
#define OT_FACE_DET_MAX 8

//! Keypoints of a detection, in the SUBJECT's terms (MediaPipe's order).
enum ot_face_kp
{
	OT_FACE_KP_RIGHT_EYE = 0,
	OT_FACE_KP_LEFT_EYE = 1,
	OT_FACE_KP_NOSE = 2,
	OT_FACE_KP_MOUTH = 3,
	OT_FACE_KP_RIGHT_EAR = 4,
	OT_FACE_KP_LEFT_EAR = 5,
};

struct ot_face_det
{
	float score;
	float box[4];                        //!< xmin, ymin, xmax, ymax (detector input px, 0..128)
	float kp[OT_FACE_DET_KEYPOINTS][2]; //!< detector input px
};

//! Anchor centres (normalised 0..1), @ref OT_FACE_DET_ANCHORS of them: strides 8, 16, 16, 16; two per cell.
void
ot_face_anchors(float out[OT_FACE_DET_ANCHORS][2]);

/*!
 * Decode the raw outputs (regressors [896][16], logits [896]) into at most
 * @p max detections with score >= @p min_score, after non-maximum
 * suppression at IoU @p nms_iou; sorted by score. Returns the count.
 */
uint32_t
ot_face_decode(const float *regressors,
               const float *logits,
               const float anchors[OT_FACE_DET_ANCHORS][2],
               float min_score,
               float nms_iou,
               struct ot_face_det *out,
               uint32_t max);

/*!
 * The landmark model's ROI from a detection, in image px: rotation from the
 * right-eye → left-eye keypoints (target 0, i.e. eyes level in the crop),
 * square of the box's long side × 1.5, centred on the box. @p det_to_img maps
 * detector input px to image px (the letterbox).
 */
void
ot_roi_from_detection(const struct ot_face_det *det, const struct ot_affine *det_to_img, struct ot_roi *out);

/*!
 * The next frame's ROI from this frame's landmarks (tracking mode): rotation
 * from the right eye's outer corner (33) to the left eye's (263), the
 * landmarks' bounding box in that rotated frame, square of its long side ×
 * 1.5, centred on it.
 */
void
ot_roi_from_landmarks(const float (*pts)[3], uint32_t count, struct ot_roi *out);

#ifdef __cplusplus
}
#endif
