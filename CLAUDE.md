# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## What this repo is

The **DisplayXR open camera tracking provider** (`DisplayXR-OpenTracking`): one out-of-process
component that turns a camera into viewer tracking (registered as `open-tracking`, the first
open viewer-tracking provider) and hand tracking (registered as `open-tracking-hands`, an input
provider). Design and status: `docs/design.md`. There is no code until the runtime's
viewer-tracking provider class lands (runtime #2144, ADR-054 N6 Part 2).

**Public repository.** No internal data anywhere (code, comments, commit messages, docs): no
vendor or product names, no internal measurements, no vendor-module internals. Refer to a display
that tracks its viewer as "a display processor that tracks", never by vendor.

**Open only.** No proprietary dependency at build time. ONNX Runtime and model blobs are fetched
as pinned, hash-checked archives; never commit a model blob.

## Rules that will matter once code exists

- The runtime owns policy: which viewer source wins per screen, consent, the camera in-use
  indicator, kill switches. This component never decides precedence and never opens a camera the
  runtime's tracker-camera denylist refuses.
- Frames stay in process; publish only poses and joints. Never write a frame to disk.
- Configuration only through the host's settings call; never `getenv`.
- Honest units: report the head distance as estimated, never invent intrinsics.
- Vendored runtime headers under `include/xrt/` are copied verbatim at a pinned runtime tag and
  never edited in place.
