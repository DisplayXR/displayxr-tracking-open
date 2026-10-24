// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The face role's per-frame inference: BlazeFace detection only when
 *         there is no face to follow, then Face Mesh V2 on a rotated crop whose
 *         region comes from the previous frame's landmarks (tracking mode).
 *
 * One frame in, 478 landmarks (image px + relative depth) out. It owns the
 * two inference sessions; the caller owns the frames, the thread and policy.
 * A later hands role runs beside it on the same frames (palm detector + hand
 * landmarks with the same ROI-tracking shape), sharing ot_face_geom's sampler.
 */

#pragma once

#include "ot_face_geom.h"
#include "ot_landmarks.h"
#include "ot_ort.h"
#include "ot_pack.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ot_face_tracker;

struct ot_face_tracker_config
{
	float detect_min_score;  //!< BlazeFace score threshold (0.5)
	float presence_min;      //!< landmark presence below this = lost (0.5)
	uint32_t detect_every;   //!< > 0: also re-detect every N frames while tracking; 0 = only on loss
	float max_roi_jump;      //!< re-detect when the tracked ROI moves more than this × its side in one frame
};

void
ot_face_tracker_config_defaults(struct ot_face_tracker_config *c);

struct ot_face_result
{
	bool found;
	bool detected;      //!< this frame ran the detector
	float presence;     //!< landmark model's face presence, 0..1
	float detect_score; //!< the detection that seeded the current track
	float best_raw_score; //!< detector frames: the best anchor's score, even below the threshold (diagnostics)
	float lm[OT_LM_COUNT][3]; //!< logical image px (x, y) + relative depth z in image px
	struct ot_roi roi;        //!< the crop the landmarks came from
	double ms_detect, ms_landmarks; //!< stage time this frame incl. its crop (0 when not run)
	double ms_crop;                 //!< of which: building the model inputs (letterbox / rotated crop)
};

/*!
 * Create both sessions from @p model_dir with @p ep. Falls back to nothing:
 * the caller decides (Core ML failed -> try the CPU).
 */
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
                       size_t err_size);

void
ot_face_tracker_destroy(struct ot_face_tracker *t);

//! How much of each model Core ML took ("face_detector 95/95 nodes, 1 partition; ..."), or "".
void
ot_face_tracker_ep_report(const struct ot_face_tracker *t, char *buf, size_t cap);

//! Forget the current track (the next frame detects).
void
ot_face_tracker_reset(struct ot_face_tracker *t);

/*!
 * One frame. False only on an inference failure (@p err set): a frame with no
 * face is true with `found` false.
 */
bool
ot_face_tracker_process(struct ot_face_tracker *t,
                        const struct ot_image *img,
                        struct ot_face_result *out,
                        char *err,
                        size_t err_size);

#ifdef __cplusplus
}
#endif
