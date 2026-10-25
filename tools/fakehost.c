// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CTest driver: runs the provider binary through the runtime's call
 *         sequence (negotiate, describe_settings, probe, describe, start,
 *         get_status, stop, destroy) against the fake host, with the file
 *         camera, and checks the contract.
 *
 *     open-tracking-fakehost <provider> <case> [--set KEY=VALUE]...
 *
 *   negotiate        another ABI major is refused; ours is accepted; settings are declared
 *   decline-disabled ENABLE=0 -> probe declines
 *   decline-models   MODEL_DIR without the models -> probe declines
 *   decline-camera   a DEVICE that matches nothing -> probe declines
 *   blank            (models; --make-blank writes the frame) a frame with no face: running, nothing pushed, clean stop, restart
 *   face             (models) a face photo: tracked samples, plausible eyes, the camera block, clean stop
 *
 * The hands role (input provider, the same binary): negotiate, probe, create_devices, then joint locates
 * through the devices' get_hand_tracking, as the runtime's state tracker makes them.
 *   hands-negotiate        another input ABI major is refused; ours is accepted; id; settings; presence UNKNOWN
 *   hands-decline-disabled ENABLE=0 -> probe declines
 *   hands-decline-models   MODEL_DIR without the models -> probe declines
 *   hands-blank            (models) no hand: two hand-tracker devices on a RIG_LOCAL origin, PRESENT, never active
 *   hands                  (models) a hand photo (--expect left|right): that hand only, plausible joints
 *
 * Exit: 0 pass, 1 a check failed, 2 usage / load error, 77 skipped (models or test image absent).
 */

#include "fake_host.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_tracking.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIP 77

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
	fflush(stdout);
	if (!ok) {
		g_fail++;
	}
}

static const char *
status_name(enum xrt_tracking_provider_status s)
{
	switch (s) {
	case XRT_TRACKING_PROVIDER_STATUS_UNKNOWN: return "UNKNOWN";
	case XRT_TRACKING_PROVIDER_STATUS_STOPPED: return "STOPPED";
	case XRT_TRACKING_PROVIDER_STATUS_RUNNING: return "RUNNING";
	case XRT_TRACKING_PROVIDER_STATUS_NO_SOURCE: return "NO_SOURCE";
	}
	return "?";
}

//! Wait up to @p ms for get_status to leave UNKNOWN (models load, Core ML compile).
static enum xrt_tracking_provider_status
wait_status(struct xrt_tracking_plugin_iface *it, struct xrt_tracking_plugin_instance *inst, uint32_t ms)
{
	enum xrt_tracking_provider_status s = it->get_status(inst);
	for (uint32_t t = 0; t < ms && s == XRT_TRACKING_PROVIDER_STATUS_UNKNOWN; t += 50) {
		fh_sleep_ms(50);
		s = it->get_status(inst);
	}
	return s;
}

static int
run_face(struct xrt_tracking_plugin_iface *it, struct xrt_tracking_plugin_instance *inst)
{
	struct xrt_tracking_provider_info info;
	memset(&info, 0, sizeof(info));
	info.struct_size = (uint32_t)sizeof(info);
	check(it->describe(inst, &info) == XRT_SUCCESS, "describe");
	check((info.tracks & XRT_TRACKING_TRACKS_EYE_PAIR) && (info.tracks & XRT_TRACKING_TRACKS_CAMERA_SPACE),
	      "describe: EYE_PAIR + CAMERA_SPACE (tracks 0x%x)", info.tracks);
	check(strncmp(info.camera_key, "file:", 5) == 0, "describe: camera_key '%s'", info.camera_key);

	const uint64_t c0 = fh_count();
	check(it->start(inst, fh_sink()) == XRT_SUCCESS, "start returns at once");
	const enum xrt_tracking_provider_status st = wait_status(it, inst, 60000);
	check(st == XRT_TRACKING_PROVIDER_STATUS_RUNNING, "status RUNNING (%s)", status_name(st));
	for (int i = 0; i < 100 && fh_count() < c0 + 20; i++) {
		fh_sleep_ms(50);
	}
	static struct fh_record recs[64];
	const uint32_t n = fh_records(c0, recs, 64);
	check(n >= 10, "samples pushed (%u)", n);
	uint32_t tracked = 0;
	for (uint32_t i = 0; i < n; i++) {
		tracked += (recs[i].s.flags & XRT_TRACKING_SAMPLE_TRACKING) ? 1u : 0u;
	}
	check(n > 0 && tracked == n, "every sample TRACKING (%u/%u)", tracked, n);
	if (n > 0) {
		const struct xrt_tracking_viewer_sample *s = &recs[n - 1].s;
		const float *l = s->left_eye, *r = s->right_eye;
		const float sep = sqrtf((l[0] - r[0]) * (l[0] - r[0]) + (l[1] - r[1]) * (l[1] - r[1]) +
		                        (l[2] - r[2]) * (l[2] - r[2]));
		printf("       left (%.4f, %.4f, %.4f) right (%.4f, %.4f, %.4f) m, sep %.1f mm, conf %.3f\n", l[0], l[1],
		       l[2], r[0], r[1], r[2], sep * 1000.0f, s->confidence);
		printf("       camera '%s' left dir (%.4f, %.4f, %.4f) %.3f m, right dir (%.4f, %.4f, %.4f) %.3f m\n",
		       s->camera.camera_key, s->camera.left_dir[0], s->camera.left_dir[1], s->camera.left_dir[2],
		       s->camera.left_distance_m, s->camera.right_dir[0], s->camera.right_dir[1],
		       s->camera.right_dir[2], s->camera.right_distance_m);
		check(s->struct_size == sizeof(*s), "struct_size");
		check((s->flags & XRT_TRACKING_SAMPLE_EYES_VALID) && (s->flags & XRT_TRACKING_SAMPLE_HEAD_POSE_VALID),
		      "EYES_VALID + HEAD_POSE_VALID");
		check(l[0] < r[0], "the user's left eye is at the smaller display x");
		check(l[2] > 0.15f && l[2] < 3.0f && r[2] > 0.15f && r[2] < 3.0f, "eyes in front of the display");
		check(sep > 0.04f && sep < 0.09f, "eye separation plausible (%.1f mm)", sep * 1000.0f);
		check((s->flags & XRT_TRACKING_SAMPLE_CAMERA_VALID) && strncmp(s->camera.camera_key, "file:", 5) == 0,
		      "camera block valid");
		const float *ld = s->camera.left_dir;
		check(fabsf(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2] - 1.0f) < 1e-4f && ld[2] > 0.0f,
		      "camera dir is a unit vector toward the viewer");
		check(s->camera.left_dir[0] < s->camera.right_dir[0], "camera frame: the user's left eye at smaller X");
		const float *q = s->head_orientation;
		check(fabsf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] - 1.0f) < 1e-3f, "head orientation unit");
		bool mono = true;
		for (uint32_t i = 1; i < n; i++) {
			mono = mono && recs[i].s.capture_ns > recs[i - 1].s.capture_ns &&
			       recs[i].push_ns >= recs[i].s.capture_ns;
		}
		check(mono, "capture_ns increasing and never after the push");
	}
	it->stop(inst);
	const uint64_t after = fh_count();
	fh_sleep_ms(300);
	check(fh_count() == after, "nothing pushed after stop returned");
	check(it->get_status(inst) == XRT_TRACKING_PROVIDER_STATUS_STOPPED, "status STOPPED after stop");
	return 0;
}

static int
run_blank(struct xrt_tracking_plugin_iface *it, struct xrt_tracking_plugin_instance *inst)
{
	for (int round = 0; round < 2; round++) {
		const uint64_t c0 = fh_count();
		check(it->start(inst, fh_sink()) == XRT_SUCCESS, "start (round %d)", round + 1);
		const enum xrt_tracking_provider_status st = wait_status(it, inst, 60000);
		check(st == XRT_TRACKING_PROVIDER_STATUS_RUNNING, "status RUNNING (%s)", status_name(st));
		fh_sleep_ms(700);
		check(fh_count() == c0, "no face, nothing pushed (the runtime drops eye-less samples)");
		it->stop(inst);
		check(it->get_status(inst) == XRT_TRACKING_PROVIDER_STATUS_STOPPED, "STOPPED after stop");
	}
	return 0;
}


/*
 *
 * The hands role.
 *
 */

static void
quat_rotate(const struct xrt_quat *q, const float v[3], float out[3])
{
	// v' = v + 2 w (q x v) + 2 q x (q x v)
	const float qx = q->x, qy = q->y, qz = q->z, qw = q->w;
	const float tx = 2.0f * (qy * v[2] - qz * v[1]);
	const float ty = 2.0f * (qz * v[0] - qx * v[2]);
	const float tz = 2.0f * (qx * v[1] - qy * v[0]);
	out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
	out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
	out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

static const struct xrt_hand_joint_value *
joint(const struct xrt_hand_joint_set *s, enum xrt_hand_joint j)
{
	return &s->values.hand_joint_set_default[j];
}

static float
dist(const struct xrt_vec3 *a, const struct xrt_vec3 *b)
{
	const float x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;
	return sqrtf(x * x + y * y + z * z);
}

//! Locate both hands until one is active (or @p ms pass). Returns the side found: 0 left, 1 right, -1 none.
static int
locate_until_active(struct xrt_device *dev[2], uint32_t ms, struct xrt_hand_joint_set out[2], int64_t *at)
{
	const enum xrt_input_name names[2] = {XRT_INPUT_HT_UNOBSTRUCTED_LEFT, XRT_INPUT_HT_UNOBSTRUCTED_RIGHT};
	for (uint32_t t = 0; t <= ms; t += 50) {
		*at = fh_now_ns() + 20000000LL; // a frame's display time, ahead of now
		int found = -1;
		for (int side = 0; side < 2; side++) {
			int64_t ts = 0;
			const xrt_result_t r = dev[side]->get_hand_tracking(dev[side], names[side], *at, &out[side], &ts);
			if (r != XRT_SUCCESS) {
				check(false, "get_hand_tracking returned %d", (int)r);
				return -1;
			}
			if (out[side].is_active && found < 0) {
				found = side;
			}
		}
		if (found >= 0) {
			return found;
		}
		fh_sleep_ms(50);
	}
	return -1;
}

static int
run_hands(struct xrt_input_plugin_iface *it, struct xrt_input_plugin_instance *inst, const char *expect)
{
	struct xrt_device *dev[XRT_INPUT_PLUGIN_MAX_DEVICES] = {0};
	uint32_t n = 0;
	check(it->create_devices(inst, dev, XRT_INPUT_PLUGIN_MAX_DEVICES, &n) == XRT_SUCCESS && n == 2,
	      "create_devices: two devices (%u)", n);
	if (n != 2) {
		return 0;
	}
	for (int side = 0; side < 2; side++) {
		struct xrt_device *d = dev[side];
		const enum xrt_input_name want = side == 0 ? XRT_INPUT_HT_UNOBSTRUCTED_LEFT : XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;
		bool has_ht = false;
		for (size_t i = 0; i < d->input_count; i++) {
			has_ht = has_ht || d->inputs[i].name == want;
		}
		check(d->device_type == XRT_DEVICE_TYPE_HAND_TRACKER && d->supported.hand_tracking && has_ht,
		      "%s: a hand tracker carrying XRT_INPUT_HT_UNOBSTRUCTED_%s", d->str, side == 0 ? "LEFT" : "RIGHT");
		check(d->tracking_origin != NULL && d->tracking_origin->type == XRT_TRACKING_TYPE_RIG_LOCAL,
		      "  display-plane relative (RIG_LOCAL origin)");
		check(d->destroy != NULL && d->get_view_poses != NULL && d->get_battery_status != NULL &&
		          d->update_inputs != NULL && d->get_tracked_pose != NULL,
		      "  every vtable slot filled");
	}
	check(it->get_presence(inst) == XRT_INPUT_PROVIDER_PRESENCE_PRESENT, "presence PRESENT after probe");

	struct xrt_hand_joint_set js[2];
	int64_t at = 0;
	if (expect == NULL) {
		const int found = locate_until_active(dev, 3000, js, &at);
		check(found < 0, "no hand in a blank frame: both sides inactive");
		check(it->get_presence(inst) == XRT_INPUT_PROVIDER_PRESENCE_PRESENT, "  still PRESENT (empty view != absent)");
	} else {
		const int want = strcmp(expect, "left") == 0 ? 0 : 1;
		const int found = locate_until_active(dev, 60000, js, &at);
		check(found == want, "the %s hand is active (found %s)", expect,
		      found == 0 ? "left" : found == 1 ? "right" : "none");
		// Let the filter settle on the still image, then locate again.
		fh_sleep_ms(400);
		(void)locate_until_active(dev, 1000, js, &at);
		check(!js[1 - want].is_active, "the other hand is not");
		if (found == want) {
			const struct xrt_hand_joint_set *s = &js[want];
			const struct xrt_vec3 *palm = &joint(s, XRT_HAND_JOINT_PALM)->relation.pose.position;
			const struct xrt_vec3 *wrist = &joint(s, XRT_HAND_JOINT_WRIST)->relation.pose.position;
			const struct xrt_vec3 *mmcp = &joint(s, XRT_HAND_JOINT_MIDDLE_PROXIMAL)->relation.pose.position;
			const struct xrt_vec3 *thumb = &joint(s, XRT_HAND_JOINT_THUMB_TIP)->relation.pose.position;
			const struct xrt_vec3 *index = &joint(s, XRT_HAND_JOINT_INDEX_TIP)->relation.pose.position;
			const struct xrt_vec3 *little = &joint(s, XRT_HAND_JOINT_LITTLE_PROXIMAL)->relation.pose.position;
			printf("       palm (%.4f, %.4f, %.4f) wrist (%.4f, %.4f, %.4f) m, wrist->middle knuckle %.1f mm, "
			       "thumb tip x %.4f\n",
			       palm->x, palm->y, palm->z, wrist->x, wrist->y, wrist->z, dist(wrist, mmcp) * 1000.0f, thumb->x);
			bool flags = true, unit = true, radii = true;
			for (int j = 0; j < XRT_HAND_JOINT_COUNT; j++) {
				const struct xrt_hand_joint_value *v = joint(s, (enum xrt_hand_joint)j);
				flags = flags && (v->relation.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) &&
				        (v->relation.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
				const struct xrt_quat *q = &v->relation.pose.orientation;
				unit = unit && fabsf(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w - 1.0f) < 1e-3f;
				radii = radii && v->radius > 0.003f && v->radius < 0.03f;
			}
			check(flags, "every joint valid + tracked");
			check(unit, "every orientation a unit quaternion");
			check(radii, "radii 3..30 mm");
			check(palm->z > 0.03f && palm->z < 1.5f, "the hand is in front of the display (palm z %.3f m)", palm->z);
			const float size = dist(wrist, mmcp);
			check(size > 0.06f && size < 0.13f, "metric hand size plausible (wrist->middle knuckle %.1f mm)",
			      size * 1000.0f);
			// Thumb side: a left hand with its palm to the camera (back to the user) has the thumb at the
			// user's right (+X); the mirrored image is a right hand, thumb at -X.
			check(want == 0 ? thumb->x > little->x : thumb->x < little->x, "thumb on the %s side (%.3f vs %.3f)",
			      want == 0 ? "+X" : "-X", thumb->x, little->x);
			// -Z along the bones: the middle finger's proximal joint looks at its intermediate joint.
			const struct xrt_hand_joint_value *mp = joint(s, XRT_HAND_JOINT_MIDDLE_PROXIMAL);
			const struct xrt_vec3 *mi = &joint(s, XRT_HAND_JOINT_MIDDLE_INTERMEDIATE)->relation.pose.position;
			const float mz[3] = {0.0f, 0.0f, -1.0f}, py[3] = {0.0f, 1.0f, 0.0f};
			float fwd[3], up[3];
			quat_rotate(&mp->relation.pose.orientation, mz, fwd);
			const float bone[3] = {mi->x - mp->relation.pose.position.x, mi->y - mp->relation.pose.position.y,
			                       mi->z - mp->relation.pose.position.z};
			const float bl = sqrtf(bone[0] * bone[0] + bone[1] * bone[1] + bone[2] * bone[2]);
			check(bl > 0 && (fwd[0] * bone[0] + fwd[1] * bone[1] + fwd[2] * bone[2]) / bl > 0.99f,
			      "joint -Z points along the bone toward the fingertip");
			// The photo shows the palm: the back of the hand (+Y) faces away from the camera, toward the user.
			quat_rotate(&joint(s, XRT_HAND_JOINT_PALM)->relation.pose.orientation, py, up);
			check(up[2] > 0.5f, "+Y out of the back of the hand: toward the user (%.2f)", up[2]);
			check(dist(thumb, index) > 0.035f, "an open hand: no pinch distance (%.0f mm)", dist(thumb, index) * 1000.0f);
		}
	}
	for (int side = 0; side < 2; side++) {
		dev[side]->destroy(dev[side]);
	}
	return 0;
}

static int
run_hands_mode(const char *provider, const char *mode, const struct fh_config *cfg, const char *models,
               const char *image, const char *expect)
{
	struct xrt_input_plugin_iface *it = NULL;
	if (strcmp(mode, "hands-negotiate") == 0) {
		xrt_result_t r = fh_load_input(provider, cfg, XRT_INPUT_PLUGIN_API_VERSION_CURRENT + 1, &it);
		check(r == XRT_ERROR_PROBER_NOT_SUPPORTED && it == NULL, "another input ABI major is refused");
		r = fh_load_input(provider, cfg, XRT_INPUT_PLUGIN_API_VERSION_CURRENT, &it);
		check(r == XRT_SUCCESS && it != NULL, "input ABI %d accepted", XRT_INPUT_PLUGIN_API_VERSION_CURRENT);
		if (it == NULL) {
			return 1;
		}
		check(it->struct_size == sizeof(*it) && strcmp(it->id, "open-tracking-hands") == 0, "id open-tracking-hands");
		check(XRT_INPUT_PLUGIN_IFACE_HAS(it, get_presence) && XRT_INPUT_PLUGIN_IFACE_HAS(it, describe_settings),
		      "get_presence + describe_settings slots");
		check(it->get_presence(NULL) == XRT_INPUT_PROVIDER_PRESENCE_UNKNOWN, "presence UNKNOWN before probe");
		const struct xrt_component_setting_desc *d = NULL;
		const uint32_t n = it->describe_settings(&d);
		bool keys_ok = n > 0 && d != NULL;
		for (uint32_t i = 0; keys_ok && i < n; i++) {
			for (const char *c = d[i].key; *c != '\0'; c++) {
				keys_ok = keys_ok && ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_');
			}
		}
		check(keys_ok, "describe_settings: %u keys, [A-Z0-9_]", n);
		return g_fail ? 1 : 0;
	}
	if (fh_load_input(provider, cfg, XRT_INPUT_PLUGIN_API_VERSION_CURRENT, &it) != XRT_SUCCESS || it == NULL) {
		return 2;
	}
	struct xrt_input_plugin_instance *inst = NULL;
	const xrt_result_t pr = it->probe(&inst);
	if (strncmp(mode, "hands-decline-", 14) == 0) {
		check(pr == XRT_ERROR_PROBER_NOT_SUPPORTED, "%s: probe declines cleanly (%d)", mode, (int)pr);
		it->destroy(inst);
		return g_fail ? 1 : 0;
	}
	if (models == NULL || image == NULL) {
		printf("[SKIP] %s needs --models and --image\n", mode);
		return SKIP;
	}
	if (pr != XRT_SUCCESS) {
		printf("[SKIP] probe declined (models or image absent?): %d\n", (int)pr);
		return SKIP;
	}
	check(true, "probe: usable");
	run_hands(it, inst, strcmp(mode, "hands") == 0 ? (expect != NULL ? expect : "left") : NULL);
	it->destroy(inst); // joins the provider's thread
	check(true, "destroy returned (thread joined)");
	return g_fail ? 1 : 0;
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <provider> <case> [--set KEY=VALUE]... [--models DIR] [--image FILE]\n", argv[0]);
		return 2;
	}
	const char *provider = argv[1], *mode = argv[2];
	const char *models = NULL, *image = NULL, *expect = NULL;
	bool make_blank = false;
	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
			if (!fh_set(argv[++i])) {
				return 2;
			}
		} else if (strcmp(argv[i], "--models") == 0 && i + 1 < argc) {
			models = argv[++i];
		} else if (strcmp(argv[i], "--make-blank") == 0) {
			make_blank = true;
		} else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
			image = argv[++i];
		} else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
			expect = argv[++i];
		} else {
			fprintf(stderr, "unknown argument %s\n", argv[i]);
			return 2;
		}
	}
	struct fh_config cfg;
	fh_config_defaults(&cfg);
	struct xrt_tracking_plugin_iface *it = NULL;

	if (strcmp(mode, "hands-negotiate") == 0) {
		return run_hands_mode(provider, mode, &cfg, models, image, expect);
	}
	if (strcmp(mode, "negotiate") == 0) {
		xrt_result_t r = fh_load(provider, &cfg, XRT_TRACKING_PLUGIN_API_VERSION_CURRENT + 1, &it);
		check(r == XRT_ERROR_PROBER_NOT_SUPPORTED && it == NULL, "another ABI major is refused");
		r = fh_load(provider, &cfg, XRT_TRACKING_PLUGIN_API_VERSION_CURRENT, &it);
		check(r == XRT_SUCCESS && it != NULL, "ABI %d accepted", XRT_TRACKING_PLUGIN_API_VERSION_CURRENT);
		if (it == NULL) {
			return 1;
		}
		check(it->struct_size == sizeof(*it) && strcmp(it->id, "open-tracking") == 0, "id open-tracking");
		const struct xrt_component_setting_desc *d = NULL;
		const uint32_t n = it->describe_settings != NULL ? it->describe_settings(&d) : 0;
		bool keys_ok = n > 0 && d != NULL;
		for (uint32_t i = 0; keys_ok && i < n; i++) {
			for (const char *c = d[i].key; *c != '\0'; c++) {
				keys_ok = keys_ok && ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_');
			}
			keys_ok = keys_ok && d[i].type != NULL && d[i].default_value != NULL && d[i].help != NULL;
		}
		check(keys_ok, "describe_settings: %u keys, [A-Z0-9_]", n);
		return g_fail ? 1 : 0;
	}

	if (models != NULL) {
		char kv[4096];
		snprintf(kv, sizeof(kv), "MODEL_DIR=%s", models);
		fh_set(kv);
	}
	if (image != NULL && make_blank) {
		// A test fixture, not a camera frame: mid-grey, no face.
		FILE *f = fopen(image, "wb");
		if (f == NULL) {
			return 2;
		}
		fprintf(f, "P6\n320 240\n255\n");
		for (int i = 0; i < 320 * 240 * 3; i++) {
			fputc(0x80, f);
		}
		fclose(f);
	}
	if (image != NULL) {
		char kv[4096];
		snprintf(kv, sizeof(kv), "DEVICE=file:%s", image);
		fh_set(kv);
	}
	if (strncmp(mode, "hands", 5) == 0) {
		return run_hands_mode(provider, mode, &cfg, models, image, expect);
	}
	if (fh_load(provider, &cfg, XRT_TRACKING_PLUGIN_API_VERSION_CURRENT, &it) != XRT_SUCCESS || it == NULL) {
		return 2;
	}
	struct xrt_tracking_plugin_instance *inst = NULL;
	const xrt_result_t pr = it->probe(&inst);

	if (strncmp(mode, "decline-", 8) == 0) {
		check(pr == XRT_ERROR_PROBER_NOT_SUPPORTED, "%s: probe declines cleanly (%d)", mode, (int)pr);
		check(it->get_status(inst) == XRT_TRACKING_PROVIDER_STATUS_STOPPED, "status STOPPED");
		it->destroy(inst);
		return g_fail ? 1 : 0;
	}
	if (models == NULL || image == NULL) {
		printf("[SKIP] %s needs --models and --image\n", mode);
		return SKIP;
	}
	if (pr != XRT_SUCCESS) {
		printf("[SKIP] probe declined (models or image absent?): %d\n", (int)pr);
		return SKIP;
	}
	check(true, "probe: usable");
	if (strcmp(mode, "face") == 0) {
		run_face(it, inst);
	} else if (strcmp(mode, "blank") == 0) {
		run_blank(it, inst);
	} else {
		fprintf(stderr, "unknown case %s\n", mode);
		return 2;
	}
	it->destroy(inst);
	return g_fail ? 1 : 0;
}
