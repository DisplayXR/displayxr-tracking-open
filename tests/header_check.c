// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The vendored runtime headers compile on their own, in this order and alone.
 */

#include "xrt/xrt_results.h"
#include "xrt/xrt_component_settings.h"
#include "xrt/xrt_camera_pose.h"
#include "xrt/xrt_tracking_plugin.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_tracking.h"

int opentrack_header_check_anchor = XRT_TRACKING_PLUGIN_API_VERSION_CURRENT + XRT_INPUT_PLUGIN_API_VERSION_CURRENT;
