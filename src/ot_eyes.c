// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Landmarks -> camera-space eyes, pose maths, One Euro filter; see ot_eyes.h.
 */

#include "ot_eyes.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool
ot_intrinsics_from_hfov(uint32_t width, uint32_t height, float hfov_deg, struct ot_intrinsics *out)
{
	if (width == 0 || height == 0 || !(hfov_deg >= 10.0f && hfov_deg <= 170.0f)) {
		return false;
	}
	out->width = width;
	out->height = height;
	out->f = 0.5f * (float)width / tanf(0.5f * hfov_deg * (float)M_PI / 180.0f);
	out->cx = 0.5f * (float)width;
	out->cy = 0.5f * (float)height;
	return true;
}

static float
norm3(const float v[3])
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void
normalize3(float v[3])
{
	const float n = norm3(v);
	if (n > 0.0f) {
		v[0] /= n;
		v[1] /= n;
		v[2] /= n;
	}
}

static void
cross3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

void
ot_pixel_to_dir(const struct ot_intrinsics *k, float u, float v, float out[3])
{
	out[0] = -(u - k->cx) / k->f;
	out[1] = -(v - k->cy) / k->f;
	out[2] = 1.0f;
	normalize3(out);
}

void
ot_eye_params_defaults(struct ot_eye_params *p)
{
	p->iris_diameter_m = 0.0117f;
	p->ipd_m = 0.063f;
	p->pixel_sigma = 0.5f;
	p->iris_pop_sigma = 0.05f;
	p->ipd_pop_sigma = 0.06f;
	p->yaw_sigma_gain = 0.25f;
}

static float
dist2(const float a[2], const float b[2])
{
	const float dx = a[0] - b[0], dy = a[1] - b[1];
	return sqrtf(dx * dx + dy * dy);
}

//! A mesh point (u, v, z px) as a camera-frame vector (the image's +x is the user's left, +y down).
static void
mesh_to_cam(const float p[3], float out[3])
{
	out[0] = -p[0];
	out[1] = -p[1];
	out[2] = p[2];
}

bool
ot_estimate_eyes(const struct ot_intrinsics *k,
                 const struct ot_eye_params *p,
                 const struct ot_face_obs *obs,
                 struct ot_eye_estimate *out)
{
	memset(out, 0, sizeof(*out));
	out->head_rotation[3] = 1.0f;

	// (a) Iris diameter per eye: the larger of the two contour extents.
	for (int e = 0; e < 2; e++) {
		const float h = dist2(obs->iris_ring[e][0], obs->iris_ring[e][2]);
		const float v = dist2(obs->iris_ring[e][1], obs->iris_ring[e][3]);
		const float d = h > v ? h : v;
		if (!(d > 1.0f)) {
			return false;
		}
		out->iris_diameter_px[e] = d;
		out->depth_iris_m[e] = k->f * p->iris_diameter_m / d;
		ot_pixel_to_dir(k, obs->iris[e][0], obs->iris[e][1], out->dir[e]);
	}

	// (b) Eye separation, corrected for yaw by the mesh's own depth: |s3| is the true
	// separation in px at the face's scale, |s2| its projection.
	float s3[3], s2;
	for (int i = 0; i < 3; i++) {
		s3[i] = obs->iris[0][i] - obs->iris[1][i];
	}
	s2 = sqrtf(s3[0] * s3[0] + s3[1] * s3[1]);
	const float l3 = norm3(s3);
	if (!(s2 > 2.0f) || !(l3 > 2.0f)) {
		return false;
	}
	float fs = s2 / l3;
	fs = fs < 0.3f ? 0.3f : fs;
	out->foreshortening = fs;
	const float z_mid_m = k->f * p->ipd_m / l3;
	const float z_mid_px = 0.5f * (obs->iris[0][2] + obs->iris[1][2]);
	for (int e = 0; e < 2; e++) {
		// The eye's own depth offset from the mid point, px -> metres at the face's depth.
		out->depth_ipd_m[e] = z_mid_m * (1.0f + (obs->iris[e][2] - z_mid_px) / k->f);
	}

	// Inverse-variance fusion, per eye. The variances are relative (σ / Z)².
	float wsum_iris = 0.0f;
	for (int e = 0; e < 2; e++) {
		const float ri = p->pixel_sigma / out->iris_diameter_px[e];
		const float var_iris = ri * ri + p->iris_pop_sigma * p->iris_pop_sigma;
		const float rs = p->pixel_sigma * 1.41421356f / s2;
		const float ry = p->yaw_sigma_gain * (1.0f - fs);
		const float var_ipd = rs * rs + p->ipd_pop_sigma * p->ipd_pop_sigma + ry * ry;
		const float wi = 1.0f / var_iris, wp = 1.0f / var_ipd;
		const float w = wi / (wi + wp);
		wsum_iris += w;
		const float z = w * out->depth_iris_m[e] + (1.0f - w) * out->depth_ipd_m[e];
		if (!(z > 0.1f && z < 5.0f) || !(out->dir[e][2] > 0.05f)) {
			return false;
		}
		out->distance_m[e] = z / out->dir[e][2];
	}
	out->weight_iris = 0.5f * wsum_iris;

	// Head orientation from the mesh (OpenXR head axes: +X the head's right, +Y up, +Z backward):
	// x from the left eye's outer corner to the right's, y from the chin to the forehead.
	float a[3], b[3], x[3], y[3], z[3], t[3];
	mesh_to_cam(obs->eye_outer[1], a);
	mesh_to_cam(obs->eye_outer[0], b);
	for (int i = 0; i < 3; i++) {
		x[i] = a[i] - b[i];
	}
	mesh_to_cam(obs->forehead, a);
	mesh_to_cam(obs->chin, b);
	for (int i = 0; i < 3; i++) {
		t[i] = a[i] - b[i];
	}
	if (norm3(x) > 1e-3f && norm3(t) > 1e-3f) {
		normalize3(x);
		cross3(x, t, z);
		normalize3(z);
		cross3(z, x, y);
		const float m[3][3] = {{x[0], y[0], z[0]}, {x[1], y[1], z[1]}, {x[2], y[2], z[2]}};
		ot_quat_from_matrix(m, out->head_rotation);
	}
	return true;
}

void
ot_quat_rotate(const float q[4], const float v[3], float out[3])
{
	// v' = v + 2w (q × v) + 2 q × (q × v)
	const float qv[3] = {q[0], q[1], q[2]};
	float t[3], u[3];
	cross3(qv, v, t);
	for (int i = 0; i < 3; i++) {
		t[i] *= 2.0f;
	}
	cross3(qv, t, u);
	for (int i = 0; i < 3; i++) {
		out[i] = v[i] + q[3] * t[i] + u[i];
	}
}

void
ot_quat_mul(const float a[4], const float b[4], float out[4])
{
	const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	out[0] = x;
	out[1] = y;
	out[2] = z;
	out[3] = w;
}

void
ot_quat_from_matrix(const float m[3][3], float out[4])
{
	const float tr = m[0][0] + m[1][1] + m[2][2];
	float x, y, z, w;
	if (tr > 0.0f) {
		const float s = sqrtf(tr + 1.0f) * 2.0f;
		w = 0.25f * s;
		x = (m[2][1] - m[1][2]) / s;
		y = (m[0][2] - m[2][0]) / s;
		z = (m[1][0] - m[0][1]) / s;
	} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
		const float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
		w = (m[2][1] - m[1][2]) / s;
		x = 0.25f * s;
		y = (m[0][1] + m[1][0]) / s;
		z = (m[0][2] + m[2][0]) / s;
	} else if (m[1][1] > m[2][2]) {
		const float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
		w = (m[0][2] - m[2][0]) / s;
		x = (m[0][1] + m[1][0]) / s;
		y = 0.25f * s;
		z = (m[1][2] + m[2][1]) / s;
	} else {
		const float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
		w = (m[1][0] - m[0][1]) / s;
		x = (m[0][2] + m[2][0]) / s;
		y = (m[1][2] + m[2][1]) / s;
		z = 0.25f * s;
	}
	const float n = sqrtf(x * x + y * y + z * z + w * w);
	out[0] = x / n;
	out[1] = y / n;
	out[2] = z / n;
	out[3] = w / n;
}

void
ot_one_euro_reset(struct ot_one_euro *f)
{
	memset(f, 0, sizeof(*f));
}

static float
smoothing(float cutoff_hz, float dt)
{
	const float tau = 1.0f / (2.0f * (float)M_PI * cutoff_hz);
	return 1.0f / (1.0f + tau / dt);
}

float
ot_one_euro(struct ot_one_euro *f, float x, int64_t t_ns, float min_cutoff_hz, float beta, float d_cutoff_hz)
{
	if (!f->init) {
		f->init = true;
		f->x_prev = x;
		f->dx_prev = 0.0f;
		f->t_prev_ns = t_ns;
		return x;
	}
	if (t_ns <= f->t_prev_ns) {
		return f->x_prev;
	}
	const float dt = (float)((double)(t_ns - f->t_prev_ns) * 1e-9);
	const float dx = (x - f->x_prev) / dt;
	const float ad = smoothing(d_cutoff_hz, dt);
	const float dx_hat = f->dx_prev + ad * (dx - f->dx_prev);
	const float cutoff = min_cutoff_hz + beta * fabsf(dx_hat);
	const float a = smoothing(cutoff, dt);
	const float x_hat = f->x_prev + a * (x - f->x_prev);
	f->x_prev = x_hat;
	f->dx_prev = dx_hat;
	f->t_prev_ns = t_ns;
	return x_hat;
}
