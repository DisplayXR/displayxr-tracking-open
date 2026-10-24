// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Model pack manifest, see ot_pack.h.
 */

#include "ot_pack.h"
#include "ot_json.h"
#include "ot_platform.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const k_roles[OT_ROLE_COUNT] = {"face_detector", "face_landmarks"};

static bool
is_hex64(const char *s)
{
	if (s == NULL || strlen(s) != 64) {
		return false;
	}
	for (int i = 0; i < 64; i++) {
		if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) {
			return false;
		}
	}
	return true;
}

static bool
parse_root(const struct ot_json *root, struct ot_pack *out, char *err, size_t err_size)
{
	memset(out, 0, sizeof(*out));
	if (root == NULL || root->type != OT_JSON_OBJECT) {
		snprintf(err, err_size, "pack is not a JSON object");
		return false;
	}
	if (strcmp(ot_json_str(root, "schema", ""), OT_PACK_SCHEMA) != 0) {
		snprintf(err, err_size, "pack schema is not %s", OT_PACK_SCHEMA);
		return false;
	}
	ot_copy_str(out->name, sizeof(out->name), ot_json_str(root, "name", ""));
	ot_copy_str(out->version, sizeof(out->version), ot_json_str(root, "version", ""));
	const struct ot_json *models = ot_json_get(root, "models");
	if (models == NULL || models->type != OT_JSON_ARRAY) {
		snprintf(err, err_size, "pack has no models array");
		return false;
	}
	for (const struct ot_json *m = models->child; m != NULL; m = m->next) {
		const char *role = ot_json_str(m, "role", "");
		int r = -1;
		for (int i = 0; i < OT_ROLE_COUNT; i++) {
			if (strcmp(role, k_roles[i]) == 0) {
				r = i;
			}
		}
		if (r < 0) {
			continue; // a role a later provider version uses (hands): not ours
		}
		struct ot_pack_model *pm = &out->models[r];
		const char *file = ot_json_str(m, "file", "");
		const char *sha = ot_json_str(m, "sha256", "");
		const double size = ot_json_num(m, "size", 0);
		if (file[0] == '\0' || strlen(file) >= sizeof(pm->file) || strchr(file, '/') != NULL ||
		    strchr(file, '\\') != NULL || strstr(file, "..") != NULL) {
			snprintf(err, err_size, "model '%s': bad file name", role);
			return false;
		}
		if (!is_hex64(sha) || !(size > 0)) {
			snprintf(err, err_size, "model '%s': sha256 / size missing", role);
			return false;
		}
		const struct ot_json *in = ot_json_get(m, "input");
		const struct ot_json *range = ot_json_get(in, "range");
		if (in == NULL || strcmp(ot_json_str(in, "layout", ""), "NHWC") != 0 || range == NULL ||
		    range->type != OT_JSON_ARRAY || range->count != 2 || ot_json_at(range, 0)->type != OT_JSON_NUMBER ||
		    ot_json_at(range, 1)->type != OT_JSON_NUMBER) {
			snprintf(err, err_size, "model '%s': input must be NHWC with a [lo, hi] range", role);
			return false;
		}
		const double w = ot_json_num(in, "width", 0), h = ot_json_num(in, "height", 0);
		if (!(w >= 16 && w <= 1024 && h >= 16 && h <= 1024)) {
			snprintf(err, err_size, "model '%s': bad input size", role);
			return false;
		}
		pm->present = true;
		ot_copy_str(pm->file, sizeof(pm->file), file);
		ot_copy_str(pm->sha256, sizeof(pm->sha256), sha);
		pm->size = (int64_t)size;
		pm->width = (uint32_t)w;
		pm->height = (uint32_t)h;
		pm->range_lo = (float)ot_json_at(range, 0)->number;
		pm->range_hi = (float)ot_json_at(range, 1)->number;
	}
	for (int i = 0; i < OT_ROLE_COUNT; i++) {
		if (!out->models[i].present) {
			snprintf(err, err_size, "pack has no '%s' model", k_roles[i]);
			return false;
		}
	}
	return true;
}

bool
ot_pack_parse(const char *text, size_t len, struct ot_pack *out, char *err, size_t err_size)
{
	struct ot_json *root = ot_json_parse(text, len, err, err_size);
	if (root == NULL) {
		return false;
	}
	const bool ok = parse_root(root, out, err, err_size);
	ot_json_free(root);
	return ok;
}

bool
ot_pack_load(const char *path, struct ot_pack *out, char *err, size_t err_size)
{
	char e[160];
	struct ot_json *root = ot_json_parse_file(path, e, sizeof(e));
	if (root == NULL) {
		snprintf(err, err_size, "%s: %s", path, e);
		return false;
	}
	const bool ok = parse_root(root, out, err, err_size);
	ot_json_free(root);
	return ok;
}

bool
ot_pack_check_files(const struct ot_pack *pack, const char *dir, bool hash, char *err, size_t err_size)
{
	for (int i = 0; i < OT_ROLE_COUNT; i++) {
		const struct ot_pack_model *m = &pack->models[i];
		char path[4096];
		snprintf(path, sizeof(path), "%s%c%s", dir, OT_PATH_SEP, m->file);
		const int64_t sz = ot_file_size(path);
		if (sz < 0) {
			snprintf(err, err_size, "model missing: %s (run scripts/fetch_models.py)", path);
			return false;
		}
		if (sz != m->size) {
			snprintf(err, err_size, "model %s is %lld bytes, the pack says %lld", m->file, (long long)sz,
			         (long long)m->size);
			return false;
		}
		if (hash) {
			char got[65];
			if (!ot_sha256_file(path, got) || strcmp(got, m->sha256) != 0) {
				snprintf(err, err_size, "model %s does not match the pack's sha256", m->file);
				return false;
			}
		}
	}
	return true;
}
