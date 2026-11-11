# EdgeDoc Cam

**状态：Sprint 0——工程准备与硬件基线**

EdgeDoc Cam 是一个离线文档采集与解析终端项目：把 IMX415 摄像头接入 LubanCat-4（RK3588S、4 GB），连接摄像头采集、Linux 媒体接口、图像质量检查、模型推理和性能测量，建立可复现的系统。

## 第一版目标

支持单张平整的中英文印刷文档。系统可以持续预览，由用户选择静态图像后再提交 OCR。手写、弯曲页面、任意表单、表格、公式和多页 PDF 不属于第一版范围。

## 数据流

```text
IMX415 → MIPI CSI-2 / D-PHY → Rockchip CIF / ISP → V4L2 帧
                                                        ├─ 预览
                                                        └─ 质量门控 → 独立快照
                                                                          → 裁剪 / 校正
                                                                          → 有界 OCR 队列
                                                                          → Xiaomi-OCR-0
                                                                          → 文本 / Markdown / JSON + 度量
```

已测量的 V4L2 节点和未解决问题记录在 [`docs/baseline.md`](docs/baseline.md)。当前设计与决策见 [`docs/architecture.md`](docs/architecture.md) 和 [`docs/adr/0001-on-demand-ocr.md`](docs/adr/0001-on-demand-ocr.md)。

## Sprint 0

Sprint 0 用于建立真实的环境基线、确认 IMX415 媒体链路、定义可重复的 CPU OCR 基准测试、记录架构并编写验收报告。五项任务及其链接见 [`docs/sprint-0-issues.md`](docs/sprint-0-issues.md)。

## 仓库目录

- `src/`：应用代码（Sprint 0 后添加接口）
- `tests/`：自动化检查与测试素材（当前主分支尚无应用测试）
- `benchmarks/`：基准测试方案和结果格式
- `scripts/`：可复现的环境准备与诊断辅助脚本
- `docs/adr/`：架构决策记录
- `docs/experiments/`：实验记录及 Sprint 0 原始命令输出
- `.github/ISSUE_TEMPLATE/`：任务模板
- `.github/PULL_REQUEST_TEMPLATE/`：PR 审查模板

## 开发规则

- 不提交模型权重、个人文档、拍摄的页面、访问令牌或本地环境文件。
- 保留 CPU 推理路径作为参考；在完整的模型、运行时和驱动链路得到验证前，将 NPU 推理视为实验。
- 每次基准测试都记录输入标识、模型与运行时版本、预热策略、计时边界、内存使用和输出质量。
- 未经批准的变更方案，不修改目标板卡的启动镜像、设备树、内核、网络设置或已安装的软件包。

## 当前证据

首次 SSH 检查发现传感器驱动已绑定、存在 V4L2 ISP 节点、RKNN 运行时库和本地 Xiaomi-OCR-0 GGUF 文件。该次检查**没有**采集新的相机帧或运行推理。较早的单次试运行日志记录了 33,670 毫秒，不能作为正式基准测试。范围与限制见 [`docs/baseline.md`](docs/baseline.md)；后续实验状态见下方项目交接说明。

## 下一步

完成 Sprint 0 的各项任务并审查验收报告，然后推进实现。尤其需要采集新的相机帧并建立可重复的 CPU 基线，才能对性能作出有依据的说明。

## 项目路线与学习记录

- [项目总体计划](docs/PROJECT_ROADMAP.md)：项目范围、架构、阶段计划和当前工程证据。
- [学习进度](docs/LEARNING_PROGRESS.md)：逐讲知识检查和待完成的工程验证。
- [项目交接说明](docs/PROJECT_HANDOFF.md)：角色分工、决策边界、PR 当前状态和恢复上下文的方法。
