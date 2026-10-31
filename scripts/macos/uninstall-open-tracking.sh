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
# A running service keeps the provider it found at start: restart it so it lets go.
CONSOLE_USER="$(stat -f%Su /dev/console 2>/dev/null || true)"
if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != root ]; then
	CONSOLE_UID="$(id -u "$CONSOLE_USER")"
	if launchctl print "gui/$CONSOLE_UID/com.displayxr.service" >/dev/null 2>&1; then
		launchctl kickstart -k "gui/$CONSOLE_UID/com.displayxr.service" >/dev/null 2>&1 || true
	fi
fi
echo "DisplayXR Open Tracking removed."
