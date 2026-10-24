// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Face Mesh V2 landmark indices this provider uses.
 *
 * MediaPipe names landmarks in the SUBJECT's terms: "right" is the viewer's
 * own right eye, which a camera that faces the viewer sees on the LEFT of an
 * un-mirrored image. Checked on an un-mirrored public-domain portrait: the
 * right iris centre (468) has the smaller image x, the left (473) the larger.
 *
 * The model outputs 478 points (x, y, z) in crop pixels; z is relative depth
 * on roughly the scale of x, smaller = closer to the camera.
 */

#pragma once

#define OT_LM_COUNT 478

#define OT_LM_RIGHT_EYE_OUTER 33  //!< the subject's right eye, outer corner
#define OT_LM_LEFT_EYE_OUTER 263  //!< the subject's left eye, outer corner
#define OT_LM_FOREHEAD 10         //!< top of the face oval, midline
#define OT_LM_CHIN 152            //!< bottom of the face oval, midline

#define OT_LM_RIGHT_IRIS_CENTRE 468 //!< followed by 469..472, its four contour points
#define OT_LM_LEFT_IRIS_CENTRE 473  //!< followed by 474..477
