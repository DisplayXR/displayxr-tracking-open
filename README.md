# DisplayXR open camera tracking provider

`DisplayXR-OpenTracking` is a [DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime)
component that turns an ordinary camera into **viewer tracking** (head and eyes) and **hand
tracking** (`XR_EXT_hand_tracking` joints), with open models only. It is what a DisplayXR box gets when its display has no
tracker of its own: a laptop webcam is enough for head-coupled parallax.

> **Status: the viewer role and the hands role work on macOS** (AVFoundation capture, ONNX
> Runtime with Core ML). The hands role also reads the display's shared stereo camera where the
> runtime offers it (any OS, in the service; verified against the fake host, not yet on a panel).
> Windows (Media Foundation) and Linux (V4L2) webcam capture and the installers are follow-ups;
> on those OSes the viewer role declines in `probe`, and the hands role does too unless the
> runtime offers the shared camera. Design:
> [`docs/design.md`](docs/design.md) (hands: §11).

| Role | Runtime class | Registry id | Status |
|---|---|---|---|
| Head + eye positions | viewer-tracking provider (ABI 1) | `open-tracking` | macOS |
| Hand joints (26 per hand), pinch | input provider (ADR-034, input ABI 1) | `open-tracking-hands` | macOS (webcam); any OS whose runtime shares the display's stereo camera (code-complete, not yet run on a panel) |

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

Same binary, its own thread, started by the first joint locate (design §11).

**Source** (`SOURCE`, default `auto`): the display's own **shared stereo camera** when the runtime
offers it, else a webcam. The shared camera comes through the input host's public slots
(`open_shared_camera` / `camera_acquire` / `camera_release` / `close_shared_camera`, runtime
input-provider spec §4d, ADR-043 Amendment 6): the runtime keeps the camera, applies its consent,
sharing toggle, session lock and in-use indicator, and hands out RECTIFIED side-by-side frames (left
eye = left half, never mirrored, usually 8-bit grey) with their rectified calibration (one focal
length and principal point for both eyes, the right eye one baseline along the left eye's image
+x). Only an input provider loaded **in the service** gets it: an in-process runtime has no camera
manager, the open fails, and `auto` falls back to a webcam (on Windows and Linux, which have no
webcam backend yet, the hands then stay ABSENT). While a webcam serves, the shared camera is tried
again every 30 s; a refused consent is not asked again in that process.

On the shared camera:

- **One eye for the models.** The palm detector and the landmark model run on the **left** eye's
  image, its grey replicated into R = G = B. Both models were trained on colour photographs, and a
  tracker camera is often infrared-lit: grey skin has less contrast against the background than
  colour, so detection is expected to be weaker than on a webcam (fewer palms found at a distance,
  lower presence). On the CC0 hand photo turned grey, detection, handedness (P(right) 0.09) and the
  joints are unchanged. An earlier spike on a real tracker camera (design §3) found the landmark
  model holds on grey once seeded but the palm detector misses about half the hands, so on this
  source the detector runs at `SHARED_DETECT_MIN` (0.3) and the landmark presence confirms. How
  it fares live is what the panel test measures.
- **The pair measures the hand's size** (`STEREO=1`). Per analysed frame, each hand's palm points
  (wrist, the thumb's base, the four knuckles) are matched along the same row of the right eye
  (normalised cross-correlation, sub-pixel), giving each a stereo depth `f·B/d`; the median of
  stereo depth / placed depth is folded into the hand-size factor (10 % of the way per frame, in
  log space, clamped 0.6–1.6; `HAND_SCALE` is where it starts). So the depth's scale comes from the
  pair while the frame-to-frame motion stays the monocular fit's, which is smooth and reprojects
  exactly. This is simpler and steadier than triangulating every landmark (fingers against the
  background, motion blur and a weak grey texture make per-landmark disparities noisy), and it
  removes the hand-size prior's 2.5–4 cm depth bias at 40 cm. `STEREO=0` uses one eye only.
- **Pose**: the intrinsics are the rectified calibration's (not `HFOV_DEG`). The camera's place on
  the panel is asked of the runtime (`get_camera_pose`, keyed by the camera's persistent id), whose
  answer names the pair's centre; the left eye is half a baseline toward the camera frame's +X.
  Until the runtime carries the display's declared camera pose (runtime issue #2296), that answer is
  the top-centre-of-the-bezel prior with no tilt: a tracker camera pitched toward the face then
  rotates the hands about the camera by that pitch.
- **Presence follows the stream** (input-provider spec §4d.2): PRESENT while frames flow; ABSENT
  while the display delivers frames only during a 3D session and none holds its tracker, while the
  stream is suspended (session locked, sharing off) or after it ended. ABSENT for such a reason
  turns PRESENT again 30 s later, so the next app's joint locate retries.

Per camera frame:

1. **Palm detection** (full, 192², letterboxed) only when fewer hands are followed than
   `MAX_HANDS`: every frame with none, every `DETECT_EVERY` frames with one.
2. **Hand landmarks** (full, 224²) per hand on a rotated crop: from the palm detection (wrist →
   middle knuckle upright, centre half a box toward the fingers, long side × 2.6), then from the
   previous frame's landmarks (tracking mode, long side × 2). Out: 21 landmarks, presence,
   handedness, and 21 metric "world" landmarks. Presence below `PRESENCE_MIN` drops the hand.
3. **Left or right**: the handedness output is P(the subject's right hand) on the un-mirrored
   image, averaged over the track; with two hands the more "right" one is right. A hand keeps its
   side until its evidence clearly says otherwise (P(right) past 0.25 / 0.75).
4. **Depth, an estimate from hand size**: the metric hand (× `HAND_SCALE`) is placed along the
   landmarks' pixel rays by linear least squares, each landmark then on its own ray at that depth
   (exact 2D reprojection). A hand 10 % larger than the model's average reads 10 % farther.
5. **Display space**: `T + R · p` with the camera's pose from the runtime (`get_camera_pose`; the
   top-centre-of-the-bezel prior until calibrated, the same answer the viewer role gets), then a
   One Euro filter per coordinate.
6. **26 joints** (`XR_EXT_hand_tracking` order): the 20 measured joints one to one; synthesised:
   each finger's metacarpal joint a quarter of the way wrist → knuckle, the palm at the middle of
   the middle metacarpal. -Z along each bone toward the fingertip, +Y out of the back of the hand
   (the thumb's turned toward the index side), radii from a nominal adult hand scaled by this hand's size. Pinch: thumb tip to index tip below
   25 mm (released above 35 mm).
7. **Published** on two `XRT_DEVICE_TYPE_HAND_TRACKER` devices (`XRT_INPUT_HT_UNOBSTRUCTED_LEFT /
   RIGHT`) whose origin is `XRT_TRACKING_TYPE_RIG_LOCAL` (display-plane relative): the runtime
   anchors it to the rig, so hands follow WASD / mouse-look and never the viewer's head parallax.
   A locate gets the newest frame moved by the palm's velocity to the requested time (at most
   `PREDICT_MAX_MS`). `CONTROLLERS=1` publishes left / right `khr/simple_controller` devices
   instead (select = pinch, grip / aim = the palm), which then take the controller roles.

Presence is `PRESENT` while a usable camera exists (no hand in view = inactive joints); on the
shared camera it follows the stream (above).
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
models in one partition. For hands, Core ML is the default on macOS: on a live hand held still at
about 40 cm, the steadiest 5 s window spread 1.6 / 0.3 / 2.3 mm (x / y / z, standard deviation)
on Core ML against 7.7 / 1.5 / 6.7 mm on the CPU, and capture to publish fell from about 70 ms to
58 ms. The viewer role keeps the CPU execution provider as its default: on a live face held still at
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
Runtime 1.31.0 as a pinned, SHA-256-checked package (`cmake/OnnxRuntime.cmake`: Microsoft's
`Microsoft.ML.OnnxRuntime` NuGet package, which carries osx-arm64, linux-x64 and win-x64) and
places the library next to the provider; it is loaded at run time by
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
| `unit-stereo` | disparity of a shifted texture to 0.1 px, no match outside the range / without texture / off the image, the palm ratio, the hand-size factor settling and its clamp, a grey frame sampled as R = G = B | no |
| `hands-shared`, `hands-shared-fallback`, `hands-shared-waiting`, `hands-decline-shared` | the fake host's shared stereo camera (the hand photo as a rectified grey pair, the right eye shifted to put the plane at 0.155 m): the left hand through grey frames, its palm at the pair's depth (the prior alone says 0.18 m); a runtime whose open fails falls back to the webcam; a stream that never delivers leaves no hand and ABSENT; `SOURCE=shared` without the slots declines | yes (not `hands-decline-shared`) |
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
`DEVICE` and `HFOV_DEG` the viewer role's values: one camera, one stream), except that `EP`
defaults to `coreml` on macOS (`cpu` elsewhere), plus:

| Key | Default | |
|---|---|---|
| `ENABLE` | 1 | 0: probe declines. The camera opens only while an app locates joints |
| `CONTROLLERS` | 0 | 1: left / right `khr/simple_controller` devices (select = pinch, grip / aim = the palm) that take a controller role when that hand pinches and give it back when the keyboard and mouse are used (ADR-034 Amendment 6: runtime PR #2460, in no release yet, so the slot is compiled in only once `include/xrt` is regenerated from a runtime that has it; until then the runtime gives them the roles whenever a camera is present). While an app runs, the camera stays open to see that pinch; 0: hand joints only |
| `MAX_HANDS` | 2 | 1 or 2 |
| `HAND_SCALE` | 1.0 | the user's hand size relative to the model's average; the depth scales with it |
| `MAX_DISTANCE_MM` | 800 | a hand whose wrist is farther from the camera is not the user's (someone behind them) and never takes a track; 0 = any distance |
| `PINCH_ON_MM`, `PINCH_OFF_MM` | 25, 35 | |
| `DETECT_MIN`, `PRESENCE_MIN`, `DETECT_EVERY` | 0.5, 0.5, 5 | palm detector threshold; landmark presence; with one hand followed, look for the other every N frames |
| `PREDICT_MAX_MS` | 50 | how far past the newest frame a locate extrapolates |
| `IDLE_S` | 5 | close the camera this long after the last locate; 0 = never |
| `SOURCE` | auto | `auto`: the display's shared stereo camera when the runtime opens it, else a webcam; `shared`: only the display's camera (probe declines on a runtime without the slots); `webcam`: never the display's camera |
| `STEREO` | 1 | shared camera: the pair measures the hand-size factor (`HAND_SCALE` is its start); 0 = one eye only |
| `SHARED_FPS` | 30 | shared camera: delivery cap in Hz (0 = the camera's rate) |
| `SHARED_DETECT_MIN` | 0.3 | shared camera: the palm detector's threshold on its grey frames, lower than `DETECT_MIN` because the detector loses about half the hands on grey (design §3); the landmark model's presence (`PRESENCE_MIN`) still has to confirm each one |
| `LOG_FRAMES` | 0 | 1: one log line per analysed frame: whether the detector ran, palms found and the best score, per hand its track, P(right), presence, the middle knuckle and the box in the image (normalised, `edge` when it touches the border), per side the published palm. For coverage tests; it is a lot of log |
| `LOG_FILE` | | also append the log lines to this file (a provider in the service has no console); frames are never written |

## Registration

POSIX: `NNN-open-tracking-tracking-provider.json` in a plug-in root
(`examples/manifest/100-open-tracking-tracking-provider.json.in`, ProbeOrder 100 — ahead of the
simulated provider at 200), and for the hands
`examples/manifest/100-open-tracking-hands-input-provider.json.in` (`NNN-open-tracking-hands-input-provider.json`,
the same binary). Development: put it in a directory with the runtime's sim plug-in
manifests and point `XRT_PLUGIN_SEARCH_PATH` at it; `displayxr-cli status --json` then reports
`viewer_source: "provider:open-tracking"` for a simulated display. Windows:
`HKLM\Software\DisplayXR\TrackingProviders\open-tracking` and
`HKLM\Software\DisplayXR\InputProviders\open-tracking-hands` (`Binary`, `ProbeOrder`, `Settings`).
The viewer role needs the Windows capture first; the hands role works there today through the
display's shared camera (a dev build: the runtime's `scripts\register_dev_plugin.bat input
<path>\DisplayXR-OpenTracking.dll open-tracking-hands` writes the row and allowlists the DLL for the
provenance gate, which also lets it open the shared camera without a consent prompt).

The install tree (`cmake --install build`, with `-DOPENTRACK_INSTALL_MODELS_DIR=models-cache` to
include the blobs) is flat: the provider, `libonnxruntime`, the pack, the four models, `licenses/`.

### macOS installer

`DisplayXR-OpenTracking-X.Y.Z.pkg` (a release asset; built by `.github/workflows/release.yml` on a
`vX.Y.Z` tag, Developer ID signed + notarized) installs the manifest into the runtime's system
discovery root and the flat tree beside the runtime's plug-ins:

```
/Library/Application Support/DisplayXR/DisplayProcessors/100-open-tracking-tracking-provider.json
/Library/Application Support/DisplayXR/lib/displayxr/plugins/open-tracking/
```

Root-owned under `/Library/Application Support/DisplayXR`, so the installed runtime treats it as
platform-trusted: no `XRT_PLUGIN_SEARCH_PATH`, no `DXR_ALLOW_DEV_PLUGIN_PATHS`. It refuses to install
over a runtime older than `MIN_RUNTIME` (the first whose tracking host may open the camera), and
restarts a running DisplayXR service afterwards (the service looks for a provider only when it
starts; the uninstaller restarts it too). Then:
`displayxr-cli status` (viewer source `provider:open-tracking`), allow the camera when macOS asks
(the prompt names DisplayXR's tracking host), and `displayxr-cli tracking calibrate` for the camera
pose (stored in `~/.config/monado/camera-calibration.json`). Remove with
`sudo "/Library/Application Support/DisplayXR/lib/displayxr/plugins/open-tracking/uninstall.sh"`.
Build one locally: `scripts/package_macos.sh <install-tree> <version> <out.pkg>` with `SIGN`
pointing at the runtime's `scripts/macos_sign_notarize.sh` (ad-hoc without signing identities).

## Follow-ups

- Windows capture (Media Foundation; DirectML build of ONNX Runtime) and Linux capture (V4L2).
  Deferred on purpose: a webcam backend must never open a display's own tracker camera (ADR-043
  Amendment 5; open/close cycles have wedged one), and the runtime's list of claimed cameras is not
  exposed to providers. The safe order is a runtime host slot that answers "may I open this
  device?" first, then the backends. On Windows the hands come from the shared camera meanwhile.
- Hands: a live-camera test (incl. two hands), a per-user hand-size calibration on a webcam (the
  shared camera measures it), a proper thumb frame; under the service the two roles run in
  different processes (design §11.7).
- Hands on the shared camera: the panel test (does a tracker camera aimed at the face see hands at
  all, and does grey input detect them); the display's declared camera pose (runtime #2296) instead
  of the bezel prior; per-landmark stereo if the scale-only fold proves too coarse.
- Installers (`DisplayXROpenTrackingSetup-*.exe`, `.deb`; the macOS `.pkg` ships), the meta-installer's "Tracking
  providers" section, `versions.json` `tracking_open`, signing / the Windows allowlist row.
- Camera intrinsics: a per-camera HFOV / distortion table, or a calibration step.

## The ABI it builds against

`include/xrt/` is generated from the runtime's headers at release `v2.70.8`
(`1adbbc577f67925904bc48f25b51eaef1a4beefc`): `xrt_tracking_plugin.h` (viewer-tracking ABI major
**1**, with the `get_camera_pose` host slot), `xrt_input_plugin.h` (input-provider ABI major **1**,
with its `get_setting`, `get_display_geometry`, `get_camera_pose` and shared-camera host slots),
`xrt_device.h` with what it includes (`xrt_defines.h`, `xrt_tracking.h`, `xrt_plane_detector.h`,
`xrt_visibility_mask.h`, `xrt_limits.h`, `xrt_compiler.h`, `xrt_windows.h`, `xrt_config_os.h`),
`xrt_stereo_camera.h` + `xrt_handles.h` and `xrt_component_license.h` (included by the plug-in headers),
`xrt_camera_pose.h`, `xrt_component_settings.h`, `xrt_results.h`. The copies are vendor-neutral:
`scripts/neutralize_headers.py` drops the runtime's hardware-specific device, input and output
names (controllers, trackers, a stylus) and its vendor-extension face and body types, and writes out the values of the implicitly
counted enums so nothing the provider exchanges with the runtime moves. CI checks both halves:
`neutralize_headers.py --check include/xrt` (no vendor names) and `scripts/abi_check.py` against the
runtime's headers at the pinned commit (every enumerator value, every struct size, every
`struct xrt_device` offset). To update, regenerate from a runtime release, never edit by hand:

```sh
python3 scripts/neutralize_headers.py <runtime>/src/xrt/include/xrt include/xrt
python3 scripts/abi_check.py <runtime>/src/xrt/include
```

and move `RUNTIME_SHA` in `.github/workflows/build.yml` and the commit above. `struct xrt_device`
is not versioned by `struct_size`: regenerate and rebuild when the runtime changes it.

## License

Boost Software License 1.0 (`LICENSE`). Third-party components and model licences:
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
