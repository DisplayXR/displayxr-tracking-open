// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hands role inference, see ot_hand_tracker.h.
 */

#include "ot_hand_tracker.h"
#include "ot_platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LM_VALUES (OT_HAND_LM_COUNT * 3)

struct ot_hand_slot
{
	bool tracking;
	struct ot_roi roi; //!< where to look this frame
	uint32_t track_id;
	float right_sum;
	uint32_t right_n;
};

struct ot_hand_tracker
{
	struct ot_hand_tracker_config cfg;
	struct ot_session *det;
	struct ot_session *lm;
	struct ot_pack_model det_m, lm_m;
	float anchors[OT_HAND_DET_ANCHORS][2];
	uint32_t det_out_boxes, det_out_scores;
	uint32_t lm_out_points, lm_out_presence, lm_out_handed, lm_out_world;
	float *det_in, *lm_in;
	float *det_boxes, *det_scores;
	float lm_points[LM_VALUES], lm_world[LM_VALUES];
	float lm_presence, lm_handed;
	struct ot_hand_slot slots[OT_HAND_MAX];
	uint32_t frames_since_detect;
	uint32_t next_id;
};

void
ot_hand_tracker_config_defaults(struct ot_hand_tracker_config *c)
{
	c->detect_min_score = 0.5f;
	c->presence_min = 0.5f;
	c->detect_every = 5;
	c->max_hands = 2;
}

static double
ms_since(int64_t t0)
{
	return (double)(ot_monotonic_ns() - t0) * 1e-6;
}

/*!
 * Outputs by element count: the detector [2016x18] + [2016x1]; the landmark model two [63] (image, then
 * world) and two scalars (presence, then handedness). The converted graph keeps the upstream output
 * names, which give that order (Identity, _1 presence, _2 handedness, _3 world); a graph whose names
 * differ falls back to the order of the outputs.
 */
static bool
map_outputs(struct ot_hand_tracker *t, char *err, size_t err_size)
{
	bool boxes = false, scores = false;
	for (uint32_t i = 0; i < ot_session_output_count(t->det); i++) {
		const size_t n = ot_session_output_elems(t->det, i);
		if (n == (size_t)OT_HAND_DET_ANCHORS * OT_HAND_DET_REGRESSORS) {
			t->det_out_boxes = i;
			boxes = true;
		} else if (n == OT_HAND_DET_ANCHORS) {
			t->det_out_scores = i;
			scores = true;
		}
	}
	int vec[2] = {-1, -1}, scal[2] = {-1, -1};
	int nv = 0, ns = 0;
	for (uint32_t i = 0; i < ot_session_output_count(t->lm); i++) {
		const size_t n = ot_session_output_elems(t->lm, i);
		if (n == LM_VALUES && nv < 2) {
			vec[nv++] = (int)i;
		} else if (n == 1 && ns < 2) {
			scal[ns++] = (int)i;
		}
	}
	if (!boxes || !scores || nv != 2 || ns != 2) {
		snprintf(err, err_size, "the models' outputs are not palm detector + hand landmarks shaped");
		return false;
	}
	t->lm_out_points = (uint32_t)vec[0];
	t->lm_out_world = (uint32_t)vec[1];
	t->lm_out_presence = (uint32_t)scal[0];
	t->lm_out_handed = (uint32_t)scal[1];
	for (uint32_t i = 0; i < ot_session_output_count(t->lm); i++) {
		const char *name = ot_session_output_name(t->lm, i);
		if (strcmp(name, "Identity") == 0) {
			t->lm_out_points = i;
		} else if (strcmp(name, "Identity_1") == 0) {
			t->lm_out_presence = i;
		} else if (strcmp(name, "Identity_2") == 0) {
			t->lm_out_handed = i;
		} else if (strcmp(name, "Identity_3") == 0) {
			t->lm_out_world = i;
		}
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

static bool
open_session(struct ot_ort *ort,
             const char *model_dir,
             const struct ot_pack_model *m,
             enum ot_ep ep,
             const struct ot_ep_config *epc,
             int threads,
             struct ot_session **out,
             char *err,
             size_t err_size)
{
	char path[4096], cache[4096];
	struct ot_ep_config sub = *epc;
	snprintf(path, sizeof(path), "%s%c%s", model_dir, OT_PATH_SEP, m->file);
	if (epc->coreml_cache_dir != NULL && epc->coreml_cache_dir[0] != '\0') {
		// One cache directory per blob: Core ML keys its compiled model by the cache directory.
		snprintf(cache, sizeof(cache), "%s%c%.16s", epc->coreml_cache_dir, OT_PATH_SEP, m->sha256);
		sub.coreml_cache_dir = ot_make_dirs(cache) ? cache : NULL;
	}
	return ot_session_create(ort, path, ep, &sub, threads, out, err, err_size);
}

bool
ot_hand_tracker_create(struct ot_ort *ort,
                       const struct ot_pack *pack,
                       const char *model_dir,
                       enum ot_ep ep,
                       const struct ot_ep_config *epc,
                       int threads,
                       const struct ot_hand_tracker_config *cfg,
                       struct ot_hand_tracker **out,
                       char *err,
                       size_t err_size)
{
	*out = NULL;
	struct ot_hand_tracker *t = (struct ot_hand_tracker *)calloc(1, sizeof(*t));
	if (t == NULL) {
		snprintf(err, err_size, "out of memory");
		return false;
	}
	t->cfg = *cfg;
	if (t->cfg.max_hands < 1 || t->cfg.max_hands > OT_HAND_MAX) {
		t->cfg.max_hands = OT_HAND_MAX;
	}
	t->det_m = pack->models[OT_ROLE_HAND_DETECTOR];
	t->lm_m = pack->models[OT_ROLE_HAND_LANDMARKS];
	ot_hand_anchors(t->anchors);
	if (!open_session(ort, model_dir, &t->det_m, ep, epc, threads, &t->det, err, err_size) ||
	    !open_session(ort, model_dir, &t->lm_m, ep, epc, threads, &t->lm, err, err_size) ||
	    !check_input(t->det, &t->det_m, err, err_size) || !check_input(t->lm, &t->lm_m, err, err_size) ||
	    !map_outputs(t, err, err_size)) {
		ot_hand_tracker_destroy(t);
		return false;
	}
	t->det_in = (float *)malloc(sizeof(float) * 3u * t->det_m.width * t->det_m.height);
	t->lm_in = (float *)malloc(sizeof(float) * 3u * t->lm_m.width * t->lm_m.height);
	t->det_boxes = (float *)malloc(sizeof(float) * OT_HAND_DET_ANCHORS * OT_HAND_DET_REGRESSORS);
	t->det_scores = (float *)malloc(sizeof(float) * OT_HAND_DET_ANCHORS);
	if (t->det_in == NULL || t->lm_in == NULL || t->det_boxes == NULL || t->det_scores == NULL) {
		snprintf(err, err_size, "out of memory");
		ot_hand_tracker_destroy(t);
		return false;
	}
	*out = t;
	return true;
}

void
ot_hand_tracker_destroy(struct ot_hand_tracker *t)
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
	free(t);
}

void
ot_hand_tracker_ep_report(const struct ot_hand_tracker *t, char *buf, size_t cap)
{
	uint32_t a, b, c, d, e, f;
	buf[0] = '\0';
	if (t != NULL && ot_session_ep_nodes(t->det, &a, &b, &c) && ot_session_ep_nodes(t->lm, &d, &e, &f)) {
		snprintf(buf, cap,
		         "Core ML took palm detector %u/%u nodes (%u partition%s), hand landmarks %u/%u (%u "
		         "partition%s)",
		         a, b, c, c == 1 ? "" : "s", d, e, f, f == 1 ? "" : "s");
	}
}

void
ot_hand_tracker_reset(struct ot_hand_tracker *t)
{
	for (int i = 0; i < OT_HAND_MAX; i++) {
		t->slots[i].tracking = false;
	}
	t->frames_since_detect = 0;
}

static float
range_scale(const struct ot_pack_model *m)
{
	return m->range_hi - m->range_lo;
}

static float
sigmoid_clamped(float v)
{
	// The landmark model's scalar heads end in a sigmoid already; clamp anyway (a NaN reads as 0).
	return v >= 0.0f && v <= 1.0f ? v : (v > 1.0f ? 1.0f : 0.0f);
}

//! Run the landmark model on @p roi; fills @p obs (not the track fields). False on an inference failure.
static bool
landmarks(struct ot_hand_tracker *t,
          const struct ot_image *img,
          const struct ot_roi *roi,
          struct ot_hand_obs *obs,
          struct ot_hand_result *out,
          char *err,
          size_t err_size)
{
	const int64_t t0 = ot_monotonic_ns();
	const uint32_t size = t->lm_m.width;
	struct ot_affine m;
	ot_roi_to_affine(roi, size, &m);
	ot_sample_affine(img, &m, size, size, range_scale(&t->lm_m), t->lm_m.range_lo, false, t->lm_in);
	out->ms_crop += ms_since(t0);
	float *outs[OT_ORT_MAX_OUTPUTS] = {0};
	size_t caps[OT_ORT_MAX_OUTPUTS] = {0};
	outs[t->lm_out_points] = t->lm_points;
	caps[t->lm_out_points] = LM_VALUES;
	outs[t->lm_out_world] = t->lm_world;
	caps[t->lm_out_world] = LM_VALUES;
	outs[t->lm_out_presence] = &t->lm_presence;
	caps[t->lm_out_presence] = 1;
	outs[t->lm_out_handed] = &t->lm_handed;
	caps[t->lm_out_handed] = 1;
	if (!ot_session_run(t->lm, t->lm_in, outs, caps, err, err_size)) {
		return false;
	}
	out->ms_landmarks += ms_since(t0);
	obs->presence = sigmoid_clamped(t->lm_presence);
	obs->right_frame = sigmoid_clamped(t->lm_handed);
	const float k = roi->side / (float)size; // crop px -> image px (z on the same scale)
	for (int i = 0; i < OT_HAND_LM_COUNT; i++) {
		const float *p = t->lm_points + i * 3;
		ot_affine_apply(&m, p[0], p[1], &obs->lm[i][0], &obs->lm[i][1]);
		obs->lm[i][2] = p[2] * k;
		memcpy(obs->world[i], t->lm_world + i * 3, sizeof(float) * 3);
	}
	obs->roi = *roi;
	return true;
}

//! A followed hand: accumulate its handedness, publish it, and aim next frame's crop at it.
static void
accept(struct ot_hand_tracker *t, struct ot_hand_slot *s, struct ot_hand_obs *obs, struct ot_hand_result *out)
{
	// Handedness averaged over the track (the newest 30 frames weigh in): one frame's answer flickers
	// on a side view, the track's does not.
	if (s->right_n >= 30) {
		s->right_sum *= 29.0f / 30.0f;
		s->right_n = 29;
	}
	s->right_sum += obs->right_frame;
	s->right_n++;
	obs->right_mean = s->right_sum / (float)s->right_n;
	obs->track_id = s->track_id;
	out->hands[out->count++] = *obs;
	struct ot_roi next;
	ot_hand_roi_from_landmarks((const float(*)[3])obs->lm, &next);
	if (next.side > 8.0f && isfinite(next.cx) && isfinite(next.cy)) {
		s->roi = next;
	} else {
		s->tracking = false; // implausible: detect again
	}
	(void)t;
}

static bool
detect(struct ot_hand_tracker *t,
       const struct ot_image *img,
       struct ot_hand_result *out,
       struct ot_hand_det *dets,
       uint32_t *n,
       struct ot_affine *lb,
       char *err,
       size_t err_size)
{
	const int64_t t0 = ot_monotonic_ns();
	ot_letterbox_affine(img->width, img->height, t->det_m.width, lb);
	ot_sample_affine(img, lb, t->det_m.width, t->det_m.height, range_scale(&t->det_m), t->det_m.range_lo, true,
	                 t->det_in);
	out->ms_crop += ms_since(t0);
	float *outs[OT_ORT_MAX_OUTPUTS] = {0};
	size_t caps[OT_ORT_MAX_OUTPUTS] = {0};
	outs[t->det_out_boxes] = t->det_boxes;
	caps[t->det_out_boxes] = (size_t)OT_HAND_DET_ANCHORS * OT_HAND_DET_REGRESSORS;
	outs[t->det_out_scores] = t->det_scores;
	caps[t->det_out_scores] = OT_HAND_DET_ANCHORS;
	if (!ot_session_run(t->det, t->det_in, outs, caps, err, err_size)) {
		return false;
	}
	*n = ot_hand_decode(t->det_boxes, t->det_scores, t->anchors, t->cfg.detect_min_score, 0.3f, dets,
	                    OT_HAND_DET_MAX);
	out->ms_detect = ms_since(t0);
	out->detected = true;
	out->palms = *n;
	float best = -100.0f;
	for (uint32_t i = 0; i < OT_HAND_DET_ANCHORS; i++) {
		best = t->det_scores[i] > best ? t->det_scores[i] : best;
	}
	out->best_raw_score = 1.0f / (1.0f + expf(-best));
	return true;
}

bool
ot_hand_tracker_process(
    struct ot_hand_tracker *t, const struct ot_image *img, struct ot_hand_result *out, char *err, size_t err_size)
{
	memset(out, 0, sizeof(*out));
	struct ot_hand_obs obs;

	// 1. The hands we follow, each on the crop its previous landmarks gave.
	for (uint32_t i = 0; i < t->cfg.max_hands; i++) {
		struct ot_hand_slot *s = &t->slots[i];
		if (!s->tracking) {
			continue;
		}
		memset(&obs, 0, sizeof(obs));
		if (!landmarks(t, img, &s->roi, &obs, out, err, err_size)) {
			return false;
		}
		if (obs.presence < t->cfg.presence_min) {
			s->tracking = false;
			continue;
		}
		accept(t, s, &obs, out);
	}
	// Two tracks that converged on one hand: keep the more confident.
	if (out->count == 2 && ot_roi_overlap(&out->hands[0].roi, &out->hands[1].roi) > 0.5f) {
		const int drop = out->hands[0].presence >= out->hands[1].presence ? 1 : 0;
		for (uint32_t i = 0; i < t->cfg.max_hands; i++) {
			if (t->slots[i].tracking && t->slots[i].track_id == out->hands[drop].track_id) {
				t->slots[i].tracking = false;
			}
		}
		if (drop == 0) {
			out->hands[0] = out->hands[1];
		}
		out->count = 1;
	}

	// 2. Look for more hands: always when none is followed, else every detect_every frames.
	const bool want =
	    out->count < t->cfg.max_hands &&
	    (out->count == 0 || t->frames_since_detect + 1 >= (t->cfg.detect_every > 0 ? t->cfg.detect_every : 1));
	if (!want) {
		t->frames_since_detect++;
		return true;
	}
	t->frames_since_detect = 0;
	struct ot_hand_det dets[OT_HAND_DET_MAX];
	uint32_t n = 0;
	struct ot_affine lb;
	if (!detect(t, img, out, dets, &n, &lb, err, err_size)) {
		return false;
	}
	for (uint32_t d = 0; d < n && out->count < t->cfg.max_hands; d++) {
		struct ot_roi roi;
		ot_hand_roi_from_detection(&dets[d], &lb, &roi);
		bool known = false;
		for (uint32_t h = 0; h < out->count; h++) {
			known = known || ot_roi_overlap(&roi, &out->hands[h].roi) > 0.3f;
		}
		if (known) {
			continue;
		}
		struct ot_hand_slot *free_slot = NULL;
		for (uint32_t i = 0; i < t->cfg.max_hands && free_slot == NULL; i++) {
			free_slot = t->slots[i].tracking ? NULL : &t->slots[i];
		}
		if (free_slot == NULL) {
			break;
		}
		memset(&obs, 0, sizeof(obs));
		if (!landmarks(t, img, &roi, &obs, out, err, err_size)) {
			return false;
		}
		if (obs.presence < t->cfg.presence_min) {
			continue;
		}
		free_slot->tracking = true;
		free_slot->track_id = ++t->next_id;
		free_slot->right_sum = 0.0f;
		free_slot->right_n = 0;
		obs.new_track = true;
		accept(t, free_slot, &obs, out);
	}
	return true;
}
