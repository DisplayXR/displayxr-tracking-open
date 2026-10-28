// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The hand's 3D side, see ot_hand_pose.h.
 */

#include "ot_hand_pose.h"

#include <math.h>
#include <string.h>

static void
sub3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[0] - b[0];
	out[1] = a[1] - b[1];
	out[2] = a[2] - b[2];
}

static float
dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void
cross3(const float a[3], const float b[3], float out[3])
{
	const float x = a[1] * b[2] - a[2] * b[1];
	const float y = a[2] * b[0] - a[0] * b[2];
	const float z = a[0] * b[1] - a[1] * b[0];
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

static float
norm3(const float a[3])
{
	return sqrtf(dot3(a, a));
}

static bool
normalize3(float a[3])
{
	const float n = norm3(a);
	if (!(n > 1e-9f)) {
		return false;
	}
	a[0] /= n;
	a[1] /= n;
	a[2] /= n;
	return true;
}

static void
lerp3(const float a[3], const float b[3], float t, float out[3])
{
	out[0] = a[0] + (b[0] - a[0]) * t;
	out[1] = a[1] + (b[1] - a[1]) * t;
	out[2] = a[2] + (b[2] - a[2]) * t;
}

void
ot_hand_world_to_camera(const float (*world)[3], float roi_rot, float (*out)[3])
{
	// Crop axes -> image axes (the ROI's rotation, ot_roi_to_affine), then image (x right, y down,
	// z away) -> camera (+X = image left, +Y up, +Z away from the camera): a 180-degree turn about z.
	const float c = cosf(roi_rot), s = sinf(roi_rot);
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		const float x = world[i][0], y = world[i][1], z = world[i][2];
		const float xi = c * x - s * y;
		const float yi = s * x + c * y;
		out[i][0] = -xi;
		out[i][1] = -yi;
		out[i][2] = z;
	}
}

bool
ot_hand_place(
    const struct ot_intrinsics *k, const float (*px)[3], const float (*shape)[3], float (*out)[3], float out_t[3])
{
	// Minimise sum |(I - d d^T)(P + T)|^2 over T: (sum (I - d d^T)) T = -sum (I - d d^T) P.
	double a[3][3] = {{0}}, b[3] = {0};
	float dirs[OT_HAND_LM_COUNT][3];
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		ot_pixel_to_dir(k, px[i][0], px[i][1], dirs[i]);
		const float *d = dirs[i];
		double m[3][3];
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				m[r][c] = (r == c ? 1.0 : 0.0) - (double)d[r] * d[c];
				a[r][c] += m[r][c];
			}
		}
		for (int r = 0; r < 3; r++) {
			b[r] -= m[r][0] * shape[i][0] + m[r][1] * shape[i][1] + m[r][2] * shape[i][2];
		}
	}
	const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
	                   a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
	                   a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	const double tr = (a[0][0] + a[1][1] + a[2][2]) / 3.0;
	// The depth is carried by the rays' spread: a hand that spans nothing has none.
	if (!(fabs(det) > 1e-9 * tr * tr * tr)) {
		return false;
	}
	double inv[3][3];
	inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
	inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
	inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
	inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
	inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
	inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
	inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
	inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
	inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
	float t[3];
	for (int r = 0; r < 3; r++) {
		t[r] = (float)(inv[r][0] * b[0] + inv[r][1] * b[1] + inv[r][2] * b[2]);
	}
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		const float q[3] = {shape[i][0] + t[0], shape[i][1] + t[1], shape[i][2] + t[2]};
		const float along = dot3(dirs[i], q);
		if (!(along > 0.01f)) {
			return false; // behind (or on) the camera
		}
		out[i][0] = dirs[i][0] * along;
		out[i][1] = dirs[i][1] * along;
		out[i][2] = dirs[i][2] * along;
	}
	if (out_t != NULL) {
		memcpy(out_t, t, sizeof(t));
	}
	return true;
}

float
ot_hand_size_m(const float (*p)[3])
{
	float d[3];
	sub3(p[OT_HL_MIDDLE_MCP], p[OT_HL_WRIST], d);
	return norm3(d);
}

//! The knuckle-plane normal index × little about the wrist (unit; the palm side for a right hand).
static bool
knuckle_normal(const float (*p)[3], float n[3])
{
	float a[3], b[3];
	sub3(p[OT_HL_INDEX_MCP], p[OT_HL_WRIST], a);
	sub3(p[OT_HL_PINKY_MCP], p[OT_HL_WRIST], b);
	cross3(a, b, n);
	return normalize3(n);
}

int
ot_hand_chirality(const float (*p)[3])
{
	float n[3];
	if (!knuckle_normal(p, n)) {
		return 0;
	}
	static const int k_bent[] = {OT_HL_INDEX_PIP,  OT_HL_INDEX_DIP,  OT_HL_INDEX_TIP, OT_HL_MIDDLE_PIP,
	                             OT_HL_MIDDLE_DIP, OT_HL_MIDDLE_TIP, OT_HL_RING_PIP,  OT_HL_RING_DIP,
	                             OT_HL_RING_TIP,   OT_HL_PINKY_PIP,  OT_HL_PINKY_DIP, OT_HL_PINKY_TIP};
	float sum = 0.0f;
	for (size_t i = 0; i < sizeof(k_bent) / sizeof(k_bent[0]); i++) {
		float d[3];
		sub3(p[k_bent[i]], p[OT_HL_WRIST], d);
		sum += dot3(d, n);
	}
	const float mean = sum / (float)(sizeof(k_bent) / sizeof(k_bent[0]));
	// Below 3 % of the hand's size the fingers are straight: no evidence either way.
	if (fabsf(mean) < 0.03f * ot_hand_size_m(p)) {
		return 0;
	}
	return mean > 0.0f ? 1 : -1;
}

//! A rotation whose -Z is @p forward and whose +Y is as close to @p up as possible.
static void
frame_quat(const float forward[3], const float up[3], float out[4])
{
	float z[3] = {-forward[0], -forward[1], -forward[2]};
	if (!normalize3(z)) {
		out[0] = out[1] = out[2] = 0.0f;
		out[3] = 1.0f;
		return;
	}
	float y[3];
	const float k = dot3(up, z);
	y[0] = up[0] - k * z[0];
	y[1] = up[1] - k * z[1];
	y[2] = up[2] - k * z[2];
	if (!normalize3(y)) {
		// up is parallel to the bone: any perpendicular will do.
		const float alt[3] = {fabsf(z[0]) < 0.9f ? 1.0f : 0.0f, fabsf(z[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
		const float a = dot3(alt, z);
		y[0] = alt[0] - a * z[0];
		y[1] = alt[1] - a * z[1];
		y[2] = alt[2] - a * z[2];
		(void)normalize3(y);
	}
	float x[3];
	cross3(y, z, x);
	const float m[3][3] = {{x[0], y[0], z[0]}, {x[1], y[1], z[1]}, {x[2], y[2], z[2]}};
	ot_quat_from_matrix(m, out);
}

void
ot_hand_joints_from_landmarks(const float (*p)[3], bool is_left, struct ot_hand_joints *out)
{
	memset(out, 0, sizeof(*out));

	// Positions: the 20 measured joints, then the synthesised ones.
	memcpy(out->pos[OT_HJ_WRIST], p[OT_HL_WRIST], sizeof(float) * 3);
	for (int j = 0; j < 4; j++) {
		memcpy(out->pos[OT_HJ_THUMB_METACARPAL + j], p[OT_HL_THUMB_CMC + j], sizeof(float) * 3);
	}
	for (int f = 0; f < 4; f++) {
		const int base_j = OT_HJ_INDEX_METACARPAL + 5 * f; // metacarpal, proximal, intermediate, distal, tip
		const int base_l = OT_HL_INDEX_MCP + 4 * f;        // MCP, PIP, DIP, tip
		lerp3(p[OT_HL_WRIST], p[base_l], OT_HAND_METACARPAL_FRACTION, out->pos[base_j]);
		for (int j = 0; j < 4; j++) {
			memcpy(out->pos[base_j + 1 + j], p[base_l + j], sizeof(float) * 3);
		}
	}
	lerp3(out->pos[OT_HJ_MIDDLE_METACARPAL], p[OT_HL_MIDDLE_MCP], 0.5f, out->pos[OT_HJ_PALM]);

	// The back of the hand.
	float back[3] = {0.0f, 0.0f, 1.0f};
	if (knuckle_normal(p, back) && !is_left) {
		back[0] = -back[0];
		back[1] = -back[1];
		back[2] = -back[2];
	}

	// The thumb's nail does not face the back of the hand: the thumb sits rotated about its own axis,
	// its nail turned toward the radial side (the index finger's side). Its +Y blends the two; the
	// radial direction, little -> index knuckle, needs no handedness.
	float thumb_up[3], radial[3];
	sub3(p[OT_HL_INDEX_MCP], p[OT_HL_PINKY_MCP], radial);
	if (!normalize3(radial)) {
		memcpy(radial, back, sizeof(radial));
	}
	for (int c = 0; c < 3; c++) {
		thumb_up[c] = back[c] + radial[c];
	}
	if (!normalize3(thumb_up)) {
		memcpy(thumb_up, back, sizeof(thumb_up));
	}

	// Orientations: each joint looks along the bone that starts at it; tips along the last bone.
	float fwd[3];
	sub3(p[OT_HL_MIDDLE_MCP], p[OT_HL_WRIST], fwd);
	frame_quat(fwd, back, out->rot[OT_HJ_WRIST]);
	memcpy(out->rot[OT_HJ_PALM], out->rot[OT_HJ_WRIST], sizeof(float) * 4);
	for (int j = OT_HJ_THUMB_METACARPAL; j <= OT_HJ_LITTLE_TIP; j++) {
		const bool tip =
		    j == OT_HJ_THUMB_TIP || (j >= OT_HJ_INDEX_METACARPAL && (j - OT_HJ_INDEX_METACARPAL) % 5 == 4);
		if (tip) {
			sub3(out->pos[j], out->pos[j - 1], fwd);
		} else {
			sub3(out->pos[j + 1], out->pos[j], fwd);
		}
		frame_quat(fwd, j <= OT_HJ_THUMB_TIP ? thumb_up : back, out->rot[j]);
	}

	// Radii, nominal adult hand (metres), scaled by this hand's size.
	float s = ot_hand_size_m(p) / OT_HAND_NOMINAL_SIZE_M;
	s = s < 0.7f ? 0.7f : (s > 1.4f ? 1.4f : s);
	static const float k_thumb[4] = {0.0130f, 0.0110f, 0.0095f, 0.0085f};
	static const float k_finger[5] = {0.0110f, 0.0100f, 0.0090f, 0.0080f, 0.0075f};
	static const float k_finger_scale[4] = {1.0f, 1.03f, 0.95f, 0.85f}; // index, middle, ring, little
	out->radius[OT_HJ_PALM] = 0.016f * s;
	out->radius[OT_HJ_WRIST] = 0.020f * s;
	for (int j = 0; j < 4; j++) {
		out->radius[OT_HJ_THUMB_METACARPAL + j] = k_thumb[j] * s;
	}
	for (int f = 0; f < 4; f++) {
		for (int j = 0; j < 5; j++) {
			out->radius[OT_HJ_INDEX_METACARPAL + 5 * f + j] = k_finger[j] * k_finger_scale[f] * s;
		}
	}
}

bool
ot_hand_pinch(const float (*p)[3], bool was_pinching, float on_m, float off_m, float *out_distance_m)
{
	float d[3];
	sub3(p[OT_HL_THUMB_TIP], p[OT_HL_INDEX_TIP], d);
	const float dist = norm3(d);
	if (out_distance_m != NULL) {
		*out_distance_m = dist;
	}
	return was_pinching ? dist < off_m : dist < on_m;
}
