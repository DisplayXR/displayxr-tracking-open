# Design: open camera tracking provider

Status: **design, 2026-10-22; viewer role implemented on macOS, 2026-10-24** (how it works,
measurements and settings: `README.md`). Source of truth for the contracts it implements is the runtime
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
light two privacy indicators.

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
hands role when an app creates a hand tracker — and close when demand ends. Hold **one** stream
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
- Hands from a webcam map into display space the same relative way as dedicated hand sensors
  (anchor + gain), because the webcam's pose relative to the display is not known until
  calibrated (§10).
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
