// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The hands role's per-frame inference: the palm detector when fewer
 *         hands are followed than wanted, then the hand-landmark model on a
 *         rotated crop per hand whose region comes from that hand's previous
 *         landmarks (tracking mode) — the face role's shape (ot_face_tracker.h),
 *         for up to two hands.
 *
 * One frame in, per hand: 21 landmarks (image px + relative depth), the
 * model's metric "world" landmarks, presence and handedness. It owns the two
 * inference sessions; the caller owns the frames, the thread and policy.
 */

#pragma once

#include "ot_face_geom.h"
#include "ot_hand_geom.h"
#include "ot_ort.h"
#include "ot_pack.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OT_HAND_MAX 2

struct ot_hand_tracker;

struct ot_hand_obs;

struct ot_hand_tracker_config
{
	float detect_min_score; //!< palm detector score threshold (0.5)
	float presence_min;     //!< landmark presence below this = the hand is lost (0.5)
	uint32_t detect_every; //!< while fewer than max_hands are followed (and at least one is), detect every N frames
	uint32_t max_hands;    //!< 1 or 2
	/*!
	 * Optional: false turns a hand away (a background hand, too far to be the user's). A followed
	 * hand it rejects is dropped; a detected one never takes a slot, so it cannot keep the user's
	 * second hand out.
	 */
	bool (*admit)(void *user, const struct ot_hand_obs *obs);
	void *admit_user;
};

void
ot_hand_tracker_config_defaults(struct ot_hand_tracker_config *c);

struct ot_hand_obs
{
	uint32_t track_id;                //!< stable while the hand is followed; a new number after a loss
	bool new_track;                   //!< seeded by a detection this frame
	float presence;                   //!< 0..1
	float right_frame;                //!< this frame's handedness: P(the subject's RIGHT hand), un-mirrored image
	float right_mean;                 //!< the same, averaged over the track
	float lm[OT_HAND_LM_COUNT][3];    //!< logical image px (x, y) + relative depth z in image px
	float world[OT_HAND_LM_COUNT][3]; //!< the model's metric landmarks, crop axes (ot_hand_world_to_camera)
	struct ot_roi roi;                //!< the crop the landmarks came from
};

struct ot_hand_result
{
	uint32_t count; //!< hands found this frame
	struct ot_hand_obs hands[OT_HAND_MAX];
	bool detected;        //!< this frame ran the palm detector
	uint32_t palms;       //!< palms the detector found (above the threshold, after NMS)
	uint32_t rejected;    //!< hands the admit callback turned away this frame
	float best_raw_score; //!< detector frames: the best anchor's score, even below the threshold
	double ms_detect, ms_landmarks, ms_crop;
};

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
                       size_t err_size);

void
ot_hand_tracker_destroy(struct ot_hand_tracker *t);

//! How much of each model Core ML took, or "".
void
ot_hand_tracker_ep_report(const struct ot_hand_tracker *t, char *buf, size_t cap);

//! Forget every hand (the next frame detects).
void
ot_hand_tracker_reset(struct ot_hand_tracker *t);

//! One frame. False only on an inference failure (@p err set).
bool
ot_hand_tracker_process(
    struct ot_hand_tracker *t, const struct ot_image *img, struct ot_hand_result *out, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
