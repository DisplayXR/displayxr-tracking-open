// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The built-in tracker-camera denylist, see ot_camera_refusal.h.
 */

#include "ot_camera_refusal.h"

#include "ot_platform.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

//! Equal to the runtime's k_builtin_tracker_vid_pid (ADR-043 Amendment 5).
static const uint16_t k_tracker_vid_pid[][2] = {
    {0x04f2, 0xb70c},
    {0x04e8, 0x20d7},
};

//! Equal to the runtime's k_builtin_tracker_names: generic words, no vendor names.
static const char *const k_tracker_names[] = {
    "tracking camera",
    "eye track",
};

//! The 4 hex digits after @p key ("vid_", "pid_") in @p id, case-insensitive.
static bool
hex4_after(const char *id, const char *key, uint16_t *out)
{
	const size_t kl = strlen(key);
	for (const char *p = id; *p != '\0'; p++) {
		size_t k = 0;
		while (k < kl && p[k] != '\0' && tolower((unsigned char)p[k]) == key[k]) {
			k++;
		}
		if (k != kl) {
			continue;
		}
		uint32_t v = 0;
		int i = 0;
		for (; i < 4; i++) {
			const int c = tolower((unsigned char)p[kl + i]);
			if (c >= '0' && c <= '9') {
				v = v * 16u + (uint32_t)(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				v = v * 16u + (uint32_t)(c - 'a' + 10);
			} else {
				break;
			}
		}
		if (i == 4) {
			*out = (uint16_t)v;
			return true;
		}
	}
	return false;
}

bool
ot_parse_vid_pid(const char *id, uint16_t *vid, uint16_t *pid)
{
	if (id == NULL) {
		return false;
	}
	uint16_t v = 0, p = 0;
	if (!hex4_after(id, "vid_", &v) || !hex4_after(id, "pid_", &p)) {
		return false;
	}
	*vid = v;
	*pid = p;
	return true;
}

const char *
ot_camera_refusal(const char *name, const char *id)
{
	uint16_t vid = 0, pid = 0;
	if (ot_parse_vid_pid(id, &vid, &pid)) {
		for (size_t i = 0; i < sizeof(k_tracker_vid_pid) / sizeof(k_tracker_vid_pid[0]); i++) {
			if (vid == k_tracker_vid_pid[i][0] && pid == k_tracker_vid_pid[i][1]) {
				return "a display's tracker camera (the runtime's built-in tracker-camera list)";
			}
		}
	}
	for (size_t i = 0; name != NULL && i < sizeof(k_tracker_names) / sizeof(k_tracker_names[0]); i++) {
		if (ot_contains_ci(name, k_tracker_names[i])) {
			return "named as a tracking camera (the runtime's built-in tracker-camera list)";
		}
	}
	return NULL;
}
