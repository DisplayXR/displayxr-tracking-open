// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The fake tracking host, see fake_host.h.
 */

#include "fake_host.h"
#include "ot_platform.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct
{
	char key[64];
	char value[1024];
} g_settings[FH_MAX_SETTINGS];
static int g_setting_count = 0;
static struct fh_config g_cfg;

static ot_mutex g_lock;
static bool g_lock_ready = false;
static struct fh_record g_ring[FH_RING];
static uint64_t g_count = 0;

void
fh_config_defaults(struct fh_config *c)
{
	memset(c, 0, sizeof(*c));
	c->panel_w_m = 0.302f;
	c->panel_h_m = 0.196f;
	c->nominal_z_m = 0.6f;
	c->iris_scale = 1.0f;
}

bool
fh_set(const char *kv)
{
	const char *eq = strchr(kv, '=');
	if (eq == NULL || eq == kv || g_setting_count >= FH_MAX_SETTINGS || (size_t)(eq - kv) >= sizeof(g_settings[0].key)) {
		return false;
	}
	// A later --set of the same key wins.
	int at = g_setting_count;
	for (int i = 0; i < g_setting_count; i++) {
		if (strncmp(g_settings[i].key, kv, (size_t)(eq - kv)) == 0 && g_settings[i].key[eq - kv] == '\0') {
			at = i;
		}
	}
	memcpy(g_settings[at].key, kv, (size_t)(eq - kv));
	g_settings[at].key[eq - kv] = '\0';
	ot_copy_str(g_settings[at].value, sizeof(g_settings[at].value), eq + 1);
	if (at == g_setting_count) {
		g_setting_count++;
	}
	return true;
}

static bool
host_get_setting(const char *id, const char *key, char *buf, uint32_t buf_size)
{
	if (id == NULL || key == NULL || buf == NULL || buf_size == 0) {
		return false;
	}
	buf[0] = '\0';
	// Both roles' ids; a --set applies to whichever role reads it.
	if (strcmp(id, "open-tracking") != 0 && strcmp(id, "open-tracking-hands") != 0) {
		return false;
	}
	if (g_cfg.env_prefix != NULL) {
		// The runtime's chain starts with DXR_TRACKING_<ID>_<KEY> in the hosting process.
		char name[192];
		snprintf(name, sizeof(name), "%s%s", g_cfg.env_prefix, key);
		const char *e = getenv(name);
		if (e != NULL) {
			ot_copy_str(buf, buf_size, e);
			return true;
		}
	}
	for (int i = 0; i < g_setting_count; i++) {
		if (strcmp(g_settings[i].key, key) == 0) {
			ot_copy_str(buf, buf_size, g_settings[i].value);
			return true;
		}
	}
	return false;
}

static xrt_result_t
host_get_display_geometry(struct xrt_tracking_host_display_geometry *inout)
{
	if (inout == NULL || inout->struct_size < sizeof(*inout)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	inout->display_width_m = g_cfg.panel_w_m;
	inout->display_height_m = g_cfg.panel_h_m;
	inout->nominal_viewer_x_m = 0.0f;
	inout->nominal_viewer_y_m = 0.0f;
	inout->nominal_viewer_z_m = g_cfg.nominal_z_m;
	return XRT_SUCCESS;
}

//! The runtime's default prior (spec §6): R = I, T = (0, H/2 + 8 mm, 0), never "not found".
static xrt_result_t
host_get_camera_pose(const void *host, const char *camera_key, const char *screen_key, struct xrt_camera_pose *inout)
{
	(void)host;
	(void)screen_key;
	if (camera_key == NULL || inout == NULL || inout->struct_size < sizeof(*inout)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	const uint32_t size = inout->struct_size;
	memset(inout, 0, sizeof(*inout));
	inout->struct_size = size;
	inout->source = XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR;
	inout->position_m[1] = 0.5f * g_cfg.panel_h_m + (float)XRT_CAMERA_POSE_BEZEL_MARGIN_M;
	inout->orientation[3] = 1.0f;
	inout->focal_scale = 1.0f;
	inout->viewer_iris_scale = g_cfg.iris_scale;
	inout->generation = 1;
	return XRT_SUCCESS;
}

static xrt_result_t
host_get_input_display_geometry(struct xrt_input_host_display_geometry *inout)
{
	if (inout == NULL || inout->struct_size < sizeof(*inout)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	inout->display_width_m = g_cfg.panel_w_m;
	inout->display_height_m = g_cfg.panel_h_m;
	inout->nominal_viewer_x_m = 0.0f;
	inout->nominal_viewer_y_m = 0.0f;
	inout->nominal_viewer_z_m = g_cfg.nominal_z_m;
	return XRT_SUCCESS;
}

static struct xrt_tracking_plugin_host_iface g_host;
static struct xrt_input_plugin_host_iface g_input_host;


/*
 *
 * The shared stereo camera (input host iface §4d): a rectified GRAY8 side-by-side stream built once from a PPM,
 * the right eye shifted by a fixed disparity (a fronto-parallel plane at f * B / d), delivered at 30 Hz.
 *
 */

struct xrt_input_host_camera
{
	uint8_t *sbs;
	uint32_t eye_w, h;
	uint64_t index;
	int64_t next_ns;
};

static uint32_t g_shared_opens = 0;

uint32_t
fh_shared_opens(void)
{
	return g_shared_opens;
}

//! A binary PPM (P6, 8 bit; # comments) -> grey. False on anything else.
static bool
read_ppm_grey(const char *path, uint8_t **out, uint32_t *w, uint32_t *h)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	char tok[4][32];
	bool ok = true;
	for (int i = 0; i < 4 && ok; i++) {
		int c = fgetc(f);
		while (c == '#' || isspace(c)) {
			if (c == '#') {
				while (c != '\n' && c != EOF) {
					c = fgetc(f);
				}
			}
			c = fgetc(f);
		}
		size_t n = 0;
		while (c != EOF && !isspace(c) && n + 1 < sizeof(tok[i])) {
			tok[i][n++] = (char)c;
			c = fgetc(f);
		}
		tok[i][n] = '\0';
		ok = n > 0;
	}
	ok = ok && strcmp(tok[0], "P6") == 0 && strcmp(tok[3], "255") == 0;
	const long iw = ok ? strtol(tok[1], NULL, 10) : 0, ih = ok ? strtol(tok[2], NULL, 10) : 0;
	ok = ok && iw > 0 && ih > 0 && iw < 16384 && ih < 16384;
	uint8_t *rgb = ok ? (uint8_t *)malloc((size_t)iw * ih * 3) : NULL;
	ok = ok && rgb != NULL && fread(rgb, 3, (size_t)iw * ih, f) == (size_t)iw * ih;
	fclose(f);
	if (!ok) {
		free(rgb);
		return false;
	}
	uint8_t *g = (uint8_t *)malloc((size_t)iw * ih);
	if (g == NULL) {
		free(rgb);
		return false;
	}
	for (long i = 0; i < iw * ih; i++) {
		g[i] = (uint8_t)((299 * rgb[i * 3] + 587 * rgb[i * 3 + 1] + 114 * rgb[i * 3 + 2] + 500) / 1000);
	}
	free(rgb);
	*out = g;
	*w = (uint32_t)iw;
	*h = (uint32_t)ih;
	return true;
}

static xrt_result_t
host_open_shared_camera(const char *provider_id,
                        struct xrt_input_host_camera_open *inout,
                        struct xrt_input_host_camera **out_cam)
{
	*out_cam = NULL;
	if (provider_id == NULL || strcmp(provider_id, "open-tracking-hands") != 0) {
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	if (inout == NULL || inout->struct_size < sizeof(*inout) ||
	    inout->output != XRT_STEREO_CAMERA_OUTPUT_RECTIFIED) {
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	if (g_cfg.shared == FH_SHARED_UNSUPPORTED) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	uint8_t *grey = NULL;
	uint32_t w = 320, h = 240;
	if (g_cfg.shared_image == NULL || !read_ppm_grey(g_cfg.shared_image, &grey, &w, &h)) {
		grey = (uint8_t *)calloc((size_t)w * h, 1); // a black pair
		if (grey == NULL) {
			return XRT_ERROR_ALLOCATION;
		}
	}
	struct xrt_input_host_camera *c = (struct xrt_input_host_camera *)calloc(1, sizeof(*c));
	uint8_t *sbs = (uint8_t *)malloc((size_t)w * 2 * h);
	if (c == NULL || sbs == NULL) {
		free(c);
		free(sbs);
		free(grey);
		return XRT_ERROR_ALLOCATION;
	}
	// Right eye: u_right = u_left - d, so right(x) = left(x + d) (linear interpolation, edge-clamped).
	const float d = g_cfg.shared_disparity_px;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = grey + (size_t)y * w;
		uint8_t *o = sbs + (size_t)y * w * 2;
		memcpy(o, row, w);
		for (uint32_t x = 0; x < w; x++) {
			float sx = (float)x + d;
			sx = sx < 0.0f ? 0.0f : sx > (float)(w - 1) ? (float)(w - 1) : sx;
			const uint32_t x0 = (uint32_t)sx, x1 = x0 + 1 < w ? x0 + 1 : x0;
			const float t = sx - (float)x0;
			o[w + x] = (uint8_t)(row[x0] + (row[x1] - row[x0]) * t + 0.5f);
		}
	}
	free(grey);
	c->sbs = sbs;
	c->eye_w = w;
	c->h = h;
	c->next_ns = ot_monotonic_ns();

	const float hfov = g_cfg.shared_hfov_deg > 0.0f ? g_cfg.shared_hfov_deg : 65.0f;
	const float f = 0.5f * (float)w / tanf(0.5f * hfov * 3.14159265f / 180.0f);
	const float b_mm = g_cfg.shared_baseline_mm > 0.0f ? g_cfg.shared_baseline_mm : 60.0f;
	const uint32_t size = inout->struct_size;
	const uint32_t output = inout->output;
	memset(inout, 0, sizeof(*inout));
	inout->struct_size = size;
	inout->output = output;
	inout->props.camera_id = 7;
	ot_copy_str(inout->props.persistent_id, sizeof(inout->props.persistent_id), "fake-stereo-camera");
	ot_copy_str(inout->props.display_name, sizeof(inout->props.display_name), "Fake stereo camera");
	inout->props.state = XRT_STEREO_CAMERA_STATE_AVAILABLE;
	inout->props.view_count = 2;
	inout->props.eye_width = w;
	inout->props.eye_height = h;
	inout->props.max_frame_rate = 30.0f;
	inout->props.baseline_mm = b_mm;
	inout->props.horizontal_fov_deg = hfov;
	inout->calib.output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
	for (int e = 0; e < 2; e++) {
		inout->calib.eye[e].width = w;
		inout->calib.eye[e].height = h;
		inout->calib.eye[e].fx = inout->calib.eye[e].fy = f;
		inout->calib.eye[e].cx = 0.5f * (float)w;
		inout->calib.eye[e].cy = 0.5f * (float)h;
	}
	inout->calib.orientation[3] = 1.0f;
	inout->calib.position[0] = b_mm * 0.001f;
	inout->calib.baseline_mm = b_mm;
	inout->layout.width = w * 2;
	inout->layout.height = h;
	inout->layout.format = XRT_STEREO_CAMERA_FORMAT_GRAY8;
	inout->layout.output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
	inout->layout.transport = XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY;
	inout->layout.max_frame_rate = 30.0f;
	*out_cam = c;
	ot_lock(&g_lock);
	g_shared_opens++;
	ot_unlock(&g_lock);
	return XRT_SUCCESS;
}

static xrt_result_t
host_camera_acquire(struct xrt_input_host_camera *c,
                    uint32_t timeout_ms,
                    struct xrt_input_host_camera_frame *out,
                    uint32_t *out_state)
{
	if (g_cfg.shared == FH_SHARED_WAITING) {
		if (out_state != NULL) {
			*out_state = XRT_STEREO_CAMERA_STATE_WAITING;
		}
		ot_sleep_ms(timeout_ms);
		return XRT_TIMEOUT;
	}
	if (out_state != NULL) {
		*out_state = XRT_STEREO_CAMERA_STATE_AVAILABLE;
	}
	const int64_t now = ot_monotonic_ns();
	if (c->next_ns > now) {
		const int64_t wait_ms = (c->next_ns - now) / 1000000 + 1;
		if (wait_ms > (int64_t)timeout_ms) {
			ot_sleep_ms(timeout_ms);
			return XRT_TIMEOUT;
		}
		ot_sleep_ms((uint32_t)wait_ms);
	}
	c->next_ns = ot_monotonic_ns() + 33333333;
	const uint32_t size = out->struct_size;
	memset(out, 0, sizeof(*out));
	out->struct_size = size;
	out->time_is_exposure = false;
	out->frame_index = ++c->index;
	out->timestamp_ns = ot_monotonic_ns();
	out->width = c->eye_w * 2;
	out->height = c->h;
	out->format = XRT_STEREO_CAMERA_FORMAT_GRAY8;
	out->calibration_generation = 1;
	out->plane[0] = c->sbs;
	out->row_pitch[0] = c->eye_w * 2;
	return XRT_SUCCESS;
}

static void
host_camera_release(struct xrt_input_host_camera *c, uint64_t frame_index)
{
	(void)c;
	(void)frame_index;
}

static void
host_close_shared_camera(struct xrt_input_host_camera *c)
{
	if (c != NULL) {
		free(c->sbs);
		free(c);
	}
}

static void
sink_push(const struct xrt_tracking_sink *sink, const struct xrt_tracking_viewer_sample *sample)
{
	(void)sink;
	const int64_t now = ot_monotonic_ns();
	ot_lock(&g_lock);
	struct fh_record *r = &g_ring[g_count % FH_RING];
	memset(r, 0, sizeof(*r));
	const size_t n = sample->struct_size < sizeof(r->s) ? sample->struct_size : sizeof(r->s);
	memcpy(&r->s, sample, n);
	r->push_ns = now;
	g_count++;
	ot_unlock(&g_lock);
}

static struct xrt_tracking_sink g_sink = {sizeof(struct xrt_tracking_sink), 0, sink_push, NULL, {NULL}};

const struct xrt_tracking_sink *
fh_sink(void)
{
	return &g_sink;
}

uint64_t
fh_count(void)
{
	ot_lock(&g_lock);
	const uint64_t c = g_count;
	ot_unlock(&g_lock);
	return c;
}

uint32_t
fh_records(uint64_t from, struct fh_record *out, uint32_t max)
{
	ot_lock(&g_lock);
	uint64_t lo = from;
	if (g_count > FH_RING && lo < g_count - FH_RING) {
		lo = g_count - FH_RING;
	}
	if (g_count > lo + max) {
		lo = g_count - max;
	}
	uint32_t n = 0;
	for (uint64_t i = lo; i < g_count; i++) {
		out[n++] = g_ring[i % FH_RING];
	}
	ot_unlock(&g_lock);
	return n;
}

int64_t
fh_now_ns(void)
{
	return ot_monotonic_ns();
}

void
fh_sleep_ms(uint32_t ms)
{
	ot_sleep_ms(ms);
}

static void *
load_lib(const char *path)
{
	char err[512];
	void *lib = ot_lib_open(path, err, sizeof(err));
	if (lib == NULL) {
		fprintf(stderr, "fake host: cannot load %s: %s\n", path, err);
	}
	return lib;
}

xrt_result_t
fh_load_input(const char *path,
              const struct fh_config *cfg,
              uint32_t api_version,
              struct xrt_input_plugin_iface **out_iface)
{
	if (!g_lock_ready) {
		ot_mutex_init(&g_lock);
		g_lock_ready = true;
	}
	g_cfg = *cfg;
	memset(&g_input_host, 0, sizeof(g_input_host));
	g_input_host.struct_size = (uint32_t)sizeof(g_input_host);
	g_input_host.host_api_version = api_version;
	g_input_host.get_display_geometry = host_get_input_display_geometry;
	g_input_host.get_setting = host_get_setting;
	g_input_host.get_camera_pose = cfg->no_pose_slot ? NULL : host_get_camera_pose;
	if (cfg->shared != FH_SHARED_NONE) {
		g_input_host.open_shared_camera = host_open_shared_camera;
		g_input_host.camera_acquire = host_camera_acquire;
		g_input_host.camera_release = host_camera_release;
		g_input_host.close_shared_camera = host_close_shared_camera;
	}

	void *lib = load_lib(path);
	if (lib == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	xrt_input_plugin_negotiate_fn_t negotiate =
	    (xrt_input_plugin_negotiate_fn_t)ot_lib_sym(lib, XRT_INPUT_PLUGIN_ENTRYPOINT_NAME);
	if (negotiate == NULL) {
		fprintf(stderr, "fake host: %s exports no %s\n", path, XRT_INPUT_PLUGIN_ENTRYPOINT_NAME);
		return XRT_ERROR_ALLOCATION;
	}
	uint32_t plugin_version = 0;
	*out_iface = NULL;
	return negotiate(api_version, &g_input_host, out_iface, &plugin_version);
}

xrt_result_t
fh_load(const char *path, const struct fh_config *cfg, uint32_t api_version, struct xrt_tracking_plugin_iface **out_iface)
{
	if (!g_lock_ready) {
		ot_mutex_init(&g_lock);
		g_lock_ready = true;
	}
	g_cfg = *cfg;
	memset(&g_host, 0, sizeof(g_host));
	g_host.struct_size = (uint32_t)sizeof(g_host);
	g_host.host_api_version = api_version;
	g_host.get_setting = host_get_setting;
	g_host.get_display_geometry = host_get_display_geometry;
	g_host.get_camera_pose = cfg->no_pose_slot ? NULL : host_get_camera_pose;

	char err[512];
	void *lib = ot_lib_open(path, err, sizeof(err));
	if (lib == NULL) {
		fprintf(stderr, "fake host: cannot load %s: %s\n", path, err);
		return XRT_ERROR_ALLOCATION;
	}
	xrt_tracking_plugin_negotiate_fn_t negotiate =
	    (xrt_tracking_plugin_negotiate_fn_t)ot_lib_sym(lib, XRT_TRACKING_PLUGIN_ENTRYPOINT_NAME);
	if (negotiate == NULL) {
		fprintf(stderr, "fake host: %s exports no %s\n", path, XRT_TRACKING_PLUGIN_ENTRYPOINT_NAME);
		return XRT_ERROR_ALLOCATION;
	}
	uint32_t plugin_version = 0;
	*out_iface = NULL;
	return negotiate(api_version, &g_host, out_iface, &plugin_version);
}
