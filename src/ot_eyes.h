// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Landmarks -> eye points in the CAMERA's frame, the pose transform
 *         into display space, and the One Euro filter. Pure C: unit-tested
 *         with synthetic landmarks at known distances.
 *
 * Camera frame (`xrt_tracking_camera_sample`): origin at the optical centre,
 * +X right as the USER sees it (un-mirrored), +Y up, +Z out of the lens toward
 * the viewer. A camera facing the user sees the user's right on the image's
 * left, so a pixel (u, v) of the un-mirrored image has the direction
 * normalize(-(u - cx) / f, -(v - cy) / f, 1).
 *
 * Intrinsics: a pinhole with square pixels, the principal point at the image
 * centre, no distortion, f = (W / 2) / tan(HFOV / 2). The field of view is a
 * setting (HFOV_DEG); nothing here invents it.
 *
 * Distance: two monocular estimates of each eye point's depth, fused by
 * inverse variance (see ot_estimate_eyes), then turned into a distance along
 * the eye's ray. Both are scaled by the viewer's per-user scale (the host's
 * `viewer_iris_scale`) at the caller.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ot_intrinsics
{
	float f;      //!< focal length, px
	float cx, cy; //!< principal point, px (continuous coordinates)
	uint32_t width, height;
};

//! From the horizontal field of view; false outside 10..170 degrees or for an empty image.
bool
ot_intrinsics_from_hfov(uint32_t width, uint32_t height, float hfov_deg, struct ot_intrinsics *out);

//! Unit direction of pixel (u, v) of the un-mirrored image, camera frame.
void
ot_pixel_to_dir(const struct ot_intrinsics *k, float u, float v, float out[3]);

/*!
 * What the estimator needs from one face, in LOGICAL image px (un-mirrored);
 * z is the mesh's relative depth in the same px scale (smaller = closer).
 * Index 0 is the user's LEFT eye (landmark 473 and its ring), 1 the RIGHT (468).
 */
struct ot_face_obs
{
	float iris[2][3];         //!< iris centres (u, v, z)
	float iris_ring[2][4][2]; //!< the four contour points of each iris (u, v)
	float eye_outer[2][3];    //!< outer eye corners: [0] = 263 (left), [1] = 33 (right)
	float forehead[3];        //!< 10
	float chin[3];            //!< 152
};

struct ot_eye_params
{
	float iris_diameter_m; //!< the population's iris diameter (11.7 mm)
	float ipd_m;           //!< the nominal interpupillary distance (63 mm)
	float pixel_sigma;     //!< landmark noise per coordinate, px (0.5)
	float iris_pop_sigma;  //!< relative spread of the iris diameter across people (0.05)
	float ipd_pop_sigma;   //!< relative spread of the IPD across people (0.06)
	float yaw_sigma_gain;  //!< relative error of the IPD estimate per unit of (1 - foreshortening) (0.25)
};

void
ot_eye_params_defaults(struct ot_eye_params *p);

struct ot_eye_estimate
{
	float dir[2][3];        //!< unit direction to each eye point; [0] = user's left
	float depth_iris_m[2];  //!< depth (Z) from each iris's diameter
	float depth_ipd_m[2];   //!< depth (Z) from the yaw-corrected eye separation
	float weight_iris;      //!< the iris estimate's share of the fused depth, 0..1
	float distance_m[2];    //!< fused, along dir; NOT scaled by the per-user scale
	float foreshortening;   //!< projected / true eye separation (cos of the head's yaw, roughly)
	float head_rotation[4]; //!< head orientation x, y, z, w in the camera frame (OpenXR head axes)
	float iris_diameter_px[2];
};

/*!
 * Landmarks -> camera-space eye points.
 *
 * (a) Iris: Z = f · D / d, D = @ref ot_eye_params::iris_diameter_m and d the
 *     iris's larger contour extent (horizontal 1-3, vertical 2-4; the larger is
 *     the less foreshortened).
 * (b) Separation: Z = f · IPD / |s3|, |s3| = the 3D distance between the iris
 *     centres in the mesh's px scale (so a yawed head is corrected by the
 *     mesh's own depth), per eye offset by the mesh's relative depth.
 * Fused per eye with weights 1 / σ², σ² = (pixel noise / measured px)² +
 * population spread² (+ a yaw term for (b)). The depth becomes a distance along
 * the eye's ray: distance = Z / dir.z.
 *
 * @return false when the geometry is unusable (degenerate iris or eye line,
 *         a depth outside 0.1 .. 5 m).
 */
bool
ot_estimate_eyes(const struct ot_intrinsics *k,
                 const struct ot_eye_params *p,
                 const struct ot_face_obs *obs,
                 struct ot_eye_estimate *out);


/*
 *
 * Pose (camera -> display): display = T + R · camera.
 *
 */

void
ot_quat_rotate(const float q[4], const float v[3], float out[3]);

void
ot_quat_mul(const float a[4], const float b[4], float out[4]);

//! Rotation matrix (columns = the frame's axes) to a unit quaternion x, y, z, w.
void
ot_quat_from_matrix(const float m[3][3], float out[4]);


/*
 *
 * One Euro filter: Casiez, Roussel, Vogel, "1€ Filter: A Simple Speed-based
 * Low-pass Filter for Noisy Input in Interactive Systems", CHI 2012.
 * A first-order low-pass whose cutoff rises with the signal's speed:
 * cutoff = min_cutoff + beta · |dx/dt| (dx/dt itself low-passed at d_cutoff).
 *
 */

struct ot_one_euro
{
	bool init;
	float x_prev;
	float dx_prev;
	int64_t t_prev_ns;
};

void
ot_one_euro_reset(struct ot_one_euro *f);

/*!
 * Filter @p x taken at @p t_ns. The first sample passes through; a
 * non-increasing timestamp returns the previous output.
 */
float
ot_one_euro(struct ot_one_euro *f, float x, int64_t t_ns, float min_cutoff_hz, float beta, float d_cutoff_hz);

#ifdef __cplusplus
}
#endif
