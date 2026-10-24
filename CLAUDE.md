# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## What this repo is

The **DisplayXR open camera tracking provider** (`DisplayXR-OpenTracking`): one component that
turns a camera into viewer tracking (registered as `open-tracking`, the first open
viewer-tracking provider) and, later, hand tracking (`open-tracking-hands`, an input provider).
Design: `docs/design.md`. Status: the viewer role runs on macOS (AVFoundation); Windows (Media
Foundation) and Linux (V4L2) capture and the hands role are follow-ups.

**Public repository.** No internal data anywhere (code, comments, commit messages, docs): no
vendor or product names, no internal measurements, no vendor-module internals. Refer to a display
that tracks its viewer as "a display processor that tracks", never by vendor. Write from public
MediaPipe documentation and model cards and our own code only.

**Open only.** No proprietary dependency at build time. ONNX Runtime and model blobs are fetched
as pinned, hash-checked archives; never commit a model blob.

## Layout

- `src/open_tracking.c` — negotiate + the vtable (probe / describe / start / stop / get_status /
  destroy / describe_settings), the settings, the provider's thread.
- `src/ot_face_tracker.{c,h}` — the face role's inference: detector on loss, Face Mesh V2 on a
  rotated crop that tracks the previous frame's landmarks. A hands role goes beside it.
- `src/ot_face_geom.{c,h}` — images, affine crops + sampler, anchors, decoding, NMS, ROIs (pure).
- `src/ot_eyes.{c,h}` — landmarks -> camera-space eyes (iris + IPD depth fusion), quaternions,
  the One Euro filter (pure).
- `src/ot_viewer.{c,h}` — one face result -> one `xrt_tracking_viewer_sample` (pure).
- `src/ot_capture*.{c,h,m}` — capture: AVFoundation (macOS), the PPM file camera (tests, every
  OS), stubs (Windows, Linux).
- `src/ot_ort.{c,h}` — ONNX Runtime by absolute path (`OrtGetApiBase` only), CPU / Core ML.
- `src/ot_pack.{c,h}`, `src/ot_json.{c,h}`, `src/ot_sha256.{c,h}`, `src/ot_platform.{c,h}`.
- `tools/fake_host.{c,h}` — the stand-in runtime host; `tools/fakehost.c` (CTest driver),
  `tools/otr_probe.c` (the dev CLI on a real camera). `tests/unit_tests.c`.
- `packs/open-tracking-default.models.json`, `scripts/fetch_models.py` (+ `mp_onnx.py`,
  `requirements-convert.txt`), `scripts/check_pack.py`.
- `include/xrt/` — vendored **verbatim** from the runtime (commit in `README.md`). Never edit.

## Rules

- The runtime owns policy: which viewer source wins per screen, consent, the camera in-use
  indicator, kill switches. This component never decides precedence and never opens a camera the
  runtime's tracker-camera denylist refuses.
- `probe` opens nothing; `start` returns at once and the provider's thread does the slow work.
- Frames stay in process; publish only poses. Never write a frame to disk.
- Configuration only through the host's settings call; never `getenv`.
- Honest units: the head distance is an estimate, intrinsics come from `HFOV_DEG`, the camera's
  pose comes from the host's `get_camera_pose` (never a private prior when the host answers).
- The camera-space block is the RAW measurement (unfiltered, not scaled by the iris scale): the
  runtime's calibration solves against it.
- Shared C stays MSVC / MinGW portable (no `pid_t`, no C11 atomics, no `<unistd.h>` in headers);
  Objective-C only in `ot_capture_avf.m`.

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENTRACK_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Inference tests need `-DOPENTRACK_TEST_MODELS_DIR=<dir>` filled by
`scripts/fetch_models.py packs/open-tracking-default.models.json <dir> --test-images <dir>/images`
(Python 3.11 with `scripts/requirements-convert.txt`).

## Conventions

- License every new file `BSL-1.0` with a `The DisplayXR Project` copyright header.
- Runtime style: tabs, 120 columns (`.clang-format`), `snake_case`, the `ot_` prefix.
- Bumping ONNX Runtime: change the version and every SHA-256 in `cmake/OnnxRuntime.cmake`.
- Changing a model or a converter pin: new pack `version`; regenerate with `--update-pack`, and
  re-run `scripts/check_pack.py --verify-dir`.
- Commits: conventional-commit subjects.
