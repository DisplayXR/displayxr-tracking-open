// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR-OpenTracking's hands role: the open input provider
 *         `open-tracking-hands`. Both hands' XR_EXT_hand_tracking joints from
 *         a plain webcam, through the MediaPipe palm detector + hand landmarks
 *         on ONNX Runtime — the same binary, and in one process the same
 *         camera stream, as the viewer role (`open-tracking`).
 *
 * Contract: the runtime's `xrt/xrt_input_plugin.h` (ABI 1) and
 * `docs/specs/runtime/input-provider-discovery.md`. Shape:
 *  - probe: cheap, opens nothing (settings, the pack's hand models by size,
 *    ONNX Runtime loads, a camera is enumerated, the OS camera permission is
 *    not denied);
 *  - create_devices: two hand-tracker devices (left, right) — or, with
 *    CONTROLLERS=1, two khr/simple_controller devices carrying the same joints
 *    (select = pinch, grip / aim = the palm). Cheap: the devices are per
 *    instance, the machinery below is per process;
 *  - on demand: the first joint locate (or controller poll) starts the
 *    provider's thread, which verifies the models, creates the sessions, opens
 *    the camera and publishes both hands per analysed frame; IDLE_S seconds
 *    after the last locate it closes the camera and ends;
 *  - get_presence: PRESENT while a usable camera exists (hands out of view are
 *    inactive joints, never ABSENT).
 * Poses are display-plane relative (`XRT_TRACKING_TYPE_RIG_LOCAL`, spec
 * §4b.4): the runtime anchors them to the rig, so they follow voluntary rig
 * motion and not head parallax (ADR-034 Amendment 2). Settings only through
 * `host->get_setting`; never the environment. Frames stay in this process.
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_tracking.h"

#include "ot_capture.h"
#include "ot_hand_tracker.h"
#include "ot_hands.h"
#include "ot_ort.h"
#include "ot_pack.h"
#include "ot_platform.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OTH_ID "open-tracking-hands"
#define OTH_VERSION "0.2.0"


/*
 *
 * Settings.
 *
 */

static const struct xrt_component_setting_desc g_descs[] = {
    {"ENABLE", XRT_COMPONENT_SETTING_TYPE_BOOL, "1",
     "0: probe declines. On by default: installing the provider is the opt-in; the camera opens only while an "
     "app locates hand joints"},
    {"DEVICE", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "camera: empty = the built-in camera (else the first), an index, or a substring of the name. Use the viewer "
     "role's value: one device, one stream for both roles"},
    {"WIDTH", XRT_COMPONENT_SETTING_TYPE_INT, "1280", "capture width (when this role opens the camera first)"},
    {"HEIGHT", XRT_COMPONENT_SETTING_TYPE_INT, "720", "capture height (when this role opens the camera first)"},
    {"FPS", XRT_COMPONENT_SETTING_TYPE_INT, "30", "capture rate (when this role opens the camera first)"},
    {"HFOV_DEG", XRT_COMPONENT_SETTING_TYPE_FLOAT, "70",
     "the camera's horizontal field of view in degrees (10..170); the hands' depth scales with it"},
    {"MIRROR", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "1: the camera delivers a mirrored image"},
    {"CONTROLLERS", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
     "1: also act as left / right khr/simple_controller (select = pinch, grip / aim = the palm), taking the "
     "controller roles from the keyboard / mouse ones while a camera is present. 0: hand joints only"},
    {"MAX_HANDS", XRT_COMPONENT_SETTING_TYPE_INT, "2", "1 or 2"},
    {"HAND_SCALE", XRT_COMPONENT_SETTING_TYPE_FLOAT, "1.0",
     "the user's hand size relative to the model's average: the monocular depth scales with it"},
    {"PINCH_ON_MM", XRT_COMPONENT_SETTING_TYPE_FLOAT, "25", "thumb tip to index tip below this = pinching"},
    {"PINCH_OFF_MM", XRT_COMPONENT_SETTING_TYPE_FLOAT, "35", "a pinch ends above this"},
    {"DETECT_MIN", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.5", "palm detector score threshold"},
    {"PRESENCE_MIN", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.5", "hand presence below this = the hand is lost"},
    {"DETECT_EVERY", XRT_COMPONENT_SETTING_TYPE_INT, "5",
     "while one hand is followed, look for the other every N frames (with none, every frame)"},
    {"PREDICT_MAX_MS", XRT_COMPONENT_SETTING_TYPE_INT, "50",
     "how far ahead of the newest frame a locate may extrapolate (the palm's velocity), ms"},
    {"IDLE_S", XRT_COMPONENT_SETTING_TYPE_INT, "5",
     "close the camera this long after the last joint locate; 0 = keep it open once started"},
    {"EP", XRT_COMPONENT_SETTING_TYPE_ENUM, "cpu", "cpu | coreml (macOS)"},
    {"COREML_UNITS", XRT_COMPONENT_SETTING_TYPE_ENUM, "ane", "ane | gpu | all | cpu (EP=coreml)"},
    {"COREML_CACHE", XRT_COMPONENT_SETTING_TYPE_BOOL, "1", "1: keep Core ML's compiled models in the user cache"},
    {"CACHE_DIR", XRT_COMPONENT_SETTING_TYPE_STRING, "", "cache root (default: the per-user cache directory)"},
    {"MODEL_DIR", XRT_COMPONENT_SETTING_TYPE_STRING, "", "model blobs + pack manifest (default: next to the binary)"},
    {"ONNXRUNTIME", XRT_COMPONENT_SETTING_TYPE_STRING, "", "ONNX Runtime library (default: next to the binary)"},
    {"THREADS", XRT_COMPONENT_SETTING_TYPE_INT, "2", "CPU intra-op threads per session (0 = ONNX Runtime's default)"},
    {"ORT_LOG", XRT_COMPONENT_SETTING_TYPE_ENUM, "warning", "warning | info | verbose"},
    {"FILTER_MIN_CUTOFF", XRT_COMPONENT_SETTING_TYPE_FLOAT, "1.0", "One Euro filter: cutoff at rest, Hz"},
    {"FILTER_BETA", XRT_COMPONENT_SETTING_TYPE_FLOAT, "3.0", "One Euro filter: cutoff increase per m/s"},
    {"FILTER_D_CUTOFF", XRT_COMPONENT_SETTING_TYPE_FLOAT, "1.0", "One Euro filter: speed cutoff, Hz"},
    {"PERMISSION_TIMEOUT_S", XRT_COMPONENT_SETTING_TYPE_INT, "120",
     "how long the thread waits for the user to answer the OS camera prompt"},
    {"STATS_S", XRT_COMPONENT_SETTING_TYPE_INT, "0", "> 0: log rate and per-stage timings every N seconds"},
};

static const struct xrt_input_plugin_host_iface *g_host = NULL;

static void
log_line(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[" OTH_ID "] %s\n", buf);
	fflush(stderr);
}

#define HOST_HAS(FIELD)                                                                                                \
	(g_host != NULL &&                                                                                             \
	 g_host->struct_size >= offsetof(struct xrt_input_plugin_host_iface, FIELD) + sizeof(g_host->FIELD) &&         \
	 g_host->FIELD != NULL)

static bool
setting(const char *key, char *buf, uint32_t cap)
{
	buf[0] = '\0';
	if (!HOST_HAS(get_setting)) {
		return false;
	}
	return g_host->get_setting(OTH_ID, key, buf, cap) && buf[0] != '\0';
}

static bool
setting_bool(const char *key, bool def)
{
	char v[16];
	if (!setting(key, v, sizeof(v))) {
		return def;
	}
	return !(strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0 || strcmp(v, "off") == 0);
}

static float
setting_float(const char *key, float def)
{
	char v[32];
	if (!setting(key, v, sizeof(v))) {
		return def;
	}
	char *end = NULL;
	const double d = strtod(v, &end);
	return end != v && isfinite(d) ? (float)d : def;
}

static long
setting_int(const char *key, long def, long lo, long hi)
{
	char v[32];
	if (!setting(key, v, sizeof(v))) {
		return def;
	}
	char *end = NULL;
	const long n = strtol(v, &end, 10);
	return (end != v && n >= lo && n <= hi) ? n : def;
}

static uint32_t
describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_descs;
	}
	return (uint32_t)(sizeof(g_descs) / sizeof(g_descs[0]));
}

struct oth_config
{
	char device[256];
	struct ot_capture_request req;
	float hfov_deg;
	bool mirror;
	bool controllers;
	char ep[16];
	enum ot_coreml_units units;
	bool coreml_cache;
	char cache_dir[2048];
	char model_dir[2048];
	char ort_path[2048];
	int threads;
	enum ot_ort_log ort_log;
	struct ot_hand_tracker_config tracker;
	struct ot_hands_params hands;
	int64_t predict_max_ns;
	uint32_t idle_s;
	uint32_t permission_timeout_ms;
	uint32_t stats_s;
};

static void
read_config(struct oth_config *c)
{
	memset(c, 0, sizeof(*c));
	setting("DEVICE", c->device, sizeof(c->device));
	c->req.width = (uint32_t)setting_int("WIDTH", 1280, 64, 7680);
	c->req.height = (uint32_t)setting_int("HEIGHT", 720, 48, 4320);
	c->req.fps = (uint32_t)setting_int("FPS", 30, 1, 240);
	c->hfov_deg = setting_float("HFOV_DEG", 70.0f);
	if (!(c->hfov_deg >= 10.0f && c->hfov_deg <= 170.0f)) {
		log_line("HFOV_DEG %.1f is outside 10..170: using 70", (double)c->hfov_deg);
		c->hfov_deg = 70.0f;
	}
	c->mirror = setting_bool("MIRROR", false);
	c->controllers = setting_bool("CONTROLLERS", false);
	if (!setting("EP", c->ep, sizeof(c->ep))) {
		ot_copy_str(c->ep, sizeof(c->ep), "cpu");
	}
	char v[32];
	c->units = OT_COREML_ANE;
	if (setting("COREML_UNITS", v, sizeof(v))) {
		c->units = strcmp(v, "gpu") == 0   ? OT_COREML_GPU
		           : strcmp(v, "all") == 0 ? OT_COREML_ALL
		           : strcmp(v, "cpu") == 0 ? OT_COREML_CPU
		                                   : OT_COREML_ANE;
	}
	c->coreml_cache = setting_bool("COREML_CACHE", true);
	if (!setting("CACHE_DIR", c->cache_dir, sizeof(c->cache_dir)) &&
	    !ot_user_cache_dir(c->cache_dir, sizeof(c->cache_dir))) {
		c->cache_dir[0] = '\0';
	}
	char here[2048] = "";
	(void)ot_module_dir(here, sizeof(here));
	if (!setting("MODEL_DIR", c->model_dir, sizeof(c->model_dir))) {
		ot_copy_str(c->model_dir, sizeof(c->model_dir), here);
	}
	if (!setting("ONNXRUNTIME", c->ort_path, sizeof(c->ort_path))) {
		snprintf(c->ort_path, sizeof(c->ort_path), "%s%c%s", here, OT_PATH_SEP, ot_ort_library_name());
	}
	c->threads = (int)setting_int("THREADS", 2, 0, 64);
	c->ort_log = OT_ORT_LOG_WARNING;
	if (setting("ORT_LOG", v, sizeof(v))) {
		c->ort_log = strcmp(v, "verbose") == 0 ? OT_ORT_LOG_VERBOSE
		             : strcmp(v, "info") == 0  ? OT_ORT_LOG_INFO
		                                       : OT_ORT_LOG_WARNING;
	}
	ot_hand_tracker_config_defaults(&c->tracker);
	c->tracker.detect_min_score = setting_float("DETECT_MIN", 0.5f);
	c->tracker.presence_min = setting_float("PRESENCE_MIN", 0.5f);
	c->tracker.detect_every = (uint32_t)setting_int("DETECT_EVERY", 5, 1, 100000);
	c->tracker.max_hands = (uint32_t)setting_int("MAX_HANDS", 2, 1, OT_HAND_MAX);
	ot_hands_params_defaults(&c->hands);
	c->hands.hand_scale = setting_float("HAND_SCALE", 1.0f);
	if (!(c->hands.hand_scale >= 0.5f && c->hands.hand_scale <= 2.0f)) {
		log_line("HAND_SCALE %.2f is outside 0.5..2: using 1", (double)c->hands.hand_scale);
		c->hands.hand_scale = 1.0f;
	}
	c->hands.pinch_on_m = setting_float("PINCH_ON_MM", 25.0f) / 1000.0f;
	c->hands.pinch_off_m = setting_float("PINCH_OFF_MM", 35.0f) / 1000.0f;
	if (c->hands.pinch_off_m < c->hands.pinch_on_m) {
		c->hands.pinch_off_m = c->hands.pinch_on_m;
	}
	c->hands.min_cutoff_hz = setting_float("FILTER_MIN_CUTOFF", 1.0f);
	c->hands.beta = setting_float("FILTER_BETA", 3.0f);
	c->hands.d_cutoff_hz = setting_float("FILTER_D_CUTOFF", 1.0f);
	c->predict_max_ns = (int64_t)setting_int("PREDICT_MAX_MS", 50, 0, 500) * 1000000LL;
	c->idle_s = (uint32_t)setting_int("IDLE_S", 5, 0, 86400);
	c->permission_timeout_ms = (uint32_t)setting_int("PERMISSION_TIMEOUT_S", 120, 1, 3600) * 1000u;
	c->stats_s = (uint32_t)setting_int("STATS_S", 0, 0, 3600);
}


/*
 *
 * The process-wide hub: one thread, one camera, the newest hands.
 *
 */

enum oth_state
{
	OTH_STATE_IDLE = 0,          //!< no app locates joints: no thread, no camera
	OTH_STATE_STARTING,          //!< verifying models, creating sessions, waiting for the permission prompt
	OTH_STATE_RUNNING,           //!< camera open, frames flowing
	OTH_STATE_NO_CAMERA,         //!< no camera (or it went away): retrying
	OTH_STATE_PERMISSION_DENIED, //!< the OS refused camera access
	OTH_STATE_MODELS_MISSING,    //!< a model blob is missing or does not match the pack
	OTH_STATE_FAILED,            //!< inference could not start on any execution provider
};

static const char *
state_name(enum oth_state s)
{
	switch (s) {
	case OTH_STATE_IDLE: return "idle";
	case OTH_STATE_STARTING: return "starting";
	case OTH_STATE_RUNNING: return "running";
	case OTH_STATE_NO_CAMERA: return "no camera";
	case OTH_STATE_PERMISSION_DENIED: return "camera permission denied";
	case OTH_STATE_MODELS_MISSING: return "models missing";
	case OTH_STATE_FAILED: return "inference failed";
	}
	return "?";
}

//! A worker that failed for good is not restarted by every locate: once per this long.
#define OTH_RETRY_NS (10LL * 1000000000LL)
//! Joints older than this (relative to the locate's clock) are reported inactive.
#define OTH_STALE_NS (300LL * 1000000LL)

struct oth_hub
{
	ot_mutex lock; //!< guards everything below
	struct oth_config cfg;
	bool probed;
	enum xrt_input_provider_presence presence;
	ot_thread thread;
	bool thread_running; //!< started and not yet joined
	bool thread_done;    //!< the worker returned; join before the next start
	volatile bool stop;
	int64_t last_demand_ns;
	int64_t retry_after_ns;
	enum oth_state state;
	char detail[256];
	char camera_name[OT_CAMERA_NAME_MAX];
	char camera_key[OT_CAMERA_KEY_MAX];
	char ep_used[64];
	struct ot_hand_state hands[2];
	uint64_t frames; //!< analysed frames since the process started (diagnostics)
};

static ot_mutex g_hub_init_lock = OT_MUTEX_INITIALIZER;
static bool g_hub_ready = false;
static struct oth_hub g_h;
static struct ot_ort *g_ort = NULL;
static uint32_t g_live_devices = 0; //!< hand devices handed out and not yet destroyed (under g_h.lock)

static void
hub_stop(void);

/*!
 * Process exit with the worker still running: stop it before the static destructors of Core ML and
 * ONNX Runtime tear down the sessions it is using (they were registered at load, before this, so
 * this runs first). The runtime keeps a loaded provider for the process lifetime and never calls
 * destroy, so without this the thread outlives the app's clean shutdown and crashes at exit.
 */
static void
hub_stop_at_exit(void)
{
	if (g_hub_ready) {
		hub_stop();
	}
}

static void
hub_init(void)
{
	ot_lock(&g_hub_init_lock);
	if (!g_hub_ready) {
		memset(&g_h, 0, sizeof(g_h));
		ot_mutex_init(&g_h.lock);
		g_h.presence = XRT_INPUT_PROVIDER_PRESENCE_UNKNOWN;
		g_hub_ready = true;
		(void)atexit(hub_stop_at_exit);
	}
	ot_unlock(&g_hub_init_lock);
}

static void
set_state(enum oth_state s, const char *fmt, ...)
{
	char d[256] = "";
	if (fmt != NULL) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(d, sizeof(d), fmt, ap);
		va_end(ap);
	}
	ot_lock(&g_h.lock);
	const bool changed = g_h.state != s || strcmp(g_h.detail, d) != 0;
	g_h.state = s;
	ot_copy_str(g_h.detail, sizeof(g_h.detail), d);
	// Presence is "is there a camera to track with", never "is a hand in view".
	if (s == OTH_STATE_RUNNING) {
		g_h.presence = XRT_INPUT_PROVIDER_PRESENCE_PRESENT;
	} else if (s == OTH_STATE_NO_CAMERA || s == OTH_STATE_PERMISSION_DENIED || s == OTH_STATE_MODELS_MISSING ||
	           s == OTH_STATE_FAILED) {
		g_h.presence = XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
	}
	ot_unlock(&g_h.lock);
	if (changed) {
		log_line("%s%s%s", state_name(s), d[0] != '\0' ? ": " : "", d);
	}
}

static void
publish(const struct ot_hand_state hands[2])
{
	ot_lock(&g_h.lock);
	g_h.hands[0] = hands[0];
	g_h.hands[1] = hands[1];
	g_h.frames++;
	ot_unlock(&g_h.lock);
}

static bool
ensure_ort(const struct oth_config *c, char *err, size_t err_size)
{
	if (g_ort != NULL) {
		return true;
	}
	char detail[256];
	if (ot_ort_load(c->ort_path, c->ort_log, &g_ort, detail, sizeof(detail)) != OT_ORT_OK) {
		snprintf(err, err_size, "%s", detail);
		return false;
	}
	log_line("%s", detail);
	return true;
}

static bool
load_pack(const struct oth_config *c, struct ot_pack *pack, bool hash, char *err, size_t err_size)
{
	char path[4096];
	snprintf(path, sizeof(path), "%s%c%s", c->model_dir, OT_PATH_SEP, OT_PACK_DEFAULT_FILE);
	return ot_pack_load(path, pack, err, err_size) &&
	       ot_pack_check_files(pack, OT_ROLES_HANDS, c->model_dir, hash, err, err_size);
}

//! The camera's pose from the runtime (spec §6); without the slot, the same top-centre prior it uses.
static void
read_pose(const char *camera_key, struct ot_cam_pose *pose, bool log_it)
{
	struct xrt_camera_pose a;
	memset(&a, 0, sizeof(a));
	a.struct_size = (uint32_t)sizeof(a);
	if (HOST_HAS(get_camera_pose) && g_host->get_camera_pose(g_host, camera_key, NULL, &a) == XRT_SUCCESS &&
	    a.source != XRT_CAMERA_POSE_SOURCE_NONE) {
		const bool changed = a.generation != pose->generation || pose->source == 0;
		memcpy(pose->t, a.position_m, sizeof(pose->t));
		memcpy(pose->q, a.orientation, sizeof(pose->q));
		pose->focal_scale = a.focal_scale > 0.0f ? a.focal_scale : 1.0f;
		pose->source = a.source;
		pose->generation = a.generation;
		if (changed && log_it) {
			log_line(
			    "camera pose from the runtime (%s): t (%.4f, %.4f, %.4f) m, q (%.4f, %.4f, %.4f, %.4f), "
			    "focal scale %.4f",
			    a.source == XRT_CAMERA_POSE_SOURCE_DISPLAY_DECLARED   ? "display-declared"
			    : a.source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION ? "user calibration"
			    : a.source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT   ? "placement hint"
			                                                          : "default prior",
			    (double)pose->t[0], (double)pose->t[1], (double)pose->t[2], (double)pose->q[0],
			    (double)pose->q[1], (double)pose->q[2], (double)pose->q[3], (double)pose->focal_scale);
		}
		return;
	}
	if (pose->source != 0 || pose->generation != 0) {
		return; // keep what we had
	}
	memset(pose, 0, sizeof(*pose));
	pose->q[3] = 1.0f;
	pose->focal_scale = 1.0f;
	struct xrt_input_host_display_geometry geo;
	memset(&geo, 0, sizeof(geo));
	geo.struct_size = (uint32_t)sizeof(geo);
	if (HOST_HAS(get_display_geometry) && g_host->get_display_geometry(&geo) == XRT_SUCCESS &&
	    geo.display_height_m > 0.0f) {
		pose->t[1] = 0.5f * geo.display_height_m + (float)XRT_CAMERA_POSE_BEZEL_MARGIN_M;
		if (log_it) {
			log_line("no camera pose from the runtime: assuming the top centre of the bezel (%.3f m up)",
			         (double)pose->t[1]);
		}
	} else if (log_it) {
		log_line(
		    "no camera pose and no display geometry from the runtime: hands reported relative to the camera");
	}
}

static bool
stop_or_idle(const struct oth_config *c)
{
	if (g_h.stop) {
		return true;
	}
	if (c->idle_s == 0) {
		return false;
	}
	ot_lock(&g_h.lock);
	const int64_t last = g_h.last_demand_ns;
	ot_unlock(&g_h.lock);
	return ot_monotonic_ns() - last > (int64_t)c->idle_s * 1000000000LL;
}

static bool
wait_or_stop(const struct oth_config *c, uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 50) {
		if (stop_or_idle(c)) {
			return true;
		}
		ot_sleep_ms(50);
	}
	return stop_or_idle(c);
}

static bool
create_tracker(const struct oth_config *c, const struct ot_pack *pack, bool force_cpu, struct ot_hand_tracker **out)
{
	char err[512];
	bool want_coreml = false;
#ifdef __APPLE__
	want_coreml = !force_cpu && strcmp(c->ep, "coreml") == 0 && ot_ort_has_coreml(g_ort);
#else
	(void)force_cpu;
#endif
	if (want_coreml) {
		char cache[2048] = "";
		if (c->coreml_cache && c->cache_dir[0] != '\0') {
			snprintf(cache, sizeof(cache), "%s%ccoreml-ort-%s", c->cache_dir, OT_PATH_SEP,
			         ot_ort_version(g_ort));
			if (!ot_make_dirs(cache)) {
				cache[0] = '\0';
			}
		}
		struct ot_ep_config epc = {c->units, cache[0] != '\0' ? cache : NULL};
		const int64_t t0 = ot_monotonic_ns();
		if (ot_hand_tracker_create(g_ort, pack, c->model_dir, OT_EP_COREML, &epc, c->threads, &c->tracker, out,
		                           err, sizeof(err))) {
			char rep[256];
			ot_hand_tracker_ep_report(*out, rep, sizeof(rep));
			log_line("Core ML sessions ready in %.0f ms%s%s", (double)(ot_monotonic_ns() - t0) * 1e-6,
			         rep[0] != '\0' ? "; " : "", rep);
			ot_lock(&g_h.lock);
			ot_copy_str(g_h.ep_used, sizeof(g_h.ep_used), "Core ML");
			ot_unlock(&g_h.lock);
			return true;
		}
		log_line("Core ML failed (%s): falling back to the CPU", err);
	}
	struct ot_ep_config epc = {OT_COREML_ALL, NULL};
	if (!ot_hand_tracker_create(g_ort, pack, c->model_dir, OT_EP_CPU, &epc, c->threads, &c->tracker, out, err,
	                            sizeof(err))) {
		log_line("CPU sessions failed: %s", err);
		return false;
	}
	ot_lock(&g_h.lock);
	ot_copy_str(g_h.ep_used, sizeof(g_h.ep_used), "CPU");
	ot_unlock(&g_h.lock);
	return true;
}

struct oth_stats
{
	int64_t since_ns;
	uint32_t frames, hands, detects;
	double det_ms, lm_ms, proc_ms, lat_ms;
};

static void
stats_flush(struct oth_stats *st)
{
	const double secs = (double)(ot_monotonic_ns() - st->since_ns) * 1e-9;
	if (st->frames > 0 && secs > 0) {
		char ep[64];
		ot_lock(&g_h.lock);
		ot_copy_str(ep, sizeof(ep), g_h.ep_used);
		ot_unlock(&g_h.lock);
		log_line(
		    "stats %s: %.1f fps analysed, %.2f hands/frame, palm detector %u x %.2f ms, landmarks %.2f "
		    "ms/frame "
		    "(all hands, crops included), frame %.2f ms, capture->publish %.1f ms",
		    ep, st->frames / secs, (double)st->hands / st->frames, st->detects,
		    st->detects > 0 ? st->det_ms / st->detects : 0.0, st->lm_ms / st->frames, st->proc_ms / st->frames,
		    st->lat_ms / st->frames);
	}
	memset(st, 0, sizeof(*st));
	st->since_ns = ot_monotonic_ns();
}

static void
worker(void *arg)
{
	(void)arg;
	ot_thread_set_latency_critical();
	struct oth_config c;
	ot_lock(&g_h.lock);
	c = g_h.cfg;
	ot_unlock(&g_h.lock);
	struct ot_hand_tracker *tracker = NULL;
	struct ot_capture *cap = NULL;
	struct ot_frame_buffer fb;
	memset(&fb, 0, sizeof(fb));
	char err[512];
	bool failed = true;

	set_state(OTH_STATE_STARTING, "loading models");
	struct ot_pack pack;
	if (!load_pack(&c, &pack, true, err, sizeof(err))) {
		set_state(OTH_STATE_MODELS_MISSING, "%s", err);
		goto done;
	}
	if (!ensure_ort(&c, err, sizeof(err))) {
		set_state(OTH_STATE_FAILED, "%s", err);
		goto done;
	}
	if (!create_tracker(&c, &pack, strcmp(c.ep, "cpu") == 0, &tracker)) {
		set_state(OTH_STATE_FAILED, "no execution provider could run the models");
		goto done;
	}
	if (strncmp(c.device, "file:", 5) != 0) {
		enum ot_camera_auth auth = ot_capture_authorization();
		if (auth == OT_CAMERA_NOT_DETERMINED) {
			set_state(OTH_STATE_STARTING, "waiting for the camera permission prompt");
			auth = ot_capture_request_access(c.permission_timeout_ms, &g_h.stop);
		}
		if (auth != OT_CAMERA_AUTHORIZED) {
			set_state(
			    OTH_STATE_PERMISSION_DENIED,
			    auth == OT_CAMERA_RESTRICTED
			        ? "a policy restricts camera access"
			        : "allow it in System Settings > Privacy & Security > Camera, then restart the app");
			goto done;
		}
	}
	failed = false;

	struct ot_camera_info cam;
	memset(&cam, 0, sizeof(cam));
	struct ot_hands hands;
	ot_hands_reset(&hands);
	struct ot_cam_pose pose;
	memset(&pose, 0, sizeof(pose));
	struct ot_intrinsics k;
	memset(&k, 0, sizeof(k));
	int64_t last_pose_read = 0;
	bool first_hand = true;
	uint32_t logged_track[2] = {0, 0};
	struct oth_stats stats;
	memset(&stats, 0, sizeof(stats));
	stats.since_ns = ot_monotonic_ns();

	while (!stop_or_idle(&c)) {
		if (cap == NULL) {
			if (!ot_capture_resolve(c.device, &cam)) {
				set_state(OTH_STATE_NO_CAMERA, "no camera matches DEVICE='%s'", c.device);
				if (wait_or_stop(&c, 2000)) {
					break;
				}
				continue;
			}
			if (!ot_capture_open(&cam, &c.req, &cap, err, sizeof(err))) {
				set_state(OTH_STATE_NO_CAMERA, "%s", err);
				if (wait_or_stop(&c, 2000)) {
					break;
				}
				continue;
			}
			uint32_t w, h;
			float fps;
			ot_capture_mode(cap, &w, &h, &fps);
			(void)ot_intrinsics_from_hfov(w, h, c.hfov_deg, &k);
			ot_lock(&g_h.lock);
			ot_copy_str(g_h.camera_name, sizeof(g_h.camera_name), cam.name);
			ot_copy_str(g_h.camera_key, sizeof(g_h.camera_key), cam.key);
			ot_unlock(&g_h.lock);
			read_pose(cam.key, &pose, true);
			last_pose_read = ot_monotonic_ns();
			ot_hand_tracker_reset(tracker);
			ot_hands_reset(&hands);
			set_state(OTH_STATE_RUNNING, "%s, %ux%u @ %.0f fps, HFOV %.0f deg (f = %.0f px)", cam.name, w,
			          h, (double)fps, (double)c.hfov_deg, (double)k.f);
		}
		if (!ot_capture_take(cap, &fb, 500)) {
			if (ot_capture_lost(cap)) {
				ot_capture_close(cap);
				cap = NULL;
				set_state(OTH_STATE_NO_CAMERA, "the camera went away");
			}
			continue;
		}
		fb.frame.image.mirrored = c.mirror;
		if (fb.frame.image.width != k.width || fb.frame.image.height != k.height) {
			(void)ot_intrinsics_from_hfov(fb.frame.image.width, fb.frame.image.height, c.hfov_deg, &k);
		}
		struct ot_hand_result res;
		const int64_t proc0 = ot_monotonic_ns();
		if (!ot_hand_tracker_process(tracker, &fb.frame.image, &res, err, sizeof(err))) {
			log_line("inference failed (%s): recreating the sessions on the CPU", err);
			ot_hand_tracker_destroy(tracker);
			tracker = NULL;
			if (!create_tracker(&c, &pack, true, &tracker)) {
				set_state(OTH_STATE_FAILED, "no execution provider could run the models");
				failed = true;
				break;
			}
			continue;
		}
		const int64_t now = ot_monotonic_ns();
		if (now - last_pose_read > 2000000000LL) {
			read_pose(cam.key, &pose, true); // follows a new calibration without a restart
			last_pose_read = now;
		}
		struct ot_hand_state out[2];
		ot_hands_update(&hands, &k, &c.hands, &pose, &res, fb.frame.capture_ns, out);
		publish(out);
		const int64_t lat = ot_monotonic_ns() - fb.frame.capture_ns;
		if (c.stats_s > 0) {
			// Diagnostics, once per track: the model's handedness against the 3D shape's
			// (ot_hand_chirality).
			for (int s = 0; s < 2; s++) {
				if (out[s].active && out[s].track_id != logged_track[s]) {
					logged_track[s] = out[s].track_id;
					const int ch = ot_hand_chirality((const float(*)[3])out[s].points);
					log_line(
					    "hand track %u -> %s: model handedness P(right) %.2f, 3D shape %s, %.0f mm "
					    "from the "
					    "camera, size %.0f mm",
					    out[s].track_id, s == OT_SIDE_LEFT ? "left" : "right",
					    (double)out[s].right_mean,
					    ch > 0   ? "right"
					    : ch < 0 ? "left"
					             : "undecided",
					    (double)out[s].depth_m * 1000.0,
					    (double)ot_hand_size_m((const float(*)[3])out[s].points) * 1000.0);
				}
			}
		}
		if (first_hand && (out[0].active || out[1].active)) {
			first_hand = false;
			const struct ot_hand_state *s = out[0].active ? &out[0] : &out[1];
			log_line(
			    "first hand (%s): palm (%.3f, %.3f, %.3f) m, %.0f mm from the camera, %.0f ms capture -> "
			    "publish",
			    out[0].active ? "left" : "right", (double)s->joints.pos[OT_HJ_PALM][0],
			    (double)s->joints.pos[OT_HJ_PALM][1], (double)s->joints.pos[OT_HJ_PALM][2],
			    (double)s->depth_m * 1000.0, (double)lat * 1e-6);
		}
		stats.frames++;
		stats.hands += res.count;
		stats.detects += res.detected ? 1u : 0u;
		stats.det_ms += res.ms_detect;
		stats.lm_ms += res.ms_landmarks;
		stats.proc_ms += (double)(ot_monotonic_ns() - proc0) * 1e-6;
		stats.lat_ms += (double)lat * 1e-6;
		if (c.stats_s > 0 && ot_monotonic_ns() - stats.since_ns >= (int64_t)c.stats_s * 1000000000LL) {
			stats_flush(&stats);
		}
	}
	if (!failed) {
		struct ot_hand_state none[2];
		memset(none, 0, sizeof(none));
		publish(none);
		set_state(OTH_STATE_IDLE, g_h.stop ? NULL : "no joint locate for %u s: camera closed", c.idle_s);
	}

done:
	ot_capture_close(cap);
	ot_frame_buffer_free(&fb);
	ot_hand_tracker_destroy(tracker);
	ot_lock(&g_h.lock);
	g_h.thread_done = true;
	if (failed) {
		g_h.retry_after_ns = ot_monotonic_ns() + OTH_RETRY_NS;
	}
	ot_unlock(&g_h.lock);
}

//! Someone wants hands now: note it, and start the thread if it is not running. Non-blocking.
static void
demand(void)
{
	const int64_t now = ot_monotonic_ns();
	bool join = false;
	ot_thread old;
	memset(&old, 0, sizeof(old));
	ot_lock(&g_h.lock);
	g_h.last_demand_ns = now;
	if (!g_h.probed || (g_h.thread_running && !g_h.thread_done) || now < g_h.retry_after_ns) {
		ot_unlock(&g_h.lock);
		return;
	}
	if (g_h.thread_running && g_h.thread_done) {
		old = g_h.thread;
		join = true;
	}
	g_h.thread_running = true; // claimed: a concurrent locate returns above
	g_h.thread_done = false;
	g_h.stop = false;
	ot_unlock(&g_h.lock);
	if (join) {
		ot_thread_join(old); // already returned: immediate
	}
	ot_thread t;
	memset(&t, 0, sizeof(t));
	const bool ok = ot_thread_start(&t, worker, NULL);
	ot_lock(&g_h.lock);
	if (ok) {
		g_h.thread = t;
	} else {
		g_h.thread_running = false;
		g_h.retry_after_ns = now + OTH_RETRY_NS;
	}
	ot_unlock(&g_h.lock);
	if (!ok) {
		log_line("cannot start the hands thread");
	}
}

static void
hub_stop(void)
{
	ot_lock(&g_h.lock);
	const bool running = g_h.thread_running;
	const ot_thread t = g_h.thread;
	g_h.stop = true;
	ot_unlock(&g_h.lock);
	if (running) {
		ot_thread_join(t);
	}
	ot_lock(&g_h.lock);
	g_h.thread_running = false;
	g_h.thread_done = false;
	ot_unlock(&g_h.lock);
}


/*
 *
 * The devices.
 *
 */

enum
{
	// Input slots of a CONTROLLERS=1 device (khr/simple_controller + the hand).
	IN_SELECT = 0,
	IN_MENU,
	IN_GRIP,
	IN_AIM,
	IN_HT,
	IN_PALM,
	IN_COUNT,
};

struct oth_device
{
	struct xrt_device base;
	struct xrt_tracking_origin origin;
	struct xrt_input inputs[IN_COUNT];
	struct xrt_output outputs[1];
	int side; //!< OT_SIDE_*
	bool controllers;
	enum xrt_input_name ht_name;
};

static struct oth_device *
oth_device(struct xrt_device *xdev)
{
	return (struct oth_device *)xdev;
}

//! A copy of one side's newest hand, inactive when stale relative to @p at_ns.
static void
snapshot(int side, int64_t at_ns, struct ot_hand_state *out)
{
	ot_lock(&g_h.lock);
	*out = g_h.hands[side];
	ot_unlock(&g_h.lock);
	if (out->active && at_ns - out->capture_ns > OTH_STALE_NS + g_h.cfg.predict_max_ns) {
		out->active = false;
	}
}

//! Seconds to extrapolate from the frame to @p at_ns, clamped to [0, PREDICT_MAX_MS].
static float
predict_dt(const struct ot_hand_state *s, int64_t at_ns)
{
	int64_t d = at_ns - s->capture_ns;
	d = d < 0 ? 0 : d;
	d = d > g_h.cfg.predict_max_ns ? g_h.cfg.predict_max_ns : d;
	return (float)((double)d * 1e-9);
}

static void
fill_relation(const float pos[3], const float rot[4], const float vel[3], float dt, struct xrt_space_relation *r)
{
	r->pose.position.x = pos[0] + vel[0] * dt;
	r->pose.position.y = pos[1] + vel[1] * dt;
	r->pose.position.z = pos[2] + vel[2] * dt;
	r->pose.orientation.x = rot[0];
	r->pose.orientation.y = rot[1];
	r->pose.orientation.z = rot[2];
	r->pose.orientation.w = rot[3];
	r->linear_velocity.x = vel[0];
	r->linear_velocity.y = vel[1];
	r->linear_velocity.z = vel[2];
	r->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	    XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
}

static xrt_result_t
dev_get_hand_tracking(struct xrt_device *xdev,
                      enum xrt_input_name name,
                      int64_t at_timestamp_ns,
                      struct xrt_hand_joint_set *out_value,
                      int64_t *out_timestamp_ns)
{
	struct oth_device *d = oth_device(xdev);
	if (name != d->ht_name) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	demand();
	memset(out_value, 0, sizeof(*out_value));
	*out_timestamp_ns = at_timestamp_ns;
	struct ot_hand_state s;
	snapshot(d->side, at_timestamp_ns, &s);
	if (!s.active) {
		out_value->is_active = false;
		return XRT_SUCCESS;
	}
	// Joints are absolute in the device's (RIG_LOCAL) space; the hand pose is the identity. The whole hand
	// moves rigidly with the palm's velocity to the requested time.
	out_value->hand_pose.pose.orientation.w = 1.0f;
	out_value->hand_pose.relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	const float dt = predict_dt(&s, at_timestamp_ns);
	for (int j = 0; j < OT_HJ_COUNT; j++) {
		struct xrt_hand_joint_value *v = &out_value->values.hand_joint_set_default[j];
		fill_relation(s.joints.pos[j], s.joints.rot[j], s.palm_velocity, dt, &v->relation);
		v->radius = s.joints.radius[j];
	}
	out_value->is_active = true;
	return XRT_SUCCESS;
}

static xrt_result_t
dev_get_tracked_pose(struct xrt_device *xdev,
                     enum xrt_input_name name,
                     int64_t at_timestamp_ns,
                     struct xrt_space_relation *out_relation)
{
	struct oth_device *d = oth_device(xdev);
	memset(out_relation, 0, sizeof(*out_relation));
	if (name != XRT_INPUT_SIMPLE_GRIP_POSE && name != XRT_INPUT_SIMPLE_AIM_POSE &&
	    name != XRT_INPUT_GENERIC_PALM_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (d->controllers) {
		demand();
	}
	struct ot_hand_state s;
	snapshot(d->side, at_timestamp_ns, &s);
	if (!s.active) {
		return XRT_SUCCESS; // no flags: not tracked
	}
	// Grip, aim and palm: the palm joint (-Z along the hand toward the fingers, +Y out of its back).
	fill_relation(s.joints.pos[OT_HJ_PALM], s.joints.rot[OT_HJ_PALM], s.palm_velocity,
	              predict_dt(&s, at_timestamp_ns), out_relation);
	return XRT_SUCCESS;
}

static xrt_result_t
dev_update_inputs(struct xrt_device *xdev)
{
	struct oth_device *d = oth_device(xdev);
	if (!d->controllers) {
		return XRT_SUCCESS;
	}
	demand();
	const int64_t now = ot_monotonic_ns();
	struct ot_hand_state s;
	snapshot(d->side, now, &s);
	for (int i = 0; i < IN_COUNT; i++) {
		d->inputs[i].timestamp = now;
	}
	d->inputs[IN_SELECT].value.boolean = s.active && s.pinching;
	d->inputs[IN_MENU].value.boolean = false;
	return XRT_SUCCESS;
}

static xrt_result_t
dev_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	// Hands have no haptics: accept and drop, so apps can bind the haptic action unconditionally.
	(void)xdev;
	(void)name;
	(void)value;
	return XRT_SUCCESS;
}

// Every other slot: not implemented (the runtime may call any slot it finds non-NULL).
static xrt_result_t
ni_face(struct xrt_device *x, enum xrt_input_name n, int64_t t, struct xrt_facial_expression_set *o)
{
	(void)x;
	(void)n;
	(void)t;
	(void)o;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_body_skeleton(struct xrt_device *x, enum xrt_input_name n, struct xrt_body_skeleton *o)
{
	(void)x;
	(void)n;
	(void)o;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_body_joints(struct xrt_device *x, enum xrt_input_name n, int64_t t, struct xrt_body_joint_set *o)
{
	(void)x;
	(void)n;
	(void)t;
	(void)o;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_reset_body(struct xrt_device *x)
{
	(void)x;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_body_height(struct xrt_device *x, float h)
{
	(void)x;
	(void)h;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_output_limits(struct xrt_device *x, struct xrt_output_limits *l)
{
	(void)x;
	(void)l;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_presence(struct xrt_device *x, bool *p)
{
	(void)x;
	(void)p;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static enum xrt_result
ni_begin_planes(struct xrt_device *x, const struct xrt_plane_detector_begin_info_ext *b, uint64_t id, uint64_t *o)
{
	(void)x;
	(void)b;
	(void)id;
	(void)o;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static enum xrt_result
ni_destroy_planes(struct xrt_device *x, uint64_t id)
{
	(void)x;
	(void)id;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static enum xrt_result
ni_plane_state(struct xrt_device *x, uint64_t id, enum xrt_plane_detector_state_ext *s)
{
	(void)x;
	(void)id;
	(void)s;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static enum xrt_result
ni_planes(struct xrt_device *x, uint64_t id, struct xrt_plane_detections_ext *d)
{
	(void)x;
	(void)id;
	(void)d;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_view_poses(struct xrt_device *x,
              const struct xrt_vec3 *e,
              int64_t t,
              uint32_t n,
              struct xrt_space_relation *h,
              struct xrt_fov *f,
              struct xrt_pose *p)
{
	(void)x;
	(void)e;
	(void)t;
	(void)n;
	(void)h;
	(void)f;
	(void)p;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_distortion(struct xrt_device *x, uint32_t v, float a, float b, struct xrt_uv_triplet *o)
{
	(void)x;
	(void)v;
	(void)a;
	(void)b;
	(void)o;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_visibility_mask(struct xrt_device *x, enum xrt_visibility_mask_type t, uint32_t v, struct xrt_visibility_mask **m)
{
	(void)x;
	(void)t;
	(void)v;
	(void)m;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_ref_space(struct xrt_device *x, enum xrt_reference_space_type t, enum xrt_input_name n, bool u)
{
	(void)x;
	(void)t;
	(void)n;
	(void)u;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static bool
ni_form_factor(struct xrt_device *x, enum xrt_form_factor f)
{
	(void)x;
	(void)f;
	return false;
}
static xrt_result_t
ni_battery(struct xrt_device *x, bool *p, bool *c, float *v)
{
	(void)x;
	(void)p;
	(void)c;
	(void)v;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_get_brightness(struct xrt_device *x, float *b)
{
	(void)x;
	(void)b;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_set_brightness(struct xrt_device *x, float b, bool r)
{
	(void)x;
	(void)b;
	(void)r;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_feature(struct xrt_device *x, enum xrt_device_feature_type t)
{
	(void)x;
	(void)t;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_set_property(struct xrt_device *x, enum xrt_device_property_id p, int32_t v)
{
	(void)x;
	(void)p;
	(void)v;
	return XRT_ERROR_NOT_IMPLEMENTED;
}
static xrt_result_t
ni_get_property(struct xrt_device *x, enum xrt_device_property_id p, int32_t *v)
{
	(void)x;
	(void)p;
	(void)v;
	return XRT_ERROR_NOT_IMPLEMENTED;
}

static void
dev_destroy(struct xrt_device *xdev)
{
	free(xdev);
	// The last device gone (the app's instance torn down): nobody can locate joints any more, so stop
	// the worker and close the camera now instead of at IDLE_S or process exit.
	ot_lock(&g_h.lock);
	const bool last = g_live_devices > 0 && --g_live_devices == 0;
	ot_unlock(&g_h.lock);
	if (last) {
		hub_stop();
	}
}

static struct xrt_device *
create_device(int side, bool controllers)
{
	struct oth_device *d = (struct oth_device *)calloc(1, sizeof(*d));
	if (d == NULL) {
		return NULL;
	}
	const bool left = side == OT_SIDE_LEFT;
	struct xrt_device *b = &d->base;
	d->side = side;
	d->controllers = controllers;
	d->ht_name = left ? XRT_INPUT_HT_UNOBSTRUCTED_LEFT : XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;

	// Display-plane relative (spec §4b.4): no stage height, no mount offset; the runtime anchors the origin
	// at the initial rig. The camera's pose in display space is applied before publishing.
	d->origin.type = XRT_TRACKING_TYPE_RIG_LOCAL;
	d->origin.initial_offset.orientation.w = 1.0f;
	snprintf(d->origin.name, sizeof(d->origin.name), "%s", "Open Tracking Hands (display plane)");
	b->tracking_origin = &d->origin;

	if (controllers) {
		static const enum xrt_input_name k_in[IN_COUNT] = {
		    XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_SIMPLE_MENU_CLICK,    XRT_INPUT_SIMPLE_GRIP_POSE,
		    XRT_INPUT_SIMPLE_AIM_POSE,     XRT_INPUT_HT_UNOBSTRUCTED_LEFT, XRT_INPUT_GENERIC_PALM_POSE};
		for (int i = 0; i < IN_COUNT; i++) {
			d->inputs[i].active = true;
			d->inputs[i].name = k_in[i];
		}
		d->inputs[IN_HT].name = d->ht_name;
		b->input_count = IN_COUNT;
		d->outputs[0].name = XRT_OUTPUT_NAME_SIMPLE_VIBRATION;
		b->output_count = 1;
		b->outputs = d->outputs;
		b->name = XRT_DEVICE_SIMPLE_CONTROLLER;
		b->device_type = left ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
		b->supported.orientation_tracking = true;
		b->supported.position_tracking = true;
	} else {
		d->inputs[0].active = true;
		d->inputs[0].name = d->ht_name;
		b->input_count = 1;
		b->name = XRT_DEVICE_HAND_TRACKER;
		b->device_type = XRT_DEVICE_TYPE_HAND_TRACKER;
	}
	b->inputs = d->inputs;
	b->supported.hand_tracking = true;
	snprintf(b->str, sizeof(b->str), "Open Tracking %s Hand%s", left ? "Left" : "Right",
	         controllers ? " (controller)" : "");
	snprintf(b->serial, sizeof(b->serial), "OPEN-TRACKING-HAND-%s", left ? "L" : "R");

	b->update_inputs = dev_update_inputs;
	b->get_tracked_pose = dev_get_tracked_pose;
	b->get_hand_tracking = dev_get_hand_tracking;
	b->get_face_tracking = ni_face;
	b->get_body_skeleton = ni_body_skeleton;
	b->get_body_joints = ni_body_joints;
	b->reset_body_tracking_calibration = ni_reset_body;
	b->set_body_tracking_calibration_override = ni_body_height;
	b->set_output = dev_set_output;
	b->get_output_limits = ni_output_limits;
	b->get_presence = ni_presence;
	b->begin_plane_detection_ext = ni_begin_planes;
	b->destroy_plane_detection_ext = ni_destroy_planes;
	b->get_plane_detection_state_ext = ni_plane_state;
	b->get_plane_detections_ext = ni_planes;
	b->get_view_poses = ni_view_poses;
	b->compute_distortion = ni_distortion;
	b->get_visibility_mask = ni_visibility_mask;
	b->ref_space_usage = ni_ref_space;
	b->is_form_factor_available = ni_form_factor;
	b->get_battery_status = ni_battery;
	b->get_brightness = ni_get_brightness;
	b->set_brightness = ni_set_brightness;
	b->begin_feature = ni_feature;
	b->end_feature = ni_feature;
	b->set_property = ni_set_property;
	b->get_property = ni_get_property;
	b->destroy = dev_destroy;
	return b;
}


/*
 *
 * The vtable.
 *
 */

static xrt_result_t
oth_probe(struct xrt_input_plugin_instance **out_inst)
{
	hub_init();
	*out_inst = (struct xrt_input_plugin_instance *)&g_h;
	if (!setting_bool("ENABLE", true)) {
		log_line("declined: ENABLE=0");
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	struct oth_config c;
	read_config(&c);
	char err[512];
	struct ot_pack pack;
	if (!load_pack(&c, &pack, false, err, sizeof(err))) {
		log_line("declined: models missing: %s", err);
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	if (!ensure_ort(&c, err, sizeof(err))) {
		log_line("declined: %s", err);
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	struct ot_camera_info cam;
	if (!ot_capture_resolve(c.device, &cam)) {
#if defined(__APPLE__)
		log_line("declined: no camera%s%s%s", c.device[0] != '\0' ? " matches DEVICE='" : "", c.device,
		         c.device[0] != '\0' ? "'" : "");
#else
		log_line("declined: no camera backend on this OS yet (only DEVICE=file:<ppm> for tests)");
#endif
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	if (strncmp(cam.key, "file:", 5) != 0) {
		const enum ot_camera_auth a = ot_capture_authorization();
		if (a == OT_CAMERA_DENIED || a == OT_CAMERA_RESTRICTED) {
			log_line(
			    "declined: camera permission %s for this process (System Settings > Privacy & Security > "
			    "Camera)",
			    a == OT_CAMERA_DENIED ? "denied" : "restricted by policy");
			return XRT_ERROR_PROBER_NOT_SUPPORTED;
		}
	}
	ot_lock(&g_h.lock);
	g_h.cfg = c;
	g_h.probed = true;
	g_h.presence = XRT_INPUT_PROVIDER_PRESENCE_PRESENT;
	ot_copy_str(g_h.camera_name, sizeof(g_h.camera_name), cam.name);
	ot_copy_str(g_h.camera_key, sizeof(g_h.camera_key), cam.key);
	ot_unlock(&g_h.lock);
	log_line("probe: usable — camera '%s' (%s); %s", cam.name, cam.key,
	         c.controllers ? "hands + controllers" : "hand joints only");
	return XRT_SUCCESS;
}

static xrt_result_t
oth_create_devices(struct xrt_input_plugin_instance *inst,
                   struct xrt_device **out_devices,
                   uint32_t max_count,
                   uint32_t *out_count)
{
	(void)inst;
	*out_count = 0;
	if (max_count < 2) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	ot_lock(&g_h.lock);
	const bool controllers = g_h.cfg.controllers;
	ot_unlock(&g_h.lock);
	struct xrt_device *l = create_device(OT_SIDE_LEFT, controllers);
	struct xrt_device *r = create_device(OT_SIDE_RIGHT, controllers);
	if (l == NULL || r == NULL) {
		free(l);
		free(r);
		return XRT_ERROR_ALLOCATION;
	}
	out_devices[0] = l;
	out_devices[1] = r;
	*out_count = 2;
	ot_lock(&g_h.lock);
	g_live_devices += 2;
	ot_unlock(&g_h.lock);
	return XRT_SUCCESS;
}

static enum xrt_input_provider_presence
oth_get_presence(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	if (!g_hub_ready) {
		return XRT_INPUT_PROVIDER_PRESENCE_UNKNOWN;
	}
	ot_lock(&g_h.lock);
	const enum xrt_input_provider_presence p = g_h.presence;
	ot_unlock(&g_h.lock);
	return p;
}

static void
oth_destroy(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	if (g_hub_ready) {
		hub_stop();
	}
}

static struct xrt_input_plugin_iface g_iface = {
    .struct_size = sizeof(struct xrt_input_plugin_iface),
    .reserved_0 = 0,
    .id = OTH_ID,
    .display_name = "DisplayXR Open Tracking: hands",
    .vendor = "DisplayXR",
    .version = OTH_VERSION,
    .probe = oth_probe,
    .create_devices = oth_create_devices,
    .destroy = oth_destroy,
    .get_presence = oth_get_presence,
    .describe_settings = describe_settings,
};

XRT_INPUT_PLUGIN_EXPORT xrt_result_t
xrtInputPluginNegotiate(uint32_t runtime_api_version,
                        const struct xrt_input_plugin_host_iface *host,
                        struct xrt_input_plugin_iface **out_iface,
                        uint32_t *out_plugin_api_version)
{
	*out_plugin_api_version = XRT_INPUT_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_INPUT_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	g_host = host;
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}
