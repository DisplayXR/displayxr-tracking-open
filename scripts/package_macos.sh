#!/bin/bash
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# package_macos.sh — build the macOS installer for the open tracking provider.
#
#   scripts/package_macos.sh <install-tree> <version> <out.pkg> [--min-runtime X.Y.Z]
#
# <install-tree> is `cmake --install build --prefix <dir>` of a Release build configured with
# -DOPENTRACK_INSTALL_MODELS_DIR=<models>: the provider, ONNX Runtime, the pack, the four ONNX
# blobs (face + hands) and licenses/. The .pkg installs where an INSTALLED runtime finds it with no
# environment:
#
#   /Library/Application Support/DisplayXR/DisplayProcessors/100-open-tracking-tracking-provider.json
#   /Library/Application Support/DisplayXR/DisplayProcessors/100-open-tracking-hands-input-provider.json
#       the two roles' manifests (one binary), in the runtime's system discovery root (the
#       -tracking-provider / -input-provider suffix routes each to its class; the
#       display-processor loader never reads them)
#   /Library/Application Support/DisplayXR/lib/displayxr/plugins/open-tracking/
#       the provider and everything it loads beside itself (ONNX Runtime, pack, models)
#
# Both are root-owned under /Library/Application Support/DisplayXR, which the runtime's loader
# classifies as platform-trusted, so DXR_ALLOW_DEV_PLUGIN_PATHS is not needed.
#
# Signing goes through the runtime's scripts/macos_sign_notarize.sh ($SIGN, required): sign-tree
# on the payload (Developer ID + hardened runtime when DXR_MACOS_APP_IDENTITY is set, else
# ad-hoc), then finalize-pkg (productsign + notarize + staple when their credentials are set).
set -euo pipefail

TREE="${1:?install tree}"
VERSION="${2:?version (X.Y.Z)}"
OUT="${3:?output .pkg}"
shift 3
MIN_RUNTIME=""
while [ $# -gt 0 ]; do
	case "$1" in
		--min-runtime) MIN_RUNTIME="${2:?}"; shift 2 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done
SIGN="${SIGN:?set SIGN to the runtime repo scripts/macos_sign_notarize.sh}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

PREFIX="/Library/Application Support/DisplayXR"
PLUGIN_DIR="lib/displayxr/plugins/open-tracking"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ROOT="$WORK/root$PREFIX"
DEST="$ROOT/$PLUGIN_DIR"
mkdir -p "$DEST" "$ROOT/DisplayProcessors"

# Everything the provider resolves next to itself (ot_module_dir), then the licences.
for f in DisplayXR-OpenTracking.dylib open-tracking-default.models.json face_detector.onnx face_landmarks.onnx \
	hand_detector.onnx hand_landmarks.onnx; do
	[ -f "$TREE/$f" ] || { echo "missing from the install tree: $f" >&2; exit 1; }
	cp "$TREE/$f" "$DEST/"
done
cp "$TREE"/libonnxruntime*.dylib "$DEST/"
cp -R "$TREE/licenses" "$DEST/licenses"

# The models must be the pack's bit-identical blobs.
python3 -I "$HERE/scripts/check_pack.py" "$HERE/packs/open-tracking-default.models.json" --verify-dir "$DEST"

for m in 100-open-tracking-tracking-provider.json 100-open-tracking-hands-input-provider.json; do
	sed -e "s|@OPENTRACK_BINARY@|$PREFIX/$PLUGIN_DIR/DisplayXR-OpenTracking.dylib|" \
	    -e "s|\"version\":      \"[^\"]*\"|\"version\":      \"$VERSION\"|" \
	    "$HERE/examples/manifest/$m.in" > "$ROOT/DisplayProcessors/$m"
	python3 -I -c 'import json,sys; json.load(open(sys.argv[1]))' "$ROOT/DisplayProcessors/$m"
done

cp "$HERE/scripts/macos/uninstall-open-tracking.sh" "$DEST/uninstall.sh"
chmod 0755 "$DEST/uninstall.sh"
chmod 0644 "$DEST"/*.dylib "$DEST"/*.onnx "$DEST"/*.json

# Sign last: nothing touches a Mach-O after this.
"$SIGN" sign-tree "$DEST"

SCRIPTS="$WORK/scripts"
mkdir -p "$SCRIPTS"
sed -e "s|@MIN_RUNTIME@|$MIN_RUNTIME|" "$HERE/scripts/macos/preinstall" > "$SCRIPTS/preinstall"
chmod 0755 "$SCRIPTS/preinstall"
install -m 0755 "$HERE/scripts/macos/postinstall" "$SCRIPTS/postinstall"

COMPONENT="$WORK/open-tracking.pkg"
pkgbuild --root "$WORK/root" --install-location / --ownership recommended \
	--identifier com.displayxr.tracking.open --version "$VERSION" \
	--scripts "$SCRIPTS" "$COMPONENT"
productbuild --package "$COMPONENT" "$OUT"
"$SIGN" finalize-pkg "$OUT"
echo "Built $OUT"
