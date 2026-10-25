// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The provider's whole OS layer: mutex + condition, a thread, sleep,
 *         the runtime's monotonic clock, the provider's own directory, files,
 *         the per-user cache directory, and loading a library by absolute path.
 *
 * Shared code stays MSVC / MinGW / POSIX portable: no pid_t, no <unistd.h> in
 * headers, no C11 atomics.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
typedef SRWLOCK ot_mutex;
typedef CONDITION_VARIABLE ot_cond;
typedef HANDLE ot_thread;
#define OT_PATH_SEP '\\'
#define OT_MUTEX_INITIALIZER SRWLOCK_INIT //!< a statically initialised ot_mutex (never destroyed)
#else
typedef pthread_mutex_t ot_mutex;
typedef pthread_cond_t ot_cond;
typedef pthread_t ot_thread;
#define OT_PATH_SEP '/'
#define OT_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER //!< a statically initialised ot_mutex (never destroyed)
#endif

void
ot_mutex_init(ot_mutex *m);
void
ot_lock(ot_mutex *m);
void
ot_unlock(ot_mutex *m);
void
ot_mutex_destroy(ot_mutex *m);

void
ot_cond_init(ot_cond *c);
//! Wake EVERY waiter (a shared camera has one waiter per consumer).
void
ot_cond_signal(ot_cond *c);
//! Wait on @p c (with @p m held) at most @p timeout_ms. False on timeout.
bool
ot_cond_wait_ms(ot_cond *c, ot_mutex *m, uint32_t timeout_ms);
void
ot_cond_destroy(ot_cond *c);

typedef void (*ot_thread_fn)(void *arg);
bool
ot_thread_start(ot_thread *t, ot_thread_fn fn, void *arg);
void
ot_thread_join(ot_thread t);

//! Mark the calling thread latency-critical (macOS: QOS_CLASS_USER_INTERACTIVE, so it is not parked
//! on efficiency cores; elsewhere nothing).
void
ot_thread_set_latency_critical(void);

void
ot_sleep_ms(uint32_t ms);

/*!
 * The runtime's monotonic clock, `os_monotonic_get_ns()`: CLOCK_MONOTONIC on
 * Linux and macOS (on macOS that clock keeps counting through system sleep,
 * i.e. mach_continuous_time), QueryPerformanceCounter on Windows. Every
 * `capture_ns` the provider publishes is in this domain.
 */
int64_t
ot_monotonic_ns(void);

/*! Directory of the module that contains this code (UTF-8, no trailing separator). */
bool
ot_module_dir(char *buf, size_t buf_size);

bool
ot_file_exists(const char *path);

//! File size in bytes; -1 when it does not exist.
int64_t
ot_file_size(const char *path);

//! Create @p path and every missing parent (owner-only on POSIX). True when it exists afterwards.
bool
ot_make_dirs(const char *path);

/*!
 * The per-user cache root for this provider (macOS: ~/Library/Caches/DisplayXR/OpenTracking,
 * Linux: ~/.cache/displayxr/open-tracking, Windows: %LOCALAPPDATA%\DisplayXR\OpenTracking\Cache
 * via the shell API). From the account database, never the environment.
 */
bool
ot_user_cache_dir(char *buf, size_t buf_size);

//! SHA-256 of a file as 64 lowercase hex digits.
bool
ot_sha256_file(const char *path, char out[65]);

/*! Load a shared library by ABSOLUTE path (Windows: its own dependencies from its directory). Never via PATH. */
void *
ot_lib_open(const char *path, char *err, size_t err_size);

void *
ot_lib_sym(void *lib, const char *name);

//! Copy at most cap-1 bytes and terminate.
void
ot_copy_str(char *dst, size_t cap, const char *src);

//! Case-insensitive substring test.
bool
ot_contains_ci(const char *hay, const char *needle);

#ifdef __cplusplus
}
#endif
