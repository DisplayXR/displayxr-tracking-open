// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A small strict JSON reader, see ot_json.h.
 */

#include "ot_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define OT_JSON_MAX_DEPTH 64

struct parser
{
	const char *p;
	const char *end;
	char *err;
	size_t err_size;
	bool failed;
};

static void
fail(struct parser *ps, const char *what)
{
	if (!ps->failed && ps->err != NULL && ps->err_size > 0) {
		snprintf(ps->err, ps->err_size, "json: %s", what);
	}
	ps->failed = true;
}

static void
skip_ws(struct parser *ps)
{
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
		ps->p++;
	}
}

static struct ot_json *
new_node(enum ot_json_type type)
{
	struct ot_json *n = (struct ot_json *)calloc(1, sizeof(*n));
	if (n != NULL) {
		n->type = type;
	}
	return n;
}

static int
hex_val(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static bool
read_hex4(struct parser *ps, uint32_t *out)
{
	if (ps->end - ps->p < 4) {
		return false;
	}
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		const int h = hex_val(ps->p[i]);
		if (h < 0) {
			return false;
		}
		v = (v << 4) | (uint32_t)h;
	}
	ps->p += 4;
	*out = v;
	return true;
}

static size_t
put_utf8(char *dst, uint32_t cp)
{
	if (cp < 0x80) {
		dst[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		dst[0] = (char)(0xC0 | (cp >> 6));
		dst[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		dst[0] = (char)(0xE0 | (cp >> 12));
		dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	dst[0] = (char)(0xF0 | (cp >> 18));
	dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	dst[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

//! At an opening quote. Returns a malloc'd UTF-8 string.
static char *
parse_string_raw(struct parser *ps)
{
	if (ps->p >= ps->end || *ps->p != '"') {
		fail(ps, "expected a string");
		return NULL;
	}
	ps->p++;
	// The decoded string is never longer than the encoded one.
	const char *start = ps->p;
	size_t cap = 1;
	for (const char *q = start; q < ps->end && *q != '"'; q++) {
		cap++;
		if (*q == '\\' && q + 1 < ps->end) {
			q++;
			cap++;
		}
	}
	char *out = (char *)malloc(cap + 4);
	if (out == NULL) {
		fail(ps, "out of memory");
		return NULL;
	}
	size_t n = 0;
	while (ps->p < ps->end) {
		const unsigned char c = (unsigned char)*ps->p++;
		if (c == '"') {
			out[n] = '\0';
			return out;
		}
		if (c < 0x20) {
			break; // control characters must be escaped
		}
		if (c != '\\') {
			out[n++] = (char)c;
			continue;
		}
		if (ps->p >= ps->end) {
			break;
		}
		const char e = *ps->p++;
		switch (e) {
		case '"': out[n++] = '"'; break;
		case '\\': out[n++] = '\\'; break;
		case '/': out[n++] = '/'; break;
		case 'b': out[n++] = '\b'; break;
		case 'f': out[n++] = '\f'; break;
		case 'n': out[n++] = '\n'; break;
		case 'r': out[n++] = '\r'; break;
		case 't': out[n++] = '\t'; break;
		case 'u': {
			uint32_t cp = 0;
			if (!read_hex4(ps, &cp)) {
				free(out);
				fail(ps, "bad \\u escape");
				return NULL;
			}
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				uint32_t lo = 0;
				if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') {
					free(out);
					fail(ps, "unpaired surrogate");
					return NULL;
				}
				ps->p += 2;
				if (!read_hex4(ps, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
					free(out);
					fail(ps, "bad surrogate pair");
					return NULL;
				}
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				free(out);
				fail(ps, "unpaired surrogate");
				return NULL;
			}
			n += put_utf8(out + n, cp);
			break;
		}
		default:
			free(out);
			fail(ps, "bad escape");
			return NULL;
		}
	}
	free(out);
	fail(ps, "unterminated string");
	return NULL;
}

static struct ot_json *
parse_value(struct parser *ps, int depth);

static bool
literal(struct parser *ps, const char *word)
{
	const size_t n = strlen(word);
	if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, word, n) == 0) {
		ps->p += n;
		return true;
	}
	return false;
}

static struct ot_json *
parse_number(struct parser *ps)
{
	const char *s = ps->p;
	if (ps->p < ps->end && *ps->p == '-') {
		ps->p++;
	}
	if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
		fail(ps, "bad number");
		return NULL;
	}
	if (*ps->p == '0') {
		ps->p++;
	} else {
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
			ps->p++;
		}
	}
	if (ps->p < ps->end && *ps->p == '.') {
		ps->p++;
		if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
			fail(ps, "bad fraction");
			return NULL;
		}
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
			ps->p++;
		}
	}
	if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
		ps->p++;
		if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) {
			ps->p++;
		}
		if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
			fail(ps, "bad exponent");
			return NULL;
		}
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
			ps->p++;
		}
	}
	char buf[64];
	const size_t len = (size_t)(ps->p - s);
	if (len >= sizeof(buf)) {
		fail(ps, "number too long");
		return NULL;
	}
	memcpy(buf, s, len);
	buf[len] = '\0';
	struct ot_json *n = new_node(OT_JSON_NUMBER);
	if (n == NULL) {
		fail(ps, "out of memory");
		return NULL;
	}
	n->number = strtod(buf, NULL);
	return n;
}

static struct ot_json *
parse_container(struct parser *ps, int depth, bool object)
{
	ps->p++; // '[' or '{'
	struct ot_json *n = new_node(object ? OT_JSON_OBJECT : OT_JSON_ARRAY);
	if (n == NULL) {
		fail(ps, "out of memory");
		return NULL;
	}
	struct ot_json **tail = &n->child;
	const char close = object ? '}' : ']';
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == close) {
		ps->p++;
		return n;
	}
	for (;;) {
		char *key = NULL;
		skip_ws(ps);
		if (object) {
			key = parse_string_raw(ps);
			if (key == NULL) {
				break;
			}
			skip_ws(ps);
			if (ps->p >= ps->end || *ps->p != ':') {
				free(key);
				fail(ps, "expected ':'");
				break;
			}
			ps->p++;
		}
		struct ot_json *v = parse_value(ps, depth + 1);
		if (v == NULL) {
			free(key);
			break;
		}
		v->key = key;
		*tail = v;
		tail = &v->next;
		n->count++;
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == close) {
			ps->p++;
			return n;
		}
		fail(ps, object ? "expected ',' or '}'" : "expected ',' or ']'");
		break;
	}
	ot_json_free(n);
	return NULL;
}

static struct ot_json *
parse_value(struct parser *ps, int depth)
{
	if (depth > OT_JSON_MAX_DEPTH) {
		fail(ps, "nesting too deep");
		return NULL;
	}
	skip_ws(ps);
	if (ps->p >= ps->end) {
		fail(ps, "unexpected end");
		return NULL;
	}
	switch (*ps->p) {
	case '{': return parse_container(ps, depth, true);
	case '[': return parse_container(ps, depth, false);
	case '"': {
		char *s = parse_string_raw(ps);
		if (s == NULL) {
			return NULL;
		}
		struct ot_json *n = new_node(OT_JSON_STRING);
		if (n == NULL) {
			free(s);
			fail(ps, "out of memory");
			return NULL;
		}
		n->string = s;
		return n;
	}
	case 't':
	case 'f':
	case 'n': {
		struct ot_json *n = NULL;
		if (literal(ps, "true")) {
			n = new_node(OT_JSON_BOOL);
			if (n != NULL) {
				n->boolean = true;
			}
		} else if (literal(ps, "false")) {
			n = new_node(OT_JSON_BOOL);
		} else if (literal(ps, "null")) {
			n = new_node(OT_JSON_NULL);
		} else {
			fail(ps, "bad literal");
			return NULL;
		}
		if (n == NULL) {
			fail(ps, "out of memory");
		}
		return n;
	}
	default: return parse_number(ps);
	}
}

struct ot_json *
ot_json_parse(const char *text, size_t len, char *err, size_t err_size)
{
	if (err != NULL && err_size > 0) {
		err[0] = '\0';
	}
	struct parser ps = {text, text + len, err, err_size, false};
	if (text == NULL || len > OT_JSON_MAX_BYTES) {
		fail(&ps, "no input or input too large");
		return NULL;
	}
	// A UTF-8 byte-order mark is tolerated.
	if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
	    (unsigned char)text[2] == 0xBF) {
		ps.p += 3;
	}
	struct ot_json *root = parse_value(&ps, 0);
	if (root == NULL) {
		return NULL;
	}
	skip_ws(&ps);
	if (ps.p != ps.end) {
		ot_json_free(root);
		fail(&ps, "trailing characters");
		return NULL;
	}
	return root;
}

static FILE *
open_utf8(const char *path)
{
#ifdef _WIN32
	wchar_t wpath[1024];
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0) {
		return NULL;
	}
	return _wfopen(wpath, L"rb");
#else
	return fopen(path, "rb");
#endif
}

struct ot_json *
ot_json_parse_file(const char *path, char *err, size_t err_size)
{
	FILE *f = path != NULL ? open_utf8(path) : NULL;
	if (f == NULL) {
		if (err != NULL && err_size > 0) {
			snprintf(err, err_size, "json: cannot open %s", path != NULL ? path : "(null)");
		}
		return NULL;
	}
	char *buf = (char *)malloc(OT_JSON_MAX_BYTES + 1);
	size_t n = buf != NULL ? fread(buf, 1, OT_JSON_MAX_BYTES + 1, f) : 0;
	fclose(f); // held open only while needed
	if (buf == NULL || n > OT_JSON_MAX_BYTES) {
		free(buf);
		if (err != NULL && err_size > 0) {
			snprintf(err, err_size, "json: %s is too large or out of memory", path);
		}
		return NULL;
	}
	struct ot_json *root = ot_json_parse(buf, n, err, err_size);
	free(buf);
	return root;
}

void
ot_json_free(struct ot_json *root)
{
	while (root != NULL) {
		struct ot_json *next = root->next;
		ot_json_free(root->child);
		free(root->string);
		free(root->key);
		free(root);
		root = next;
	}
}

const struct ot_json *
ot_json_get(const struct ot_json *obj, const char *key)
{
	if (obj == NULL || obj->type != OT_JSON_OBJECT || key == NULL) {
		return NULL;
	}
	for (const struct ot_json *c = obj->child; c != NULL; c = c->next) {
		if (c->key != NULL && strcmp(c->key, key) == 0) {
			return c;
		}
	}
	return NULL;
}

const struct ot_json *
ot_json_at(const struct ot_json *arr, size_t index)
{
	if (arr == NULL || arr->type != OT_JSON_ARRAY) {
		return NULL;
	}
	const struct ot_json *c = arr->child;
	for (size_t i = 0; c != NULL && i < index; i++) {
		c = c->next;
	}
	return c;
}

const char *
ot_json_str(const struct ot_json *obj, const char *key, const char *def)
{
	const struct ot_json *v = ot_json_get(obj, key);
	return v != NULL && v->type == OT_JSON_STRING ? v->string : def;
}

double
ot_json_num(const struct ot_json *obj, const char *key, double def)
{
	const struct ot_json *v = ot_json_get(obj, key);
	return v != NULL && v->type == OT_JSON_NUMBER ? v->number : def;
}

int
ot_json_bool(const struct ot_json *obj, const char *key)
{
	const struct ot_json *v = ot_json_get(obj, key);
	if (v == NULL || v->type != OT_JSON_BOOL) {
		return -1;
	}
	return v->boolean ? 1 : 0;
}
