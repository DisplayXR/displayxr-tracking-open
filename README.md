# DisplayXR open camera tracking provider

`DisplayXR-OpenTracking` is a [DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime)
component that turns an ordinary camera into **viewer tracking** (head and eyes) and **hand
tracking**, with open models only. It is what every DisplayXR box gets when its display has no
tracker of its own: a laptop webcam is enough for head-coupled parallax and hand input.

> **Status: design.** No code yet. The viewer-tracking provider class it implements lands in the
> runtime first (ADR-054 N6 Part 2, runtime issue #2144). See [`docs/design.md`](docs/design.md).

One process, one camera, two registrations:

| Role | Runtime class | Registry id | Replaces |
|---|---|---|---|
| Head + eye positions | viewer-tracking provider (first open implementation) | `open-tracking` | the fly camera / nominal viewer |
| Hand joints, pinch | input provider (ADR-034) | `open-tracking-hands` | nothing (ranks below dedicated hand sensors) |

- **A display that tracks the viewer itself always wins.** When the active display processor
  weaves from its own tracked eyes, this provider's viewer role stays in standby on that screen
  and never opens a camera.
- **It never opens a display's tracker camera.** When the display plug-in shares its camera
  through the runtime's stereo camera source (ADR-043), the hands role consumes that stream as an
  ordinary camera consumer; otherwise it uses a plain webcam the runtime's tracker-camera denylist
  allows.
- **Inference:** the MediaPipe face, iris, palm and hand landmark models (Apache-2.0), converted
  to ONNX and run through ONNX Runtime, loaded at run time from the provider's own directory.

## License

Boost Software License 1.0 (`LICENSE`). Model weights keep their upstream licences; nothing is
committed, packs are fetched by hash.
