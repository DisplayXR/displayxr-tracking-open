// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  otr-probe: run the provider against the fake host on a real camera
 *         and print what it publishes. A developer tool.
 *
 *     otr-probe [--provider PATH] [--seconds N] [--still S] [--set KEY=VALUE]...
 *               [--panel WxH] [--iris-scale X] [--quiet] [--hands]
 *
 * --hands runs the HANDS role instead (the input provider): it creates the two
 * hand devices and locates their joints at 30 Hz, as an app's xrLocateHandJointsEXT
 * would, and prints every second each hand's share of active locates, its palm
 * and wrist in display space, its camera distance, its size and the pinch
 * distance; at the end the palm jitter over the last S seconds. Settings then
 * also come from DXR_INPUT_OPEN_TRACKING_HANDS_<KEY>.
 *
 * Every second: samples/s, tracking share, capture -> push latency (median,
 * max), the eyes in display space, each eye's camera-space distance, the eye
 * separation. At the end: the jitter (standard deviation of each eye's
 * display-space position, mm) over the last S seconds (default 3: hold still).
 * The provider's own per-stage timings come on stderr (STATS_S=1 is set).
 *
 * The first run on macOS triggers the camera permission prompt for the
 * terminal application; answer it, then run again if it timed out.
 */

#include "fake_host.h"
#include "ot_platform.h"

#include "xrt/xrt_device.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
cmp_double(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static float
vdist(const struct xrt_vec3 *a, const struct xrt_vec3 *b)
{
	const float x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;
	return sqrtf(x * x + y * y + z * z);
}

//! The hands role on a real camera: joint locates at 30 Hz through the devices.
static int
run_hands(const char *provider, struct fh_config *cfg, double seconds, double still, bool quiet)
{
	struct xrt_input_plugin_iface *it = NULL;
	if (fh_load_input(provider, cfg, XRT_INPUT_PLUGIN_API_VERSION_CURRENT, &it) != XRT_SUCCESS || it == NULL) {
		fprintf(stderr, "cannot load / negotiate the hands role of %s\n", provider);
		return 2;
	}
	struct xrt_input_plugin_instance *inst = NULL;
	const int64_t tp = fh_now_ns();
	if (it->probe(&inst) != XRT_SUCCESS) {
		fprintf(stderr, "probe declined (see the [open-tracking-hands] line above)\n");
		return 3;
	}
	printf("probe: usable (%.1f ms)\n", (double)(fh_now_ns() - tp) * 1e-6);
	struct xrt_device *dev[XRT_INPUT_PLUGIN_MAX_DEVICES] = {0};
	uint32_t n = 0;
	if (it->create_devices(inst, dev, XRT_INPUT_PLUGIN_MAX_DEVICES, &n) != XRT_SUCCESS || n < 2) {
		fprintf(stderr, "create_devices failed\n");
		return 3;
	}
	const enum xrt_input_name names[2] = {XRT_INPUT_HT_UNOBSTRUCTED_LEFT, XRT_INPUT_HT_UNOBSTRUCTED_RIGHT};
	const char *side_name[2] = {"L", "R"};
	enum
	{
		HIST = 4096
	};
	static struct xrt_vec3 hist[2][HIST];
	static int64_t hist_t[2][HIST];
	uint32_t hn[2] = {0, 0};
	const int64_t run0 = fh_now_ns();
	int64_t next_print = run0 + 1000000000LL;
	uint32_t locates = 0, active[2] = {0, 0};
	struct xrt_hand_joint_set js[2];
	while (fh_now_ns() - run0 < (int64_t)(seconds * 1e9)) {
		const int64_t at = fh_now_ns();
		for (int s = 0; s < 2; s++) {
			int64_t ts = 0;
			dev[s]->get_hand_tracking(dev[s], names[s], at, &js[s], &ts);
			if (js[s].is_active) {
				active[s]++;
				if (hn[s] < HIST) {
					hist[s][hn[s]] = js[s].values.hand_joint_set_default[XRT_HAND_JOINT_PALM].relation.pose.position;
					hist_t[s][hn[s]++] = at;
				}
			}
		}
		locates++;
		if (fh_now_ns() >= next_print) {
			next_print += 1000000000LL;
			if (!quiet) {
				printf("t=%5.1fs", (double)(fh_now_ns() - run0) * 1e-9);
				for (int s = 0; s < 2; s++) {
					const struct xrt_hand_joint_value *v = js[s].values.hand_joint_set_default;
					if (!js[s].is_active) {
						printf("  %s: -- (%3u%%)", side_name[s], active[s] * 100u / (locates ? locates : 1));
						continue;
					}
					const struct xrt_vec3 *palm = &v[XRT_HAND_JOINT_PALM].relation.pose.position;
					const struct xrt_vec3 *wrist = &v[XRT_HAND_JOINT_WRIST].relation.pose.position;
					printf("  %s: %3u%% palm(%+.3f %+.3f %.3f) wrist(%+.3f %+.3f %.3f) size %.0f mm pinch %.0f mm",
					       side_name[s], active[s] * 100u / (locates ? locates : 1), palm->x, palm->y, palm->z,
					       wrist->x, wrist->y, wrist->z,
					       vdist(wrist, &v[XRT_HAND_JOINT_MIDDLE_PROXIMAL].relation.pose.position) * 1000.0f,
					       vdist(&v[XRT_HAND_JOINT_THUMB_TIP].relation.pose.position,
					             &v[XRT_HAND_JOINT_INDEX_TIP].relation.pose.position) *
					           1000.0f);
				}
				printf("\n");
			}
			locates = 0;
			active[0] = active[1] = 0;
		}
		fh_sleep_ms(33);
	}
	for (int s = 0; s < 2; s++) {
		dev[s]->destroy(dev[s]);
	}
	it->destroy(inst);

	// Palm jitter over the last `still` seconds, per hand.
	const int64_t end = fh_now_ns();
	for (int s = 0; s < 2; s++) {
		double sum[3] = {0}, sq[3] = {0};
		uint32_t m = 0;
		for (uint32_t i = 0; i < hn[s]; i++) {
			if (end - hist_t[s][i] > (int64_t)(still * 1e9)) {
				continue;
			}
			const float p[3] = {hist[s][i].x, hist[s][i].y, hist[s][i].z};
			for (int c = 0; c < 3; c++) {
				sum[c] += p[c];
				sq[c] += (double)p[c] * p[c];
			}
			m++;
		}
		if (m < 2) {
			printf("%s hand: not enough active locates in the last %.1f s\n", side_name[s], still);
			continue;
		}
		double sd[3], mean[3];
		for (int c = 0; c < 3; c++) {
			mean[c] = sum[c] / m;
			const double v = sq[c] / m - mean[c] * mean[c];
			sd[c] = sqrt(v > 0 ? v : 0) * 1000.0;
		}
		printf("%s hand, last %.1f s, %u active locates: palm mean (%+.4f, %+.4f, %.4f) m, std dev x %.2f y %.2f "
		       "z %.2f mm\n",
		       side_name[s], still, m, mean[0], mean[1], mean[2], sd[0], sd[1], sd[2]);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	char provider[4096] = "";
	double seconds = 10.0, still = 3.0;
	bool quiet = false, hands = false;
	struct fh_config cfg;
	fh_config_defaults(&cfg);
	cfg.env_prefix = "DXR_TRACKING_OPEN_TRACKING_";
	fh_set("STATS_S=1");
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
			ot_copy_str(provider, sizeof(provider), argv[++i]);
		} else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
			seconds = atof(argv[++i]);
		} else if (strcmp(argv[i], "--still") == 0 && i + 1 < argc) {
			still = atof(argv[++i]);
		} else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
			if (!fh_set(argv[++i])) {
				fprintf(stderr, "bad --set\n");
				return 2;
			}
		} else if (strcmp(argv[i], "--panel") == 0 && i + 1 < argc) {
			float w = 0, h = 0;
			if (sscanf(argv[++i], "%fx%f", &w, &h) != 2 || w <= 0 || h <= 0) {
				fprintf(stderr, "--panel WxH in metres\n");
				return 2;
			}
			cfg.panel_w_m = w;
			cfg.panel_h_m = h;
		} else if (strcmp(argv[i], "--iris-scale") == 0 && i + 1 < argc) {
			cfg.iris_scale = (float)atof(argv[++i]);
		} else if (strcmp(argv[i], "--quiet") == 0) {
			quiet = true;
		} else if (strcmp(argv[i], "--hands") == 0) {
			hands = true;
		} else {
			fprintf(stderr,
			        "usage: %s [--provider PATH] [--seconds N] [--still S] [--set KEY=VALUE]... [--panel WxH] "
			        "[--iris-scale X] [--quiet] [--hands]\n",
			        argv[0]);
			return 2;
		}
	}
	if (provider[0] == '\0') {
		// Next to this executable in the build tree.
		char dir[4096];
		if (!ot_module_dir(dir, sizeof(dir))) {
			ot_copy_str(dir, sizeof(dir), ".");
		}
#if defined(_WIN32)
		snprintf(provider, sizeof(provider), "%s\\DisplayXR-OpenTracking.dll", dir);
#elif defined(__APPLE__)
		snprintf(provider, sizeof(provider), "%s/DisplayXR-OpenTracking.dylib", dir);
#else
		snprintf(provider, sizeof(provider), "%s/DisplayXR-OpenTracking.so", dir);
#endif
	}

	if (hands) {
		cfg.env_prefix = "DXR_INPUT_OPEN_TRACKING_HANDS_";
		return run_hands(provider, &cfg, seconds, still, quiet);
	}

	struct xrt_tracking_plugin_iface *it = NULL;
	if (fh_load(provider, &cfg, XRT_TRACKING_PLUGIN_API_VERSION_CURRENT, &it) != XRT_SUCCESS || it == NULL) {
		fprintf(stderr, "cannot load / negotiate %s\n", provider);
		return 2;
	}
	struct xrt_tracking_plugin_instance *inst = NULL;
	const int64_t tp = fh_now_ns();
	if (it->probe(&inst) != XRT_SUCCESS) {
		fprintf(stderr, "probe declined (see the [open-tracking] line above)\n");
		return 3;
	}
	printf("probe: usable (%.1f ms)\n", (double)(fh_now_ns() - tp) * 1e-6);
	const int64_t t0 = fh_now_ns();
	if (it->start(inst, fh_sink()) != XRT_SUCCESS) {
		fprintf(stderr, "start failed\n");
		return 3;
	}
	enum xrt_tracking_provider_status st = it->get_status(inst);
	while (st == XRT_TRACKING_PROVIDER_STATUS_UNKNOWN && fh_now_ns() - t0 < 180000000000LL) {
		fh_sleep_ms(20);
		st = it->get_status(inst);
	}
	struct xrt_tracking_provider_info info;
	memset(&info, 0, sizeof(info));
	info.struct_size = (uint32_t)sizeof(info);
	it->describe(inst, &info);
	printf("start -> %s in %.0f ms; source: %s; camera key: %s\n",
	       st == XRT_TRACKING_PROVIDER_STATUS_RUNNING ? "RUNNING" : "NOT RUNNING",
	       (double)(fh_now_ns() - t0) * 1e-6, info.source, info.camera_key);
	if (st != XRT_TRACKING_PROVIDER_STATUS_RUNNING) {
		it->stop(inst);
		return 4;
	}

	static struct fh_record recs[FH_RING];
	uint64_t cursor = fh_count();
	const int64_t run0 = fh_now_ns();
	int64_t first_sample_ns = 0;
	while (fh_now_ns() - run0 < (int64_t)(seconds * 1e9)) {
		fh_sleep_ms(1000);
		const uint32_t n = fh_records(cursor, recs, FH_RING);
		cursor += n;
		if (n == 0) {
			printf("t=%5.1fs  no samples (no face yet?)\n", (double)(fh_now_ns() - run0) * 1e-9);
			continue;
		}
		if (first_sample_ns == 0) {
			first_sample_ns = recs[0].push_ns;
		}
		double lat[FH_RING];
		uint32_t tracked = 0;
		for (uint32_t i = 0; i < n; i++) {
			lat[i] = (double)(recs[i].push_ns - recs[i].s.capture_ns) * 1e-6;
			tracked += (recs[i].s.flags & XRT_TRACKING_SAMPLE_TRACKING) ? 1u : 0u;
		}
		qsort(lat, n, sizeof(double), cmp_double);
		const struct xrt_tracking_viewer_sample *s = &recs[n - 1].s;
		const float *l = s->left_eye, *r = s->right_eye;
		const float sep = sqrtf((l[0] - r[0]) * (l[0] - r[0]) + (l[1] - r[1]) * (l[1] - r[1]) +
		                        (l[2] - r[2]) * (l[2] - r[2]));
		if (!quiet) {
			printf("t=%5.1fs %3u/s tracked %3u%%  lat %5.1f/%5.1f ms  L(%+.3f %+.3f %.3f) R(%+.3f %+.3f %.3f) m  "
			       "cam dist %.3f/%.3f m  sep %.1f mm\n",
			       (double)(fh_now_ns() - run0) * 1e-9, n, tracked * 100u / n, lat[n / 2], lat[n - 1], l[0], l[1],
			       l[2], r[0], r[1], r[2], s->camera.left_distance_m, s->camera.right_distance_m, sep * 1000.0f);
		}
	}
	it->stop(inst);

	// Jitter over the last `still` seconds.
	const uint32_t n = fh_records(0, recs, FH_RING);
	const int64_t end = n > 0 ? recs[n - 1].s.capture_ns : 0;
	double sum[2][3] = {{0}}, sq[2][3] = {{0}}, dist[2] = {0}, lat_sum = 0;
	uint32_t m = 0;
	for (uint32_t i = 0; i < n; i++) {
		const struct xrt_tracking_viewer_sample *s = &recs[i].s;
		if (end - s->capture_ns > (int64_t)(still * 1e9) || !(s->flags & XRT_TRACKING_SAMPLE_TRACKING)) {
			continue;
		}
		for (int k = 0; k < 3; k++) {
			sum[0][k] += s->left_eye[k];
			sum[1][k] += s->right_eye[k];
			sq[0][k] += (double)s->left_eye[k] * s->left_eye[k];
			sq[1][k] += (double)s->right_eye[k] * s->right_eye[k];
		}
		dist[0] += s->camera.left_distance_m;
		dist[1] += s->camera.right_distance_m;
		lat_sum += (double)(recs[i].push_ns - s->capture_ns) * 1e-6;
		m++;
	}
	if (m < 2) {
		printf("jitter: not enough tracked samples in the last %.1f s\n", still);
		return 5;
	}
	printf("\nlast %.1f s, %u tracked samples (%.1f /s), mean capture->push %.1f ms\n", still, m, m / still,
	       lat_sum / m);
	const char *eye[2] = {"left ", "right"};
	for (int e = 0; e < 2; e++) {
		double sd[3], mean[3];
		for (int k = 0; k < 3; k++) {
			mean[k] = sum[e][k] / m;
			const double v = sq[e][k] / m - mean[k] * mean[k];
			sd[k] = sqrt(v > 0 ? v : 0) * 1000.0;
		}
		printf("%s eye mean (%+.4f, %+.4f, %.4f) m  std dev x %.2f  y %.2f  z %.2f mm  raw camera distance %.3f m\n",
		       eye[e], mean[0], mean[1], mean[2], sd[0], sd[1], sd[2], dist[e] / m);
	}
	return 0;
}
