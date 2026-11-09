# Architecture and data flow

## Product boundary

The first release handles one flat, printed Chinese or English document at a time. It provides continuous preview, explicit capture, and local OCR for a selected still image. Automatic capture may be added only after quality and stability measurements exist.

## Runtime path

```text
IMX415
  → MIPI CSI-2 / D-PHY
  → Rockchip CIF and ISP
  → V4L2 capture buffers
  → preview + quality/stability gate
  → owned immutable snapshot
  → crop / perspective correction / model preprocessing
  → bounded OCR job queue
  → Xiaomi-OCR-0 CPU backend (reference)
  → validate / persist result and metrics
```

The control path for exposure and sensor mode is separate from the pixel data path. Sprint 0 observed the sensor and media graph, but did not capture a frame. Code must discover or configure the selected V4L2 node from board facts rather than assume `/dev/videoN` is stable across images.

## Proposed modules

| Module | Responsibility | Boundary |
| --- | --- | --- |
| `camera` | Open V4L2 node, negotiate format, queue/dequeue buffers, publish frame metadata | Does not call OCR. |
| `quality` | Blur, brightness, clipping, document occupancy, and frame-stability checks | Pure image analysis where possible; thresholds measured and documented. |
| `capture` | Freeze one owned snapshot and assign a capture ID | Snapshot lifetime is independent of reusable V4L2 buffers. |
| `preprocess` | Crop, optional perspective correction, color conversion, resize/tiling | Input/output dimensions and transformations are logged. |
| `ocr` | Submit a snapshot to CPU or explicitly selected backend | One bounded queue; no inference per preview frame. |
| `result` | Validate response and write text/Markdown/JSON plus metrics | Avoid retaining sensitive document images by default. |
| `service` | Expose local control/status API and manage lifecycle | Queue limits, cancellation, timeout, and error states are explicit. |

## Concurrency model

Preview and capture stay responsive while OCR runs as a job. The camera produces reusable buffers; a capture request must copy or otherwise take ownership of a stable snapshot before re-queuing that buffer. A bounded queue prevents a slow CPU model from accumulating unbounded images. Each job carries its capture ID, timestamps, backend, model revision, and terminal state.

## Backend strategy

The CPU llama.cpp path is the correctness and performance reference. RKNN is a separate experiment: first confirm the model's graph and operator support, conversion path, quantization effects, memory, and quality. The presence of an RKNPU kernel driver or `librknnrt.so` is not proof that this GGUF model can run on the NPU.

## Data retention

Captured pages may contain private material. The first release stores only the artifacts requested by the user, makes retention explicit, and excludes camera frames, local model weights, and personally identifiable document samples from Git by default.
