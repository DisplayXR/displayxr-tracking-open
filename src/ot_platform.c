// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OS layer, see ot_platform.h.
 */

#ifndef _WIN32
#define _GNU_SOURCE // dladdr / Dl_info (glibc)
#endif

#include "ot_platform.h"
#include "ot_sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#ifdef _WIN32
#include <process.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

void
ot_thread_set_latency_critical(void)
{
#ifdef __APPLE__
	(void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

void
ot_copy_str(char *dst, size_t cap, const char *src)
{
	if (dst == NULL || cap == 0) {
		return;
	}
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

bool
ot_contains_ci(const char *hay, const char *needle)
{
	if (hay == NULL || needle == NULL) {
		return false;
	}
	const size_t n = strlen(needle);
	for (const char *h = hay; *h != '\0'; h++) {
		size_t i = 0;
		while (i < n && h[i] != '\0' && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == n) {
			return true;
		}
	}
	return n == 0;
}

bool
ot_sha256_file(const char *path, char out[65])
{
	FILE *f = NULL;
#ifdef _WIN32
	wchar_t w[MAX_PATH * 2];
	if (path == NULL || MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH * 2) == 0) {
		return false;
	}
	f = _wfopen(w, L"rb");
#else
	f = path != NULL ? fopen(path, "rb") : NULL;
#endif
	if (f == NULL) {
		return false;
	}
	struct ot_sha256_ctx c;
	ot_sha256_init(&c);
	unsigned char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		ot_sha256_update(&c, buf, n);
	}
	const bool ok = ferror(f) == 0;
	fclose(f);
	ot_sha256_final_hex(&c, out);
	return ok;
}

#ifdef _WIN32

void
ot_mutex_init(ot_mutex *m)
{
	InitializeSRWLock(m);
}
void
ot_lock(ot_mutex *m)
{
	AcquireSRWLockExclusive(m);
}
void
ot_unlock(ot_mutex *m)
{
	ReleaseSRWLockExclusive(m);
}
void
ot_mutex_destroy(ot_mutex *m)
{
	(void)m;
}
void
ot_cond_init(ot_cond *c)
{
	InitializeConditionVariable(c);
}
void
ot_cond_signal(ot_cond *c)
{
	WakeAllConditionVariable(c);
}
bool
ot_cond_wait_ms(ot_cond *c, ot_mutex *m, uint32_t timeout_ms)
{
	return SleepConditionVariableSRW(c, m, timeout_ms, 0) != 0;
}
void
ot_cond_destroy(ot_cond *c)
{
	(void)c;
}

struct tramp
{
	ot_thread_fn fn;
	void *arg;
};

static unsigned __stdcall thread_tramp(void *p)
{
	struct tramp t = *(struct tramp *)p;
	free(p);
	t.fn(t.arg);
	return 0;
}

bool
ot_thread_start(ot_thread *t, ot_thread_fn fn, void *arg)
{
	struct tramp *p = (struct tramp *)malloc(sizeof(*p));
	if (p == NULL) {
		return false;
	}
	p->fn = fn;
	p->arg = arg;
	uintptr_t h = _beginthreadex(NULL, 0, thread_tramp, p, 0, NULL);
	if (h == 0) {
		free(p);
		return false;
	}
	*t = (HANDLE)h;
	return true;
}

void
ot_thread_join(ot_thread t)
{
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
}

void
ot_sleep_ms(uint32_t ms)
{
	Sleep(ms);
}

int64_t
ot_monotonic_ns(void)
{
	static LARGE_INTEGER freq;
	if (freq.QuadPart == 0) {
		QueryPerformanceFrequency(&freq);
	}
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	// Split to avoid overflow: whole seconds, then the remainder.
	const int64_t s = c.QuadPart / freq.QuadPart;
	const int64_t r = c.QuadPart % freq.QuadPart;
	return s * 1000000000LL + (r * 1000000000LL) / freq.QuadPart;
}

static bool
to_wide(const char *s, wchar_t *w, int cap)
{
	return s != NULL && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap) != 0;
}

bool
ot_module_dir(char *buf, size_t buf_size)
{
	HMODULE self = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)(void *)&ot_module_dir, &self)) {
		return false;
	}
	wchar_t w[MAX_PATH * 2];
	const DWORD n = GetModuleFileNameW(self, w, (DWORD)(sizeof(w) / sizeof(w[0])));
	if (n == 0 || n >= sizeof(w) / sizeof(w[0])) {
		return false;
	}
	wchar_t *slash = wcsrchr(w, L'\\');
	if (slash == NULL) {
		return false;
	}
	*slash = L'\0';
	return WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)buf_size, NULL, NULL) != 0;
}

bool
ot_file_exists(const char *path)
{
	wchar_t w[MAX_PATH * 2];
	if (!to_wide(path, w, MAX_PATH * 2)) {
		return false;
	}
	const DWORD a = GetFileAttributesW(w);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int64_t
ot_file_size(const char *path)
{
	wchar_t w[MAX_PATH * 2];
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!to_wide(path, w, MAX_PATH * 2) || !GetFileAttributesExW(w, GetFileExInfoStandard, &d) ||
	    (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
		return -1;
	}
	return ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

bool
ot_make_dirs(const char *path)
{
	wchar_t w[MAX_PATH * 2];
	if (!to_wide(path, w, MAX_PATH * 2)) {
		return false;
	}
	for (wchar_t *p = w + 1; *p != 0; p++) {
		if (*p == L'\\' || *p == L'/') {
			const wchar_t c = *p;
			*p = 0;
			(void)CreateDirectoryW(w, NULL);
			*p = c;
		}
	}
	(void)CreateDirectoryW(w, NULL);
	const DWORD a = GetFileAttributesW(w);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool
ot_user_cache_dir(char *buf, size_t buf_size)
{
	PWSTR p = NULL;
	if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &p))) {
		return false;
	}
	char base[MAX_PATH * 3];
	const bool ok = WideCharToMultiByte(CP_UTF8, 0, p, -1, base, (int)sizeof(base), NULL, NULL) != 0;
	CoTaskMemFree(p);
	if (!ok) {
		return false;
	}
	snprintf(buf, buf_size, "%s\\DisplayXR\\OpenTracking\\Cache", base);
	return true;
}

void *
ot_lib_open(const char *path, char *err, size_t err_size)
{
	wchar_t w[MAX_PATH * 2];
	if (!to_wide(path, w, MAX_PATH * 2)) {
		snprintf(err, err_size, "bad path");
		return NULL;
	}
	for (wchar_t *c = w; *c != 0; c++) {
		if (*c == L'/') {
			*c = L'\\'; // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs backslashes
		}
	}
	HMODULE m = LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (m == NULL) {
		snprintf(err, err_size, "LoadLibraryExW failed (error %lu)", (unsigned long)GetLastError());
	}
	return (void *)m;
}

void *
ot_lib_sym(void *lib, const char *name)
{
	return lib != NULL ? (void *)GetProcAddress((HMODULE)lib, name) : NULL;
}

#else // POSIX

void
ot_mutex_init(ot_mutex *m)
{
	pthread_mutex_init(m, NULL);
}
void
ot_lock(ot_mutex *m)
{
	pthread_mutex_lock(m);
}
void
ot_unlock(ot_mutex *m)
{
	pthread_mutex_unlock(m);
}
void
ot_mutex_destroy(ot_mutex *m)
{
	pthread_mutex_destroy(m);
}
void
ot_cond_init(ot_cond *c)
{
	pthread_cond_init(c, NULL);
}
void
ot_cond_signal(ot_cond *c)
{
	pthread_cond_broadcast(c);
}
bool
ot_cond_wait_ms(ot_cond *c, ot_mutex *m, uint32_t timeout_ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts); // pthread_cond_timedwait's default clock
	ts.tv_sec += (time_t)(timeout_ms / 1000u);
	ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}
	return pthread_cond_timedwait(c, m, &ts) == 0;
}
void
ot_cond_destroy(ot_cond *c)
{
	pthread_cond_destroy(c);
}

struct tramp
{
	ot_thread_fn fn;
	void *arg;
};

static void *
thread_tramp(void *p)
{
	struct tramp t = *(struct tramp *)p;
	free(p);
	t.fn(t.arg);
	return NULL;
}

bool
ot_thread_start(ot_thread *t, ot_thread_fn fn, void *arg)
{
	struct tramp *p = (struct tramp *)malloc(sizeof(*p));
	if (p == NULL) {
		return false;
	}
	p->fn = fn;
	p->arg = arg;
	if (pthread_create(t, NULL, thread_tramp, p) != 0) {
		free(p);
		return false;
	}
	return true;
}

void
ot_thread_join(ot_thread t)
{
	pthread_join(t, NULL);
}

void
ot_sleep_ms(uint32_t ms)
{
	struct timespec ts;
	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	nanosleep(&ts, NULL);
}

int64_t
ot_monotonic_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

bool
ot_module_dir(char *buf, size_t buf_size)
{
	Dl_info info;
	if (dladdr((void *)&ot_module_dir, &info) == 0 || info.dli_fname == NULL) {
		return false;
	}
	char full[4096];
	if (realpath(info.dli_fname, full) == NULL) {
		ot_copy_str(full, sizeof(full), info.dli_fname);
	}
	char *slash = strrchr(full, '/');
	if (slash == NULL) {
		return false;
	}
	*slash = '\0';
	ot_copy_str(buf, buf_size, full);
	return strlen(full) < buf_size;
}

bool
ot_file_exists(const char *path)
{
	struct stat st;
	return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int64_t
ot_file_size(const char *path)
{
	struct stat st;
	if (path == NULL || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		return -1;
	}
	return (int64_t)st.st_size;
}

bool
ot_make_dirs(const char *path)
{
	if (path == NULL || path[0] == '\0') {
		return false;
	}
	char tmp[4096];
	ot_copy_str(tmp, sizeof(tmp), path);
	for (char *p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(tmp, 0700);
			*p = '/';
		}
	}
	(void)mkdir(tmp, 0700);
	struct stat st;
	return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
}

bool
ot_user_cache_dir(char *buf, size_t buf_size)
{
	struct passwd pw, *res = NULL;
	char pbuf[4096];
	if (getpwuid_r(getuid(), &pw, pbuf, sizeof(pbuf), &res) != 0 || res == NULL || pw.pw_dir == NULL ||
	    pw.pw_dir[0] != '/') {
		return false;
	}
#ifdef __APPLE__
	snprintf(buf, buf_size, "%s/Library/Caches/DisplayXR/OpenTracking", pw.pw_dir);
#else
	snprintf(buf, buf_size, "%s/.cache/displayxr/open-tracking", pw.pw_dir);
#endif
	return true;
}

void *
ot_lib_open(const char *path, char *err, size_t err_size)
{
	void *m = path != NULL ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
	if (m == NULL) {
		const char *e = dlerror();
		snprintf(err, err_size, "dlopen failed: %s", e != NULL ? e : "bad path");
	}
	return m;
}

void *
ot_lib_sym(void *lib, const char *name)
{
	return lib != NULL ? dlsym(lib, name) : NULL;
}

#endif
