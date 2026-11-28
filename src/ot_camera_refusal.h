// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cameras a webcam backend must never open: a display's own tracker
 *         camera (pure, every OS).
 *
 * A display that tracks its viewer reads its own camera; opening that camera
 * from here would at best fight the display's tracker for it and at worst
 * wedge it (open/close cycles have done so). The runtime keeps such cameras
 * off its own UVC source with a denylist (ADR-043 Amendment 5) whose built-in
 * part is a few USB VID:PIDs that display plug-ins have reported as their
 * tracker cameras, plus generic words in a device name. The runtime does not
 * expose the full list (the active display plug-in's declarations) to
 * providers yet, so the webcam backends apply the SAME built-in part: a
 * refused device is never listed, so it is never opened, whatever DEVICE says.
 *
 * Keep this list equal to the runtime's (`u_stereo_uvc.c`,
 * `k_builtin_tracker_vid_pid` / `k_builtin_tracker_names`). Numbers only: no
 * vendor names.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Parse "vid_XXXX" and "pid_YYYY" (any case, hex) out of an OS device id such
 * as a Windows symbolic link or a sysfs path. False when either is missing.
 */
bool
ot_parse_vid_pid(const char *id, uint16_t *vid, uint16_t *pid);

/*!
 * Why the device named @p name with OS id @p id (either may be NULL) must not
 * be opened, or NULL when it may.
 */
const char *
ot_camera_refusal(const char *name, const char *id);

#ifdef __cplusplus
}
#endif
