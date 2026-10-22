# Design: open camera tracking provider

Status: **design, 2026-10-22.** Source of truth for the contracts it implements is the runtime
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
   triangulated metric hand points. Gated on a measurement: these streams are greyscale, about
   640×480 per eye at 30 Hz, aimed at the face; the landmark models are trained on colour. Not
   default until a spike shows hands detect at typical positions.
2. **A plain colour webcam** the runtime's tracker-camera denylist allows (ADR-043 Amendment 5).
3. **None** → the role reports unavailable.

The viewer role uses only (2): it is only ever active on a screen whose display does not track,
and such a display has no shared tracker stream.

Streams open **on demand** — the viewer role when a session is running on a screen it serves, the
hands role when an app creates a hand tracker — and close when demand ends. Opening a shared
display camera keeps that display's tracker running, so it must not be held open idle.

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
way the display status snapshot reports warnings as data.

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

## 7. Inference

- MediaPipe face landmarker (478 points incl. irises), palm detector and hand landmarker
  (Apache-2.0), converted to ONNX; ONNX Runtime loaded by absolute path at run time (the same
  pattern as the open conversion provider), DirectML / Core ML / CPU.
- Re-implemented around the models: region-of-interest tracking from the previous frame's
  landmarks, detection only on loss, left/right hand association, temporal filtering.
- Metric head distance from iris diameter (about 11.7 mm across people) and the camera's field of
  view; camera placement assumed top-centre of the bezel, corrected by a one-step calibration.
- Hands map into display space the same relative way as dedicated hand sensors (anchor + gain),
  because the camera's pose relative to the display is not known.
- First step is a spike: conversion fidelity against the reference models, and latency on an
  integrated GPU.

## 8. Packaging

| Where | What |
|---|---|
| This repo | `DisplayXROpenTrackingSetup-*.exe`, `DisplayXR-OpenTracking-*.pkg`, `displayxr-open-tracking_*.deb`; each depends on the runtime |
| Meta-installer bundle | a "Tracking providers" section, selected by default |
| Runtime installer | not included; a runtime-only install keeps the nominal viewer |
| `versions.json` | `tracking_open` |
| Android | deferred (vendor components ship inside the runtime APK there) |

## 9. Order of work

1. Runtime: the viewer-tracking provider class (#2144 Part 2), the viewer-policy slot (§4),
   provider-as-camera-consumer, the composed advertisement, `sim_tracking`.
2. Spike: model conversion and latency (parallel with 1); shared-display-camera hand spike.
3. This repo: viewer role.
4. Hands role.
5. Installers, bundle section, `versions.json`, Dashboard options and calibration.
