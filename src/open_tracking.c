// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR-OpenTracking: the open viewer-tracking provider (id
 *         `open-tracking`). Head + eye positions from a plain webcam, through
 *         the MediaPipe face detector + Face Mesh V2 on ONNX Runtime.
 *
 * Contract: the runtime's `xrt/xrt_tracking_plugin.h` (ABI 1) and
 * `docs/specs/runtime/tracking-provider-discovery.md`. Shape:
 *  - probe: cheap, opens nothing (settings, the pack's files by size, ONNX
 *    Runtime loads, a camera is enumerated, the OS camera permission is not
 *    denied);
 *  - start: returns at once; the provider's own thread verifies the models,
 *    creates the sessions (Core ML -> CPU fallback), asks for camera
 *    permission when the OS has not decided yet, opens the camera and pushes
 *    one sample per analysed frame;
 *  - stop: joins the thread (no push after it returns), closes the camera.
 * Settings only through `host->get_setting`; never the environment.
 * Frames stay in this process and are never written anywhere.
 */

#include "xrt/xrt_tracking_plugin.h"
#include "xrt/xrt_results.h"

#include "ot_capture.h"
#include "ot_eyes.h"
#include "ot_face_tracker.h"
#include "ot_ort.h"
#include "ot_pack.h"
#include "ot_platform.h"
#include "ot_viewer.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OT_ID "open-tracking"
#define OT_VERSION "0.1.0"


/*
 *
 * Settings.
 *
 */

static const struct xrt_component_setting_desc g_descs[] = {
    {"ENABLE", XRT_COMPONENT_SETTING_TYPE_BOOL, "1",
     "0: probe declines. On by default: installing the provider is the opt-in, and the runtime's own Enabled "
     "switch and per-screen arbitration decide the rest"},
    {"DEVICE", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "camera: empty = the built-in camera (else the first), an index, or a substring of the name"},
    {"WIDTH", XRT_COMPONENT_SETTING_TYPE_INT, "1280", "capture width: the best mode at or below it"},
    {"HEIGHT", XRT_COMPONENT_SETTING_TYPE_INT, "720", "capture height: the best mode at or below it"},
    {"FPS", XRT_COMPONENT_SETTING_TYPE_INT, "30", "capture rate: the highest at or below it"},
    {"HFOV_DEG", XRT_COMPONENT_SETTING_TYPE_FLOAT, "70",
     "the camera's horizontal field of view in degrees (10..170): sets the focal length, so the distance "
     "scales with it"},
    {"MIRROR", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "1: the camera delivers a mirrored image"},
    {"EP", XRT_COMPONENT_SETTING_TYPE_ENUM, "cpu",
     "cpu | coreml (macOS; auto = cpu). CPU by default: the Neural Engine runs half precision and its eye "
     "depth jitters about 10x more on a still face"},
    {"COREML_UNITS", XRT_COMPONENT_SETTING_TYPE_ENUM, "ane",
     "ane | gpu | all | cpu: Core ML compute units (EP=coreml). ane (CPU + Neural Engine) by default: the "
     "lowest CPU cost, and it leaves the GPU to the renderer"},
    {"COREML_CACHE", XRT_COMPONENT_SETTING_TYPE_BOOL, "1",
     "1: keep Core ML's compiled models in the user cache directory (the compile is then one-time)"},
    {"CACHE_DIR", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "cache root (default: ~/Library/Caches/DisplayXR/OpenTracking on macOS)"},
    {"MODEL_DIR", XRT_COMPONENT_SETTING_TYPE_STRING, "", "model blobs + pack manifest (default: next to the binary)"},
    {"ONNXRUNTIME", XRT_COMPONENT_SETTING_TYPE_STRING, "", "ONNX Runtime library (default: next to the binary)"},
    {"THREADS", XRT_COMPONENT_SETTING_TYPE_INT, "2", "CPU intra-op threads per session (0 = ONNX Runtime's default)"},
    {"ORT_LOG", XRT_COMPONENT_SETTING_TYPE_ENUM, "warning", "warning | info | verbose"},
    {"DETECT_EVERY", XRT_COMPONENT_SETTING_TYPE_INT, "0",
     "> 0: run the face detector every N frames even while tracking; 0 = only when the face is lost"},
    {"DETECT_MIN", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.5", "face detector score threshold"},
    {"PRESENCE_MIN", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.5", "landmark presence below this = face lost"},
    {"IRIS_MM", XRT_COMPONENT_SETTING_TYPE_FLOAT, "11.7", "population iris diameter for the depth estimate"},
    {"IPD_MM", XRT_COMPONENT_SETTING_TYPE_FLOAT, "63", "nominal interpupillary distance for the depth estimate"},
    {"FILTER_MIN_CUTOFF", XRT_COMPONENT_SETTING_TYPE_FLOAT, "1.0", "One Euro filter: cutoff at rest, Hz"},
    {"FILTER_BETA", XRT_COMPONENT_SETTING_TYPE_FLOAT, "3.0", "One Euro filter: cutoff increase per m/s"},
    {"FILTER_D_CUTOFF", XRT_COMPONENT_SETTING_TYPE_FLOAT, "1.0", "One Euro filter: speed cutoff, Hz"},
    {"PERMISSION_TIMEOUT_S", XRT_COMPONENT_SETTING_TYPE_INT, "120",
     "how long start waits for the user to answer the OS camera prompt"},
    {"STATS_S", XRT_COMPONENT_SETTING_TYPE_INT, "0",
     "> 0: log frame rate and per-stage timings (detector, landmarks, capture -> push) every N seconds"},
};

static const struct xrt_tracking_plugin_host_iface *g_host = NULL;

static void
log_line(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[open-tracking] %s\n", buf);
	fflush(stderr);
}

static bool
setting(const char *key, char *buf, uint32_t cap)
{
	buf[0] = '\0';
	if (g_host == NULL ||
	    g_host->struct_size <
	        offsetof(struct xrt_tracking_plugin_host_iface, get_setting) + sizeof(g_host->get_setting) ||
	    g_host->get_setting == NULL) {
		return false;
	}
	return g_host->get_setting(OT_ID, key, buf, cap) && buf[0] != '\0';
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


/*
 *
 * State.
 *
 */

//! What the provider is doing, finer than the ABI's status enum (shown in describe().source and logs).
enum ot_state
{
	OT_STATE_STOPPED = 0,
	OT_STATE_STARTING,           //!< verifying models, creating sessions, waiting for the permission prompt
	OT_STATE_RUNNING,            //!< camera open, frames flowing
	OT_STATE_NO_CAMERA,          //!< no camera (or it went away): retrying
	OT_STATE_PERMISSION_DENIED,  //!< the OS refused camera access
	OT_STATE_MODELS_MISSING,     //!< a model blob is missing or does not match the pack
	OT_STATE_FAILED,             //!< inference could not start on any execution provider
};

static const char *
state_name(enum ot_state s)
{
	switch (s) {
	case OT_STATE_STOPPED: return "stopped";
	case OT_STATE_STARTING: return "starting";
	case OT_STATE_RUNNING: return "running";
	case OT_STATE_NO_CAMERA: return "no camera";
	case OT_STATE_PERMISSION_DENIED: return "camera permission denied";
	case OT_STATE_MODELS_MISSING: return "models missing";
	case OT_STATE_FAILED: return "inference failed";
	}
	return "?";
}

struct ot_config
{
	char device[256];
	struct ot_capture_request req;
	float hfov_deg;
	bool mirror;
	char ep[16];
	enum ot_coreml_units units;
	bool coreml_cache;
	char cache_dir[2048];
	char model_dir[2048];
	char ort_path[2048];
	int threads;
	enum ot_ort_log ort_log;
	struct ot_face_tracker_config tracker;
	struct ot_eye_params eyes;
	struct ot_filter_params filter;
	uint32_t permission_timeout_ms;
	uint32_t stats_s;
};

struct ot_provider
{
	ot_mutex lock; //!< guards the fields below the line
	bool lock_ready;
	ot_thread thread;
	bool thread_running;
	volatile bool stop;
	const struct xrt_tracking_sink *sink;
	struct ot_config cfg;
	// ---- guarded by lock
	enum ot_state state;
	char detail[256];
	char camera_name[OT_CAMERA_NAME_MAX];
	char camera_key[OT_CAMERA_KEY_MAX];
	char ep_used[64];
	int64_t latency_ns; //!< smoothed capture -> push
};

static struct ot_provider g_p; // one per process: the runtime loads a provider once
static struct ot_ort *g_ort = NULL;

static void
set_state(enum ot_state s, const char *fmt, ...)
{
	char d[256] = "";
	if (fmt != NULL) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(d, sizeof(d), fmt, ap);
		va_end(ap);
	}
	ot_lock(&g_p.lock);
	const bool changed = g_p.state != s || strcmp(g_p.detail, d) != 0;
	g_p.state = s;
	ot_copy_str(g_p.detail, sizeof(g_p.detail), d);
	ot_unlock(&g_p.lock);
	if (changed) {
		log_line("%s%s%s", state_name(s), d[0] != '\0' ? ": " : "", d);
	}
}

static void
read_config(struct ot_config *c)
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
	if (!setting("EP", c->ep, sizeof(c->ep))) {
		ot_copy_str(c->ep, sizeof(c->ep), "cpu");
	}
	char v[32];
	c->units = OT_COREML_ANE;
	if (setting("COREML_UNITS", v, sizeof(v))) {
		c->units = strcmp(v, "gpu") == 0 ? OT_COREML_GPU
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
	ot_face_tracker_config_defaults(&c->tracker);
	c->tracker.detect_every = (uint32_t)setting_int("DETECT_EVERY", 0, 0, 100000);
	c->tracker.detect_min_score = setting_float("DETECT_MIN", 0.5f);
	c->tracker.presence_min = setting_float("PRESENCE_MIN", 0.5f);
	ot_eye_params_defaults(&c->eyes);
	c->eyes.iris_diameter_m = setting_float("IRIS_MM", 11.7f) / 1000.0f;
	c->eyes.ipd_m = setting_float("IPD_MM", 63.0f) / 1000.0f;
	c->filter.min_cutoff_hz = setting_float("FILTER_MIN_CUTOFF", 1.0f);
	c->filter.beta = setting_float("FILTER_BETA", 3.0f);
	c->filter.d_cutoff_hz = setting_float("FILTER_D_CUTOFF", 1.0f);
	c->permission_timeout_ms = (uint32_t)setting_int("PERMISSION_TIMEOUT_S", 120, 1, 3600) * 1000u;
	c->stats_s = (uint32_t)setting_int("STATS_S", 0, 0, 3600);
}

//! Per-stage timing totals over one STATS_S window.
struct ot_stats
{
	int64_t since_ns;
	uint32_t frames, faces, detects;
	double det_ms, lm_ms, crop_ms, proc_ms, lat_ms, lat_max_ms;
	double stamp_ms;      //!< capture stamp -> the backend's arrival (the camera + driver)
	double wait_ms;       //!< arrival -> processing starts (the mailbox)
	double luma;          //!< sum of per-frame mean luma (0..255)
	float best_det_score; //!< the best detector score seen in the window
	float presence_min;   //!< the lowest landmark presence while a face was followed
};

//! Mean luma of a sparse grid of pixels: "the camera delivers black frames" is a common failure.
static double
mean_luma(const struct ot_image *im)
{
	double sum = 0;
	uint32_t n = 0;
	const uint32_t bpp = im->format == OT_FMT_BGRA ? 4u : 3u;
	for (uint32_t y = 0; y < im->height; y += 16) {
		const uint8_t *row = im->data + (size_t)y * im->stride;
		for (uint32_t x = 0; x < im->width; x += 16) {
			const uint8_t *p = row + (size_t)x * bpp;
			const double r = im->format == OT_FMT_BGRA ? p[2] : p[0], b = im->format == OT_FMT_BGRA ? p[0] : p[2];
			sum += 0.299 * r + 0.587 * p[1] + 0.114 * b;
			n++;
		}
	}
	return n > 0 ? sum / n : 0.0;
}

static void
stats_flush(struct ot_stats *st, const char *ep)
{
	const double secs = (double)(ot_monotonic_ns() - st->since_ns) * 1e-9;
	if (st->frames > 0 && secs > 0) {
		log_line("stats %s: %.1f fps analysed, face %u/%u, detector %u x %.2f ms, landmarks %.2f ms, (crops %.2f "
		         "ms), frame %.2f ms, capture->push %.1f ms (max %.1f) = stamp->arrival %.1f + arrival->process %.1f + "
		         "process; frame luma %.0f, best detector score %.2f",
		         ep, st->frames / secs, st->faces, st->frames, st->detects,
		         st->detects > 0 ? st->det_ms / st->detects : 0.0, st->lm_ms / st->frames, st->crop_ms / st->frames,
		         st->proc_ms / st->frames, st->faces > 0 ? st->lat_ms / st->faces : 0.0, st->lat_max_ms,
		         st->stamp_ms / st->frames, st->wait_ms / st->frames, st->luma / st->frames, (double)st->best_det_score);
	}
	memset(st, 0, sizeof(*st));
	st->since_ns = ot_monotonic_ns();
}

static bool
load_pack(const struct ot_config *c, struct ot_pack *pack, char *err, size_t err_size)
{
	char path[4096];
	snprintf(path, sizeof(path), "%s%c%s", c->model_dir, OT_PATH_SEP, OT_PACK_DEFAULT_FILE);
	return ot_pack_load(path, pack, err, err_size);
}

static bool
ensure_ort(const struct ot_config *c, char *err, size_t err_size)
{
	if (g_ort != NULL) {
		return true;
	}
	char detail[256];
	const enum ot_ort_status st = ot_ort_load(c->ort_path, c->ort_log, &g_ort, detail, sizeof(detail));
	if (st != OT_ORT_OK) {
		snprintf(err, err_size, "%s", detail);
		return false;
	}
	log_line("%s", detail);
	return true;
}


/*
 *
 * Camera pose (the runtime's answer, spec §6).
 *
 */

static void
read_pose(const char *camera_key, struct ot_pose *pose, bool log_it)
{
	struct xrt_camera_pose a;
	memset(&a, 0, sizeof(a));
	a.struct_size = (uint32_t)sizeof(a);
	if (XRT_TRACKING_PLUGIN_HOST_HAS_CAMERA_POSE(g_host) &&
	    g_host->get_camera_pose(g_host, camera_key, NULL, &a) == XRT_SUCCESS &&
	    a.source != XRT_CAMERA_POSE_SOURCE_NONE) {
		const bool changed = a.generation != pose->generation || pose->source == 0;
		memcpy(pose->t, a.position_m, sizeof(pose->t));
		memcpy(pose->q, a.orientation, sizeof(pose->q));
		pose->focal_scale = a.focal_scale > 0.0f ? a.focal_scale : 1.0f;
		pose->iris_scale = a.viewer_iris_scale > 0.0f ? a.viewer_iris_scale : 1.0f;
		pose->source = a.source;
		pose->generation = a.generation;
		if (changed && log_it) {
			log_line("camera pose from the runtime (%s): t (%.4f, %.4f, %.4f) m, q (%.4f, %.4f, %.4f, %.4f), "
			         "focal scale %.4f, viewer iris scale %.4f",
			         a.source == XRT_CAMERA_POSE_SOURCE_DISPLAY_DECLARED   ? "display-declared"
			         : a.source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION ? "user calibration"
			         : a.source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT   ? "placement hint"
			                                                               : "default prior",
			         (double)pose->t[0], (double)pose->t[1], (double)pose->t[2], (double)pose->q[0],
			         (double)pose->q[1], (double)pose->q[2], (double)pose->q[3], (double)pose->focal_scale,
			         (double)pose->iris_scale);
		}
		return;
	}
	if (pose->source != 0 || pose->generation != 0) {
		return; // keep what we had (a transient NOT_READY)
	}
	// An older runtime without the slot, or no geometry yet: the same top-centre prior the runtime uses,
	// from the panel height when the host knows it.
	memset(pose, 0, sizeof(*pose));
	pose->q[3] = 1.0f;
	pose->focal_scale = 1.0f;
	pose->iris_scale = 1.0f;
	struct xrt_tracking_host_display_geometry geo;
	memset(&geo, 0, sizeof(geo));
	geo.struct_size = (uint32_t)sizeof(geo);
	if (g_host != NULL &&
	    g_host->struct_size >= offsetof(struct xrt_tracking_plugin_host_iface, get_display_geometry) +
	                               sizeof(g_host->get_display_geometry) &&
	    g_host->get_display_geometry != NULL && g_host->get_display_geometry(&geo) == XRT_SUCCESS &&
	    geo.display_height_m > 0.0f) {
		pose->t[1] = 0.5f * geo.display_height_m + (float)XRT_CAMERA_POSE_BEZEL_MARGIN_M;
		if (log_it) {
			log_line("no camera pose from the runtime: assuming the top centre of the bezel (%.3f m up)",
			         (double)pose->t[1]);
		}
	} else if (log_it) {
		log_line("no camera pose and no display geometry from the runtime: eyes reported relative to the camera");
	}
}


/*
 *
 * The provider's thread.
 *
 */

//! Sleep in small steps until @p ms passed or stop was asked. True when stopping.
static bool
wait_or_stop(uint32_t ms)
{
	for (uint32_t t = 0; t < ms && !g_p.stop; t += 50) {
		ot_sleep_ms(50);
	}
	return g_p.stop;
}

static bool
create_tracker(const struct ot_config *c, const struct ot_pack *pack, bool force_cpu, struct ot_face_tracker **out)
{
	char err[512];
	bool want_coreml = false;
#ifdef __APPLE__
	want_coreml = !force_cpu && strcmp(c->ep, "coreml") == 0 && ot_ort_has_coreml(g_ort);
#else
	(void)force_cpu;
	if (strcmp(c->ep, "coreml") == 0) {
		log_line("EP=coreml is macOS-only: using the CPU");
	}
#endif
	if (want_coreml) {
		char cache[2048] = "";
		if (c->coreml_cache && c->cache_dir[0] != '\0') {
			snprintf(cache, sizeof(cache), "%s%ccoreml-ort-%s", c->cache_dir, OT_PATH_SEP, ot_ort_version(g_ort));
			if (!ot_make_dirs(cache)) {
				log_line("cannot create the Core ML cache %s: compiling without it", cache);
				cache[0] = '\0';
			}
		}
		struct ot_ep_config epc = {c->units, cache[0] != '\0' ? cache : NULL};
		const int64_t t0 = ot_monotonic_ns();
		if (ot_face_tracker_create(g_ort, pack, c->model_dir, OT_EP_COREML, &epc, c->threads, &c->tracker, out,
		                           err, sizeof(err))) {
			char rep[256];
			ot_face_tracker_ep_report(*out, rep, sizeof(rep));
			log_line("Core ML sessions ready in %.0f ms%s%s", (double)(ot_monotonic_ns() - t0) * 1e-6,
			         rep[0] != '\0' ? "; " : "", rep);
			ot_lock(&g_p.lock);
			ot_copy_str(g_p.ep_used, sizeof(g_p.ep_used), "Core ML");
			ot_unlock(&g_p.lock);
			return true;
		}
		log_line("Core ML failed (%s): falling back to the CPU", err);
	}
	struct ot_ep_config epc = {OT_COREML_ALL, NULL};
	if (!ot_face_tracker_create(g_ort, pack, c->model_dir, OT_EP_CPU, &epc, c->threads, &c->tracker, out, err,
	                            sizeof(err))) {
		log_line("CPU sessions failed: %s", err);
		return false;
	}
	ot_lock(&g_p.lock);
	ot_copy_str(g_p.ep_used, sizeof(g_p.ep_used), "CPU");
	ot_unlock(&g_p.lock);
	return true;
}

static void
worker(void *arg)
{
	(void)arg;
	ot_thread_set_latency_critical();
	const struct ot_config *c = &g_p.cfg;
	struct ot_face_tracker *tracker = NULL;
	struct ot_capture *cap = NULL;
	struct ot_frame_buffer fb;
	memset(&fb, 0, sizeof(fb));
	char err[512];

	set_state(OT_STATE_STARTING, "loading models");
	struct ot_pack pack;
	if (!load_pack(c, &pack, err, sizeof(err)) || !ot_pack_check_files(&pack, c->model_dir, true, err, sizeof(err))) {
		set_state(OT_STATE_MODELS_MISSING, "%s", err);
		return;
	}
	if (!ensure_ort(c, err, sizeof(err))) {
		set_state(OT_STATE_FAILED, "%s", err);
		return;
	}
	if (!create_tracker(c, &pack, strcmp(c->ep, "cpu") == 0, &tracker)) {
		set_state(OT_STATE_FAILED, "no execution provider could run the models");
		return;
	}

	struct ot_camera_info cam;
	bool is_file = strncmp(c->device, "file:", 5) == 0;
	if (!is_file) {
		enum ot_camera_auth auth = ot_capture_authorization();
		if (auth == OT_CAMERA_NOT_DETERMINED) {
			set_state(OT_STATE_STARTING, "waiting for the camera permission prompt");
			auth = ot_capture_request_access(c->permission_timeout_ms, &g_p.stop);
		}
		if (auth != OT_CAMERA_AUTHORIZED) {
			set_state(OT_STATE_PERMISSION_DENIED,
			          auth == OT_CAMERA_RESTRICTED
			              ? "a policy restricts camera access"
			              : "allow it in System Settings > Privacy & Security > Camera, then restart the app");
			ot_face_tracker_destroy(tracker);
			return;
		}
	}

	struct ot_viewer viewer;
	ot_viewer_reset(&viewer);
	struct ot_pose pose;
	memset(&pose, 0, sizeof(pose));
	struct ot_intrinsics k;
	memset(&k, 0, sizeof(k));
	int64_t last_pose_read = 0;
	bool first_sample = true;
	struct ot_stats stats;
	memset(&stats, 0, sizeof(stats));
	stats.since_ns = ot_monotonic_ns();

	while (!g_p.stop) {
		if (cap == NULL) {
			if (!ot_capture_resolve(c->device, &cam)) {
				set_state(OT_STATE_NO_CAMERA, "no camera matches DEVICE='%s'", c->device);
				if (wait_or_stop(2000)) {
					break;
				}
				continue;
			}
			if (!ot_capture_open(&cam, &c->req, &cap, err, sizeof(err))) {
				set_state(OT_STATE_NO_CAMERA, "%s", err);
				if (wait_or_stop(2000)) {
					break;
				}
				continue;
			}
			uint32_t w, h;
			float fps;
			ot_capture_mode(cap, &w, &h, &fps);
			(void)ot_intrinsics_from_hfov(w, h, c->hfov_deg, &k);
			ot_lock(&g_p.lock);
			ot_copy_str(g_p.camera_name, sizeof(g_p.camera_name), cam.name);
			ot_copy_str(g_p.camera_key, sizeof(g_p.camera_key), cam.key);
			ot_unlock(&g_p.lock);
			read_pose(cam.key, &pose, true);
			last_pose_read = ot_monotonic_ns();
			ot_face_tracker_reset(tracker);
			set_state(OT_STATE_RUNNING, "%s, %ux%u @ %.0f fps, HFOV %.0f deg (f = %.0f px)", cam.name, w, h,
			          (double)fps, (double)c->hfov_deg, (double)k.f);
		}
		if (!ot_capture_take(cap, &fb, 500)) {
			if (ot_capture_lost(cap)) {
				ot_capture_close(cap);
				cap = NULL;
				set_state(OT_STATE_NO_CAMERA, "the camera went away");
			}
			continue;
		}
		fb.frame.image.mirrored = c->mirror;
		if (fb.frame.image.width != k.width || fb.frame.image.height != k.height) {
			(void)ot_intrinsics_from_hfov(fb.frame.image.width, fb.frame.image.height, c->hfov_deg, &k);
		}

		struct ot_face_result face;
		const int64_t proc0 = ot_monotonic_ns();
		if (!ot_face_tracker_process(tracker, &fb.frame.image, &face, err, sizeof(err))) {
			log_line("inference failed (%s): recreating the sessions on the CPU", err);
			ot_face_tracker_destroy(tracker);
			tracker = NULL;
			if (!create_tracker(c, &pack, true, &tracker)) {
				set_state(OT_STATE_FAILED, "no execution provider could run the models");
				break;
			}
			continue;
		}

		const int64_t now = ot_monotonic_ns();
		stats.frames++;
		stats.faces += face.found ? 1u : 0u;
		stats.detects += face.detected ? 1u : 0u;
		stats.det_ms += face.ms_detect;
		stats.lm_ms += face.ms_landmarks;
		stats.crop_ms += face.ms_crop;
		stats.proc_ms += (double)(now - proc0) * 1e-6;
		stats.stamp_ms += (double)(fb.frame.arrival_ns - fb.frame.capture_ns) * 1e-6;
		stats.wait_ms += (double)(proc0 - fb.frame.arrival_ns) * 1e-6;
		if (c->stats_s > 0) {
			stats.luma += mean_luma(&fb.frame.image);
			stats.best_det_score =
			    face.best_raw_score > stats.best_det_score ? face.best_raw_score : stats.best_det_score;
		}
		if (now - last_pose_read > 2000000000LL) {
			read_pose(cam.key, &pose, true); // follows a new calibration without a restart
			last_pose_read = now;
		}
		struct xrt_tracking_viewer_sample s;
		if (ot_viewer_update(&viewer, &k, &c->eyes, &c->filter, &pose, &face, fb.frame.capture_ns, cam.key, &s,
		                     NULL)) {
			g_p.sink->push(g_p.sink, &s);
			const int64_t lat = ot_monotonic_ns() - fb.frame.capture_ns;
			ot_lock(&g_p.lock);
			g_p.latency_ns = g_p.latency_ns == 0 ? lat : (g_p.latency_ns * 15 + lat) / 16;
			ot_unlock(&g_p.lock);
			if (face.found) {
				stats.lat_ms += (double)lat * 1e-6;
				stats.lat_max_ms = (double)lat * 1e-6 > stats.lat_max_ms ? (double)lat * 1e-6 : stats.lat_max_ms;
			}
			if (first_sample && (s.flags & XRT_TRACKING_SAMPLE_TRACKING) != 0) {
				first_sample = false;
				log_line("first face: eyes L (%.3f, %.3f, %.3f) R (%.3f, %.3f, %.3f) m, %.0f ms capture -> push",
				         (double)s.left_eye[0], (double)s.left_eye[1], (double)s.left_eye[2],
				         (double)s.right_eye[0], (double)s.right_eye[1], (double)s.right_eye[2],
				         (double)lat * 1e-6);
			}
		}
		if (c->stats_s > 0 && ot_monotonic_ns() - stats.since_ns >= (int64_t)c->stats_s * 1000000000LL) {
			char ep[64];
			ot_lock(&g_p.lock);
			ot_copy_str(ep, sizeof(ep), g_p.ep_used);
			ot_unlock(&g_p.lock);
			stats_flush(&stats, ep);
		}
	}

	ot_capture_close(cap);
	ot_frame_buffer_free(&fb);
	(void)stats;
	ot_face_tracker_destroy(tracker);
}


/*
 *
 * The vtable.
 *
 */

static xrt_result_t
ot_probe(struct xrt_tracking_plugin_instance **out_inst)
{
	*out_inst = (struct xrt_tracking_plugin_instance *)&g_p;
	if (!g_p.lock_ready) {
		ot_mutex_init(&g_p.lock);
		g_p.lock_ready = true;
	}
	if (!setting_bool("ENABLE", true)) {
		log_line("declined: ENABLE=0");
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	struct ot_config c;
	read_config(&c);
	char err[512];
	struct ot_pack pack;
	if (!load_pack(&c, &pack, err, sizeof(err)) || !ot_pack_check_files(&pack, c.model_dir, false, err, sizeof(err))) {
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
			log_line("declined: camera permission %s for this process (System Settings > Privacy & Security > "
			         "Camera)",
			         a == OT_CAMERA_DENIED ? "denied" : "restricted by policy");
			return XRT_ERROR_PROBER_NOT_SUPPORTED;
		}
	}
	ot_lock(&g_p.lock);
	ot_copy_str(g_p.camera_name, sizeof(g_p.camera_name), cam.name);
	ot_copy_str(g_p.camera_key, sizeof(g_p.camera_key), cam.key);
	ot_unlock(&g_p.lock);
	log_line("probe: usable — camera '%s' (%s)", cam.name, cam.key);
	return XRT_SUCCESS;
}

static xrt_result_t
ot_describe(struct xrt_tracking_plugin_instance *inst, struct xrt_tracking_provider_info *out)
{
	(void)inst;
	if (out == NULL || out->struct_size < offsetof(struct xrt_tracking_provider_info, camera_key)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	out->tracks = XRT_TRACKING_TRACKS_EYE_PAIR | XRT_TRACKING_TRACKS_HEAD_POSE | XRT_TRACKING_TRACKS_CAMERA_SPACE;
	ot_lock(&g_p.lock);
	out->typical_latency_ns = g_p.latency_ns;
	if (g_p.state == OT_STATE_STOPPED || g_p.state == OT_STATE_RUNNING) {
		snprintf(out->source, sizeof(out->source), "webcam '%.96s'%s%s%s", g_p.camera_name,
		         g_p.ep_used[0] != '\0' ? " (" : "", g_p.ep_used, g_p.ep_used[0] != '\0' ? ")" : "");
	} else {
		snprintf(out->source, sizeof(out->source), "webcam '%.80s': %s", g_p.camera_name,
		         state_name(g_p.state));
	}
	if (out->struct_size >= offsetof(struct xrt_tracking_provider_info, camera_key) + sizeof(out->camera_key)) {
		ot_copy_str(out->camera_key, sizeof(out->camera_key), g_p.camera_key);
	}
	ot_unlock(&g_p.lock);
	return XRT_SUCCESS;
}

static xrt_result_t
ot_start(struct xrt_tracking_plugin_instance *inst, const struct xrt_tracking_sink *sink)
{
	(void)inst;
	if (sink == NULL || sink->push == NULL) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (g_p.thread_running) {
		return XRT_SUCCESS;
	}
	if (!g_p.lock_ready) {
		ot_mutex_init(&g_p.lock);
		g_p.lock_ready = true;
	}
	read_config(&g_p.cfg);
	g_p.sink = sink;
	g_p.stop = false;
	ot_lock(&g_p.lock);
	g_p.state = OT_STATE_STARTING;
	g_p.detail[0] = '\0';
	g_p.latency_ns = 0;
	ot_unlock(&g_p.lock);
	if (!ot_thread_start(&g_p.thread, worker, NULL)) {
		g_p.sink = NULL;
		return XRT_ERROR_ALLOCATION;
	}
	g_p.thread_running = true;
	return XRT_SUCCESS;
}

static void
ot_stop(struct xrt_tracking_plugin_instance *inst)
{
	(void)inst;
	if (!g_p.thread_running) {
		return;
	}
	g_p.stop = true;
	ot_thread_join(g_p.thread); // the worker closes the camera; no push after this
	g_p.thread_running = false;
	g_p.sink = NULL;
	set_state(OT_STATE_STOPPED, NULL);
}

static enum xrt_tracking_provider_status
ot_get_status(struct xrt_tracking_plugin_instance *inst)
{
	(void)inst;
	if (!g_p.lock_ready) {
		return XRT_TRACKING_PROVIDER_STATUS_STOPPED;
	}
	ot_lock(&g_p.lock);
	const enum ot_state s = g_p.state;
	ot_unlock(&g_p.lock);
	switch (s) {
	case OT_STATE_STOPPED: return XRT_TRACKING_PROVIDER_STATUS_STOPPED;
	case OT_STATE_STARTING: return XRT_TRACKING_PROVIDER_STATUS_UNKNOWN;
	case OT_STATE_RUNNING: return XRT_TRACKING_PROVIDER_STATUS_RUNNING;
	default: return XRT_TRACKING_PROVIDER_STATUS_NO_SOURCE;
	}
}

static void
ot_destroy(struct xrt_tracking_plugin_instance *inst)
{
	ot_stop(inst);
}

static struct xrt_tracking_plugin_iface g_iface = {
    .struct_size = sizeof(struct xrt_tracking_plugin_iface),
    .reserved_0 = 0,
    .id = OT_ID,
    .display_name = "DisplayXR Open Tracking",
    .vendor = "DisplayXR",
    .version = OT_VERSION,
    .probe = ot_probe,
    .describe = ot_describe,
    .start = ot_start,
    .stop = ot_stop,
    .get_status = ot_get_status,
    .destroy = ot_destroy,
    .describe_settings = describe_settings,
};

XRT_TRACKING_PLUGIN_EXPORT xrt_result_t
xrtTrackingPluginNegotiate(uint32_t runtime_api_version,
                           const struct xrt_tracking_plugin_host_iface *host,
                           struct xrt_tracking_plugin_iface **out_iface,
                           uint32_t *out_plugin_api_version)
{
	*out_plugin_api_version = XRT_TRACKING_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_TRACKING_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	g_host = host;
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}
