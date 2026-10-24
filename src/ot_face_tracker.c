// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Face role inference, see ot_face_tracker.h.
 */

#include "ot_face_tracker.h"
#include "ot_platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ot_face_tracker
{
	struct ot_face_tracker_config cfg;
	struct ot_session *det;
	struct ot_session *lm;
	struct ot_pack_model det_m, lm_m;
	float anchors[OT_FACE_DET_ANCHORS][2];
	uint32_t det_out_boxes, det_out_scores; //!< output indices
	uint32_t lm_out_points, lm_out_presence;
	float *det_in, *lm_in;
	float *det_boxes, *det_scores, *lm_points;
	float lm_presence;
	bool tracking;
	struct ot_roi roi;
	float seed_score;
	uint32_t frames_since_detect;
};

void
ot_face_tracker_config_defaults(struct ot_face_tracker_config *c)
{
	c->detect_min_score = 0.5f;
	c->presence_min = 0.5f;
	c->detect_every = 0;
	c->max_roi_jump = 0.5f;
}

static double
ms_since(int64_t t0)
{
	return (double)(ot_monotonic_ns() - t0) * 1e-6;
}

//! Find the outputs by element count: detector [896x16] + [896x1]; landmarks [1434] + presence [1].
static bool
map_outputs(struct ot_face_tracker *t, char *err, size_t err_size)
{
	bool boxes = false, scores = false, points = false, presence = false;
	for (uint32_t i = 0; i < ot_session_output_count(t->det); i++) {
		const size_t n = ot_session_output_elems(t->det, i);
		if (n == OT_FACE_DET_ANCHORS * 16u) {
			t->det_out_boxes = i;
			boxes = true;
		} else if (n == OT_FACE_DET_ANCHORS) {
			t->det_out_scores = i;
			scores = true;
		}
	}
	// Face Mesh V2: points [1,1,1,1434], presence logit [1,1,1,1], and a third [1,1] head we do not use.
	for (uint32_t i = 0; i < ot_session_output_count(t->lm); i++) {
		const size_t n = ot_session_output_elems(t->lm, i);
		if (n == OT_LM_COUNT * 3u && !points) {
			t->lm_out_points = i;
			points = true;
		} else if (n == 1 && !presence) {
			t->lm_out_presence = i; // the first scalar output is the presence logit
			presence = true;
		}
	}
	if (!boxes || !scores || !points || !presence) {
		snprintf(err, err_size, "the models' outputs are not BlazeFace + Face Mesh V2 shaped");
		return false;
	}
	return true;
}

static bool
check_input(struct ot_session *s, const struct ot_pack_model *m, char *err, size_t err_size)
{
	int64_t shape[4];
	if (!ot_session_input_shape(s, shape) || shape[0] != 1 || shape[1] != (int64_t)m->height ||
	    shape[2] != (int64_t)m->width || shape[3] != 3) {
		snprintf(err, err_size, "%s: input is not [1,%u,%u,3]", m->file, m->height, m->width);
		return false;
	}
	return true;
}

bool
ot_face_tracker_create(struct ot_ort *ort,
                       const struct ot_pack *pack,
                       const char *model_dir,
                       enum ot_ep ep,
                       const struct ot_ep_config *epc,
                       int threads,
                       const struct ot_face_tracker_config *cfg,
                       struct ot_face_tracker **out,
                       char *err,
                       size_t err_size)
{
	*out = NULL;
	struct ot_face_tracker *t = (struct ot_face_tracker *)calloc(1, sizeof(*t));
	if (t == NULL) {
		snprintf(err, err_size, "out of memory");
		return false;
	}
	t->cfg = *cfg;
	t->det_m = pack->models[OT_ROLE_FACE_DETECTOR];
	t->lm_m = pack->models[OT_ROLE_FACE_LANDMARKS];
	ot_face_anchors(t->anchors);
	char path[4096];
	struct ot_ep_config sub = *epc;
	char cache[4096];
	snprintf(path, sizeof(path), "%s%c%s", model_dir, OT_PATH_SEP, t->det_m.file);
	if (epc->coreml_cache_dir != NULL && epc->coreml_cache_dir[0] != '\0') {
		// One cache directory per blob: Core ML keys its compiled model by the cache directory.
		snprintf(cache, sizeof(cache), "%s%c%.16s", epc->coreml_cache_dir, OT_PATH_SEP, t->det_m.sha256);
		sub.coreml_cache_dir = ot_make_dirs(cache) ? cache : NULL;
	}
	if (!ot_session_create(ort, path, ep, &sub, threads, &t->det, err, err_size)) {
		goto fail;
	}
	snprintf(path, sizeof(path), "%s%c%s", model_dir, OT_PATH_SEP, t->lm_m.file);
	if (epc->coreml_cache_dir != NULL && epc->coreml_cache_dir[0] != '\0') {
		snprintf(cache, sizeof(cache), "%s%c%.16s", epc->coreml_cache_dir, OT_PATH_SEP, t->lm_m.sha256);
		sub.coreml_cache_dir = ot_make_dirs(cache) ? cache : NULL;
	}
	if (!ot_session_create(ort, path, ep, &sub, threads, &t->lm, err, err_size)) {
		goto fail;
	}
	if (!check_input(t->det, &t->det_m, err, err_size) || !check_input(t->lm, &t->lm_m, err, err_size) ||
	    !map_outputs(t, err, err_size)) {
		goto fail;
	}
	t->det_in = (float *)malloc(sizeof(float) * 3u * t->det_m.width * t->det_m.height);
	t->lm_in = (float *)malloc(sizeof(float) * 3u * t->lm_m.width * t->lm_m.height);
	t->det_boxes = (float *)malloc(sizeof(float) * OT_FACE_DET_ANCHORS * 16u);
	t->det_scores = (float *)malloc(sizeof(float) * OT_FACE_DET_ANCHORS);
	t->lm_points = (float *)malloc(sizeof(float) * OT_LM_COUNT * 3u);
	if (t->det_in == NULL || t->lm_in == NULL || t->det_boxes == NULL || t->det_scores == NULL ||
	    t->lm_points == NULL) {
		snprintf(err, err_size, "out of memory");
		goto fail;
	}
	*out = t;
	return true;

fail:
	ot_face_tracker_destroy(t);
	return false;
}

void
ot_face_tracker_destroy(struct ot_face_tracker *t)
{
	if (t == NULL) {
		return;
	}
	ot_session_destroy(t->det);
	ot_session_destroy(t->lm);
	free(t->det_in);
	free(t->lm_in);
	free(t->det_boxes);
	free(t->det_scores);
	free(t->lm_points);
	free(t);
}

void
ot_face_tracker_ep_report(const struct ot_face_tracker *t, char *buf, size_t cap)
{
	uint32_t a, b, c, d, e, f;
	buf[0] = '\0';
	if (t != NULL && ot_session_ep_nodes(t->det, &a, &b, &c) && ot_session_ep_nodes(t->lm, &d, &e, &f)) {
		snprintf(buf, cap, "Core ML took detector %u/%u nodes (%u partition%s), landmarks %u/%u (%u partition%s)",
		         a, b, c, c == 1 ? "" : "s", d, e, f, f == 1 ? "" : "s");
	}
}

void
ot_face_tracker_reset(struct ot_face_tracker *t)
{
	t->tracking = false;
	t->frames_since_detect = 0;
}

static float
range_scale(const struct ot_pack_model *m)
{
	return m->range_hi - m->range_lo;
}

//! Run the detector on the letterboxed frame; the best (largest = closest) face's ROI.
static bool
detect(struct ot_face_tracker *t, const struct ot_image *img, struct ot_face_result *out, bool *found, char *err,
       size_t err_size)
{
	const int64_t t0 = ot_monotonic_ns();
	struct ot_affine lb;
	ot_letterbox_affine(img->width, img->height, t->det_m.width, &lb);
	ot_sample_affine(img, &lb, t->det_m.width, t->det_m.height, range_scale(&t->det_m), t->det_m.range_lo, true,
	                 t->det_in);
	out->ms_crop += ms_since(t0);
	float *outs[OT_ORT_MAX_OUTPUTS] = {0};
	size_t caps[OT_ORT_MAX_OUTPUTS] = {0};
	outs[t->det_out_boxes] = t->det_boxes;
	caps[t->det_out_boxes] = OT_FACE_DET_ANCHORS * 16u;
	outs[t->det_out_scores] = t->det_scores;
	caps[t->det_out_scores] = OT_FACE_DET_ANCHORS;
	if (!ot_session_run(t->det, t->det_in, outs, caps, err, err_size)) {
		return false;
	}
	struct ot_face_det dets[OT_FACE_DET_MAX];
	uint32_t n = ot_face_decode(t->det_boxes, t->det_scores, t->anchors, t->cfg.detect_min_score, 0.3f,
	                                  dets, OT_FACE_DET_MAX);
	out->ms_detect = ms_since(t0);
	out->detected = true;
	float best = -100.0f;
	for (uint32_t i = 0; i < OT_FACE_DET_ANCHORS; i++) {
		best = t->det_scores[i] > best ? t->det_scores[i] : best;
	}
	out->best_raw_score = 1.0f / (1.0f + expf(-best));
	*found = false;
	if (n == 0) {
		return true;
	}
	// The viewer is the largest face (the closest to the screen), not the most confident.
	uint32_t best_i = 0;
	float best_area = 0.0f;
	for (uint32_t i = 0; i < n; i++) {
		const float area = (dets[i].box[2] - dets[i].box[0]) * (dets[i].box[3] - dets[i].box[1]);
		if (area > best_area) {
			best_area = area;
			best_i = i;
		}
	}
	ot_roi_from_detection(&dets[best_i], &lb, &t->roi);
	t->seed_score = dets[best_i].score;
	*found = true;
	return true;
}

static bool
landmarks(struct ot_face_tracker *t, const struct ot_image *img, struct ot_face_result *out, char *err, size_t err_size)
{
	const int64_t t0 = ot_monotonic_ns();
	const uint32_t size = t->lm_m.width;
	struct ot_affine m;
	ot_roi_to_affine(&t->roi, size, &m);
	ot_sample_affine(img, &m, size, size, range_scale(&t->lm_m), t->lm_m.range_lo, false, t->lm_in);
	out->ms_crop += ms_since(t0);
	float *outs[OT_ORT_MAX_OUTPUTS] = {0};
	size_t caps[OT_ORT_MAX_OUTPUTS] = {0};
	outs[t->lm_out_points] = t->lm_points;
	caps[t->lm_out_points] = OT_LM_COUNT * 3u;
	outs[t->lm_out_presence] = &t->lm_presence;
	caps[t->lm_out_presence] = 1;
	if (!ot_session_run(t->lm, t->lm_in, outs, caps, err, err_size)) {
		return false;
	}
	out->ms_landmarks += ms_since(t0);
	float lg = t->lm_presence;
	lg = lg < -100.0f ? -100.0f : (lg > 100.0f ? 100.0f : lg);
	out->presence = 1.0f / (1.0f + expf(-lg));
	const float k = t->roi.side / (float)size; // crop px -> image px (z on the same scale)
	for (uint32_t i = 0; i < OT_LM_COUNT; i++) {
		const float *p = t->lm_points + (size_t)i * 3u;
		ot_affine_apply(&m, p[0], p[1], &out->lm[i][0], &out->lm[i][1]);
		out->lm[i][2] = p[2] * k;
	}
	out->roi = t->roi;
	return true;
}

bool
ot_face_tracker_process(struct ot_face_tracker *t,
                        const struct ot_image *img,
                        struct ot_face_result *out,
                        char *err,
                        size_t err_size)
{
	memset(out, 0, sizeof(*out));
	const bool periodic = t->cfg.detect_every > 0 && t->frames_since_detect >= t->cfg.detect_every;
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!t->tracking || periodic) {
			bool found = false;
			if (!detect(t, img, out, &found, err, err_size)) {
				return false;
			}
			t->frames_since_detect = 0;
			if (!found) {
				t->tracking = false;
				return true;
			}
			t->tracking = true;
		}
		if (!landmarks(t, img, out, err, err_size)) {
			return false;
		}
		if (out->presence >= t->cfg.presence_min) {
			break;
		}
		// Lost: a tracked ROI that slid off the face gets one fresh detection this frame.
		const bool had_detected = out->detected;
		t->tracking = false;
		if (had_detected) {
			return true;
		}
	}
	if (!t->tracking || out->presence < t->cfg.presence_min) {
		t->tracking = false;
		return true;
	}
	out->found = true;
	out->detect_score = t->seed_score;
	t->frames_since_detect++;
	// Tracking mode: the next frame's crop comes from these landmarks.
	struct ot_roi next;
	ot_roi_from_landmarks((const float(*)[3])out->lm, OT_LM_COUNT, &next);
	const float jump = hypotf(next.cx - t->roi.cx, next.cy - t->roi.cy);
	if (!(next.side > 8.0f) || jump > t->cfg.max_roi_jump * t->roi.side) {
		t->tracking = false; // implausible: detect again next frame
	} else {
		t->roi = next;
	}
	return true;
}
