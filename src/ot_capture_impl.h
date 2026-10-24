// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  What a capture backend implements (private to src/ot_capture_*).
 */

#pragma once

#include "ot_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ot_capture_ops
{
	void (*close)(struct ot_capture *c); //!< stop delivering, free impl (not the mailbox)
};

struct ot_capture
{
	const struct ot_capture_ops *ops;
	struct ot_mailbox *mb;
	uint32_t width, height;
	float fps;
	bool is_file;
	void *impl;
};

//! A capture with a mailbox; the backend fills ops / impl / mode.
struct ot_capture *
ot_capture_alloc(void);

/*
 * The platform backend (one per OS: ot_capture_avf.m, ot_capture_stub.c).
 */

uint32_t
ot_capture_platform_enumerate(struct ot_camera_info *out, uint32_t max);

bool
ot_capture_platform_open(const struct ot_camera_info *cam,
                         const struct ot_capture_request *req,
                         struct ot_capture **out,
                         char *err,
                         size_t err_size);

enum ot_camera_auth
ot_capture_platform_authorization(void);

enum ot_camera_auth
ot_capture_platform_request_access(uint32_t timeout_ms, volatile bool *cancel);

#ifdef __cplusplus
}
#endif
