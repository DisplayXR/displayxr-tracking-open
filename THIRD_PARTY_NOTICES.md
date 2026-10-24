# Third-party components

The provider's own source is [BSL-1.0](LICENSE). It ships next to, and loads at run time, the
components below; none is linked into the provider binary and none is committed to this
repository (the build fetches ONNX Runtime as a pinned, hash-checked release archive, see
`cmake/OnnxRuntime.cmake`; the model blobs are produced by `scripts/fetch_models.py` from a
pinned upstream download, see `packs/open-tracking-default.models.json`).

## Inference runtime

| Component | Version | Licence | Where it comes from | Platforms |
|---|---|---|---|---|
| ONNX Runtime | 1.31.0 | MIT | `onnxruntime-{osx-arm64,linux-x64}-1.31.0.tgz`, `onnxruntime-win-x64-1.31.0.zip` GitHub release assets of [microsoft/onnxruntime](https://github.com/microsoft/onnxruntime/releases/tag/v1.31.0) | all |

The licence texts are installed under `licenses/` next to the binary (`onnxruntime-LICENSE.txt`,
`onnxruntime-ThirdPartyNotices.txt`). On macOS the Core ML execution provider, part of the
official osx-arm64 build, drives the operating system's own Core ML framework; nothing else is
redistributed.

## Models (the default pack, `packs/open-tracking-default.models.json`)

Both come from one upstream bundle, MediaPipe's Face Landmarker
(`face_landmarker.task`, float16, version 1:
<https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task>,
sha256 `64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff`), converted to ONNX by
this repository's `scripts/fetch_models.py` (tf2onnx, opset 17; the batch dimension fixed to 1; a
numerically neutral graph rewrite for the Core ML execution provider; canonical tensor names).
The weights are unchanged.

| Model | Upstream file | Licence | Model card |
|---|---|---|---|
| BlazeFace (short range), face detector, 128 × 128 | `face_detector.tflite` in the bundle | Apache-2.0 | [MediaPipe BlazeFace Model Card (Short Range)](https://storage.googleapis.com/mediapipe-assets/MediaPipe%20BlazeFace%20Model%20Card%20(Short%20Range).pdf) |
| Face Mesh V2, 478 landmarks incl. irises, 256 × 256 | `face_landmarks_detector.tflite` in the bundle | Apache-2.0 | [Model Card MediaPipe Face Mesh V2](https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Face%20Mesh%20V2.pdf) |

Copyright of the models: Google LLC, licensed under the Apache License, Version 2.0
(<https://www.apache.org/licenses/LICENSE-2.0>). The MediaPipe solution documentation
(<https://ai.google.dev/edge/mediapipe/solutions/vision/face_landmarker>) describes the models'
inputs, outputs and the region-of-interest scheme this provider re-implements.

## Test fixture (fetched by script, never committed)

| File | Source | Licence |
|---|---|---|
| `portrait.jpg` (converted to `images/portrait.ppm` by `fetch_models.py --test-images`) | <https://storage.googleapis.com/mediapipe-assets/portrait.jpg> | public domain (an official US-government portrait) |
