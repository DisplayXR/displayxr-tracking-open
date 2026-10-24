// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The model pack manifest (`packs/open-tracking-default.models.json`,
 *         schema `displayxr-tracking-pack-1`): which ONNX file plays which role,
 *         its sha256 and size. The provider loads blobs only through it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OT_PACK_SCHEMA "displayxr-tracking-pack-1"
#define OT_PACK_DEFAULT_FILE "open-tracking-default.models.json"
#define OT_PACK_MAX_MODELS 8

enum ot_model_role
{
	OT_ROLE_FACE_DETECTOR = 0,
	OT_ROLE_FACE_LANDMARKS = 1,
	OT_ROLE_COUNT
};

struct ot_pack_model
{
	bool present;
	char file[128];
	char sha256[65];
	int64_t size;
	uint32_t width, height; //!< the model's input
	float range_lo, range_hi;
};

struct ot_pack
{
	char name[64];
	char version[32];
	struct ot_pack_model models[OT_ROLE_COUNT];
};

/*!
 * Parse @p text. Strict: the schema string, every role this provider needs,
 * a file name without a path separator, a 64-hex sha256, a positive size,
 * an NHWC input and a range. @p err gets a sentence on failure.
 */
bool
ot_pack_parse(const char *text, size_t len, struct ot_pack *out, char *err, size_t err_size);

bool
ot_pack_load(const char *path, struct ot_pack *out, char *err, size_t err_size);

/*!
 * Check every model file in @p dir. @p hash: also verify the sha256 (start);
 * without it only existence and size (probe: cheap).
 */
bool
ot_pack_check_files(const struct ot_pack *pack, const char *dir, bool hash, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
