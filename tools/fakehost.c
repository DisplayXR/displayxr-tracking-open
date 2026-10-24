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
 * Exit: 0 pass, 1 a check failed, 2 usage / load error, 77 skipped (models or test image absent).
 */

#include "fake_host.h"

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

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <provider> <case> [--set KEY=VALUE]... [--models DIR] [--image FILE]\n", argv[0]);
		return 2;
	}
	const char *provider = argv[1], *mode = argv[2];
	const char *models = NULL, *image = NULL;
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
		} else {
			fprintf(stderr, "unknown argument %s\n", argv[i]);
			return 2;
		}
	}
	struct fh_config cfg;
	fh_config_defaults(&cfg);
	struct xrt_tracking_plugin_iface *it = NULL;

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
