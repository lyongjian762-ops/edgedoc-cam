# Sprint 0 hardware and software baseline

**Review date:** 2026-11-09 (Asia/Shanghai session date)  
**Access:** SSH through the host alias already present in the local SSH configuration, as account `cat`  
**Scope:** read-only inventory. No packages were installed, no service was started, no configuration was changed, and no new camera frame or OCR inference was run.

## Verified during this review

| Area | Observation | Meaning / limit |
| --- | --- | --- |
| Board | Device tree model reports `Embedfire LubanCat-4 V1`; kernel identifies `aarch64` | Consistent with the project hardware stated by the owner. |
| OS / kernel | Debian GNU/Linux 12; `Linux lubancat 6.1.99-rk3588 #10` | Captured from the live board. |
| Memory | 3.8 GiB total, 3.6 GiB available at inspection, no swap | A point-in-time idle reading, not an inference peak. |
| Storage | `/` is ext4 on `/dev/mmcblk1p3` (58 GiB); `/boot` is ext2 on `/dev/mmcblk1p2` (120 MiB); `/dev/mmcblk0` is 29.1 GiB and not mounted | Device names and mounts are reported as seen; their mapping to the owner's eMMC/TF labels has not been established. |
| Sensor binding | I²C device `6-001a` resolves to the `imx415` driver; kernel log reports detected ID `0000e0` and four lanes | Driver probe and binding are observed. This alone does not prove a fresh frame was captured. |
| Media graph | `/dev/media0` is `rkcif-mipi-lvds2`; enabled graph links connect `m02_b_imx415 6-001a` → `rockchip-csi2-dphy0` → `rockchip-mipi-csi2` → CIF nodes. `/dev/media1` is the RKISP graph. | The active topology is board-specific; do not hard-code other boards' node numbers. |
| V4L2 ISP node | `/dev/video-camera0` links to `/dev/video11` (`rkisp_mainpath`); V4L2 reports streaming capability, current 1920×1080 NV12, and formats up to 1920×1080 | Capability and format enumeration only. No streaming test or image inspection was performed. |
| CIF raw node | `/dev/video0` is a multiplanar capture node with 10-bit Bayer formats and sizes up to 1944×1096 | Enumeration only; actual frame delivery remains unverified. |
| NPU kernel path | `/dev/dri/renderD129` maps through sysfs to platform device `fdab0000.npu` and driver `RKNPU`; boot log reports RKNPU DRM driver 0.9.8 initialized | Kernel-side presence is verified. The absence of `/dev/rknpu*` is not evidence that the NPU is absent; this image exposes it through DRM. |
| RKNN runtime | `/usr/lib/librknnrt.so` exists; its embedded version string reports 2.3.0 (2024-11-07). A second copy exists at `~/ai/xiaomi-ocr-0/rockchip/lib/librknnrt.so`; `/usr/bin/rknn_server` exists. | Runtime files are present, but no RKNN model has been loaded. `ldconfig -p` returned no RKNN entry and `dpkg-query` did not associate the library with a package, so runtime resolution/compatibility still needs a controlled check. |
| Python | System Python is 3.11.2. A project `.venv` exists. The filtered package inventory found no matching OCR/RKNN/ONNX/Torch entries. | The model's CPU server is a compiled llama.cpp executable; the Python demo and its dependency set still need a complete inventory. |
| Xiaomi-OCR-0 assets | `llama.cpp/build/bin/llama-server` exists; model files are `Xiaomi-OCR-0.Q4_K_M.gguf` (505 MiB) and `Xiaomi-OCR-0.mmproj-bf16.gguf` (198 MiB). | Weights and binaries are present on the board; they must not be copied into this repository. |

## Historical smoke artifacts (not a baseline)

The board contains older smoke artifacts. `ocr-smoke-result.json` reports `latency_ms: 33670` for model alias `xiaomi-ocr-0`; `model-smoke.log` reports prompt evaluation at 32,946 ms for 175 tokens and generation at 698 ms for 16 tokens. `demo-smoke.log` records HTTP 200 for health and OCR endpoints. The prior launcher script requests `--threads 4`, `--ctx-size 2048`, and `--n-gpu-layers 0`, but the logs do not prove which exact invocation produced that result.

This is one historical smoke request with no controlled input set, repetition, warm/cold separation, thermal record, or verified timing boundary. Do not present 33.67 s as the official before-optimization score. No inference was run during this review. The saved PID values were checked and neither process was running at inspection.

## Clock caveat

The board's `date -Is` returned `2026-04-28T03:53:14+08:00`, while this review was performed on 2026-11-09. The board clock is therefore stale or otherwise inconsistent; its timestamps and file modification dates must not be used to date measurements until the clock is corrected through an approved plan.

## Open verification items

1. Stream a short V4L2 capture from the selected ISP node and inspect the resulting image. This was intentionally left for a tracked, supervised camera experiment.
2. Inventory the model demo's virtual-environment dependencies and record the exact binary/build revision.
3. Run a controlled CPU OCR benchmark with a fixed public/test image set, documented timing boundary, warm-up policy, repeat count, temperature, peak memory, and output-quality check.
4. Confirm RKNN runtime loading and driver/runtime compatibility without assuming Xiaomi-OCR-0 can be converted to or run on RKNN.
5. Correct or explain the board clock using an approved change plan before relying on board-side timestamps.
