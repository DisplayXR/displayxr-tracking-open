# DisplayXR open camera tracking provider

`DisplayXR-OpenTracking` is a [DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime)
component that turns an ordinary camera into **viewer tracking** (head and eyes) and **hand
tracking** (`XR_EXT_hand_tracking` joints), with open models only. It is what a DisplayXR box gets when its display has no
tracker of its own: a laptop webcam is enough for head-coupled parallax.

> **Status: the viewer role and the hands role work on macOS** (AVFoundation capture, ONNX
> Runtime with Core ML). The hands role is verified end to end on still images through its ABI,
> not yet on a live camera. Windows (Media Foundation) and Linux (V4L2) capture and the installers
> are follow-ups; on those OSes the provider builds and declines in `probe`. Design:
> [`docs/design.md`](docs/design.md) (hands: §11).

| Role | Runtime class | Registry id | Status |
|---|---|---|---|
| Head + eye positions | viewer-tracking provider (ABI 1) | `open-tracking` | macOS |
| Hand joints (26 per hand), pinch | input provider (ADR-034, input ABI 1) | `open-tracking-hands` | macOS |

- **A display that tracks the viewer itself always wins** (runtime policy): on such a screen this
  provider stays in standby and never opens a camera. It serves screens whose display declares
  `EXTERNAL_OK` and does not track.
- **Frames never leave the process** and are never written anywhere; only eye and head poses and
  hand joints are published.
- **One camera, two roles:** both entry points are in one binary, and within a process both roles
  share one stream per camera (one privacy indicator); each runs its own inference on the frames.

## How it works

One thread per started provider. Per camera frame (the newest; older ones are dropped):

1. **Face detection** (BlazeFace short range, 128²) on the letterboxed frame — only when there is
   no face to follow (or every `DETECT_EVERY` frames). The largest face is the viewer.
2. **Landmarks** (Face Mesh V2, 478 points incl. both irises, 256²) on a rotated crop: from the
   detection on the first frame, then from the previous frame's landmarks (tracking mode: the eye
   corners 33 → 263 give the roll, the landmarks' box × 1.5 the size). A presence score below
   `PRESENCE_MIN` (or an implausible jump) drops back to detection.
3. **Eyes in the camera frame.** Iris centres 473 (the user's **left** eye) and 468 (the
   **right**): MediaPipe names landmarks in the subject's terms, and an un-mirrored camera that
   faces the user sees their right eye on the image's left. The capture forces mirroring off;
   `MIRROR=1` is for a camera that mirrors anyway. Direction of pixel `(u, v)`:
   `normalize(-(u - cx)/f, -(v - cy)/f, 1)` (+X the user's right, +Y up, +Z toward the viewer),
   `f = (W/2) / tan(HFOV/2)`, principal point at the centre, no distortion.
4. **Distance**, two estimates fused per eye by inverse variance:
   - *iris*: depth `Z = f · 11.7 mm / d`, `d` the iris contour's larger extent in px;
   - *separation*: depth `Z = f · 63 mm / |s3|`, `|s3|` the 3D distance between the iris centres in
     the mesh's own pixel scale — the mesh's relative depth corrects a yawed head, which a 2D
     separation would read as farther — plus each eye's mesh depth offset.
   - Variances (relative): `(0.5 px / measured px)²` + the population spread (iris 5 %, IPD 6 %)
     + for the separation `(0.25 · (1 − foreshortening))²`. Frontal at 60 cm the iris gets about
     half the weight; a turned head shifts it to the iris.
   - The depth becomes a distance along the eye's ray (`Z / dir.z`). Both estimates are multiplied
     by the host's per-user `viewer_iris_scale` (from `get_camera_pose`).
5. **One Euro filter** (Casiez, Roussel, Vogel, CHI 2012; implemented here) on each coordinate of
   the two camera-space eye points (`FILTER_MIN_CUTOFF` 1 Hz, `FILTER_BETA` 3 per m/s,
   `FILTER_D_CUTOFF` 1 Hz).
6. **Display space**: `eye = T + R · p` with the camera's pose as the runtime answers it
   (`host->get_camera_pose`: display-declared > user calibration > placement hint > the default
   prior, the top centre of the bezel), re-read every 2 s so a new calibration applies without a
   restart; its focal scale `k` is applied as `normalize(u_x, u_y, k·u_z)`. Head position = the
   eyes' midpoint; head orientation from the mesh (eye corners and the forehead–chin line).
7. **Push** one `xrt_tracking_viewer_sample` per analysed frame, `capture_ns` = the frame's
   capture time: `EYES_VALID | TRACKING | HEAD_POSE_VALID | CAMERA_VALID`, the confidence = the
   landmark presence. The camera-space block (camera key = AVCaptureDevice `uniqueID`, per-eye
   direction + distance) is the **raw** measurement of the frame — unfiltered and not scaled by
   the iris scale — because the runtime's camera-pose calibration solves against it. On loss: the
   last known eyes with `TRACKING` clear; before the first face nothing (the runtime ignores a
   sample without eyes).

## How the hands role works

Same binary, its own thread, started by the first joint locate (design §11). Per camera frame:

1. **Palm detection** (full, 192², letterboxed) only when fewer hands are followed than
   `MAX_HANDS`: every frame with none, every `DETECT_EVERY` frames with one.
2. **Hand landmarks** (full, 224²) per hand on a rotated crop: from the palm detection (wrist →
   middle knuckle upright, centre half a box toward the fingers, long side × 2.6), then from the
   previous frame's landmarks (tracking mode, long side × 2). Out: 21 landmarks, presence,
   handedness, and 21 metric "world" landmarks. Presence below `PRESENCE_MIN` drops the hand.
3. **Left or right**: the handedness output is P(the subject's right hand) on the un-mirrored
   image, averaged over the track; with two hands the more "right" one is right.
4. **Depth, an estimate from hand size**: the metric hand (× `HAND_SCALE`) is placed along the
   landmarks' pixel rays by linear least squares, each landmark then on its own ray at that depth
   (exact 2D reprojection). A hand 10 % larger than the model's average reads 10 % farther.
5. **Display space**: `T + R · p` with the camera's pose from the runtime (`get_camera_pose`; the
   top-centre-of-the-bezel prior until calibrated, the same answer the viewer role gets), then a
   One Euro filter per coordinate.
6. **26 joints** (`XR_EXT_hand_tracking` order): the 20 measured joints one to one; synthesised:
   each finger's metacarpal joint a quarter of the way wrist → knuckle, the palm at the middle of
   the middle metacarpal. -Z along each bone toward the fingertip, +Y out of the back of the hand,
   radii from a nominal adult hand scaled by this hand's size. Pinch: thumb tip to index tip below
   25 mm (released above 35 mm).
7. **Published** on two `XRT_DEVICE_TYPE_HAND_TRACKER` devices (`XRT_INPUT_HT_UNOBSTRUCTED_LEFT /
   RIGHT`) whose origin is `XRT_TRACKING_TYPE_RIG_LOCAL` (display-plane relative): the runtime
   anchors it to the rig, so hands follow WASD / mouse-look and never the viewer's head parallax.
   A locate gets the newest frame moved by the palm's velocity to the requested time (at most
   `PREDICT_MAX_MS`). `CONTROLLERS=1` publishes left / right `khr/simple_controller` devices
   instead (select = pinch, grip / aim = the palm), which then take the controller roles.

Presence is `PRESENT` while a usable camera exists (no hand in view = inactive joints).
`IDLE_S` seconds after the last locate the camera closes. The provider fills `struct xrt_device`
from the vendored header, which has no `struct_size`: a runtime that changes it needs a rebuild.

### Capture time

AVFoundation stamps each frame with its presentation time on the capture session's
synchronization clock. The provider converts it to the host time clock (`CMClockGetHostTimeClock`,
`mach_absolute_time` ticks, then ns through `mach_timebase_info`). That clock is
`CLOCK_UPTIME_RAW` and stops during system sleep, whereas the runtime's `os_monotonic_get_ns()` is
`clock_gettime(CLOCK_MONOTONIC)`, which on macOS keeps counting through sleep. Each frame is
therefore shifted by `CLOCK_MONOTONIC − CLOCK_UPTIME_RAW` read at arrival (the time slept since
boot). A stamp in the future or more than 1 s old falls back to the arrival time, with one log line.

### Lifetime

- `probe` opens nothing: `ENABLE`, the pack's model files by size, ONNX Runtime loads, a camera is
  enumerated, and the OS camera permission is not *denied* / *restricted* (a permission not asked
  yet is fine: it is asked at start).
- `start` returns at once. The provider's thread verifies the models' sha256, creates the
  sessions (Core ML, falling back to the CPU; a failed run also moves to the CPU), asks for the
  camera permission if macOS has not decided yet (`PERMISSION_TIMEOUT_S`), and opens the camera
  (retrying every 2 s while there is none).
- `get_status`: `UNKNOWN` while starting, `RUNNING`, `NO_SOURCE` for no camera / permission denied
  / models missing / no execution provider, `STOPPED`. The precise reason is in `describe().source`
  and the log (`[open-tracking] ...` on stderr, one line per change).
- `stop` joins the thread; nothing is pushed after it returns; the camera is closed.

### macOS camera permission

macOS asks the process it holds *responsible* for the one opening the camera, and reads the usage
string from that process's Info.plist:

- **In-process** (an app hosts the provider): the app. A binary started from a terminal is
  attributed to the terminal application, so `otr-probe` and test apps run from Terminal use
  Terminal's grant (macOS asks once: "Terminal would like to access the camera").
- **Under the service** the provider runs in `displayxr-tracking-host`. The runtime embeds a
  camera usage string in that executable (runtime PR `feat/tracking-host-camera-usage`), but
  today the host inherits the service's responsibility; spawning it with responsibility
  disclaimed is a runtime follow-up.
- Denied: `probe` declines and logs where to change it (System Settings → Privacy & Security →
  Camera).

## Measurements (Apple M1 Pro, macOS 26.3, ONNX Runtime 1.31.0)

Per analysed frame in tracking mode (no detector), from the provider's `STATS_S` log, the file
camera replaying a portrait (so the face never moves); load average 3–5 from unrelated work.

| | Core ML, Neural Engine | Core ML, GPU | CPU EP (2 threads, default) |
|---|---|---|---|
| landmarks incl. crop | 7.4 ms | 8.3 ms | 4.9 ms |
| capture → push | 8.0 ms | 9.0 ms | 5.0 ms |
| process CPU time | 0.10 s per s | 0.15 s per s | 0.49 s per s |
| sessions ready, first / cached | 612 / 74 ms | 430 ms / — | — |

The detector alone: 2–4 ms (Core ML), 3 ms (CPU).

**Hands role**, same machine, the file camera replaying the CC0 hand photo (one hand, so the palm
detector runs every 5th frame), from `STATS_S`:

| per analysed frame | CPU EP (2 threads) | Core ML (Neural Engine) |
|---|---|---|
| hand landmarks incl. crop, one hand | 12.1–12.5 ms | 6.3–6.4 ms |
| palm detector incl. letterbox (when it runs) | 14–15 ms | 6.8–7.2 ms |
| frame (average) | 14.9–15.6 ms | 7.6–7.9 ms |
| sessions ready | — | 419 ms (first compile) |

Two hands add one landmark run per frame (not measured: no two-hand fixture). Both models go to
Core ML in one partition each. Both execution providers take every node of both
models in one partition. The CPU execution provider is the default. On a live face held still at
50 cm, the Neural Engine's eye depth jitters 5–6 mm (standard deviation) against 0.4–0.7 mm on
the CPU, because the Neural Engine runs half precision; sideways and vertical jitter are
0.4–0.6 mm against 0.2–0.3 mm. `EP=coreml` (Neural Engine) costs about 5× less CPU and leaves
the GPU to the renderer, when that matters more than depth noise. Live-camera numbers: see the
pull request.

Camera latency. On a built-in laptop camera the frame reaches the provider 70–150 ms after its
capture stamp, while processing takes about 5 ms. The delay sits in the operating system's camera
stack, not in this provider. A bare capture session with no processing shows the same delay, and a
screen-flash test puts the stamp itself 35–80 ms after the light change, which is one frame plus
the display. The delay is a sawtooth with a period of about 20 s (buffering drifting against the
sensor rate). Locking the frame rate, a smaller preset or another pixel format does not remove it.
Each sample carries the capture stamp, so the runtime can predict across the delay. The `STATS_S`
log splits it: `stamp->arrival` (the camera stack), `arrival->process` (this provider's mailbox)
and the processing time.

## Model pack

`packs/open-tracking-default.models.json` (schema `displayxr-tracking-pack-2`, version 1.1.0)
pins the upstream bundles (MediaPipe `face_landmarker.task` and `hand_landmarker.task`, float16
v1, by URL + sha256) and the sha256 + size of each derived ONNX blob. **The ONNX bytes are reproducible**: `scripts/fetch_models.py` downloads the bundles, checks
them, extracts the four `.tflite` models (checked), converts them with tf2onnx (opset 17), then
`scripts/mp_onnx.py` fixes every symbolic dimension to 1 (what
`onnxruntime.tools.make_dynamic_shape_fixed` does, for whatever name tf2onnx gave the batch
dimension), applies the Core ML graph fix (explicit zero `pads` on Conv / MaxPool, PRelu slopes
`[1,C,1,1]` → `[C,1,1]`, an explicit Pad value; numerically neutral), and renames tensors
canonically (tf2onnx's names depend on hash order, so two runs otherwise differ byte-wise). With
the converter versions in `scripts/requirements-convert.txt` (also recorded in the pack, and
checked), two runs give identical bytes; CI re-proves it on macOS. No blob is committed or hosted.

```
python3.11 -m venv .venv && .venv/bin/pip install -r scripts/requirements-convert.txt
.venv/bin/python -s -P scripts/fetch_models.py packs/open-tracking-default.models.json models-cache \
    --test-images models-cache/images
```

`models-cache/` then holds `face_detector.onnx`, `face_landmarks.onnx`, `hand_detector.onnx`,
`hand_landmarks.onnx` and the pack — copy them next to the provider (or point `MODEL_DIR` at it).
Each role requires only its own pair (`ot_pack_require`). `--update-pack` rewrites the pins after a
deliberate converter or model change. `scripts/check_pack.py` validates a pack
(`--verify-dir DIR` also hashes the blobs).

## Build and test

Plain CMake and a C11 compiler (Objective-C for the macOS capture). The build fetches ONNX
Runtime 1.31.0 as a pinned, SHA-256-checked release archive (`cmake/OnnxRuntime.cmake`:
osx-arm64, linux-x64, win-x64) and places it next to the provider; it is loaded at run time by
absolute path through `OrtGetApiBase`, never linked.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENTRACK_TEST_MODELS_DIR=$PWD/models-cache
cmake --build build
ctest --test-dir build --output-on-failure
```

| CTest | What it proves | Needs models |
|---|---|---|
| `unit-one-euro` | the filter: pass-through, the first-order step, convergence, less lag with β | no |
| `unit-pixel-dir` | focal length from HFOV, the axis, the edge at HFOV/2, the user's left = −X | no |
| `unit-distance` | synthetic faces at 0.4 / 0.6 / 1.0 m: both estimates within 1 %; a 30° yaw: the separation corrected, the head yaw recovered | no |
| `unit-roi` | the ROI affine, the letterbox, ROI rotation from the eye corners, the sampler (BGRA = RGB, mirroring, padding) | no |
| `unit-decode` | anchors, box / keypoint decoding, sigmoid, NMS, ROI from a detection | no |
| `unit-viewer` | display = T + R·p, the iris scale on the display eyes but not on the raw camera block, a pitched camera, loss | no |
| `unit-pack` | manifest parsing (and refusals); the repository's pack | no |
| `negotiate`, `decline-*` | ABI gate, declared settings; probe declines cleanly (ENABLE=0, no models, no camera) | no |
| `blank`, `face`, `face-coreml` | the fake host drives probe / describe / start / status / stop with the **file camera**: a grey frame (running, nothing pushed, restartable), a public-domain portrait (tracked samples, plausible eyes, the camera block, increasing `capture_ns`, nothing after stop) | yes |
| `unit-hand-decode`, `unit-hand-roi` | palm anchors + decoding + NMS; the palm and landmark ROIs (rotation, shift toward the fingers, size) | no |
| `unit-hand-place` | the monocular placement recovers a known hand position to 1 mm and reprojects exactly; depth scales with the size prior; crop → camera axes | no |
| `unit-hand-chirality`, `unit-hand-joints` | handedness from the 3D shape; the 21 → 26 mapping, -Z along bones, +Y out of the back, a right-handed basis, radii, pinch hysteresis | no |
| `unit-hands`, `unit-capture-share` | side assignment, display = T + R·p (incl. a pitched camera), velocity, loss; one shared stream per device (file camera) | no |
| `hands-negotiate`, `hands-decline-*` | the input ABI gate, settings, presence UNKNOWN before probe; probe declines cleanly | no |
| `hands-blank`, `hands`, `hands-mirror`, `hands-coreml` | through the input ABI with the file camera: two hand trackers on a RIG_LOCAL origin, PRESENT, nothing active on a grey frame; on a CC0 left-hand photo only the left hand, plausible metric joints, thumb side, bone axes (mirrored: the right hand) | yes |
| `check-pack` | the pack validator | no |

**`otr-probe`** runs the provider on a real camera and prints, every second, samples/s,
tracking share, capture → push latency, the eyes in display space, the raw distances and the eye
separation, then the jitter (standard deviation of each eye over the last `--still` seconds) and
the provider's per-stage timings (stderr):

```
build/otr-probe --seconds 15 --still 3 --set MODEL_DIR=$PWD/models-cache [--set EP=cpu] [--set HFOV_DEG=..]
build/otr-probe --hands --seconds 15 --still 3 --set MODEL_DIR=$PWD/models-cache
```

`--hands` drives the hands role instead: it locates both hands' joints at 30 Hz through the
devices, as an app would, and prints each hand's share of active locates, palm and wrist in
display space, size and pinch distance, then the palm jitter over the last `--still` seconds.

## Settings

Read only through `host->get_setting` (`DXR_TRACKING_OPEN_TRACKING_<KEY>` in the hosting
process, then the per-user store, then the manifest's `settings`); `displayxr-cli tracking options
open-tracking` lists them.

| Key | Default | |
|---|---|---|
| `ENABLE` | 1 | 0: probe declines. On by default, unlike the simulated providers: installing a real provider is the opt-in, the runtime's own `enabled` switch and per-screen arbitration decide the rest, and the camera opens only when a viewer is needed |
| `DEVICE` | built-in | index, a substring of the name, or `file:<ppm or .txt list>` (tests) |
| `WIDTH`, `HEIGHT`, `FPS` | 1280, 720, 30 | the best mode at or below |
| `HFOV_DEG` | 70 | the camera's horizontal field of view; the distance scales with it. Measure it once: at distance D, the visible width W gives `2·atan(W / 2D)` |
| `MIRROR` | 0 | the camera mirrors its image |
| `EP` | cpu | `cpu`, `coreml` (macOS; Neural Engine by default, see `COREML_UNITS`) |
| `COREML_UNITS` | ane | `ane`, `gpu`, `all`, `cpu` |
| `COREML_CACHE`, `CACHE_DIR` | 1, user cache | compiled Core ML models under `~/Library/Caches/DisplayXR/OpenTracking/coreml-ort-<v>/<blob>` |
| `MODEL_DIR`, `ONNXRUNTIME` | next to the binary | |
| `THREADS` | 2 | CPU intra-op threads |
| `DETECT_EVERY`, `DETECT_MIN`, `PRESENCE_MIN` | 0, 0.5, 0.5 | |
| `IRIS_MM`, `IPD_MM` | 11.7, 63 | population priors |
| `FILTER_MIN_CUTOFF`, `FILTER_BETA`, `FILTER_D_CUTOFF` | 1.0, 3.0, 1.0 | One Euro |
| `PERMISSION_TIMEOUT_S` | 120 | |
| `STATS_S` | 0 | > 0: a timing / diagnostics log line every N s |

**Hands role** (`open-tracking-hands`: `DXR_INPUT_OPEN_TRACKING_HANDS_<KEY>`, the per-user store,
the manifest; `displayxr-cli input options open-tracking-hands`). `DEVICE`, `WIDTH`, `HEIGHT`,
`FPS`, `HFOV_DEG`, `MIRROR`, `EP`, `COREML_UNITS`, `COREML_CACHE`, `CACHE_DIR`, `MODEL_DIR`,
`ONNXRUNTIME`, `THREADS`, `ORT_LOG`, `FILTER_*`, `PERMISSION_TIMEOUT_S`, `STATS_S` as above (give
`DEVICE` and `HFOV_DEG` the viewer role's values: one camera, one stream), plus:

| Key | Default | |
|---|---|---|
| `ENABLE` | 1 | 0: probe declines. The camera opens only while an app locates joints |
| `CONTROLLERS` | 0 | 1: left / right `khr/simple_controller` devices (select = pinch, grip / aim = the palm) that take the controller roles while a camera is present; 0: hand joints only |
| `MAX_HANDS` | 2 | 1 or 2 |
| `HAND_SCALE` | 1.0 | the user's hand size relative to the model's average; the depth scales with it |
| `PINCH_ON_MM`, `PINCH_OFF_MM` | 25, 35 | |
| `DETECT_MIN`, `PRESENCE_MIN`, `DETECT_EVERY` | 0.5, 0.5, 5 | palm detector threshold; landmark presence; with one hand followed, look for the other every N frames |
| `PREDICT_MAX_MS` | 50 | how far past the newest frame a locate extrapolates |
| `IDLE_S` | 5 | close the camera this long after the last locate; 0 = never |

## Registration

POSIX: `NNN-open-tracking-tracking-provider.json` in a plug-in root
(`examples/manifest/100-open-tracking-tracking-provider.json.in`, ProbeOrder 100 — ahead of the
simulated provider at 200), and for the hands
`examples/manifest/100-open-tracking-hands-input-provider.json.in` (`NNN-open-tracking-hands-input-provider.json`,
the same binary). Development: put it in a directory with the runtime's sim plug-in
manifests and point `XRT_PLUGIN_SEARCH_PATH` at it; `displayxr-cli status --json` then reports
`viewer_source: "provider:open-tracking"` for a simulated display. Windows:
`HKLM\Software\DisplayXR\TrackingProviders\open-tracking` and
`HKLM\Software\DisplayXR\InputProviders\open-tracking-hands` (`Binary`, `ProbeOrder`, `Settings`)
— once the Windows capture exists.

The install tree (`cmake --install build`, with `-DOPENTRACK_INSTALL_MODELS_DIR=models-cache` to
include the blobs) is flat: the provider, `libonnxruntime`, the pack, the four models, `licenses/`.

## Follow-ups

- Windows capture (Media Foundation; DirectML build of ONNX Runtime) and Linux capture (V4L2).
- Hands: a live-camera test (incl. two hands), a per-user hand-size calibration, the display's
  shared stereo camera as a source, a proper thumb frame; under the service the two roles run in
  different processes (design §11.7).
- Installers (`DisplayXROpenTrackingSetup-*.exe`, `.pkg`, `.deb`), the meta-installer's "Tracking
  providers" section, `versions.json` `tracking_open`, signing / the Windows allowlist row.
- Runtime: spawn `displayxr-tracking-host` with responsibility disclaimed on macOS.
- Camera intrinsics: a per-camera HFOV / distortion table, or a calibration step.

## The ABI it builds against

`include/xrt/` is vendored **verbatim** from the runtime at `v2.53.0-17-ge5d7ffa57`
(`e5d7ffa576833cded4b409105ca7c58b3a62466a`): `xrt_tracking_plugin.h` (viewer-tracking ABI major
**1**, with the `get_camera_pose` host slot), `xrt_input_plugin.h` (input-provider ABI major **1**,
with its `get_setting`, `get_display_geometry` and `get_camera_pose` host slots), `xrt_device.h`
with what it includes (`xrt_defines.h`, `xrt_tracking.h`, `xrt_plane_detector.h`,
`xrt_visibility_mask.h`, `xrt_limits.h`, `xrt_compiler.h`, `xrt_windows.h`, `xrt_config_os.h`),
`xrt_camera_pose.h`, `xrt_component_settings.h`, `xrt_results.h`. Update by copying from a runtime
release; never edit in place. `struct xrt_device` is not versioned by `struct_size`: re-vendor and
rebuild when the runtime changes it.

## License

Boost Software License 1.0 (`LICENSE`). Third-party components and model licences:
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
