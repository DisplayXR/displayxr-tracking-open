// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The file camera: replays one binary PPM (or every `*.ppm` listed in
 *         a `.txt` file, one path per line) at the requested rate, stamped
 *         with the time it is published. For tests; selected only by
 *         `DEVICE=file:<path>`.
 */

#include "ot_capture_impl.h"
#include "ot_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAX_FRAMES 64

struct file_cam
{
	uint8_t *frames[FILE_MAX_FRAMES];
	uint32_t count;
	uint32_t w, h;
	uint32_t period_ms;
	volatile bool stop;
	ot_thread thread;
	bool running;
	struct ot_mailbox *mb;
};

static FILE *
open_read(const char *path)
{
#ifdef _WIN32
	wchar_t w[MAX_PATH * 2];
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH * 2) == 0) {
		return NULL;
	}
	return _wfopen(w, L"rb");
#else
	return fopen(path, "rb");
#endif
}

//! Next header token of a PPM, skipping whitespace and comments.
static bool
ppm_token(FILE *f, char *tok, size_t cap)
{
	int c;
	size_t n = 0;
	for (;;) {
		c = fgetc(f);
		if (c == '#') {
			while (c != '\n' && c != EOF) {
				c = fgetc(f);
			}
		}
		if (c == EOF) {
			return false;
		}
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
			break;
		}
	}
	while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r' && n + 1 < cap) {
		tok[n++] = (char)c;
		c = fgetc(f);
	}
	tok[n] = '\0';
	return n > 0; // the single whitespace after maxval is consumed here
}

bool
ot_ppm_read(const char *path, uint8_t **data, uint32_t *w, uint32_t *h)
{
	*data = NULL;
	FILE *f = open_read(path);
	if (f == NULL) {
		return false;
	}
	char t[4][16];
	bool ok = true;
	for (int i = 0; i < 4 && ok; i++) {
		ok = ppm_token(f, t[i], sizeof(t[i]));
	}
	const long ww = ok ? strtol(t[1], NULL, 10) : 0, hh = ok ? strtol(t[2], NULL, 10) : 0;
	ok = ok && strcmp(t[0], "P6") == 0 && strcmp(t[3], "255") == 0 && ww > 0 && hh > 0 && ww <= 8192 && hh <= 8192;
	if (ok) {
		const size_t n = (size_t)ww * (size_t)hh * 3u;
		*data = (uint8_t *)malloc(n);
		ok = *data != NULL && fread(*data, 1, n, f) == n;
		if (!ok) {
			free(*data);
			*data = NULL;
		}
	}
	fclose(f);
	if (ok) {
		*w = (uint32_t)ww;
		*h = (uint32_t)hh;
	}
	return ok;
}

static bool
ends_with(const char *s, const char *suffix)
{
	const size_t a = strlen(s), b = strlen(suffix);
	return a >= b && strcmp(s + a - b, suffix) == 0;
}

bool
ot_capture_file_resolve(const char *path, struct ot_camera_info *out)
{
	if (path == NULL || !ot_file_exists(path)) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	const char *base = strrchr(path, '/');
	const char *base2 = strrchr(path, '\\');
	base = base2 > base ? base2 : base;
	base = base != NULL ? base + 1 : path;
	snprintf(out->key, sizeof(out->key), "file:%s", base);
	snprintf(out->locator, sizeof(out->locator), "file:%s", path);
	snprintf(out->name, sizeof(out->name), "file camera '%s'", base);
	return true;
}

static void
file_run(void *arg)
{
	struct file_cam *fc = (struct file_cam *)arg;
	uint32_t i = 0;
	while (!fc->stop) {
		const int64_t now = ot_monotonic_ns();
		ot_mailbox_put(fc->mb, fc->frames[i], fc->w, fc->h, fc->w * 3u, OT_FMT_RGB, now, now);
		i = (i + 1) % fc->count;
		for (uint32_t slept = 0; slept < fc->period_ms && !fc->stop; slept += 5) {
			ot_sleep_ms(5);
		}
	}
}

static void
file_close(struct ot_capture *c)
{
	struct file_cam *fc = (struct file_cam *)c->impl;
	if (fc == NULL) {
		return;
	}
	fc->stop = true;
	if (fc->running) {
		ot_thread_join(fc->thread);
	}
	for (uint32_t i = 0; i < fc->count; i++) {
		free(fc->frames[i]);
	}
	free(fc);
	c->impl = NULL;
}

static const struct ot_capture_ops g_file_ops = {file_close};

static bool
load_frame(struct file_cam *fc, const char *path, char *err, size_t err_size)
{
	uint8_t *d = NULL;
	uint32_t w = 0, h = 0;
	if (!ot_ppm_read(path, &d, &w, &h)) {
		snprintf(err, err_size, "file camera: %s is not a binary PPM (P6, 8 bit)", path);
		return false;
	}
	if (fc->count > 0 && (w != fc->w || h != fc->h)) {
		free(d);
		snprintf(err, err_size, "file camera: %s is %ux%u, the first frame %ux%u", path, w, h, fc->w, fc->h);
		return false;
	}
	fc->w = w;
	fc->h = h;
	fc->frames[fc->count++] = d;
	return true;
}

bool
ot_capture_file_open(const char *path,
                     const struct ot_capture_request *req,
                     struct ot_capture **out,
                     char *err,
                     size_t err_size)
{
	*out = NULL;
	struct ot_capture *c = ot_capture_alloc();
	struct file_cam *fc = (struct file_cam *)calloc(1, sizeof(*fc));
	if (c == NULL || fc == NULL) {
		free(fc);
		ot_capture_close(c);
		snprintf(err, err_size, "out of memory");
		return false;
	}
	c->ops = &g_file_ops;
	c->impl = fc;
	c->is_file = true;
	fc->mb = c->mb;
	bool ok = true;
	if (ends_with(path, ".txt")) {
		FILE *f = open_read(path);
		char line[4096];
		ok = f != NULL;
		while (ok && f != NULL && fgets(line, sizeof(line), f) != NULL && fc->count < FILE_MAX_FRAMES) {
			line[strcspn(line, "\r\n")] = '\0';
			if (line[0] != '\0' && line[0] != '#') {
				ok = load_frame(fc, line, err, err_size);
			}
		}
		if (f != NULL) {
			fclose(f);
		}
		if (ok && fc->count == 0) {
			snprintf(err, err_size, "file camera: %s lists no frames", path);
			ok = false;
		}
	} else {
		ok = load_frame(fc, path, err, err_size);
	}
	if (!ok) {
		ot_capture_close(c);
		return false;
	}
	const uint32_t fps = req->fps > 0 && req->fps <= 240 ? req->fps : 30;
	fc->period_ms = 1000u / fps;
	c->width = fc->w;
	c->height = fc->h;
	c->fps = (float)fps;
	if (!ot_thread_start(&fc->thread, file_run, fc)) {
		ot_capture_close(c);
		snprintf(err, err_size, "file camera: no thread");
		return false;
	}
	fc->running = true;
	*out = c;
	return true;
}
