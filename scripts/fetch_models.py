#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Download the pinned upstream MediaPipe bundle, convert its face models to
ONNX reproducibly, and check every blob against the pack.

    fetch_models.py PACK DIR [--cache DIR] [--update-pack] [--test-images DIR]

  PACK            packs/open-tracking-default.models.json
  DIR             where the ONNX blobs and a copy of the pack land (the
                  provider's MODEL_DIR; next to the provider binary by default)
  --cache DIR     keep the downloaded .task here (default: DIR/.cache)
  --update-pack   write the sha256 / size this run produced into PACK instead
                  of checking them (maintainers, when a pin changes)
  --test-images   also fetch the public-domain test photos the fake-host face
                  test uses and write them as binary PPM into that directory

For development, CI and the release build. Needs the pinned converter
environment (scripts/requirements-convert.txt, Python 3.11): the ONNX bytes
depend on the tf2onnx / tensorflow / onnx / protobuf versions, so the pack
records them and this script refuses to run with others unless --update-pack.

Pipeline per model: unzip the .tflite from the .task (sha256-checked) ->
tf2onnx (opset 17) -> scripts/mp_onnx.py (symbolic dims fixed to 1, Core ML
graph fix, canonical names) -> sha256 / size check against the pack.
Nothing is written under its final name before it verified.
"""

import argparse
import hashlib
import importlib.metadata
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, dst, sha, size, attempts=8):
    if os.path.isfile(dst) and sha256_file(dst) == sha:
        print("have %s" % os.path.basename(dst))
        return
    tmp = dst + ".part"
    last = None
    for i in range(attempts):
        try:
            print("get  %s" % url)
            with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as out:
                shutil.copyfileobj(r, out, 1 << 20)
            got = sha256_file(tmp)
            if got != sha or (size and os.path.getsize(tmp) != size):
                raise RuntimeError("sha256 %s / size %d do not match the pin" % (got, os.path.getsize(tmp)))
            os.replace(tmp, dst)
            return
        except Exception as e:  # network is flaky: retry with backoff
            last = e
            print("     attempt %d failed: %s" % (i + 1, e))
            time.sleep(min(30, 2 ** i))
    raise SystemExit("download failed: %s (%s)" % (url, last))


def check_versions(conv, strict):
    bad = []
    for pkg, want in conv["versions"].items():
        try:
            have = importlib.metadata.version(pkg)
        except importlib.metadata.PackageNotFoundError:
            have = None
        if have != want:
            bad.append("%s %s (pack pins %s)" % (pkg, have, want))
    if bad and strict:
        raise SystemExit("converter environment differs from the pack's pins: " + "; ".join(bad) +
                         "\n  pip install -r scripts/requirements-convert.txt")
    for b in bad:
        print("warning: " + b)


def to_ppm(src, dst):
    from PIL import Image
    Image.open(src).convert("RGB").save(dst, format="PPM")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pack")
    ap.add_argument("dir")
    ap.add_argument("--cache")
    ap.add_argument("--update-pack", action="store_true")
    ap.add_argument("--test-images")
    args = ap.parse_args()

    with open(args.pack, encoding="utf-8") as f:
        pack = json.load(f)
    os.makedirs(args.dir, exist_ok=True)
    cache = args.cache or os.path.join(args.dir, ".cache")
    os.makedirs(cache, exist_ok=True)

    if args.test_images:
        os.makedirs(args.test_images, exist_ok=True)
        for img in pack.get("test_images", []):
            raw = os.path.join(cache, img["sha256"] + os.path.splitext(img["url"])[1])
            download(img["url"], raw, img["sha256"], img.get("size", 0))
            to_ppm(raw, os.path.join(args.test_images, img["name"] + ".ppm"))
            print("ok   test image %s.ppm" % img["name"])

    conv = pack["conversion"]
    check_versions(conv, strict=not args.update_pack)
    import mp_onnx  # needs onnx + onnxruntime from the converter environment

    src = pack["source"]
    task = os.path.join(cache, src["sha256"] + ".task")
    download(src["url"], task, src["sha256"], src["size"])

    failures = 0
    with tempfile.TemporaryDirectory() as work, zipfile.ZipFile(task) as z:
        for m in pack["models"]:
            tfl = os.path.join(work, m["from"])
            with z.open(m["from"]) as fi, open(tfl, "wb") as fo:
                shutil.copyfileobj(fi, fo)
            if sha256_file(tfl) != m["from_sha256"]:
                raise SystemExit("%s in the .task does not match from_sha256" % m["from"])
            raw = os.path.join(work, m["role"] + ".tf2onnx.onnx")
            env = dict(os.environ, PYTHONHASHSEED="0", TF_CPP_MIN_LOG_LEVEL="3", CUDA_VISIBLE_DEVICES="")
            r = subprocess.run([sys.executable, "-s", "-P", "-m", "tf2onnx.convert", "--tflite", tfl, "--output",
                                raw, "--opset", str(conv["opset"])], env=env, capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout + r.stderr)
                raise SystemExit("tf2onnx failed on %s" % m["from"])
            out = os.path.join(work, m["file"])
            print("conv %s: %s" % (m["file"], mp_onnx.finish(raw, out)))
            sha, size = sha256_file(out), os.path.getsize(out)
            if args.update_pack:
                m["sha256"], m["size"] = sha, size
            elif sha != m.get("sha256") or size != m.get("size"):
                print("FAIL %s: sha256 %s size %d, the pack pins %s / %s" % (m["file"], sha, size, m.get("sha256"),
                                                                          m.get("size")))
                failures += 1
                continue
            shutil.copyfile(out, os.path.join(args.dir, m["file"] + ".part"))
            os.replace(os.path.join(args.dir, m["file"] + ".part"), os.path.join(args.dir, m["file"]))
            print("ok   %s %s (%d bytes)" % (m["file"], sha, size))

    if args.update_pack:
        with open(args.pack, "w", encoding="utf-8") as f:
            json.dump(pack, f, indent=2)
            f.write("\n")
        print("updated %s" % args.pack)
    if failures == 0:
        shutil.copyfile(args.pack, os.path.join(args.dir, os.path.basename(args.pack)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
