// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Capture dispatch, device selection and the latest-wins mailbox.
 */

#include "ot_capture_impl.h"
#include "ot_platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct ot_mailbox
{
	ot_mutex lock;
	ot_cond cond;
	uint8_t *data;
	size_t cap;
	uint32_t w, h, stride;
	enum ot_pixel_format fmt;
	int64_t capture_ns, arrival_ns;
	uint64_t seq;
	bool lost;
};

struct ot_mailbox *
ot_mailbox_create(void)
{
	struct ot_mailbox *mb = (struct ot_mailbox *)calloc(1, sizeof(*mb));
	if (mb != NULL) {
		ot_mutex_init(&mb->lock);
		ot_cond_init(&mb->cond);
	}
	return mb;
}

void
ot_mailbox_destroy(struct ot_mailbox *mb)
{
	if (mb == NULL) {
		return;
	}
	ot_cond_destroy(&mb->cond);
	ot_mutex_destroy(&mb->lock);
	free(mb->data);
	free(mb);
}

void
ot_mailbox_put(struct ot_mailbox *mb,
               const uint8_t *data,
               uint32_t w,
               uint32_t h,
               uint32_t stride,
               enum ot_pixel_format fmt,
               int64_t capture_ns,
               int64_t arrival_ns)
{
	const uint32_t bpp = fmt == OT_FMT_BGRA ? 4u : 3u;
	const size_t row = (size_t)w * bpp;
	const size_t need = row * h;
	ot_lock(&mb->lock);
	if (mb->cap < need) {
		uint8_t *p = (uint8_t *)realloc(mb->data, need);
		if (p == NULL) {
			ot_unlock(&mb->lock);
			return;
		}
		mb->data = p;
		mb->cap = need;
	}
	for (uint32_t y = 0; y < h; y++) {
		memcpy(mb->data + y * row, data + (size_t)y * stride, row); // tightly packed in the mailbox
	}
	mb->w = w;
	mb->h = h;
	mb->stride = (uint32_t)row;
	mb->fmt = fmt;
	mb->capture_ns = capture_ns;
	mb->arrival_ns = arrival_ns;
	mb->seq++;
	ot_cond_signal(&mb->cond);
	ot_unlock(&mb->lock);
}

bool
ot_mailbox_take(struct ot_mailbox *mb, struct ot_frame_buffer *buf, uint32_t timeout_ms)
{
	const int64_t deadline = ot_monotonic_ns() + (int64_t)timeout_ms * 1000000LL;
	ot_lock(&mb->lock);
	while (mb->seq == buf->frame.seq && !mb->lost) {
		const int64_t left = deadline - ot_monotonic_ns();
		if (left <= 0) {
			ot_unlock(&mb->lock);
			return false;
		}
		(void)ot_cond_wait_ms(&mb->cond, &mb->lock, (uint32_t)((left + 999999) / 1000000));
	}
	if (mb->seq == buf->frame.seq) {
		ot_unlock(&mb->lock);
		return false; // lost
	}
	const size_t need = (size_t)mb->stride * mb->h;
	if (buf->cap < need) {
		uint8_t *p = (uint8_t *)realloc(buf->data, need);
		if (p == NULL) {
			ot_unlock(&mb->lock);
			return false;
		}
		buf->data = p;
		buf->cap = need;
	}
	memcpy(buf->data, mb->data, need);
	struct ot_frame *f = &buf->frame;
	f->image.data = buf->data;
	f->image.width = mb->w;
	f->image.height = mb->h;
	f->image.stride = mb->stride;
	f->image.format = mb->fmt;
	f->image.mirrored = false;
	f->capture_ns = mb->capture_ns;
	f->arrival_ns = mb->arrival_ns;
	f->seq = mb->seq;
	ot_unlock(&mb->lock);
	return true;
}

void
ot_mailbox_set_lost(struct ot_mailbox *mb)
{
	ot_lock(&mb->lock);
	mb->lost = true;
	ot_cond_signal(&mb->cond);
	ot_unlock(&mb->lock);
}

bool
ot_mailbox_lost(struct ot_mailbox *mb)
{
	ot_lock(&mb->lock);
	const bool l = mb->lost;
	ot_unlock(&mb->lock);
	return l;
}

struct ot_capture *
ot_capture_alloc(void)
{
	struct ot_capture *c = (struct ot_capture *)calloc(1, sizeof(*c));
	if (c == NULL) {
		return NULL;
	}
	c->mb = ot_mailbox_create();
	if (c->mb == NULL) {
		free(c);
		return NULL;
	}
	return c;
}

uint32_t
ot_capture_enumerate(struct ot_camera_info *out, uint32_t max)
{
	return ot_capture_platform_enumerate(out, max);
}

static bool
all_digits(const char *s)
{
	if (*s == '\0') {
		return false;
	}
	for (; *s != '\0'; s++) {
		if (!isdigit((unsigned char)*s)) {
			return false;
		}
	}
	return true;
}

bool
ot_capture_resolve(const char *sel, struct ot_camera_info *out)
{
	if (sel != NULL && strncmp(sel, "file:", 5) == 0) {
		return ot_capture_file_resolve(sel + 5, out);
	}
	struct ot_camera_info cams[OT_CAMERA_MAX];
	const uint32_t n = ot_capture_platform_enumerate(cams, OT_CAMERA_MAX);
	if (n == 0) {
		return false;
	}
	if (sel == NULL || sel[0] == '\0') {
		for (uint32_t i = 0; i < n; i++) {
			if (cams[i].builtin) {
				*out = cams[i];
				return true;
			}
		}
		*out = cams[0];
		return true;
	}
	if (all_digits(sel)) {
		const long idx = strtol(sel, NULL, 10);
		if (idx >= 0 && (uint32_t)idx < n) {
			*out = cams[idx];
			return true;
		}
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (ot_contains_ci(cams[i].name, sel) || strcmp(cams[i].key, sel) == 0) {
			*out = cams[i];
			return true;
		}
	}
	return false;
}

enum ot_camera_auth
ot_capture_authorization(void)
{
	return ot_capture_platform_authorization();
}

enum ot_camera_auth
ot_capture_request_access(uint32_t timeout_ms, volatile bool *cancel)
{
	return ot_capture_platform_request_access(timeout_ms, cancel);
}

// The streams this process has open, by locator (ot_capture.h: one per device, shared).
static ot_mutex g_shared_lock = OT_MUTEX_INITIALIZER;
static struct ot_capture *g_shared = NULL;

bool
ot_capture_open(const struct ot_camera_info *cam,
                const struct ot_capture_request *req,
                struct ot_capture **out,
                char *err,
                size_t err_size)
{
	ot_lock(&g_shared_lock);
	for (struct ot_capture *c = g_shared; c != NULL; c = c->next_shared) {
		if (strcmp(c->locator, cam->locator) == 0 && !ot_mailbox_lost(c->mb)) {
			c->refs++;
			*out = c;
			ot_unlock(&g_shared_lock);
			return true;
		}
	}
	// Opening under the lock: a second role asking at the same moment waits for this stream instead of
	// opening the device twice.
	const bool ok = strncmp(cam->locator, "file:", 5) == 0
	                    ? ot_capture_file_open(cam->locator + 5, req, out, err, err_size)
	                    : ot_capture_platform_open(cam, req, out, err, err_size);
	if (ok) {
		ot_copy_str((*out)->locator, sizeof((*out)->locator), cam->locator);
		(*out)->refs = 1;
		(*out)->next_shared = g_shared;
		g_shared = *out;
	}
	ot_unlock(&g_shared_lock);
	return ok;
}

void
ot_capture_mode(const struct ot_capture *c, uint32_t *w, uint32_t *h, float *fps)
{
	*w = c->width;
	*h = c->height;
	*fps = c->fps;
}

bool
ot_capture_take(struct ot_capture *c, struct ot_frame_buffer *buf, uint32_t timeout_ms)
{
	return ot_mailbox_take(c->mb, buf, timeout_ms);
}

bool
ot_capture_lost(const struct ot_capture *c)
{
	return ot_mailbox_lost(c->mb);
}

bool
ot_capture_is_file(const struct ot_capture *c)
{
	return c != NULL && c->is_file;
}

void
ot_capture_close(struct ot_capture *c)
{
	if (c == NULL) {
		return;
	}
	ot_lock(&g_shared_lock);
	if (c->refs > 1) {
		c->refs--;
		ot_unlock(&g_shared_lock);
		return;
	}
	for (struct ot_capture **p = &g_shared; *p != NULL; p = &(*p)->next_shared) {
		if (*p == c) {
			*p = c->next_shared;
			break;
		}
	}
	ot_unlock(&g_shared_lock);
	if (c->ops != NULL && c->ops->close != NULL) {
		c->ops->close(c);
	}
	ot_mailbox_destroy(c->mb);
	free(c);
}

void
ot_frame_buffer_free(struct ot_frame_buffer *buf)
{
	free(buf->data);
	memset(buf, 0, sizeof(*buf));
}
