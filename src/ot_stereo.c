// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo on a rectified pair, see ot_stereo.h.
 */

#include "ot_stereo.h"

#include <math.h>
#include <stdlib.h>

static inline float
grey(const struct ot_image *img, int x, int y)
{
	const uint8_t *p = img->data + (size_t)y * img->stride + (size_t)x * ot_pixel_bytes(img->format);
	switch (img->format) {
	case OT_FMT_GRAY8: return (float)p[0];
	case OT_FMT_BGRA: return 0.299f * p[2] + 0.587f * p[1] + 0.114f * p[0];
	default: return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
	}
}

static bool
inside(const struct ot_image *img, int x0, int y0, int x1, int y1)
{
	return x0 >= 0 && y0 >= 0 && x1 < (int)img->width && y1 < (int)img->height;
}

bool
ot_stereo_disparity(const struct ot_image *left,
                    const struct ot_image *right,
                    float u,
                    float v,
                    float d_min,
                    float d_max,
                    int radius,
                    float *out_d,
                    float *out_ncc)
{
	if (left->mirrored || right->mirrored || radius < 1 || radius > 16 || !(d_max > d_min) || d_min < 0.0f) {
		return false;
	}
	const int cx = (int)floorf(u + 0.5f), cy = (int)floorf(v + 0.5f);
	int lo = (int)ceilf(d_min), hi = (int)floorf(d_max);
	// The part of the range whose patch lies inside the right image (cx - d ± radius).
	if (hi > cx - radius) {
		hi = cx - radius;
	}
	if (lo < cx + radius - ((int)right->width - 1)) {
		lo = cx + radius - ((int)right->width - 1);
	}
	if (hi - lo < 2 || !inside(left, cx - radius, cy - radius, cx + radius, cy + radius) ||
	    !inside(right, cx - hi - radius, cy - radius, cx - lo + radius, cy + radius)) {
		return false;
	}
	// The left patch, zero-mean.
	enum
	{
		MAXN = 33 * 33
	};
	float a[MAXN];
	const int side = 2 * radius + 1, n = side * side;
	double mean = 0;
	for (int y = 0; y < side; y++) {
		for (int x = 0; x < side; x++) {
			a[y * side + x] = grey(left, cx - radius + x, cy - radius + y);
			mean += a[y * side + x];
		}
	}
	mean /= n;
	double aa = 0;
	for (int i = 0; i < n; i++) {
		a[i] -= (float)mean;
		aa += (double)a[i] * a[i];
	}
	if (sqrt(aa / n) < OT_STEREO_MIN_STD) {
		return false; // no texture: any disparity matches
	}
	const int count = hi - lo + 1;
	float score[1024];
	if (count > (int)(sizeof(score) / sizeof(score[0]))) {
		return false;
	}
	int best = -1;
	for (int d = lo; d <= hi; d++) {
		double s = 0, ss = 0, sab = 0;
		for (int y = 0; y < side; y++) {
			for (int x = 0; x < side; x++) {
				const double b = grey(right, cx - d - radius + x, cy - radius + y);
				s += b;
				ss += b * b;
				sab += a[y * side + x] * b; // a is zero-mean: sum a (b - mean b) = sum a b
			}
		}
		const double bb = ss - s * s / n;
		const float ncc = bb > 1e-6 ? (float)(sab / sqrt(aa * bb)) : -1.0f;
		score[d - lo] = ncc;
		if (best < 0 || ncc > score[best]) {
			best = d - lo;
		}
	}
	const bool ok = best > 0 && best < count - 1 && score[best] >= OT_STEREO_MIN_NCC;
	if (ok) {
		// Parabola through the best score and its neighbours.
		const float l = score[best - 1], c = score[best], r = score[best + 1];
		const float den = l - 2.0f * c + r;
		float off = den < -1e-6f ? 0.5f * (l - r) / den : 0.0f;
		if (off > 0.5f) {
			off = 0.5f;
		} else if (off < -0.5f) {
			off = -0.5f;
		}
		*out_d = (float)(lo + best) + off;
		*out_ncc = c;
	}
	return ok;
}

static int
cmp_float(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

bool
ot_stereo_hand_ratio(const struct ot_image *left,
                     const struct ot_image *right,
                     float focal_px,
                     float baseline_m,
                     const float (*lm)[3],
                     const float (*cam)[3],
                     float *out_ratio,
                     uint32_t *out_points)
{
	*out_points = 0;
	if (!(focal_px > 0.0f) || !(baseline_m > 0.0f)) {
		return false;
	}
	// The palm's interior: textured skin at nearly one depth, away from the fingers' silhouettes.
	static const int pts[] = {OT_HL_WRIST,      OT_HL_THUMB_CMC, OT_HL_INDEX_MCP,
	                          OT_HL_MIDDLE_MCP, OT_HL_RING_MCP,  OT_HL_PINKY_MCP};
	// A patch about a fifth of the palm wide (wrist -> middle knuckle in px).
	const float dx = lm[OT_HL_MIDDLE_MCP][0] - lm[OT_HL_WRIST][0],
	            dy = lm[OT_HL_MIDDLE_MCP][1] - lm[OT_HL_WRIST][1];
	int radius = (int)(0.1f * sqrtf(dx * dx + dy * dy));
	radius = radius < 3 ? 3 : radius > 12 ? 12 : radius;
	float ratios[sizeof(pts) / sizeof(pts[0])];
	uint32_t n = 0;
	for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
		const float z = cam[pts[i]][2];
		if (!(z > 0.05f)) {
			continue;
		}
		const float d_mono = focal_px * baseline_m / z;
		// Depth z·s for s in the allowed scale range <=> disparity d_mono / s.
		const float d_min = d_mono / OT_STEREO_SCALE_MAX - 1.0f, d_max = d_mono / OT_STEREO_SCALE_MIN + 1.0f;
		float d, ncc;
		if (!ot_stereo_disparity(left, right, lm[pts[i]][0], lm[pts[i]][1], d_min > 0.0f ? d_min : 0.0f, d_max,
		                         radius, &d, &ncc) ||
		    !(d > 0.5f)) {
			continue;
		}
		ratios[n++] = (focal_px * baseline_m / d) / z;
	}
	*out_points = n;
	if (n < OT_STEREO_MIN_POINTS) {
		return false;
	}
	qsort(ratios, n, sizeof(float), cmp_float);
	*out_ratio = (n & 1u) ? ratios[n / 2] : 0.5f * (ratios[n / 2 - 1] + ratios[n / 2]);
	return true;
}

float
ot_stereo_scale_update(float scale, float ratio, float gain)
{
	if (!(scale > 0.0f) || !(ratio > 0.0f)) {
		return scale;
	}
	gain = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
	float s = scale * expf(gain * logf(ratio));
	s = s < OT_STEREO_SCALE_MIN ? OT_STEREO_SCALE_MIN : s > OT_STEREO_SCALE_MAX ? OT_STEREO_SCALE_MAX : s;
	return s;
}
