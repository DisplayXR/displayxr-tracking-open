#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Validate a tracking model pack manifest (schema displayxr-tracking-pack-2).

    check_pack.py PACK [--verify-dir DIR]

Checks the schema, that every upstream source and every model carry a sha256,
a size and an SPDX licence, that every model names its source bundle and its
.tflite inside it by sha256, that the converter versions are pinned, and that
no licence is non-commercial. With --verify-dir, also hashes the blobs in DIR.
Exit 0 = valid.
"""

import argparse
import hashlib
import json
import os
import re
import sys

HEX64 = re.compile(r"^[0-9a-f]{64}$")
OK_LICENCES = {"Apache-2.0", "MIT", "BSD-3-Clause", "BSL-1.0"}
ROLES = {"face_detector", "face_landmarks", "hand_detector", "hand_landmarks"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("--verify-dir")
    a = ap.parse_args()
    with open(a.pack, encoding="utf-8") as f:
        p = json.load(f)
    errs = []

    def need(cond, msg):
        if not cond:
            errs.append(msg)

    need(p.get("schema") == "displayxr-tracking-pack-2", "schema")
    need(bool(p.get("name")) and bool(p.get("version")), "name / version")
    sources = set()
    for src in p.get("sources", []):
        n = src.get("name", "")
        need(bool(n) and n not in sources, "source name %r" % n)
        sources.add(n)
        need(src.get("url", "").startswith("https://"), "%s: url must be https" % n)
        need(bool(HEX64.match(src.get("sha256", ""))) and src.get("size", 0) > 0, "%s: sha256 / size" % n)
        need(src.get("license") in OK_LICENCES, "%s: licence %r" % (n, src.get("license")))
    need(bool(sources), "no sources")
    conv = p.get("conversion", {})
    for pkg in ("tensorflow", "tf2onnx", "onnx", "onnxruntime", "numpy", "protobuf"):
        need(bool(re.match(r"^\d+(\.\d+)+$", conv.get("versions", {}).get(pkg, ""))), "conversion pin %s" % pkg)
    need(conv.get("opset") == 17, "opset 17")
    roles = set()
    for m in p.get("models", []):
        r = m.get("role")
        roles.add(r)
        need(bool(HEX64.match(m.get("sha256", ""))) and m.get("size", 0) > 0, "%s: sha256 / size" % r)
        need(bool(HEX64.match(m.get("from_sha256", ""))) and m.get("from", "").endswith(".tflite"), "%s: from" % r)
        need(m.get("source") in sources, "%s: source %r is not in sources" % (r, m.get("source")))
        need(m.get("license") in OK_LICENCES, "%s: licence %r" % (r, m.get("license")))
        f = m.get("file", "")
        need(f.endswith(".onnx") and "/" not in f and "\\" not in f, "%s: file name" % r)
        need(m.get("input", {}).get("layout") == "NHWC", "%s: NHWC input" % r)
        if a.verify_dir and HEX64.match(m.get("sha256", "")):
            path = os.path.join(a.verify_dir, f)
            if not os.path.isfile(path):
                errs.append("%s: %s missing" % (r, path))
            else:
                with open(path, "rb") as fh:
                    got = hashlib.sha256(fh.read()).hexdigest()
                need(got == m["sha256"], "%s: %s sha256 %s" % (r, f, got))
    need(ROLES <= roles, "roles %s missing" % sorted(ROLES - roles))
    for img in p.get("test_images", []):
        need(img.get("url", "").startswith("https://") and bool(HEX64.match(img.get("sha256", ""))),
             "test image %r: https url + sha256" % img.get("name"))
        need(bool(img.get("license")), "test image %r: licence" % img.get("name"))
    for e in errs:
        print("FAIL " + e)
    print("ok" if not errs else "%d problem(s)" % len(errs))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
