// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A stand-in for the runtime's tracking host: loads a provider binary
 *         by path, negotiates, serves settings / display geometry / the camera
 *         pose (the runtime's default prior), and records what the provider
 *         pushes into its sink. Used by the CTest driver and by otr-probe.
 */

#pragma once

#include "xrt/xrt_tracking_plugin.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FH_MAX_SETTINGS 64
#define FH_RING 4096

struct fh_record
{
	struct xrt_tracking_viewer_sample s;
	int64_t push_ns; //!< when the sink received it (monotonic)
};

struct fh_config
{
	float panel_w_m, panel_h_m;  //!< the "display" (default 0.302 x 0.196 m, a 14-inch laptop panel)
	float nominal_z_m;           //!< default 0.6
	float iris_scale;            //!< the host's per-user viewer iris scale (default 1)
	bool no_pose_slot;           //!< act like an older runtime: get_camera_pose = NULL
	const char *env_prefix;      //!< also read settings from <prefix><KEY> in the environment (NULL: no)
};

void
fh_config_defaults(struct fh_config *c);

//! Add a provider setting (KEY=VALUE). False when full or malformed.
bool
fh_set(const char *kv);

/*!
 * Load @p path and negotiate with @p api_version. On success *out_iface is the
 * provider's vtable. Returns the negotiate result (or XRT_ERROR_* for a load failure).
 */
xrt_result_t
fh_load(const char *path, const struct fh_config *cfg, uint32_t api_version, struct xrt_tracking_plugin_iface **out_iface);

//! The sink to pass to start().
const struct xrt_tracking_sink *
fh_sink(void);

//! Samples pushed so far (monotonic count).
uint64_t
fh_count(void);

//! Copy the records with index >= @p from (at most @p max, the newest ones) into @p out; returns the count.
uint32_t
fh_records(uint64_t from, struct fh_record *out, uint32_t max);

int64_t
fh_now_ns(void);

void
fh_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
