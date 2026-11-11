# EdgeDoc Cam

**Status: Sprint 0 — engineering setup and hardware baseline**

EdgeDoc Cam is an offline document capture and parsing terminal for a LubanCat-4 (RK3588S, 4 GB) and IMX415 camera. It is intended to connect camera capture, Linux media APIs, image quality checks, model inference, and measurable performance in one reproducible system.

EdgeDoc Cam 是一个离线文档采集与解析终端项目：把 IMX415 摄像头接入 LubanCat-4（RK3588S、4 GB），完成画面采集、质量判断、快照处理和板端 OCR，并用可复现的实验说明性能与稳定性。

## First release goal

Support a single, flat, printed Chinese or English page. Preview may be continuous; OCR is submitted for a selected still image. Handwriting, curved pages, arbitrary forms, tables, formulas, and multipage PDF are outside the first release.

## Data flow

```text
IMX415 → MIPI CSI-2 / D-PHY → Rockchip CIF / ISP → V4L2 frame
                                                        ├─ preview
                                                        └─ quality gate → immutable snapshot
                                                                          → crop / rectify
                                                                          → bounded OCR queue
                                                                          → Xiaomi-OCR-0
                                                                          → text / Markdown / JSON + metrics
```

The measured V4L2 nodes and open questions are recorded in [`docs/baseline.md`](docs/baseline.md). The current design and decisions are in [`docs/architecture.md`](docs/architecture.md) and [`docs/adr/0001-on-demand-ocr.md`](docs/adr/0001-on-demand-ocr.md).

## Sprint 0

Sprint 0 establishes a truthful environment baseline, confirms the IMX415 media path, defines a repeatable CPU OCR benchmark, records the architecture, and produces an acceptance report. The five tasks and their links are in [`docs/sprint-0-issues.md`](docs/sprint-0-issues.md).

## Repository layout

- `src/` — application code (interfaces to be added after Sprint 0)
- `tests/` — automated checks and fixtures (no application tests exist yet)
- `benchmarks/` — benchmark protocol and result schemas
- `scripts/` — reproducible setup and diagnostic helpers
- `docs/adr/` — architecture decisions
- `docs/experiments/` — experiment records and raw Sprint 0 command output
- `.github/ISSUE_TEMPLATE/` — task templates
- `.github/PULL_REQUEST_TEMPLATE/` — review template

## Development rules

- Do not commit model weights, personal documents, captured pages, access tokens, or local environment files.
- Keep a CPU inference route as the reference. Treat NPU inference as an experiment until the complete model/runtime/driver path is verified.
- Record input identity, model/runtime versions, warm-up policy, timing boundaries, memory use, and output quality for every benchmark.
- Do not modify the target board's boot image, device tree, kernel, network settings, or installed packages without an approved change plan.

## Current evidence

The first SSH review found the sensor driver bound, a V4L2 ISP node, an RKNN runtime library, and local Xiaomi-OCR-0 GGUF files. It did **not** capture a fresh camera frame or run inference. An older smoke log reports a single 33,670 ms request and is not a formal benchmark. See [`docs/baseline.md`](docs/baseline.md) for scope and caveats.

## Next step

Complete the Sprint 0 issues, then review the acceptance report before starting implementation. In particular, establish a fresh camera frame and a repeatable CPU baseline before making performance claims.


## Project roadmap and learning

- [Overall roadmap](docs/PROJECT_ROADMAP.md) — scope, architecture, milestones and current engineering evidence.
- [Learning progress](docs/LEARNING_PROGRESS.md) — lesson-by-lesson knowledge checks and remaining engineering validation.
- [Project handoff](docs/PROJECT_HANDOFF.md) — roles, decision boundaries, current PR status and how to resume.
