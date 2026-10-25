// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Image-side geometry around the hand models, see ot_hand_geom.h.
 */

#include "ot_hand_geom.h"

#include <float.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void
ot_hand_anchors(float out[OT_HAND_DET_ANCHORS][2])
{
	// SSD anchors of the full palm model: stride 8 (24 x 24, 2 per cell), then three stride-16 layers
	// sharing one 12 x 12 grid (2 each -> 6 per cell). Fixed anchor size 1: only the centres matter.
	uint32_t n = 0;
	const struct
	{
		uint32_t grid, per_cell;
	} layers[2] = {{24, 2}, {12, 6}};
	for (int l = 0; l < 2; l++) {
		const uint32_t g = layers[l].grid;
		for (uint32_t y = 0; y < g; y++) {
			for (uint32_t x = 0; x < g; x++) {
				for (uint32_t i = 0; i < layers[l].per_cell; i++) {
					out[n][0] = ((float)x + 0.5f) / (float)g;
					out[n][1] = ((float)y + 0.5f) / (float)g;
					n++;
				}
			}
		}
	}
}

static float
box_iou(const float a[4], const float b[4])
{
	const float ix = fmaxf(0.0f, fminf(a[2], b[2]) - fmaxf(a[0], b[0]));
	const float iy = fmaxf(0.0f, fminf(a[3], b[3]) - fmaxf(a[1], b[1]));
	const float inter = ix * iy;
	const float u = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter;
	return u > 0.0f ? inter / u : 0.0f;
}

uint32_t
ot_hand_decode(const float *regressors,
               const float *logits,
               const float anchors[OT_HAND_DET_ANCHORS][2],
               float min_score,
               float nms_iou,
               struct ot_hand_det *out,
               uint32_t max)
{
	enum
	{
		CAND = 64
	};
	struct ot_hand_det cand[CAND];
	uint32_t nc = 0;
	const float size = (float)OT_HAND_DET_SIZE;
	for (uint32_t i = 0; i < OT_HAND_DET_ANCHORS; i++) {
		float lg = logits[i];
		lg = lg < -100.0f ? -100.0f : (lg > 100.0f ? 100.0f : lg);
		const float score = 1.0f / (1.0f + expf(-lg));
		if (score < min_score) {
			continue;
		}
		const float *r = regressors + (size_t)i * OT_HAND_DET_REGRESSORS;
		struct ot_hand_det d;
		d.score = score;
		const float ax = anchors[i][0] * size, ay = anchors[i][1] * size;
		const float cx = r[0] + ax, cy = r[1] + ay, w = r[2], h = r[3];
		d.box[0] = cx - 0.5f * w;
		d.box[1] = cy - 0.5f * h;
		d.box[2] = cx + 0.5f * w;
		d.box[3] = cy + 0.5f * h;
		for (int k = 0; k < OT_HAND_DET_KEYPOINTS; k++) {
			d.kp[k][0] = r[4 + 2 * k] + ax;
			d.kp[k][1] = r[5 + 2 * k] + ay;
		}
		if (nc == CAND && cand[CAND - 1].score >= score) {
			continue;
		}
		uint32_t at = nc < CAND ? nc : CAND - 1;
		while (at > 0 && cand[at - 1].score < score) {
			cand[at] = cand[at - 1];
			at--;
		}
		cand[at] = d;
		if (nc < CAND) {
			nc++;
		}
	}
	uint32_t n = 0;
	for (uint32_t i = 0; i < nc && n < max; i++) {
		bool keep = true;
		for (uint32_t j = 0; j < n; j++) {
			if (box_iou(cand[i].box, out[j].box) >= nms_iou) {
				keep = false;
				break;
			}
		}
		if (keep) {
			out[n++] = cand[i];
		}
	}
	return n;
}

static float
wrap_angle(float a)
{
	while (a > (float)M_PI) {
		a -= 2.0f * (float)M_PI;
	}
	while (a < -(float)M_PI) {
		a += 2.0f * (float)M_PI;
	}
	return a;
}

//! The ROI rotation that makes a -> b point UP in the crop (target angle 90 degrees on screen).
static float
upright_rotation(float ax, float ay, float bx, float by)
{
	// Image +y is down, so the on-screen angle of a -> b is atan2(-(by - ay), bx - ax).
	return wrap_angle(0.5f * (float)M_PI - atan2f(-(by - ay), bx - ax));
}

//! Move @p roi's centre by @p shift × @p h along the crop's +v axis (negative = toward the crop's top).
static void
shift_v(struct ot_roi *roi, float shift, float h)
{
	const float c = cosf(roi->rot), s = sinf(roi->rot);
	// The crop's +v axis in the image is (-sin, cos) (ot_roi_to_affine).
	roi->cx += -s * shift * h;
	roi->cy += c * shift * h;
}

void
ot_hand_roi_from_detection(const struct ot_hand_det *det, const struct ot_affine *det_to_img, struct ot_roi *out)
{
	float x0, y0, x1, y1, ax, ay, bx, by;
	ot_affine_apply(det_to_img, det->box[0], det->box[1], &x0, &y0);
	ot_affine_apply(det_to_img, det->box[2], det->box[3], &x1, &y1);
	ot_affine_apply(det_to_img, det->kp[OT_HAND_KP_WRIST][0], det->kp[OT_HAND_KP_WRIST][1], &ax, &ay);
	ot_affine_apply(det_to_img, det->kp[OT_HAND_KP_MIDDLE_MCP][0], det->kp[OT_HAND_KP_MIDDLE_MCP][1], &bx, &by);
	const float w = x1 - x0, h = y1 - y0;
	out->rot = upright_rotation(ax, ay, bx, by);
	out->cx = 0.5f * (x0 + x1);
	out->cy = 0.5f * (y0 + y1);
	// The palm box covers the palm only: centre half a box toward the fingers, then a square big enough
	// for the whole hand.
	shift_v(out, -0.5f, h);
	out->side = 2.6f * (w > h ? w : h);
}

void
ot_hand_roi_from_landmarks(const float (*pts)[3], struct ot_roi *out)
{
	// Rotation: wrist -> the mean of (the mean of the index and ring knuckles) and the middle knuckle.
	const float mx = 0.5f * (0.5f * (pts[OT_HL_INDEX_MCP][0] + pts[OT_HL_RING_MCP][0]) + pts[OT_HL_MIDDLE_MCP][0]);
	const float my = 0.5f * (0.5f * (pts[OT_HL_INDEX_MCP][1] + pts[OT_HL_RING_MCP][1]) + pts[OT_HL_MIDDLE_MCP][1]);
	const float rot = upright_rotation(pts[OT_HL_WRIST][0], pts[OT_HL_WRIST][1], mx, my);
	// The palm and the lower finger joints: stable under finger motion.
	static const int k_part[] = {OT_HL_WRIST,     OT_HL_THUMB_CMC, OT_HL_THUMB_MCP,  OT_HL_THUMB_IP,
	                             OT_HL_INDEX_MCP, OT_HL_INDEX_PIP, OT_HL_MIDDLE_MCP, OT_HL_MIDDLE_PIP,
	                             OT_HL_RING_MCP,  OT_HL_RING_PIP,  OT_HL_PINKY_MCP,  OT_HL_PINKY_PIP};
	const float c = cosf(rot), s = sinf(rot);
	float minu = FLT_MAX, minv = FLT_MAX, maxu = -FLT_MAX, maxv = -FLT_MAX;
	for (size_t i = 0; i < sizeof(k_part) / sizeof(k_part[0]); i++) {
		const float x = pts[k_part[i]][0], y = pts[k_part[i]][1];
		const float u = c * x + s * y;
		const float v = -s * x + c * y;
		minu = fminf(minu, u);
		maxu = fmaxf(maxu, u);
		minv = fminf(minv, v);
		maxv = fmaxf(maxv, v);
	}
	const float mu = 0.5f * (minu + maxu), mv = 0.5f * (minv + maxv);
	const float w = maxu - minu, h = maxv - minv;
	out->rot = rot;
	out->cx = c * mu - s * mv;
	out->cy = s * mu + c * mv;
	shift_v(out, -0.1f, h);
	out->side = 2.0f * (w > h ? w : h);
}

static void
roi_aabb(const struct ot_roi *r, float out[4])
{
	// A rotated square's axis-aligned extent: half side × (|cos| + |sin|) each way.
	const float e = 0.5f * r->side * (fabsf(cosf(r->rot)) + fabsf(sinf(r->rot)));
	out[0] = r->cx - e;
	out[1] = r->cy - e;
	out[2] = r->cx + e;
	out[3] = r->cy + e;
}

float
ot_roi_overlap(const struct ot_roi *a, const struct ot_roi *b)
{
	float ba[4], bb[4];
	roi_aabb(a, ba);
	roi_aabb(b, bb);
	return box_iou(ba, bb);
}
