#!/bin/bash
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Remove the open tracking provider. The runtime, its calibration store and camera consent are
# left alone. Run with sudo.
set -e
PREFIX="/Library/Application Support/DisplayXR"
rm -f "$PREFIX/DisplayProcessors/100-open-tracking-tracking-provider.json"
rm -rf "$PREFIX/lib/displayxr/plugins/open-tracking"
pkgutil --forget com.displayxr.tracking.open >/dev/null 2>&1 || true
echo "DisplayXR Open Tracking removed."
