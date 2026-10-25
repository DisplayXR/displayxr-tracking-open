# Design: open camera tracking provider

Status: **design, 2026-10-22; viewer role implemented on macOS, 2026-10-24; hands role
implemented (webcam path, macOS capture), 2026-10-25, §11** (how it works, measurements and
settings: `README.md`). Source of truth for the contracts it implements is the runtime
repository (ADR-054 component model, ADR-034 input providers, ADR-043 stereo camera source,
`docs/specs/runtime/input-provider-discovery.md`, and the viewer-tracking provider spec that
ADR-054 N6 Part 2, runtime #2144, introduces).

## 1. What it is

One out-of-process component that owns at most one camera stream and publishes two things:

1. **Viewer pose** — head position and rotation, plus the left/right eye positions, in display
   space, timestamped at capture, for the runtime's viewer-tracking arbitration.
2. **Hand joints** — `XR_EXT_hand_tracking` joint sets for both hands, plus pinch, for the
   input-provider path.

It registers as two components, so each role is enabled, preferred and shown separately:
`TrackingProviders\open-tracking` and `InputProviders\open-tracking-hands`. Both rows point at the
same binary; the runtime starts one process, which serves whichever roles are wanted.

Why one process: both roles need the same frames. Two processes would contend for one camera and
light two privacy indicators. **As built (§11.1):** both entry points live in one binary, and
within a process the capture layer keeps one stream per device that both roles subscribe to.
Whether the runtime puts both roles in one process is the runtime's hosting decision: today an
in-process app loads both into itself (one stream), while under the service the viewer role runs
in `displayxr-tracking-host` and input providers in `displayxr-service`, so each opens its own
stream on the same device (macOS allows concurrent capture from two processes; Windows Media
Foundation generally does not) — see §11.7.

## 2. Who wins

### Viewer (per screen)

The runtime selects, per screen: **the display processor's own eyes, if it weaves from tracked
eyes** → **the best enabled viewer-tracking provider** → **the nominal viewer / fly camera**.

- A display that tracks always wins on its screen, including while its tracker has lost the
  face. Weave and rendered views must come from the same eyes; the runtime never splices a second
  source into a DP-tracked screen.
- On such a screen the viewer role is **standby**: enabled, outranked, no camera opened.
- A display may also declare it must never be viewer-coupled (an N-view panel whose optics supply
  the parallax). The runtime then leaves its views fixed whatever providers are installed. See §4.

### Hands

No display processor provides hands today. Among hand sources the existing rules apply
(presence = last-used input wins, then `Preferred`), and a dedicated hand sensor outranks camera
hands by ProbeOrder. A vendor that later ships its own hand provider sets `Preferred` in its
installer; the user can change it.

## 3. Camera source

Chosen per machine, in order:

1. **The display's shared stereo camera** (ADR-043): when the active display plug-in exposes its
   tracker camera through the runtime's stereo camera source, the hands role consumes it as a
   camera consumer — no second device, no new privacy indicator, and calibrated stereo pairs give
   triangulated metric hand points. Measured on one panel (displayxr-tracking-open#1): 640×480
   per eye, GRAY8, rectified side-by-side, about 56–60 Hz, HFOV 64° per eye, baseline 120 mm.
   Hands raised **in front of the screen** are fully in frame; keyboard-level hands are outside it
   by geometry (about 40° below the axis against a ±25° vertical field), so they need path (2).
   Model spike: the hand-landmark model holds on greyscale once seeded, but the palm detector
   loses about half the hands, so re-detection needs a seed (a face-anchored region, or a lower
   detector threshold confirmed by landmark presence).
   **Scope:** the shared stream carries frames only while the display tracks a viewer on that
   screen (a 3D session there). A camera consumer does not wake the tracker; the source sits in
   `WAITING` with no frames otherwise. The hands role therefore works on this source only during
   a 3D session on that screen, unless the display vendor adds a camera-only wake or the
   provider can register as a tracking party — both outside this repo.
2. **A plain colour webcam** the runtime's tracker-camera denylist allows (ADR-043 Amendment 5).
3. **None** → the role reports unavailable.

The viewer role uses only (2): it is only ever active on a screen whose display does not track,
and such a display has no shared tracker stream.

Streams open **on demand** — the viewer role when a session is running on a screen it serves, the
hands role when an app locates hand joints (the input-provider ABI has no "tracker created"
signal, so the first `get_hand_tracking` is the demand; §11.5) — and close when demand ends. Hold **one** stream
per demand window (each stream start raises the runtime's camera-in-use notice). On a shared
display camera the role must tolerate `WAITING` with no frames and report it (§5).

## 4. What the display advertises (`XR_DXR_display_info`)

The display plug-in reports only what **it** does. A display with no tracker stays honest:
`supported_eye_tracking_modes = 0`, no `HAS_TRACKING` mode flag. What the application sees is
composed by the runtime:

```
advertised tracking(screen) = DP tracks
                           OR (DP accepts an external viewer AND a viewer-tracking provider is
                               installed, enabled and has a usable camera at system creation)
```

- **Static per instance.** Capabilities are fixed when the system is created, as the extension
  already specifies; a provider installed, enabled or disabled later takes effect at the next
  instance (the same rule as `displayxr-cli dp set`).
- **Live state rides the existing event.** "Supported" is not "tracking now": no face, camera
  unplugged or provider stopped → `is_tracking` false and `XrEventDataEyeTrackingStateChangedDXR`,
  exactly the edges the simulated display's `TRACKING_PERIOD_MS` already exercises.
- **Mode flags too.** The runtime sets `HAS_TRACKING` on the display's 3D modes when the composed
  answer is yes, and reports `MANAGED` (plus `MANUAL`) as supported.
- **The display must say whether it accepts an external viewer.** A new append-only plug-in slot
  (ADR-020, `struct_size`-gated) declares the viewer policy: `OWN` (it tracks), `EXTERNAL_OK`
  (views may follow a provider's viewer), `FIXED` (never viewer-coupled). A plug-in without the
  slot is treated as `FIXED`, so no installed display changes behaviour. The simulated display
  declares `EXTERNAL_OK`; its built-in webcam tracker moves out to the in-tree `sim_tracking`
  reference provider.

## 5. Dashboard and status

Standby is not disabled: the user's `Enabled` stays as set, and the reason is shown, the same
way the display status snapshot reports warnings as data. Two distinct reasons: **standby**
(outranked: the display tracks this screen) and **source paused** (the shared display camera
has no frames: no 3D session on this screen).

Per-screen selection reads the **segment's** display processor for that screen (each screen may
have its own), not the primary's, and re-evaluates on hotplug.

```
Tracking providers
  Open tracking        Standby on screen A — "the display tracks the viewer on this screen"
                       Active on screen B — webcam "<name>", tracking
Input providers
  Open tracking: hands Active — source: display stereo camera (shared)   Options ▸ camera
Cameras (in use)
  Display stereo camera   in use by: Open tracking (hands), <other consumers>
```

`displayxr-cli status --json` carries the same per-screen facts — viewer source, hands source,
why — so the Dashboard and MCP read one snapshot.

## 6. Privacy

- Camera access is the runtime's camera consent model for shared display cameras (per-executable
  consent, in-use indicator, both kill switches) and the OS camera permission for webcams (the
  macOS usage string lives on this provider's executable).
- Frames never leave the process; only poses and joints are published. No frame is written to
  disk.
- A service-started provider has no user to answer a per-executable consent prompt (60 s
  timeout on Windows). The installer pre-consents the registered provider executable (as the
  runtime installer allowlists the simulated display), and the Dashboard shows and revokes it.

## 7. Inference

- MediaPipe face landmarker (478 points incl. irises), palm detector and hand landmarker
  (Apache-2.0), converted to ONNX; ONNX Runtime loaded by absolute path at run time (the same
  pattern as the open conversion provider), DirectML / Core ML / CPU.
- Re-implemented around the models: region-of-interest tracking from the previous frame's
  landmarks, detection only on loss, left/right hand association, temporal filtering.
- Metric head distance from iris diameter (about 11.7 mm across people) and the camera's field of
  view; camera placement assumed top-centre of the bezel, corrected by a one-step calibration.
- Hands from a webcam are published **display-plane relative** (`RIG_LOCAL`, the frame the
  runtime's scripted hands use), mapped through the camera's pose: the runtime's
  `get_camera_pose` answer, which is the top-centre-of-the-bezel prior until the user calibrates
  (§10) and the calibrated pose after. Metric, with no anchor or gain; the depth is a hand-size
  estimate (§11.3). (This replaces an earlier "anchor + gain" plan.)
- Hands from a shared display camera: the stream carries intrinsics and the stereo pose but not
  the camera's pose in display space. Proposed (runtime): an append-only ADR-043 field "camera
  pose in display space" that the display plug-in fills. With it, shared-camera hands are metric
  directly (triangulate, transform) with no anchor, gain or calibration, and §10 becomes the
  webcam-only path.
- **Latency.** Samples are capture-timestamped and published into the runtime's target-time
  eye ring (#2144 Part 1), which extrapolates to the frame's display time; without it a 30 Hz
  webcam plus inference (50–100 ms) makes the image swim. Budget: capture → pose ≤ 25 ms
  (inference measured at about 6.5 ms per frame on Apple Silicon GPU/NE for face + two hands in
  tracking mode). Spike exit criterion: end-to-end latency measured on a hybrid Windows laptop.
- Spike done on Apple Silicon: tf2onnx converts all four models exactly (< 3e-4 px against
  TFLite); the Core ML execution provider needs MLProgram plus a small graph rewrite (explicit
  convolution padding, PRelu slope shape), after which each model runs as one partition.

## 8. Packaging

| Where | What |
|---|---|
| This repo | `DisplayXROpenTrackingSetup-*.exe`, `DisplayXR-OpenTracking-*.pkg`, `displayxr-open-tracking_*.deb`; each depends on the runtime |
| Meta-installer bundle | a "Tracking providers" section, selected by default |
| Runtime installer | not included; a runtime-only install keeps the nominal viewer |
| `versions.json` | `tracking_open` |
| Android | deferred (vendor components ship inside the runtime APK there) |

## 9. Order of work

1. Runtime: the viewer-tracking provider class (#2144 Part 2, PR #2292), the viewer-policy slot
   (§4), the composed advertisement, `sim_tracking`. The viewer-policy slot ships in the first
   runtime release after #2292 merges; display plug-ins pick it up through the downstream
   `features` repin, and until then they read as `FIXED`. Then: service hosting, per-screen
   routing, provider-as-camera-consumer, the ADR-043 display-space camera pose field.
2. Spike: model conversion and latency (parallel with 1); shared-display-camera hand spike.
3. This repo: viewer role.
4. Hands role.
5. Installers, bundle section, `versions.json`, Dashboard options and calibration.

## 10. Camera pose calibration (mirror alignment)

The display's front surface is a weak mirror. An eye at display-space `E = (x, y, z)` has its
virtual image at `(x, y, −z)`, so the line from an eye to its own reflection is the display
normal through `(x, y, 0)`. Two exact consequences:

- **Own pupil:** an open eye sees a dot overlap its own pupil's reflection exactly when that eye
  is on the normal through the dot. Distance from the display does not matter.
- **Closed-eye variant (the easiest to sight):** the right eye sees the closed left eye's
  reflection along `R → L'`, which crosses the display at `t = z_R / (z_R + z_L)`: the midpoint of
  the two eyes for a level head. Ten degrees of head yaw moves it by about 0.3 mm.

Each alignment therefore pins the target eye point's `(x, y)` to the dot's known physical position
(pixel offset × pixel pitch, from the display's reported metres), with no depth at all.

**Model.** Camera pose `(R, T)` in display space. The face landmarker gives the eye point's
direction `u_i` in camera space (accurate) and a depth `s̃_i` from iris size (about ±5–10 %).
For each sample `i`, `T + s_i·R·u_i` has `x, y` = the dot `D_i` (two equations, one new unknown
`s_i`).

| Assumption | Unknowns | Samples needed | Notes |
|---|---|---|---|
| `R = I`, use the iris depth | `T_x, T_y` | 1 | lateral error ≈ depth error × ray angle (≈ 20 mm at 60 cm for a 10 % depth error), so only a first guess |
| `R = I`, `T_z = 0`, depth solved | `T_x, T_y, s_1…s_N` | 2, at **distinct dots** | one dot at several distances is degenerate (scale ambiguity); the dot baseline fixes the scale |
| + pitch (clip-on webcams) | + `θ` | 3 | needs two head distances for conditioning |
| full `R`, plus focal length | 6 + 1 | ≥ 7 in theory, 9 in practice | 3×3 grid at 15 % inset, two distances |

- **`T_z` is not observable** from the mirror when `R = I` (it never enters the `x, y`
  equations). Assume 0 for a bezel camera; it biases viewer distance one-for-one by a few mm,
  which is negligible for the projection.
- **A built-in laptop camera is coplanar with the panel** (rigid lid), so `R = I` is exact there
  and two to three samples suffice. Default prior before calibration: top-centre, `T_y = H/2 + 8 mm`.
- **Bonus:** the solved `s_i` against the iris estimate `s̃_i` gives the user's own iris size,
  which removes the largest depth error.

**Procedure.**
1. Black screen with a white cross (2D mode on a 3D panel). Light the face with a dim ring far
   from the cross, so the reflection is visible.
2. "Close your left eye. With your right eye, find the reflection of your closed eye and move your
   head until the cross sits on it." Capture on Space / pinch, or on 0.5 s of stable head pose.
3. Points: centre, then left, right, top and bottom at 30 % inset, then centre again leaning
   back. The head must stay inside the camera's view, so use an inset, not the corners.
4. Least-squares solve; reject a sample whose residual exceeds 5 mm; show the RMS.
5. **Live check:** draw the dot at the predicted foot of the eye point in real time. If the
   calibration is right, the dot stays on the eye's reflection while the head moves. Arrow keys
   nudge `T_x, T_y` for a final touch.

**Limits.** A matte or anti-glare screen gives no usable reflection; those need a fallback (a 3D
panel can use a "swim" test: the user adjusts until a stereo object at depth stops moving as the
head moves). A display that tracks the viewer itself needs no calibration. The result is stored
per camera and per screen through the host settings.

**Where it lives.** The **Calibrate camera…** button sits in the Dashboard's tracking-provider
card, per screen and only on screens where the provider is the viewer source. The Dashboard
stays "dumb" (ADR-051 D4: it links no runtime code and only drives `displayxr-cli`), so the split is:

- **Dashboard:** the UI only. A borderless full-screen window on the target monitor (black, the
  cross, prompts), capture on Space / stable pose, the live-check dot.
- **`displayxr-cli tracking calibrate --screen <key> --watch --json`:** streams the provider's
  raw camera-space eye-point samples as NDJSON (the `status --watch` pattern).
- **`displayxr-cli tracking calibrate solve`:** the solver, runtime C code with unit tests, reusable
  by any camera-based tracking provider. It takes the captured samples plus the dot positions in
  metres and returns the pose, per-sample residuals and the iris scale.
- **`displayxr-cli tracking options set`:** commits the result as provider settings, keyed per
  camera and per screen.

The method and solver belong to the tracking class, not to this provider: any provider that
reports camera-space eye points gets calibration for free.

## 11. Hands role (as built)

The input provider `open-tracking-hands` (`xrtInputPluginNegotiate`, input ABI 1), in the same
binary as the viewer role.

### 11.1 One camera, two roles

- `ot_capture_open` keeps a process-wide table of open streams keyed by the device's locator. A
  second open of the same device returns the same stream with a reference added; the last close
  stops it. The mailbox is latest-wins per consumer (each frame buffer remembers the last frame it
  took) and wakes every waiter, so both roles see every frame and run their own inference on it,
  each on its own thread with its own ONNX Runtime sessions. A slow role drops frames; it never
  delays the other.
- The first opener's mode (size, rate) wins. Both roles read their own settings, so `DEVICE` (and
  `HFOV_DEG`) should match; a different `DEVICE` opens a second camera.
- Hands could run at a lower rate than the viewer; on Apple Silicon one hand costs about a frame's
  budget on the CPU and half on Core ML (README), so the role runs every frame it gets.

### 11.2 Inference

The face role's shape: the palm detector (full, 192², letterboxed) only when fewer hands are
followed than `MAX_HANDS` — every frame with none, every `DETECT_EVERY` frames with one — then the
hand-landmark model (full, 224²) per hand on a rotated crop. The crop comes from a palm detection
(rotation wrist → middle knuckle upright, the box moved half its height toward the fingers, its
long side × 2.6) or, in tracking mode, from the previous frame's landmarks (wrist → the mean of
the index / middle / ring knuckles upright, the palm landmarks' box moved 0.1 × its height
toward the fingers, long side × 2). A presence below `PRESENCE_MIN` drops the hand; two tracks on
one hand (ROI overlap > 0.5) keep the more confident; a detection overlapping a followed hand is
skipped. The model's outputs, identified by name: 21 landmarks in crop px, presence, handedness,
and 21 "world" landmarks (metric, about the hand's centre, crop axes).

**Handedness.** The handedness output is P(the subject's right hand) for an **un-mirrored**
image. Established on a CC0 photo of a left hand showing its palm (score 0.10; 0.92 mirrored) and
cross-checked against the 3D shape's chirality (`ot_hand_chirality`: the knuckle-plane normal
index × little points to the palm side for a right hand; the palm side is where the fingers bend).
The capture forces mirroring off and the sampler un-mirrors a `MIRROR=1` camera, so the model
always sees an un-mirrored image. Per track the score is averaged (the newest 30 frames); with two
hands the more "right" one is right, with one the threshold is 0.5.

### 11.3 Depth (monocular, honest)

The world landmarks give the hand's metric shape, from the model's learned average hand. The
provider finds the translation `T` that puts every shape point + `T` closest to its landmark's
pixel ray (linear least squares, `ot_hand_place`), then places each landmark on its own ray at the
depth of shape + `T`, so the result reprojects exactly onto the 2D landmarks. The depth is
therefore a **hand-size estimate**: a hand 10 % larger than the model's average reads 10 % farther.
`HAND_SCALE` corrects it per user (the hands' analogue of the viewer's iris scale; a calibration
step for it is open). The focal length comes from `HFOV_DEG` × the camera pose's focal scale.

### 11.4 Joints, frames, devices

- **21 → 26.** The 20 measured joints map one to one (thumb CMC / MCP / IP / tip → metacarpal /
  proximal / distal / tip; each finger's MCP / PIP / DIP / tip → proximal / intermediate /
  distal / tip; wrist → wrist). Synthesised: each finger's metacarpal joint a quarter of the way
  from the wrist to its knuckle (the carpometacarpal joint the model does not output), the palm at
  the middle of the middle finger's metacarpal bone, as the extension defines it.
- **Orientation.** -Z along the bone that starts at the joint (tips: the last bone; wrist and palm:
  toward the middle knuckle), +Y out of the back of the hand (from the knuckle plane and the
  handedness), X = Y × Z. The thumb uses the hand's back as its up vector, an approximation.
- **Radii.** A nominal adult hand's (fingertips 6–8 mm, palm 16 mm, wrist 20 mm), scaled by the
  measured wrist → middle-knuckle length.
- **Filtering.** One Euro per coordinate of the 21 points in display space, restarted on a new
  track or a gap over 300 ms.
- **Frame.** `XRT_TRACKING_TYPE_RIG_LOCAL`: display centre, +X right, +Y up, +Z toward the viewer.
  `display = T + R · p` with the camera's pose; the runtime anchors the origin at the initial rig,
  so the hands follow voluntary rig motion and never the viewer's head parallax (ADR-034
  Amendment 2). The joint set's hand pose is the identity; joints are absolute.
- **Devices.** Two `XRT_DEVICE_TYPE_HAND_TRACKER` devices carrying
  `XRT_INPUT_HT_UNOBSTRUCTED_{LEFT,RIGHT}`: they claim only the hand-tracking roles, so a webcam
  never takes the controller roles from the keyboard / mouse fallback. `CONTROLLERS=1` makes them
  left / right `khr/simple_controller` devices instead (select = pinch with hysteresis, grip / aim =
  the palm joint), for apps that only bind controllers.
- **`xrt_device` is not a versioned ABI.** Unlike the plug-in ifaces it has no `struct_size`; the
  provider fills it from the vendored `xrt_device.h` (every slot set, unused ones return
  `XRT_ERROR_NOT_IMPLEMENTED`), so a runtime that changes `struct xrt_device` needs a rebuild of
  this provider. The in-tree providers avoid this by building in the runtime tree.

### 11.5 Lifetime

- `probe` opens nothing (the hand models by size, ONNX Runtime, a camera, the permission).
- `create_devices` is cheap; the machinery is per process (input-provider spec §3 *Lifetime*).
- **On demand.** The first `get_hand_tracking` (or, with `CONTROLLERS=1`, a controller poll)
  starts the role's thread: verify the models, create the sessions, ask for the camera permission
  if needed, open (or share) the camera. `IDLE_S` seconds after the last locate the thread closes
  the camera and ends; the next locate starts it again. A thread that failed for good (models,
  permission, no execution provider) is retried at most every 10 s.
- **Presence** is "is there a usable camera": `PRESENT` after a successful probe and while
  running, `ABSENT` on no camera / permission denied / models missing; hands out of view are
  inactive joints, never `ABSENT`.
- **Prediction.** Each locate returns the newest frame's joints moved rigidly by the palm's
  velocity to the requested time, at most `PREDICT_MAX_MS` ahead; joints older than 300 ms (plus
  that horizon) are inactive.

### 11.6 Verified, and not

- Verified (CI + this repository's tests): the 21 → 26 mapping, orientations, radii, the placement
  solve (recovers a known translation to 1 mm; depth scales with the size prior), chirality, side
  assignment, the display mapping incl. a pitched camera, camera sharing on the file camera, and
  end to end through the input ABI on the file camera: a CC0 left-hand photo comes out as the left
  hand only (the mirrored photo as the right), metric size 98–101 mm wrist → middle knuckle, thumb
  on the correct side, -Z along the bones, +Y out of the back of the hand, no pinch, on CPU and
  Core ML.
- **Not verified:** a live camera hand test, two hands at once, the runtime's own loader and
  `xrLocateHandJointsEXT` end to end (the fake host stands in for the runtime), Windows and Linux
  capture (stubs, as for the viewer role).

### 11.7 Open

- Under the service the two roles run in different processes (§1): either the runtime hosts the
  input provider beside the viewer provider in one satellite, or each role opens its own stream
  (fine on macOS, a conflict on Windows unless the frame server shares the device).
- A per-user hand-size calibration (`HAND_SCALE`), e.g. from the viewer's calibrated depth when a
  hand touches a known point, or from the shared stereo camera's triangulation (§3, path 1).
- The display's shared stereo camera (§3, path 1) is not consumed yet: it needs the runtime's
  camera-consumer path and greyscale re-detection.
- Thumb orientation (a proper thumb frame), and `XR_EXT_hand_interaction` / aim-pose conventions
  beyond `khr/simple_controller`.
