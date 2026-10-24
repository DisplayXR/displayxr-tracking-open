// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows / Linux: no camera backend yet. Media Foundation (Windows)
 *         and V4L2 (Linux) are follow-ups; until then enumeration is empty,
 *         so `probe` declines ("no camera backend on this OS yet"), and only
 *         the file camera works (tests).
 */

#include "ot_capture_impl.h"

#include <stdio.h>

uint32_t
ot_capture_platform_enumerate(struct ot_camera_info *out, uint32_t max)
{
	(void)out;
	(void)max;
	return 0;
}

bool
ot_capture_platform_open(const struct ot_camera_info *cam,
                         const struct ot_capture_request *req,
                         struct ot_capture **out,
                         char *err,
                         size_t err_size)
{
	(void)cam;
	(void)req;
	*out = NULL;
	snprintf(err, err_size, "no camera backend on this OS yet");
	return false;
}

enum ot_camera_auth
ot_capture_platform_authorization(void)
{
	return OT_CAMERA_AUTHORIZED;
}

enum ot_camera_auth
ot_capture_platform_request_access(uint32_t timeout_ms, volatile bool *cancel)
{
	(void)timeout_ms;
	(void)cancel;
	return OT_CAMERA_AUTHORIZED;
}
