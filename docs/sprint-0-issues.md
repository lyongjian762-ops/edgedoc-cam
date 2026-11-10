# Sprint 0 GitHub issues

The private repository tracks these tasks in [Sprint 0 milestone #1](https://github.com/lyongjian762-ops/edgedoc-cam/milestone/1). The GitHub issue is the source of truth for progress.

## [1. Establish and document the RK3588S environment baseline](https://github.com/lyongjian762-ops/edgedoc-cam/issues/1)

**Proposed labels:** `sprint:0`, `type:experiment`, `area:platform`  
**Acceptance:** board/OS/kernel/RAM/storage; camera/V4L2 graph; NPU device/driver/runtime file and version inventory; model assets and runtime inventory; all commands and raw output saved; unknowns clearly marked; no environment changes.

Initial read-only output is in `docs/experiments/sprint-0-baseline/raw/`; see `docs/baseline.md`. Resolve the remaining verification items through separate tracked experiments.

## [2. Verify the IMX415 media path with a real frame](https://github.com/lyongjian762-ops/edgedoc-cam/issues/2)

**Proposed labels:** `sprint:0`, `type:experiment`, `area:camera`  
**Acceptance:** identify the active Media Controller path; document sensor mode and V4L2 node/format; capture and inspect one frame; record command, dimensions, pixel format, bytes, and errors; explain where ISP processing occurs; do not change device tree or kernel.

## [3. Establish a reproducible Xiaomi-OCR-0 CPU benchmark](https://github.com/lyongjian762-ops/edgedoc-cam/issues/3)

**Proposed labels:** `sprint:0`, `type:benchmark`, `area:ocr`  
**Acceptance:** exact llama.cpp build and model files; fixed test images; explicit CPU configuration; cold start and warm request separated; at least five repeated requests per image; latency definition, temperature, peak RSS, token counts, and output-quality checks recorded; results committed without private document images or model weights.

The old 33,670 ms smoke result is context only and must not be used as the formal baseline.

## [4. Review architecture and camera-to-result data flow](https://github.com/lyongjian762-ops/edgedoc-cam/issues/4)

**Proposed labels:** `sprint:0`, `type:design`, `area:architecture`  
**Acceptance:** agree on module boundaries, buffer ownership, bounded queue behavior, job lifecycle, error recovery, data retention, CPU reference path, and NPU experiment boundary; update architecture and ADR docs after owner review.

## [5. Publish the Sprint 0 acceptance report](https://github.com/lyongjian762-ops/edgedoc-cam/issues/5)

**Proposed labels:** `sprint:0`, `type:documentation`, `area:project`  
**Acceptance:** link all four prerequisite issues/PRs; summarize facts, measurements, unresolved items, and safety constraints; list learning outcomes and Sprint 1 entry criteria; project owner records accept/defer decisions.
