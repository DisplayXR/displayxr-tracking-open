// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests of the hands role's pure parts: palm decoding and the
 *         hand ROIs, placing the metric hand along the rays (monocular depth),
 *         handedness from the 3D shape, the 21 -> 26 joint mapping, the side
 *         assignment and per-frame mapping, and the process-wide camera
 *         sharing (on the file camera).
 *
 *     open-tracking-hand-tests <case> [scratch dir]
 */

#include "ot_capture.h"
#include "ot_hand_geom.h"
#include "ot_hand_pose.h"
#include "ot_hands.h"
#include "ot_platform.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0;

static void
check(bool ok, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("[%s] ", ok ? "PASS" : "FAIL");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	if (!ok) {
		g_fail++;
	}
}

static bool
approx(double a, double b, double tol)
{
	return fabs(a - b) <= tol;
}

static float
dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float
len3(const float a[3])
{
	return sqrtf(dot3(a, a));
}

/*!
 * A synthetic RIGHT hand in display space: wrist at the origin, fingers up (+Y), the back of the hand toward
 * the user (+Z), so the thumb is on the user's left (-X); the fingers bend slightly toward the palm (-Z).
 */
static void
make_right_hand(float p[OT_HAND_LM_COUNT][3])
{
	static const float k[OT_HAND_LM_COUNT][3] = {
	    {0.000f, 0.000f, 0.000f},                                                           // wrist
	    {-0.030f, 0.020f, -0.010f}, {-0.050f, 0.040f, -0.015f}, {-0.065f, 0.060f, -0.020f}, // thumb CMC MCP IP
	    {-0.075f, 0.075f, -0.025f},                                                         // thumb tip
	    {-0.030f, 0.090f, 0.000f},  {-0.032f, 0.130f, -0.010f}, {-0.033f, 0.155f, -0.020f}, // index MCP PIP DIP
	    {-0.034f, 0.175f, -0.030f},                                                         // index tip
	    {-0.010f, 0.095f, 0.000f},  {-0.010f, 0.140f, -0.010f}, {-0.010f, 0.168f, -0.020f}, // middle
	    {-0.010f, 0.190f, -0.030f},                                                         //
	    {0.010f, 0.090f, 0.000f},   {0.011f, 0.132f, -0.010f},  {0.012f, 0.158f, -0.020f},  // ring
	    {0.013f, 0.178f, -0.030f},                                                          //
	    {0.030f, 0.080f, 0.000f},   {0.033f, 0.112f, -0.010f},  {0.035f, 0.132f, -0.018f},  // little
	    {0.036f, 0.148f, -0.026f},                                                          //
	};
	memcpy(p, k, sizeof(k));
}

static void
mirror_x(float p[OT_HAND_LM_COUNT][3])
{
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		p[i][0] = -p[i][0];
	}
}

//! Pixel of a camera-frame point (inverse of ot_pixel_to_dir).
static void
project(const struct ot_intrinsics *k, const float p[3], float out[3])
{
	out[0] = k->cx - k->f * p[0] / p[2];
	out[1] = k->cy - k->f * p[1] / p[2];
	out[2] = 0.0f;
}


/*
 * Palm decoding + ROIs.
 */

static void
test_decode(void)
{
	static float anchors[OT_HAND_DET_ANCHORS][2];
	ot_hand_anchors(anchors);
	check(approx(anchors[0][0], 0.5 / 24, 1e-6) && approx(anchors[1151][1], 23.5 / 24, 1e-6) &&
	          approx(anchors[1152][0], 0.5 / 12, 1e-6) && approx(anchors[2015][0], 11.5 / 12, 1e-6),
	      "anchors: 24x24x2 then 12x12x6");
	static float reg[OT_HAND_DET_ANCHORS * OT_HAND_DET_REGRESSORS], logit[OT_HAND_DET_ANCHORS];
	memset(reg, 0, sizeof(reg));
	for (int i = 0; i < OT_HAND_DET_ANCHORS; i++) {
		logit[i] = -10.0f;
	}
	// One palm at anchor 600 (+ a weaker duplicate next to it), box 40 x 50 px offset (+3, -2), keypoints.
	const int a = 600;
	for (int d = 0; d < 2; d++) {
		float *r = reg + (size_t)(a + d) * OT_HAND_DET_REGRESSORS;
		r[0] = 3.0f - (float)d * (anchors[a + 1][0] - anchors[a][0]) * 192.0f;
		r[1] = -2.0f - (float)d * (anchors[a + 1][1] - anchors[a][1]) * 192.0f;
		r[2] = 40.0f;
		r[3] = 50.0f;
		r[4] = 1.0f; // wrist
		r[5] = 20.0f;
		r[8] = 2.0f; // middle knuckle
		r[9] = -15.0f;
		logit[a + d] = d == 0 ? 4.0f : 2.0f;
	}
	struct ot_hand_det det[4];
	const uint32_t n = ot_hand_decode(reg, logit, anchors, 0.5f, 0.3f, det, 4);
	const float ax = anchors[a][0] * 192.0f, ay = anchors[a][1] * 192.0f;
	check(n == 1, "one palm after NMS (%u)", n);
	check(approx(det[0].score, 1.0 / (1.0 + exp(-4.0)), 1e-5), "score = sigmoid(logit)");
	check(approx(det[0].box[0], ax + 3.0 - 20.0, 1e-3) && approx(det[0].box[3], ay - 2.0 + 25.0, 1e-3),
	      "box decoded");
	check(approx(det[0].kp[OT_HAND_KP_WRIST][1], ay + 20.0, 1e-3) &&
	          approx(det[0].kp[OT_HAND_KP_MIDDLE_MCP][0], ax + 2.0, 1e-3),
	      "keypoints decoded");
}

static void
test_roi(void)
{
	// An upright palm: wrist below the knuckle -> no rotation; centre half a box up; side 2.6 x the long side.
	struct ot_hand_det d;
	memset(&d, 0, sizeof(d));
	d.box[0] = 80.0f;
	d.box[1] = 90.0f;
	d.box[2] = 120.0f;
	d.box[3] = 140.0f;
	d.kp[OT_HAND_KP_WRIST][0] = 100.0f;
	d.kp[OT_HAND_KP_WRIST][1] = 138.0f;
	d.kp[OT_HAND_KP_MIDDLE_MCP][0] = 100.0f;
	d.kp[OT_HAND_KP_MIDDLE_MCP][1] = 95.0f;
	const struct ot_affine id = {1, 0, 0, 0, 1, 0};
	struct ot_roi roi;
	ot_hand_roi_from_detection(&d, &id, &roi);
	check(approx(roi.rot, 0.0, 1e-5) && approx(roi.cx, 100.0, 1e-3) && approx(roi.cy, 115.0 - 25.0, 1e-3) &&
	          approx(roi.side, 130.0, 1e-3),
	      "detection ROI: upright, shifted to the fingers, 2.6 x (rot %.3f c %.1f,%.1f side %.1f)", roi.rot, roi.cx,
	      roi.cy, roi.side);

	// A palm pointing right on screen: the crop's "up" must map to the image's +x.
	d.kp[OT_HAND_KP_WRIST][0] = 82.0f;
	d.kp[OT_HAND_KP_WRIST][1] = 115.0f;
	d.kp[OT_HAND_KP_MIDDLE_MCP][0] = 118.0f;
	d.kp[OT_HAND_KP_MIDDLE_MCP][1] = 115.0f;
	ot_hand_roi_from_detection(&d, &id, &roi);
	struct ot_affine m;
	ot_roi_to_affine(&roi, 224, &m);
	float x0, y0, x1, y1;
	ot_affine_apply(&m, 112.0f, 150.0f, &x0, &y0);
	ot_affine_apply(&m, 112.0f, 50.0f, &x1, &y1); // up in the crop
	check(x1 - x0 > 50.0f && fabsf(y1 - y0) < 1e-2f, "a right-pointing hand: crop up = image right (rot %.3f)",
	      roi.rot);
	check(roi.cx > 100.0f + 20.0f, "  and the centre moved toward the fingers (%.1f)", roi.cx);

	// The landmark ROI of an upright synthetic hand (image px: flip y of the display-space hand).
	float h[OT_HAND_LM_COUNT][3], px[OT_HAND_LM_COUNT][3];
	make_right_hand(h);
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		px[i][0] = 400.0f + 1000.0f * h[i][0];
		px[i][1] = 500.0f - 1000.0f * h[i][1];
		px[i][2] = 0.0f;
	}
	ot_hand_roi_from_landmarks((const float(*)[3])px, &roi);
	check(fabsf(roi.rot) < 0.15f, "landmark ROI of an upright hand: near-zero rotation (%.3f)", roi.rot);
	check(roi.side > 2.0f * 100.0f && roi.side < 2.0f * 160.0f, "  side 2 x the palm box (%.1f)", roi.side);
	check(roi.cy < 500.0f - 0.5f * 90.0f, "  centred over the hand, above the wrist (%.1f)", roi.cy);

	struct ot_roi a = {100, 100, 50, 0}, b = {105, 100, 50, 0.3f}, c = {300, 100, 50, 0};
	check(ot_roi_overlap(&a, &b) > 0.5f && ot_roi_overlap(&a, &c) == 0.0f, "ROI overlap: same hand vs another");
}


/*
 * 3D: placement, chirality, joints.
 */

static void
test_place(void)
{
	struct ot_intrinsics k;
	ot_intrinsics_from_hfov(1280, 720, 70.0f, &k);
	float shape[OT_HAND_LM_COUNT][3], px[OT_HAND_LM_COUNT][3], out[OT_HAND_LM_COUNT][3], t[3] = {0, 0, 0};
	make_right_hand(shape);
	// Centre the shape (the model's world landmarks are about the hand's middle).
	float c[3] = {0, 0, 0};
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		for (int j = 0; j < 3; j++) {
			c[j] += shape[i][j] / OT_HAND_LM_COUNT;
		}
	}
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		for (int j = 0; j < 3; j++) {
			shape[i][j] -= c[j];
		}
	}
	const float truth[3][3] = {{0.05f, -0.12f, 0.45f}, {-0.2f, 0.05f, 0.8f}, {0.0f, 0.0f, 0.3f}};
	for (int s = 0; s < 3; s++) {
		for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
			const float p[3] = {shape[i][0] + truth[s][0], shape[i][1] + truth[s][1],
			                    shape[i][2] + truth[s][2]};
			project(&k, p, px[i]);
		}
		const bool ok = ot_hand_place(&k, (const float(*)[3])px, (const float(*)[3])shape, out, t);
		check(ok && approx(t[0], truth[s][0], 1e-3) && approx(t[1], truth[s][1], 1e-3) &&
		          approx(t[2], truth[s][2], 2e-3),
		      "placement recovers T (%.3f, %.3f, %.3f) -> (%.4f, %.4f, %.4f)", truth[s][0], truth[s][1],
		      truth[s][2], t[0], t[1], t[2]);
		float rp[3];
		project(&k, out[OT_HL_INDEX_TIP], rp);
		check(approx(rp[0], px[OT_HL_INDEX_TIP][0], 1e-2) && approx(rp[1], px[OT_HL_INDEX_TIP][1], 1e-2),
		      "  the placed landmarks reproject onto the 2D ones");
	}
	// The hand-size prior: a 10 % larger hand reads 10 % farther.
	float big[OT_HAND_LM_COUNT][3];
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		for (int j = 0; j < 3; j++) {
			big[i][j] = shape[i][j] * 1.1f;
		}
	}
	float t2[3] = {0, 0, 0};
	ot_hand_place(&k, (const float(*)[3])px, (const float(*)[3])big, out, t2);
	check(approx(t2[2] / t[2], 1.1, 2e-3), "depth scales with the hand-size prior (x%.4f)", t2[2] / t[2]);
	float flat[OT_HAND_LM_COUNT][3];
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		flat[i][0] = k.cx;
		flat[i][1] = k.cy;
		flat[i][2] = 0.0f;
	}
	check(!ot_hand_place(&k, (const float(*)[3])flat, (const float(*)[3])shape, out, t2),
	      "all landmarks on one pixel: no depth (refused)");

	// world -> camera: crop axes (x right, y down, z away) undo the ROI rotation, then +X = image left, +Y up.
	float w[OT_HAND_LM_COUNT][3] = {{0}}, cam[OT_HAND_LM_COUNT][3];
	w[0][0] = 1.0f; // crop +x
	w[1][1] = 1.0f; // crop +y
	w[2][2] = 1.0f;
	ot_hand_world_to_camera((const float(*)[3])w, 0.0f, cam);
	check(approx(cam[0][0], -1, 1e-6) && approx(cam[1][1], -1, 1e-6) && approx(cam[2][2], 1, 1e-6),
	      "world -> camera, rot 0: (-x, -y, z)");
	ot_hand_world_to_camera((const float(*)[3])w, (float)M_PI / 2.0f, cam);
	// rot = 90 deg: the crop's +x is the image's +y (down) -> camera -Y.
	check(approx(cam[0][0], 0, 1e-6) && approx(cam[0][1], -1, 1e-6),
	      "world -> camera, rot 90: crop +x = image down");
}

static void
test_chirality(void)
{
	float p[OT_HAND_LM_COUNT][3];
	make_right_hand(p);
	check(ot_hand_chirality((const float(*)[3])p) == 1, "the synthetic right hand reads right");
	mirror_x(p);
	check(ot_hand_chirality((const float(*)[3])p) == -1, "its mirror image reads left");
	// Turning the hand around (back to the camera instead) keeps its handedness.
	make_right_hand(p);
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		p[i][0] = -p[i][0];
		p[i][2] = -p[i][2]; // a 180-degree turn about +Y
	}
	check(ot_hand_chirality((const float(*)[3])p) == 1, "rotated 180 degrees, still right");
	make_right_hand(p);
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		p[i][2] = 0.0f;
	}
	check(ot_hand_chirality((const float(*)[3])p) == 0, "a flat hand cannot tell");
}

static void
axis(const float q[4], int which, float out[3])
{
	float v[3] = {0, 0, 0};
	v[which] = 1.0f;
	ot_quat_rotate(q, v, out);
}

static void
test_joints(void)
{
	float p[OT_HAND_LM_COUNT][3];
	make_right_hand(p);
	struct ot_hand_joints j;
	ot_hand_joints_from_landmarks((const float(*)[3])p, false, &j);

	check(memcmp(j.pos[OT_HJ_WRIST], p[OT_HL_WRIST], sizeof(float) * 3) == 0 &&
	          memcmp(j.pos[OT_HJ_THUMB_METACARPAL], p[OT_HL_THUMB_CMC], sizeof(float) * 3) == 0 &&
	          memcmp(j.pos[OT_HJ_THUMB_TIP], p[OT_HL_THUMB_TIP], sizeof(float) * 3) == 0 &&
	          memcmp(j.pos[OT_HJ_INDEX_METACARPAL + 1], p[OT_HL_INDEX_MCP], sizeof(float) * 3) == 0 &&
	          memcmp(j.pos[OT_HJ_RING_METACARPAL + 3], p[OT_HL_RING_DIP], sizeof(float) * 3) == 0 &&
	          memcmp(j.pos[OT_HJ_LITTLE_TIP], p[OT_HL_PINKY_TIP], sizeof(float) * 3) == 0,
	      "the 20 measured joints map 1:1 (thumb CMC->metacarpal, MCP->proximal, ..., tips)");
	bool meta = true;
	for (int f = 0; f < 4; f++) {
		const int l = OT_HL_INDEX_MCP + 4 * f;
		for (int c = 0; c < 3; c++) {
			meta = meta && approx(j.pos[OT_HJ_INDEX_METACARPAL + 5 * f][c], 0.25 * p[l][c], 1e-6);
		}
	}
	check(meta, "metacarpals synthesised a quarter of the way wrist -> knuckle");
	check(approx(j.pos[OT_HJ_PALM][1], 0.5 * (0.25 * 0.095 + 0.095), 1e-6) &&
	          approx(j.pos[OT_HJ_PALM][0], -0.01 * 0.625, 1e-6),
	      "palm at the middle of the middle metacarpal (%.4f, %.4f)", j.pos[OT_HJ_PALM][0], j.pos[OT_HJ_PALM][1]);

	// -Z along each joint's bone, toward the tip.
	bool along = true;
	for (int jj = OT_HJ_THUMB_METACARPAL; jj < OT_HJ_COUNT; jj++) {
		const bool tip =
		    jj == OT_HJ_THUMB_TIP || (jj >= OT_HJ_INDEX_METACARPAL && (jj - OT_HJ_INDEX_METACARPAL) % 5 == 4);
		float bone[3];
		for (int c = 0; c < 3; c++) {
			bone[c] = tip ? j.pos[jj][c] - j.pos[jj - 1][c] : j.pos[jj + 1][c] - j.pos[jj][c];
		}
		float z[3];
		axis(j.rot[jj], 2, z);
		along = along && -dot3(z, bone) / len3(bone) > 0.999f;
	}
	check(along, "every finger joint's -Z points along its bone, toward the fingertip");
	float y[3], x[3], z[3];
	axis(j.rot[OT_HJ_PALM], 1, y);
	axis(j.rot[OT_HJ_PALM], 0, x);
	axis(j.rot[OT_HJ_PALM], 2, z);
	check(y[2] > 0.95f, "palm +Y out of the back of the hand (toward the user, %.3f)", y[2]);
	check(z[1] < -0.95f, "palm -Z toward the fingers (+Y)");
	float yz[3] = {y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]};
	check(dot3(yz, x) > 0.999f, "right-handed basis: X = Y x Z");
	bool back = true;
	for (int jj = OT_HJ_INDEX_METACARPAL; jj < OT_HJ_COUNT; jj++) {
		axis(j.rot[jj], 1, y);
		back = back && y[2] > 0.0f;
	}
	check(back, "every finger joint's +Y on the back-of-hand side");
	// The thumb's nail turns toward the radial side (the thumb is on -X for this right hand).
	bool thumb = true;
	for (int jj = OT_HJ_THUMB_METACARPAL; jj <= OT_HJ_THUMB_TIP; jj++) {
		axis(j.rot[jj], 1, y);
		thumb = thumb && y[2] > 0.2f && y[0] < -0.2f;
	}
	axis(j.rot[OT_HJ_THUMB_PROXIMAL], 1, y);
	check(thumb, "the thumb's +Y between the back of the hand and the radial side (%.2f, %.2f, %.2f)", y[0],
	      y[1], y[2]);

	// The left hand (the mirror image): its back faces the user too.
	float m[OT_HAND_LM_COUNT][3];
	make_right_hand(m);
	mirror_x(m);
	struct ot_hand_joints jl;
	ot_hand_joints_from_landmarks((const float(*)[3])m, true, &jl);
	axis(jl.rot[OT_HJ_PALM], 1, y);
	check(y[2] > 0.95f, "left hand: palm +Y toward the user too (%.3f)", y[2]);
	axis(jl.rot[OT_HJ_THUMB_PROXIMAL], 1, y);
	check(y[2] > 0.2f && y[0] > 0.2f, "left hand: the thumb's +Y toward its radial side, +X (%.2f, %.2f)", y[0],
	      y[2]);

	// Radii: plausible, and they scale with the hand.
	bool ok = true;
	for (int jj = 0; jj < OT_HJ_COUNT; jj++) {
		ok = ok && j.radius[jj] > 0.004f && j.radius[jj] < 0.025f;
	}
	check(ok && j.radius[OT_HJ_WRIST] > j.radius[OT_HJ_INDEX_METACARPAL + 4], "radii 4..25 mm, wrist > fingertip");
	float big[OT_HAND_LM_COUNT][3];
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		for (int c = 0; c < 3; c++) {
			big[i][c] = p[i][c] * 1.2f;
		}
	}
	struct ot_hand_joints jb;
	ot_hand_joints_from_landmarks((const float(*)[3])big, false, &jb);
	check(approx(jb.radius[OT_HJ_PALM] / j.radius[OT_HJ_PALM], 1.2, 1e-4), "radii scale with the hand size");

	// Pinch, with hysteresis.
	float d = 0;
	check(!ot_hand_pinch((const float(*)[3])p, false, 0.025f, 0.035f, &d), "an open hand does not pinch (%.0f mm)",
	      d * 1000.0f);
	memcpy(p[OT_HL_THUMB_TIP], p[OT_HL_INDEX_TIP], sizeof(float) * 3);
	p[OT_HL_THUMB_TIP][0] += 0.03f;
	check(!ot_hand_pinch((const float(*)[3])p, false, 0.025f, 0.035f, &d) &&
	          ot_hand_pinch((const float(*)[3])p, true, 0.025f, 0.035f, &d),
	      "30 mm: no new pinch, but a held one continues (hysteresis)");
	p[OT_HL_THUMB_TIP][0] -= 0.02f;
	check(ot_hand_pinch((const float(*)[3])p, false, 0.025f, 0.035f, &d), "10 mm: pinching");
}


/*
 * Side assignment + the per-frame mapping.
 */

static void
obs_from_camera_hand(const struct ot_intrinsics *k,
                     const float cam[OT_HAND_LM_COUNT][3],
                     float right,
                     uint32_t id,
                     struct ot_hand_obs *o)
{
	memset(o, 0, sizeof(*o));
	float c[3] = {0, 0, 0};
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		for (int j = 0; j < 3; j++) {
			c[j] += cam[i][j] / OT_HAND_LM_COUNT;
		}
	}
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		project(k, cam[i], o->lm[i]);
		// The model's world landmarks: crop axes (roi.rot = 0): x = -X, y = -Y, z = Z, about the centre.
		o->world[i][0] = -(cam[i][0] - c[0]);
		o->world[i][1] = -(cam[i][1] - c[1]);
		o->world[i][2] = cam[i][2] - c[2];
	}
	o->presence = 0.99f;
	o->right_frame = right;
	o->right_mean = right;
	o->track_id = id;
}

static void
test_hands(void)
{
	struct ot_hand_result r;
	memset(&r, 0, sizeof(r));
	int idx[2];
	r.count = 2;
	r.hands[0].right_mean = 0.2f;
	r.hands[1].right_mean = 0.7f;
	ot_hands_assign(&r, idx);
	check(idx[OT_SIDE_RIGHT] == 1 && idx[OT_SIDE_LEFT] == 0, "two hands: the more 'right' one is right");
	r.hands[0].right_mean = 0.6f;
	r.hands[1].right_mean = 0.55f;
	ot_hands_assign(&r, idx);
	check(idx[OT_SIDE_RIGHT] == 0 && idx[OT_SIDE_LEFT] == 1, "two hands both read right: still one each");
	r.count = 1;
	r.hands[0].right_mean = 0.4f;
	ot_hands_assign(&r, idx);
	check(idx[OT_SIDE_LEFT] == 0 && idx[OT_SIDE_RIGHT] == -1, "one hand at 0.4: left");

	// Sticky sides: track 3 holds the right side.
	struct ot_hands held;
	ot_hands_reset(&held);
	held.have[OT_SIDE_RIGHT] = true;
	held.track[OT_SIDE_RIGHT] = 3;
	memset(&r, 0, sizeof(r));
	r.count = 1;
	r.hands[0].track_id = 3;
	r.hands[0].right_mean = 0.45f;
	ot_hands_assign_sticky(&held, &r, idx);
	check(idx[OT_SIDE_RIGHT] == 0, "sticky: a held right hand turned edge-on (0.45) stays right");
	r.hands[0].right_mean = 0.2f;
	ot_hands_assign_sticky(&held, &r, idx);
	check(idx[OT_SIDE_LEFT] == 0 && idx[OT_SIDE_RIGHT] == -1, "sticky: clear evidence (0.2) still flips it");
	r.count = 2;
	r.hands[0].right_mean = 0.55f;
	r.hands[1].track_id = 9;
	r.hands[1].right_mean = 0.7f;
	ot_hands_assign_sticky(&held, &r, idx);
	check(idx[OT_SIDE_RIGHT] == 0 && idx[OT_SIDE_LEFT] == 1,
	      "sticky: a second hand arriving does not take the held hand's side");
	r.hands[0].track_id = 4;
	ot_hands_assign_sticky(&held, &r, idx);
	check(idx[OT_SIDE_RIGHT] == 1 && idx[OT_SIDE_LEFT] == 0, "sticky: no held track -> by handedness");

	// A right hand 45 cm in front of a camera at the top centre of the bezel.
	struct ot_intrinsics k;
	ot_intrinsics_from_hfov(1280, 720, 70.0f, &k);
	float hand[OT_HAND_LM_COUNT][3], cam[OT_HAND_LM_COUNT][3];
	make_right_hand(hand);
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		cam[i][0] = hand[i][0] + 0.05f;
		cam[i][1] = hand[i][1] - 0.25f;
		cam[i][2] = hand[i][2] + 0.45f;
	}
	struct ot_cam_pose pose;
	memset(&pose, 0, sizeof(pose));
	pose.t[1] = 0.106f;
	pose.q[3] = 1.0f;
	pose.focal_scale = 1.0f;
	struct ot_hands_params prm;
	ot_hands_params_defaults(&prm);
	memset(&r, 0, sizeof(r));
	r.count = 1;
	obs_from_camera_hand(&k, (const float(*)[3])cam, 0.9f, 7, &r.hands[0]);
	struct ot_hands h;
	ot_hands_reset(&h);
	struct ot_hand_state st[2];
	ot_hands_update(&h, &k, &prm, &pose, &r, 1000000000LL, st);
	check(st[OT_SIDE_RIGHT].active && !st[OT_SIDE_LEFT].active, "one right hand -> the right side");
	float w[3];
	memcpy(w, st[OT_SIDE_RIGHT].joints.pos[OT_HJ_WRIST], sizeof(w));
	check(approx(w[0], 0.05, 1e-3) && approx(w[1], 0.106 - 0.25, 1e-3) && approx(w[2], 0.45, 3e-3),
	      "display = T + R p (wrist %.4f, %.4f, %.4f)", w[0], w[1], w[2]);
	check(approx(st[OT_SIDE_RIGHT].depth_m, sqrt(0.05 * 0.05 + 0.25 * 0.25 + 0.45 * 0.45), 3e-3),
	      "depth = the camera -> wrist distance (%.4f)", st[OT_SIDE_RIGHT].depth_m);
	check(!st[OT_SIDE_RIGHT].pinching, "  no pinch");

	// The same hand 1 cm to the user's right (+X; R = I) 100 ms later: the palm velocity follows, filtered
	// and half-smoothed, so below the raw 0.1 m/s.
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		cam[i][0] += 0.01f;
	}
	obs_from_camera_hand(&k, (const float(*)[3])cam, 0.9f, 7, &r.hands[0]);
	ot_hands_update(&h, &k, &prm, &pose, &r, 1100000000LL, st);
	check(st[OT_SIDE_RIGHT].active && st[OT_SIDE_RIGHT].palm_velocity[0] > 0.0f &&
	          st[OT_SIDE_RIGHT].palm_velocity[0] < 0.11f,
	      "moving +X: palm velocity +X, filtered (%.3f m/s)", st[OT_SIDE_RIGHT].palm_velocity[0]);

	// The camera pitched down 20 degrees: the same camera-frame hand lands lower in display space.
	struct ot_cam_pose pitched = pose;
	const float a = 20.0f * (float)M_PI / 180.0f;
	pitched.q[0] = sinf(0.5f * a);
	pitched.q[3] = cosf(0.5f * a);
	struct ot_hands h2;
	ot_hands_reset(&h2);
	ot_hands_update(&h2, &k, &prm, &pitched, &r, 1000000000LL, st);
	check(st[OT_SIDE_RIGHT].joints.pos[OT_HJ_WRIST][1] < w[1] - 0.05f,
	      "a down-pitched camera: the hand lands lower");

	// A background hand: the near one (0.52 m) is the user's, the same hand 1.2 m away is not.
	struct ot_hands_admit adm = {.k = &k, .p = &prm, .pose = &pose, .last_rejected_m = 0.0f};
	check(ot_hands_admit(&adm, &r.hands[0]), "a hand at 0.52 m is admitted");
	float distant[OT_HAND_LM_COUNT][3];
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		distant[i][0] = hand[i][0] - 0.2f;
		distant[i][1] = hand[i][1] - 0.1f;
		distant[i][2] = hand[i][2] + 1.2f;
	}
	struct ot_hand_obs fo;
	obs_from_camera_hand(&k, (const float(*)[3])distant, 0.2f, 8, &fo);
	check(!ot_hands_admit(&adm, &fo) && approx(adm.last_rejected_m, 1.22, 0.02),
	      "a hand at 1.2 m is turned away (%.3f m)", adm.last_rejected_m);
	prm.max_distance_m = 0.0f;
	check(ot_hands_admit(&adm, &fo), "MAX_DISTANCE 0: any distance");
	prm.max_distance_m = 0.8f;

	// No hand: inactive.
	r.count = 0;
	ot_hands_update(&h, &k, &prm, &pose, &r, 1200000000LL, st);
	check(!st[0].active && !st[1].active, "no hand: both sides inactive");
}


/*
 * Camera sharing (the file camera).
 */

static void
test_capture_share(const char *dir)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s%cshare-test.ppm", dir != NULL ? dir : ".", OT_PATH_SEP);
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		check(false, "cannot write %s", path);
		return;
	}
	fprintf(f, "P6\n32 24\n255\n");
	for (int i = 0; i < 32 * 24 * 3; i++) {
		fputc(0x40, f);
	}
	fclose(f);
	char sel[1100];
	snprintf(sel, sizeof(sel), "file:%s", path);
	struct ot_camera_info cam;
	check(ot_capture_resolve(sel, &cam), "file camera resolves");
	const struct ot_capture_request req = {32, 24, 30};
	struct ot_capture *a = NULL, *b = NULL;
	char err[256];
	check(ot_capture_open(&cam, &req, &a, err, sizeof(err)), "first open");
	check(ot_capture_open(&cam, &req, &b, err, sizeof(err)) && a == b,
	      "second open of the same device: the same stream");
	struct ot_frame_buffer fa, fb;
	memset(&fa, 0, sizeof(fa));
	memset(&fb, 0, sizeof(fb));
	check(ot_capture_take(a, &fa, 2000) && ot_capture_take(b, &fb, 2000), "both consumers get a frame");
	const uint64_t s1 = fb.frame.seq;
	check(ot_capture_take(b, &fb, 2000) && fb.frame.seq > s1, "each consumer gets the next frame on its own");
	ot_capture_close(a);
	check(ot_capture_take(b, &fb, 2000), "closing one reference keeps the stream for the other");
	ot_capture_close(b);
	struct ot_capture *c = NULL;
	check(ot_capture_open(&cam, &req, &c, err, sizeof(err)), "after the last close, a new open works");
	ot_capture_close(c);
	ot_frame_buffer_free(&fa);
	ot_frame_buffer_free(&fb);
	remove(path);
}

int
main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "all";
	const bool all = strcmp(which, "all") == 0;
	if (all || strcmp(which, "hand-decode") == 0) {
		test_decode();
	}
	if (all || strcmp(which, "hand-roi") == 0) {
		test_roi();
	}
	if (all || strcmp(which, "hand-place") == 0) {
		test_place();
	}
	if (all || strcmp(which, "hand-chirality") == 0) {
		test_chirality();
	}
	if (all || strcmp(which, "hand-joints") == 0) {
		test_joints();
	}
	if (all || strcmp(which, "hands") == 0) {
		test_hands();
	}
	if (all || strcmp(which, "capture-share") == 0) {
		test_capture_share(argc > 2 ? argv[2] : NULL);
	}
	printf("%s\n", g_fail ? "FAILED" : "ok");
	return g_fail ? 1 : 0;
}
