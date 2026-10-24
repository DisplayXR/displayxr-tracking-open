// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A small strict JSON reader (RFC 8259): enough for model-pack
 *         manifests, shared by the provider and the fake host.
 *
 * Parses into a malloc'd tree; no comments, no trailing commas. Depth is
 * capped at 64 and input at 4 MiB, so a hostile manifest cannot exhaust the
 * stack or the heap of the hosting process.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OT_JSON_MAX_BYTES (4u * 1024u * 1024u)

enum ot_json_type
{
	OT_JSON_NULL = 0,
	OT_JSON_BOOL,
	OT_JSON_NUMBER,
	OT_JSON_STRING,
	OT_JSON_ARRAY,
	OT_JSON_OBJECT,
};

struct ot_json
{
	enum ot_json_type type;
	bool boolean;
	double number;
	char *string;          //!< OT_JSON_STRING: UTF-8, NUL-terminated
	char *key;             //!< member name when the parent is an object
	struct ot_json *child; //!< first element / member
	struct ot_json *next;  //!< next sibling
	size_t count;          //!< elements / members
};

/*! Parse @p len bytes. Returns NULL and a message in @p err on failure. */
struct ot_json *
ot_json_parse(const char *text, size_t len, char *err, size_t err_size);

/*! Read a whole file (≤ OT_JSON_MAX_BYTES) and parse it. UTF-8 path. */
struct ot_json *
ot_json_parse_file(const char *path, char *err, size_t err_size);

void
ot_json_free(struct ot_json *root);

/*! Member @p key of an object, or NULL. */
const struct ot_json *
ot_json_get(const struct ot_json *obj, const char *key);

/*! Element @p index of an array, or NULL. */
const struct ot_json *
ot_json_at(const struct ot_json *arr, size_t index);

/*! Typed member accessors: the default when absent or of another type. */
const char *
ot_json_str(const struct ot_json *obj, const char *key, const char *def);
double
ot_json_num(const struct ot_json *obj, const char *key, double def);
/*! -1 = absent / not a boolean, 0 = false, 1 = true. */
int
ot_json_bool(const struct ot_json *obj, const char *key);

#ifdef __cplusplus
}
#endif
