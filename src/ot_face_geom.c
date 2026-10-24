// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Image-side geometry around the face models, see ot_face_geom.h.
 */

#include "ot_face_geom.h"
#include "ot_landmarks.h"

#include <float.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void
ot_affine_apply(const struct ot_affine *m, float u, float v, float *x, float *y)
{
	*x = m->a * u + m->b * v + m->c;
	*y = m->d * u + m->e * v + m->f;
}

void
ot_roi_to_affine(const struct ot_roi *roi, uint32_t size, struct ot_affine *out)
{
	const float k = roi->side / (float)size;
	const float c = cosf(roi->rot), s = sinf(roi->rot);
	// Crop (u, v) about its centre, scaled, rotated, then moved to the ROI centre.
	out->a = c * k;
	out->b = -s * k;
	out->d = s * k;
	out->e = c * k;
	const float h = 0.5f * (float)size;
	out->c = roi->cx - (out->a * h + out->b * h);
	out->f = roi->cy - (out->d * h + out->e * h);
}

void
ot_letterbox_affine(uint32_t img_w, uint32_t img_h, uint32_t size, struct ot_affine *out)
{
	const float big = (float)(img_w > img_h ? img_w : img_h);
	const float k = big / (float)size; // crop px -> image px
	memset(out, 0, sizeof(*out));
	out->a = k;
	out->e = k;
	out->c = 0.5f * ((float)img_w - big);
	out->f = 0.5f * ((float)img_h - big);
}

static inline void
fetch(const struct ot_image *img, int x, int y, float rgb[3])
{
	if (img->mirrored) {
		x = (int)img->width - 1 - x;
	}
	const uint8_t *p = img->data + (size_t)y * img->stride;
	if (img->format == OT_FMT_BGRA) {
		p += (size_t)x * 4u;
		rgb[0] = p[2];
		rgb[1] = p[1];
		rgb[2] = p[0];
	} else {
		p += (size_t)x * 3u;
		rgb[0] = p[0];
		rgb[1] = p[1];
		rgb[2] = p[2];
	}
}

void
ot_sample_affine(const struct ot_image *img,
                 const struct ot_affine *m,
                 uint32_t w,
                 uint32_t h,
                 float scale,
                 float bias,
                 bool zero_pad,
                 float *nhwc)
{
	const int iw = (int)img->width, ih = (int)img->height;
	const float k = scale / 255.0f;
	for (uint32_t v = 0; v < h; v++) {
		for (uint32_t u = 0; u < w; u++) {
			float x, y;
			// Pixel centre in, pixel-index coordinates out.
			ot_affine_apply(m, (float)u + 0.5f, (float)v + 0.5f, &x, &y);
			x -= 0.5f;
			y -= 0.5f;
			float *o = nhwc + ((size_t)v * w + u) * 3u;
			const bool outside = x < -0.5f || y < -0.5f || x > (float)iw - 0.5f || y > (float)ih - 0.5f;
			if (zero_pad && outside) {
				o[0] = o[1] = o[2] = bias;
				continue;
			}
			if (x < 0.0f) {
				x = 0.0f;
			}
			if (y < 0.0f) {
				y = 0.0f;
			}
			if (x > (float)(iw - 1)) {
				x = (float)(iw - 1);
			}
			if (y > (float)(ih - 1)) {
				y = (float)(ih - 1);
			}
			const int x0 = (int)x, y0 = (int)y;
			const int x1 = x0 + 1 < iw ? x0 + 1 : x0, y1 = y0 + 1 < ih ? y0 + 1 : y0;
			const float fx = x - (float)x0, fy = y - (float)y0;
			float p00[3], p01[3], p10[3], p11[3];
			fetch(img, x0, y0, p00);
			fetch(img, x1, y0, p01);
			fetch(img, x0, y1, p10);
			fetch(img, x1, y1, p11);
			for (int ch = 0; ch < 3; ch++) {
				const float top = p00[ch] + (p01[ch] - p00[ch]) * fx;
				const float bot = p10[ch] + (p11[ch] - p10[ch]) * fx;
				o[ch] = (top + (bot - top) * fy) * k + bias;
			}
		}
	}
}

void
ot_face_anchors(float out[OT_FACE_DET_ANCHORS][2])
{
	// SSD anchors of the short-range model: a 16x16 grid (stride 8) with 2 anchors per cell, then
	// three stride-16 layers sharing one 8x8 grid (2 anchors each per layer -> 6 per cell). Fixed
	// anchor size 1: the regressors are absolute offsets, so only the centres matter.
	uint32_t n = 0;
	const struct
	{
		uint32_t grid, per_cell;
	} layers[2] = {{16, 2}, {8, 6}};
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
iou(const float a[4], const float b[4])
{
	const float ix = fmaxf(0.0f, fminf(a[2], b[2]) - fmaxf(a[0], b[0]));
	const float iy = fmaxf(0.0f, fminf(a[3], b[3]) - fmaxf(a[1], b[1]));
	const float inter = ix * iy;
	const float u = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter;
	return u > 0.0f ? inter / u : 0.0f;
}

uint32_t
ot_face_decode(const float *regressors,
               const float *logits,
               const float anchors[OT_FACE_DET_ANCHORS][2],
               float min_score,
               float nms_iou,
               struct ot_face_det *out,
               uint32_t max)
{
	// Candidates above the threshold, best first (insertion into a small sorted buffer).
	enum
	{
		CAND = 64
	};
	struct ot_face_det cand[CAND];
	uint32_t nc = 0;
	const float size = (float)OT_FACE_DET_SIZE;
	for (uint32_t i = 0; i < OT_FACE_DET_ANCHORS; i++) {
		float lg = logits[i];
		if (lg < -100.0f) {
			lg = -100.0f;
		}
		if (lg > 100.0f) {
			lg = 100.0f;
		}
		const float score = 1.0f / (1.0f + expf(-lg));
		if (score < min_score) {
			continue;
		}
		const float *r = regressors + (size_t)i * 16u;
		struct ot_face_det d;
		d.score = score;
		const float ax = anchors[i][0] * size, ay = anchors[i][1] * size;
		const float cx = r[0] + ax, cy = r[1] + ay, w = r[2], h = r[3];
		d.box[0] = cx - 0.5f * w;
		d.box[1] = cy - 0.5f * h;
		d.box[2] = cx + 0.5f * w;
		d.box[3] = cy + 0.5f * h;
		for (int k = 0; k < OT_FACE_DET_KEYPOINTS; k++) {
			d.kp[k][0] = r[4 + 2 * k] + ax;
			d.kp[k][1] = r[5 + 2 * k] + ay;
		}
		uint32_t at = nc < CAND ? nc : CAND - 1;
		if (nc == CAND && cand[CAND - 1].score >= score) {
			continue;
		}
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
			if (iou(cand[i].box, out[j].box) >= nms_iou) {
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

//! The rotation that makes the vector a -> b point along the crop's +x (a level eye line).
static float
level_rotation(float ax, float ay, float bx, float by)
{
	// Image +y is down, so the on-screen angle of a -> b is atan2(-(by - ay), bx - ax); the crop is
	// rotated by its negative (target 0).
	return wrap_angle(-atan2f(-(by - ay), bx - ax));
}

void
ot_roi_from_detection(const struct ot_face_det *det, const struct ot_affine *det_to_img, struct ot_roi *out)
{
	float x0, y0, x1, y1, ax, ay, bx, by;
	ot_affine_apply(det_to_img, det->box[0], det->box[1], &x0, &y0);
	ot_affine_apply(det_to_img, det->box[2], det->box[3], &x1, &y1);
	ot_affine_apply(det_to_img, det->kp[OT_FACE_KP_RIGHT_EYE][0], det->kp[OT_FACE_KP_RIGHT_EYE][1], &ax, &ay);
	ot_affine_apply(det_to_img, det->kp[OT_FACE_KP_LEFT_EYE][0], det->kp[OT_FACE_KP_LEFT_EYE][1], &bx, &by);
	out->rot = level_rotation(ax, ay, bx, by);
	out->cx = 0.5f * (x0 + x1);
	out->cy = 0.5f * (y0 + y1);
	const float w = x1 - x0, h = y1 - y0;
	out->side = 1.5f * (w > h ? w : h);
}

void
ot_roi_from_landmarks(const float (*pts)[3], uint32_t count, struct ot_roi *out)
{
	const float rot = level_rotation(pts[OT_LM_RIGHT_EYE_OUTER][0], pts[OT_LM_RIGHT_EYE_OUTER][1],
	                                 pts[OT_LM_LEFT_EYE_OUTER][0], pts[OT_LM_LEFT_EYE_OUTER][1]);
	// Bounding box in the crop's frame: rotate every point by -rot about the origin.
	const float c = cosf(rot), s = sinf(rot);
	float minu = FLT_MAX, minv = FLT_MAX, maxu = -FLT_MAX, maxv = -FLT_MAX;
	for (uint32_t i = 0; i < count; i++) {
		const float x = pts[i][0], y = pts[i][1];
		const float u = c * x + s * y;
		const float v = -s * x + c * y;
		minu = fminf(minu, u);
		maxu = fmaxf(maxu, u);
		minv = fminf(minv, v);
		maxv = fmaxf(maxv, v);
	}
	const float mu = 0.5f * (minu + maxu), mv = 0.5f * (minv + maxv);
	out->rot = rot;
	out->cx = c * mu - s * mv;
	out->cy = s * mu + c * mv;
	const float w = maxu - minu, h = maxv - minv;
	out->side = 1.5f * (w > h ? w : h);
}
