// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The fake tracking host, see fake_host.h.
 */

#include "fake_host.h"
#include "ot_platform.h"

#include <ctype.h>
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
