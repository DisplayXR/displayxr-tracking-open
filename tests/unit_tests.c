// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests of the pure parts: One Euro filter, pixel -> direction,
 *         the iris / IPD distance maths on synthetic faces at known distances,
 *         the ROI affine and sampler, BlazeFace decoding, the pack manifest,
 *         and the viewer role's display mapping.
 *
 *     open-tracking-unit-tests <case> [pack.json]
 */

#include "ot_eyes.h"
#include "ot_face_geom.h"
#include "ot_landmarks.h"
#include "ot_pack.h"
#include "ot_viewer.h"

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
near(double a, double b, double tol)
{
	return fabs(a - b) <= tol;
}

/*
 * One Euro.
 */

static void
test_one_euro(void)
{
	struct ot_one_euro f;
	ot_one_euro_reset(&f);
	check(ot_one_euro(&f, 2.0f, 1000, 1.0f, 0.0f, 1.0f) == 2.0f, "first sample passes through");
	check(ot_one_euro(&f, 5.0f, 1000, 1.0f, 0.0f, 1.0f) == 2.0f, "a non-increasing time returns the previous output");
	// beta = 0: a plain low-pass with alpha = 1 / (1 + tau / dt), tau = 1 / (2 pi fc).
	const float dt = 1.0f / 30.0f, fc = 1.0f;
	const float tau = 1.0f / (2.0f * (float)M_PI * fc);
	const float alpha = 1.0f / (1.0f + tau / dt);
	const float y = ot_one_euro(&f, 3.0f, 1000 + (int64_t)(dt * 1e9), fc, 0.0f, 1.0f);
	check(near(y, 2.0f + alpha * 1.0f, 1e-4), "beta 0 = first-order low-pass (%.5f vs %.5f)", y, 2.0f + alpha);
	// Constant input stays constant; a step converges.
	ot_one_euro_reset(&f);
	float out = 0;
	for (int i = 0; i < 300; i++) {
		out = ot_one_euro(&f, i == 0 ? 0.0f : 1.0f, (int64_t)i * 33333333LL, 1.0f, 0.0f, 1.0f);
	}
	check(near(out, 1.0f, 1e-3), "a step converges (%.5f)", out);
	// Speed raises the cutoff: with beta > 0 a ramp lags less.
	struct ot_one_euro a, b;
	ot_one_euro_reset(&a);
	ot_one_euro_reset(&b);
	float ya = 0, yb = 0;
	for (int i = 0; i < 60; i++) {
		const float x = 0.01f * (float)i; // 0.3 m/s
		ya = ot_one_euro(&a, x, (int64_t)i * 33333333LL, 1.0f, 0.0f, 1.0f);
		yb = ot_one_euro(&b, x, (int64_t)i * 33333333LL, 1.0f, 3.0f, 1.0f);
	}
	check(fabsf(0.59f - yb) < fabsf(0.59f - ya), "beta reduces lag on motion (lag %.4f vs %.4f m)", 0.59f - yb,
	      0.59f - ya);
}

/*
 * Pixel -> direction.
 */

static void
test_pixel_dir(void)
{
	struct ot_intrinsics k;
	check(ot_intrinsics_from_hfov(1280, 720, 70.0f, &k), "intrinsics from 70 deg");
	check(near(k.f, 640.0 / tan(35.0 * M_PI / 180.0), 1e-2), "f = (W/2) / tan(HFOV/2) = %.2f", k.f);
	check(!ot_intrinsics_from_hfov(1280, 720, 5.0f, &k), "HFOV outside 10..170 refused");
	ot_intrinsics_from_hfov(1280, 720, 70.0f, &k);
	float d[3];
	ot_pixel_to_dir(&k, 640.0f, 360.0f, d);
	check(near(d[0], 0, 1e-6) && near(d[1], 0, 1e-6) && near(d[2], 1, 1e-6), "the centre is the optical axis");
	ot_pixel_to_dir(&k, 1280.0f, 360.0f, d);
	const double ang = atan2(-d[0], d[2]) * 180.0 / M_PI;
	check(near(ang, 35.0, 1e-3), "the image's right edge is HFOV/2 off axis (%.3f deg)", ang);
	check(d[0] < 0, "the image's right is the user's LEFT (camera -X)");
	ot_pixel_to_dir(&k, 640.0f, 0.0f, d);
	check(d[1] > 0, "the image's top is +Y");
	check(near(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0, 1e-6), "unit length");
}

/*
 * Synthetic faces: project a simple head model through the pinhole.
 */

//! Camera-frame point -> image px (u, v) and the mesh's relative depth (px at the face's scale).
static void
project(const struct ot_intrinsics *k, const float p[3], float z_ref, float out[3])
{
	out[0] = k->cx - k->f * p[0] / p[2];
	out[1] = k->cy - k->f * p[1] / p[2];
	out[2] = (p[2] - z_ref) * k->f / z_ref;
}

//! Rotate (x, z) by yaw about the head centre @p c.
static void
yawed(const float local[3], const float c[3], float yaw, float out[3])
{
	const float cs = cosf(yaw), sn = sinf(yaw);
	out[0] = c[0] + cs * local[0] + sn * local[2];
	out[1] = c[1] + local[1];
	out[2] = c[2] - sn * local[0] + cs * local[2];
}

/*!
 * A face whose eye midpoint is at camera point @p mid, yawed by @p yaw, with
 * the given IPD and iris diameter. Head-local frame = camera frame (+X the
 * user's right, +Z toward the viewer's back, away from the camera).
 */
static void
make_face(const struct ot_intrinsics *k, const float mid[3], float yaw, float ipd, float iris, struct ot_face_obs *o)
{
	memset(o, 0, sizeof(*o));
	const float c[3] = {mid[0], mid[1], mid[2] + 0.09f}; // rotate about a point 9 cm behind the eyes
	const float z_ref = mid[2];
	for (int e = 0; e < 2; e++) {
		const float sx = e == 0 ? -0.5f : 0.5f; // [0] = the user's left = camera -X
		const float centre[3] = {sx * ipd, 0.0f, -0.09f};
		float p[3];
		yawed(centre, c, yaw, p);
		project(k, p, z_ref, o->iris[e]);
		// The iris contour: a circle in the eye's (rotated) x-y plane. Order: +x, +y(up), -x, -y.
		const float r = 0.5f * iris;
		const float ring[4][3] = {{sx * ipd + r, 0, -0.09f}, {sx * ipd, r, -0.09f}, {sx * ipd - r, 0, -0.09f},
		                          {sx * ipd, -r, -0.09f}};
		for (int i = 0; i < 4; i++) {
			float q[3], px[3];
			yawed(ring[i], c, yaw, q);
			project(k, q, z_ref, px);
			o->iris_ring[e][i][0] = px[0];
			o->iris_ring[e][i][1] = px[1];
		}
		const float corner[3] = {sx * (ipd * 0.5f + 0.045f) * 1.0f, 0.0f, -0.08f};
		yawed(corner, c, yaw, p);
		project(k, p, z_ref, o->eye_outer[e]);
	}
	const float fh[3] = {0, 0.08f, -0.08f}, ch[3] = {0, -0.11f, -0.07f};
	float p[3];
	yawed(fh, c, yaw, p);
	project(k, p, z_ref, o->forehead);
	yawed(ch, c, yaw, p);
	project(k, p, z_ref, o->chin);
}

static void
test_distance(void)
{
	struct ot_intrinsics k;
	ot_intrinsics_from_hfov(1280, 720, 70.0f, &k);
	struct ot_eye_params prm;
	ot_eye_params_defaults(&prm);
	const float dists[3] = {0.4f, 0.6f, 1.0f};
	for (int i = 0; i < 3; i++) {
		const float mid[3] = {0.05f, -0.03f, dists[i]};
		struct ot_face_obs o;
		make_face(&k, mid, 0.0f, prm.ipd_m, prm.iris_diameter_m, &o);
		struct ot_eye_estimate e;
		check(ot_estimate_eyes(&k, &prm, &o, &e), "estimate at %.1f m", dists[i]);
		// The true distance of the left eye along its ray.
		const float lx = mid[0] - 0.5f * prm.ipd_m;
		const float truth = sqrtf(lx * lx + mid[1] * mid[1] + mid[2] * mid[2]);
		check(near(e.distance_m[0], truth, 0.01 * truth), "frontal %.1f m: left eye %.4f m (truth %.4f)", dists[i],
		      e.distance_m[0], truth);
		check(near(e.depth_iris_m[0], dists[i], 0.01 * dists[i]) && near(e.depth_ipd_m[0], dists[i], 0.01 * dists[i]),
		      "  both estimates agree: iris %.4f, separation %.4f m", e.depth_iris_m[0], e.depth_ipd_m[0]);
		check(e.dir[0][0] < e.dir[1][0], "  [0] is the user's left (smaller X)");
		check(e.weight_iris > 0.05f && e.weight_iris < 0.95f, "  fused, iris weight %.2f", e.weight_iris);
		check(near(e.head_rotation[3], 1.0, 1e-3), "  frontal head: identity orientation (w %.4f)", e.head_rotation[3]);
	}
	// A yawed head: the projected separation shrinks; the mesh depth corrects it.
	const float mid[3] = {0.0f, 0.0f, 0.6f};
	struct ot_face_obs o;
	const float yaw = 30.0f * (float)M_PI / 180.0f;
	make_face(&k, mid, yaw, prm.ipd_m, prm.iris_diameter_m, &o);
	struct ot_eye_estimate e;
	check(ot_estimate_eyes(&k, &prm, &o, &e), "estimate, 30 deg yaw");
	const double naive = k.f * prm.ipd_m / hypot(o.iris[0][0] - o.iris[1][0], o.iris[0][1] - o.iris[1][1]);
	// The left eye's true depth: the head turns about a point 9 cm behind the eyes.
	const double left_true = 0.69 + sin(yaw) * 0.5 * prm.ipd_m - cos(yaw) * 0.09;
	check(near(e.foreshortening, cos(yaw), 0.03), "foreshortening %.3f = cos(yaw) %.3f", e.foreshortening, cos(yaw));
	check(near(e.depth_ipd_m[0], left_true, left_true * 0.04) && fabs(naive - 0.6) > 0.1,
	      "yaw-corrected separation depth %.4f m, truth %.4f (uncorrected would read %.4f)", e.depth_ipd_m[0],
	      left_true, naive);
	check(near(e.depth_iris_m[0], left_true, left_true * 0.04), "iris depth %.4f m", e.depth_iris_m[0]);
	check(e.weight_iris > 0.5f, "a yawed head trusts the iris more (iris weight %.2f)", e.weight_iris);
	// Head orientation: yaw about +Y.
	const float *q = e.head_rotation;
	const double est_yaw = 2.0 * atan2(q[1], q[3]);
	check(near(fabs(est_yaw), yaw, 3.0 * M_PI / 180.0), "head yaw %.1f deg", est_yaw * 180.0 / M_PI);
	// Degenerate input.
	struct ot_face_obs z;
	memset(&z, 0, sizeof(z));
	check(!ot_estimate_eyes(&k, &prm, &z, &e), "a collapsed face is refused");
}

/*
 * ROI, affine, sampler.
 */

static void
test_roi(void)
{
	struct ot_roi roi = {400.0f, 300.0f, 256.0f, 0.3f};
	struct ot_affine m;
	ot_roi_to_affine(&roi, 256, &m);
	float x, y;
	ot_affine_apply(&m, 128.0f, 128.0f, &x, &y);
	check(near(x, 400, 1e-3) && near(y, 300, 1e-3), "crop centre -> ROI centre");
	ot_affine_apply(&m, 256.0f, 128.0f, &x, &y);
	check(near(x, 400 + 128 * cos(0.3), 1e-3) && near(y, 300 + 128 * sin(0.3), 1e-3),
	      "crop +u = image +x rotated by rot");
	ot_letterbox_affine(1280, 720, 128, &m);
	ot_affine_apply(&m, 64.0f, 64.0f, &x, &y);
	check(near(x, 640, 1e-3) && near(y, 360, 1e-3), "letterbox centre");
	ot_affine_apply(&m, 0.0f, 0.0f, &x, &y);
	check(near(x, 0, 1e-3) && near(y, -280, 1e-3), "letterbox pads top and bottom");

	// Landmarks rotated by a known angle: the ROI recovers it.
	float pts[OT_LM_COUNT][3];
	memset(pts, 0, sizeof(pts));
	const float ang = 0.25f, c = cosf(ang), s = sinf(ang);
	for (int i = 0; i < OT_LM_COUNT; i++) {
		const float u = (float)((i * 37) % 100) - 50.0f, v = (float)((i * 61) % 120) - 60.0f;
		pts[i][0] = 500 + c * u - s * v;
		pts[i][1] = 300 + s * u + c * v;
	}
	// Right eye outer corner (33) on the image left, left eye outer (263) on the right, level in the face frame.
	pts[OT_LM_RIGHT_EYE_OUTER][0] = 500 + c * -40;
	pts[OT_LM_RIGHT_EYE_OUTER][1] = 300 + s * -40;
	pts[OT_LM_LEFT_EYE_OUTER][0] = 500 + c * 40;
	pts[OT_LM_LEFT_EYE_OUTER][1] = 300 + s * 40;
	struct ot_roi r;
	ot_roi_from_landmarks((const float(*)[3])pts, OT_LM_COUNT, &r);
	check(near(r.rot, ang, 1e-4), "ROI rotation from the eye corners (%.4f)", r.rot);
	check(near(r.side, 1.5 * 119.0, 1.0), "ROI side = 1.5 x the long side (%.1f)", r.side);

	// Sampler: a horizontal ramp image, identity-ish affine, plus the mirror.
	enum
	{
		W = 8,
		H = 4
	};
	uint8_t img[W * H * 3];
	for (int yy = 0; yy < H; yy++) {
		for (int xx = 0; xx < W; xx++) {
			img[(yy * W + xx) * 3 + 0] = (uint8_t)(xx * 30);
			img[(yy * W + xx) * 3 + 1] = 0;
			img[(yy * W + xx) * 3 + 2] = 255;
		}
	}
	struct ot_image im = {img, W, H, W * 3, OT_FMT_RGB, false};
	struct ot_affine id = {1, 0, 0, 0, 1, 0};
	float out[W * H * 3];
	ot_sample_affine(&im, &id, W, H, 2.0f, -1.0f, true, out);
	check(near(out[(1 * W + 3) * 3], 3 * 30 / 255.0 * 2 - 1, 1e-5) && near(out[2], 1.0, 1e-5),
	      "identity resample, range [-1, 1]");
	im.mirrored = true;
	ot_sample_affine(&im, &id, W, H, 1.0f, 0.0f, true, out);
	check(near(out[(1 * W + 0) * 3], 7 * 30 / 255.0, 1e-5), "a mirrored source is un-mirrored");
	// BGRA (the AVFoundation format) samples the same as RGB.
	uint8_t bgra[W * H * 4];
	for (int i = 0; i < W * H; i++) {
		bgra[i * 4 + 0] = img[i * 3 + 2];
		bgra[i * 4 + 1] = img[i * 3 + 1];
		bgra[i * 4 + 2] = img[i * 3 + 0];
		bgra[i * 4 + 3] = 255;
	}
	struct ot_image ib = {bgra, W, H, W * 4, OT_FMT_BGRA, false};
	float out2[W * H * 3];
	im.mirrored = false;
	struct ot_affine sub = {0.7f, 0.1f, 0.3f, -0.1f, 0.6f, 0.5f};
	ot_sample_affine(&im, &sub, W, H, 1.0f, 0.0f, false, out);
	ot_sample_affine(&ib, &sub, W, H, 1.0f, 0.0f, false, out2);
	check(memcmp(out, out2, sizeof(out)) == 0, "BGRA samples exactly like RGB");
	im.mirrored = true;
	struct ot_affine shift = {1, 0, -100, 0, 1, 0};
	ot_sample_affine(&im, &shift, W, H, 2.0f, -1.0f, true, out);
	check(near(out[0], -1.0, 1e-6), "outside the image: zero padding");
}

/*
 * BlazeFace decoding.
 */

static void
test_decode(void)
{
	static float anchors[OT_FACE_DET_ANCHORS][2];
	ot_face_anchors(anchors);
	check(near(anchors[0][0], 1.0 / 32.0, 1e-6) && near(anchors[511][0], 31.0 / 32.0, 1e-6) &&
	          near(anchors[512][0], 1.0 / 16.0, 1e-6) && near(anchors[895][1], 15.0 / 16.0, 1e-6),
	      "anchors: 16x16x2 then 8x8x6");
	static float reg[OT_FACE_DET_ANCHORS * 16];
	static float logit[OT_FACE_DET_ANCHORS];
	for (int i = 0; i < OT_FACE_DET_ANCHORS; i++) {
		logit[i] = -10.0f;
	}
	// A face at anchor 600 (8x8 grid), 40 px wide, eyes level; a duplicate at 601.
	for (int a = 600; a <= 601; a++) {
		float *r = reg + a * 16;
		r[0] = 2.0f;
		r[1] = -1.0f;
		r[2] = 40.0f;
		r[3] = 44.0f;
		r[4] = -9.0f; // right eye (image left)
		r[5] = -6.0f;
		r[6] = 9.0f; // left eye
		r[7] = -6.0f;
		logit[a] = a == 600 ? 4.0f : 3.0f;
	}
	struct ot_face_det d[4];
	const uint32_t n = ot_face_decode(reg, logit, anchors, 0.5f, 0.3f, d, 4);
	check(n == 1, "NMS keeps one of two overlapping detections (%u)", n);
	const float ax = anchors[600][0] * 128.0f, ay = anchors[600][1] * 128.0f;
	check(near(d[0].box[0], ax + 2 - 20, 1e-4) && near(d[0].box[3], ay - 1 + 22, 1e-4), "box decoded");
	check(near(d[0].score, 1.0 / (1.0 + exp(-4.0)), 1e-5), "sigmoid score");
	struct ot_affine lb;
	ot_letterbox_affine(1280, 720, 128, &lb);
	struct ot_roi roi;
	ot_roi_from_detection(&d[0], &lb, &roi);
	check(near(roi.rot, 0, 1e-5), "level eyes -> no rotation");
	check(near(roi.side, 1.5 * 44.0 * 10.0, 1e-2), "ROI side = 1.5 x long side, in image px (%.1f)", roi.side);
}

/*
 * Pack.
 */

static const char *k_pack_ok =
    "{\"schema\":\"displayxr-tracking-pack-1\",\"name\":\"t\",\"version\":\"1\",\"models\":["
    "{\"role\":\"face_detector\",\"file\":\"a.onnx\",\"sha256\":\"%s\",\"size\":10,"
    "\"input\":{\"width\":128,\"height\":128,\"layout\":\"NHWC\",\"range\":[-1,1]}},"
    "{\"role\":\"face_landmarks\",\"file\":\"%s\",\"sha256\":\"%s\",\"size\":20,"
    "\"input\":{\"width\":256,\"height\":256,\"layout\":\"NHWC\",\"range\":[0,1]}},"
    "{\"role\":\"palm_detector\",\"file\":\"x.onnx\"}]}";

static bool
parse_variant(const char *file2, const char *sha, char *err, size_t err_size, struct ot_pack *p)
{
	char buf[2048];
	snprintf(buf, sizeof(buf), k_pack_ok, sha, file2, sha);
	return ot_pack_parse(buf, strlen(buf), p, err, err_size);
}

static void
test_pack(const char *repo_pack)
{
	const char *sha = "af191c89e0a8654f46b78af8e81ea5ffdef726a0cf8cde433ffbdb5ee5747ba0";
	struct ot_pack p;
	char err[256];
	check(parse_variant("b.onnx", sha, err, sizeof(err), &p), "a valid pack parses (%s)", err);
	check(p.models[OT_ROLE_FACE_LANDMARKS].width == 256 && p.models[OT_ROLE_FACE_DETECTOR].range_lo == -1.0f,
	      "input size and range read");
	check(!parse_variant("../b.onnx", sha, err, sizeof(err), &p), "a path in a file name is refused (%s)", err);
	check(!parse_variant("b.onnx", "AF191C", err, sizeof(err), &p), "a malformed sha256 is refused (%s)", err);
	const char *missing = "{\"schema\":\"displayxr-tracking-pack-1\",\"models\":[]}";
	check(!ot_pack_parse(missing, strlen(missing), &p, err, sizeof(err)), "a missing role is refused (%s)", err);
	const char *schema = "{\"schema\":\"other\",\"models\":[]}";
	check(!ot_pack_parse(schema, strlen(schema), &p, err, sizeof(err)), "another schema is refused (%s)", err);
	if (repo_pack != NULL) {
		check(ot_pack_load(repo_pack, &p, err, sizeof(err)), "the repository's pack parses (%s)", err);
		check(strcmp(p.models[OT_ROLE_FACE_DETECTOR].sha256, sha) == 0, "  detector sha256 pinned");
	}
}

/*
 * Viewer role: camera -> display mapping.
 */

static void
test_viewer(void)
{
	struct ot_intrinsics k;
	ot_intrinsics_from_hfov(1280, 720, 70.0f, &k);
	struct ot_eye_params prm;
	ot_eye_params_defaults(&prm);
	struct ot_filter_params fp = {1.0f, 3.0f, 1.0f};
	struct ot_pose pose;
	memset(&pose, 0, sizeof(pose));
	pose.t[1] = 0.106f;
	pose.q[3] = 1.0f;
	pose.focal_scale = 1.0f;
	pose.iris_scale = 1.0f;

	// A face result built from a synthetic face at 0.6 m on the optical axis.
	static struct ot_face_result fr;
	memset(&fr, 0, sizeof(fr));
	struct ot_face_obs o;
	const float mid[3] = {0.0f, 0.0f, 0.6f};
	make_face(&k, mid, 0.0f, prm.ipd_m, prm.iris_diameter_m, &o);
	const int centre[2] = {OT_LM_LEFT_IRIS_CENTRE, OT_LM_RIGHT_IRIS_CENTRE};
	for (int e = 0; e < 2; e++) {
		memcpy(fr.lm[centre[e]], o.iris[e], sizeof(o.iris[e]));
		for (int i = 0; i < 4; i++) {
			fr.lm[centre[e] + 1 + i][0] = o.iris_ring[e][i][0];
			fr.lm[centre[e] + 1 + i][1] = o.iris_ring[e][i][1];
		}
	}
	memcpy(fr.lm[OT_LM_LEFT_EYE_OUTER], o.eye_outer[0], sizeof(o.eye_outer[0]));
	memcpy(fr.lm[OT_LM_RIGHT_EYE_OUTER], o.eye_outer[1], sizeof(o.eye_outer[1]));
	memcpy(fr.lm[OT_LM_FOREHEAD], o.forehead, sizeof(o.forehead));
	memcpy(fr.lm[OT_LM_CHIN], o.chin, sizeof(o.chin));
	fr.presence = 0.99f;

	struct ot_viewer v;
	ot_viewer_reset(&v);
	struct xrt_tracking_viewer_sample s;
	fr.found = false;
	check(!ot_viewer_update(&v, &k, &prm, &fp, &pose, &fr, 1000, "cam", &s, NULL), "nothing before the first face");
	fr.found = true;
	check(ot_viewer_update(&v, &k, &prm, &fp, &pose, &fr, 2000, "cam", &s, NULL), "a face -> a sample");
	check(near(s.left_eye[0], -0.0315, 1e-3) && near(s.left_eye[1], 0.106, 1e-3) && near(s.left_eye[2], 0.6, 6e-3),
	      "display eye = T + R p (%.4f, %.4f, %.4f)", s.left_eye[0], s.left_eye[1], s.left_eye[2]);
	check((s.flags & XRT_TRACKING_SAMPLE_TRACKING) && (s.flags & XRT_TRACKING_SAMPLE_CAMERA_VALID) &&
	          strcmp(s.camera.camera_key, "cam") == 0,
	      "TRACKING + CAMERA_VALID + key");
	const float raw = s.camera.left_distance_m;

	// The per-user scale applies to the display eyes, NOT to the camera block.
	struct ot_viewer v2;
	ot_viewer_reset(&v2);
	pose.iris_scale = 0.9f;
	struct xrt_tracking_viewer_sample s2;
	ot_viewer_update(&v2, &k, &prm, &fp, &pose, &fr, 2000, "cam", &s2, NULL);
	check(near(s2.left_eye[2], 0.9 * s.left_eye[2], 1e-4), "iris scale scales the display depth (%.4f)", s2.left_eye[2]);
	check(near(s2.camera.left_distance_m, raw, 1e-6), "the camera block stays raw (%.4f)", s2.camera.left_distance_m);

	// A rotated camera, pitched 10 deg down (R = Rx(+10 deg): the optical axis +Z -> (0, -sin, cos)).
	struct ot_viewer v3;
	ot_viewer_reset(&v3);
	pose.iris_scale = 1.0f;
	const float a = 10.0f * (float)M_PI / 180.0f;
	pose.q[0] = sinf(0.5f * a);
	pose.q[3] = cosf(0.5f * a);
	struct xrt_tracking_viewer_sample s3;
	ot_viewer_update(&v3, &k, &prm, &fp, &pose, &fr, 2000, "cam", &s3, NULL);
	check(near(s3.left_eye[1], 0.106 - 0.6 * sin(a), 6e-3), "rotation applied: a down-pitched camera sees the eyes "
	      "lower (y %.4f)", s3.left_eye[1]);

	// Loss: last known, TRACKING clear, no camera block.
	fr.found = false;
	struct xrt_tracking_viewer_sample s4;
	check(ot_viewer_update(&v, &k, &prm, &fp, &pose, &fr, 3000, "cam", &s4, NULL), "loss -> a sample");
	check(!(s4.flags & XRT_TRACKING_SAMPLE_TRACKING) && (s4.flags & XRT_TRACKING_SAMPLE_EYES_VALID) &&
	          !(s4.flags & XRT_TRACKING_SAMPLE_CAMERA_VALID) && s4.capture_ns == 3000 &&
	          near(s4.left_eye[0], s.left_eye[0], 1e-7),
	      "  last known eyes, not tracking");
}

int
main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "all";
	const bool all = strcmp(which, "all") == 0;
	if (all || strcmp(which, "one-euro") == 0) {
		test_one_euro();
	}
	if (all || strcmp(which, "pixel-dir") == 0) {
		test_pixel_dir();
	}
	if (all || strcmp(which, "distance") == 0) {
		test_distance();
	}
	if (all || strcmp(which, "roi") == 0) {
		test_roi();
	}
	if (all || strcmp(which, "decode") == 0) {
		test_decode();
	}
	if (all || strcmp(which, "pack") == 0) {
		test_pack(argc > 2 ? argv[2] : NULL);
	}
	if (all || strcmp(which, "viewer") == 0) {
		test_viewer();
	}
	printf("%s\n", g_fail ? "FAILED" : "ok");
	return g_fail ? 1 : 0;
}
