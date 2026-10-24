// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Camera capture behind one C interface. Backends:
 *
 *  - `ot_capture_avf.m` — macOS AVFoundation (the only real camera today);
 *  - `ot_capture_stub.c` — Windows / Linux: no camera yet (Media Foundation
 *    and V4L2 are follow-ups), so enumeration finds nothing and the provider
 *    declines in probe;
 *  - `ot_capture_file.c` — a "camera" that replays still images (binary PPM),
 *    every OS, selected by `DEVICE=file:<path>`: the hardware-free source of
 *    the fake-host tests. Never a default.
 *
 * Frames are kept in the process (never written anywhere). The backend copies
 * each frame into a latest-wins mailbox; the provider's thread takes the
 * newest one, so a slow inference drops frames instead of queueing them.
 * Timestamps are the CAPTURE time in the runtime's monotonic domain
 * (ot_monotonic_ns), not the arrival time.
 */

#pragma once

#include "ot_face_geom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OT_CAMERA_KEY_MAX 128
#define OT_CAMERA_NAME_MAX 128
#define OT_CAMERA_MAX 16

struct ot_camera_info
{
	char key[OT_CAMERA_KEY_MAX];   //!< stable id (AVCaptureDevice uniqueID; "file:<file name>")
	char locator[1024];            //!< what opens it (the uniqueID; "file:<path>")
	char name[OT_CAMERA_NAME_MAX]; //!< human name
	bool builtin;                  //!< the machine's own camera
};

//! The OS camera permission for this process (macOS TCC; elsewhere always AUTHORIZED).
enum ot_camera_auth
{
	OT_CAMERA_AUTHORIZED = 0,
	OT_CAMERA_NOT_DETERMINED = 1, //!< the OS asks the user on first use
	OT_CAMERA_DENIED = 2,
	OT_CAMERA_RESTRICTED = 3, //!< a policy forbids it
};

struct ot_capture_request
{
	uint32_t width, height, fps; //!< the best mode at or below this is chosen
};

struct ot_frame
{
	struct ot_image image; //!< valid until the next ot_capture_take on the same ot_frame_buffer
	int64_t capture_ns;    //!< capture time, runtime monotonic domain
	int64_t arrival_ns;    //!< when the backend got it (latency diagnostics)
	uint64_t seq;
};

//! The consumer's own copy of a frame.
struct ot_frame_buffer
{
	uint8_t *data;
	size_t cap;
	struct ot_frame frame;
};

struct ot_capture;

/*
 * Enumeration and permission: never open a device.
 */

//! Cameras present now (the real backend only; a `file:` device is resolved by ot_capture_resolve).
uint32_t
ot_capture_enumerate(struct ot_camera_info *out, uint32_t max);

/*!
 * Which device @p sel names: NULL / "" = the built-in camera, else the first;
 * all digits = that index; "file:<path>" = the file camera (exists?);
 * anything else = the first whose name contains it (case-insensitive).
 */
bool
ot_capture_resolve(const char *sel, struct ot_camera_info *out);

enum ot_camera_auth
ot_capture_authorization(void);

/*!
 * Ask the OS for permission (macOS: the TCC prompt, attributed to the
 * process's responsible app). Blocks up to @p timeout_ms, polling @p cancel.
 */
enum ot_camera_auth
ot_capture_request_access(uint32_t timeout_ms, volatile bool *cancel);

/*
 * Streaming.
 */

bool
ot_capture_open(const struct ot_camera_info *cam,
                const struct ot_capture_request *req,
                struct ot_capture **out,
                char *err,
                size_t err_size);

//! The mode actually opened.
void
ot_capture_mode(const struct ot_capture *c, uint32_t *w, uint32_t *h, float *fps);

/*!
 * Copy the newest frame newer than @p buf's into @p buf, waiting up to
 * @p timeout_ms. False on timeout (or when the device is gone).
 */
bool
ot_capture_take(struct ot_capture *c, struct ot_frame_buffer *buf, uint32_t timeout_ms);

//! The device disappeared or the session failed.
bool
ot_capture_lost(const struct ot_capture *c);

void
ot_capture_close(struct ot_capture *c);

void
ot_frame_buffer_free(struct ot_frame_buffer *buf);


/*
 * The mailbox the backends share (ot_capture_common.c).
 */

struct ot_mailbox;

struct ot_mailbox *
ot_mailbox_create(void);
void
ot_mailbox_destroy(struct ot_mailbox *mb);
//! Producer: copy a frame in (any thread).
void
ot_mailbox_put(struct ot_mailbox *mb,
               const uint8_t *data,
               uint32_t w,
               uint32_t h,
               uint32_t stride,
               enum ot_pixel_format fmt,
               int64_t capture_ns,
               int64_t arrival_ns);
bool
ot_mailbox_take(struct ot_mailbox *mb, struct ot_frame_buffer *buf, uint32_t timeout_ms);
void
ot_mailbox_set_lost(struct ot_mailbox *mb);
bool
ot_mailbox_lost(struct ot_mailbox *mb);

/*
 * The file camera (ot_capture_file.c), shared by every OS.
 */

bool
ot_capture_file_resolve(const char *path, struct ot_camera_info *out);
bool
ot_capture_file_open(const char *path,
                     const struct ot_capture_request *req,
                     struct ot_capture **out,
                     char *err,
                     size_t err_size);
bool
ot_capture_is_file(const struct ot_capture *c);

//! Read a binary PPM (P6, maxval 255). Caller frees *data.
bool
ot_ppm_read(const char *path, uint8_t **data, uint32_t *w, uint32_t *h);

#ifdef __cplusplus
}
#endif
