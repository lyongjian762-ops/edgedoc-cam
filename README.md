# EdgeDoc Cam

离线文档采集与解析终端：LubanCat-4（RK3588S，4 GB）+ IMX415 + Xiaomi-OCR-0。当前处于 Sprint 0，目标是建立可信的环境基线与工程结构。

## 首版范围

面向平整、单张、中英文印刷文档，提供连续预览、按需拍摄及板端 OCR。

```text
IMX415 → MIPI CSI-2 / D-PHY → CIF / ISP → V4L2
  → 预览 / 质量判断 → 独立快照 → 预处理 → 有界 OCR 队列
  → Xiaomi-OCR-0 CPU 参考路径 → 文本 / Markdown / JSON 与指标
```

预览和 OCR 分离：摄像头持续采集，OCR 只处理选定的静态图像。NPU 是待验证的独立实验，不能仅凭运行库存在就断言模型兼容。

## Sprint 0 任务

- [环境基线](https://github.com/lyongjian762-ops/edgedoc-cam/issues/1)
- [IMX415 媒体链路](https://github.com/lyongjian762-ops/edgedoc-cam/issues/2)
- [Xiaomi-OCR-0 CPU 基线](https://github.com/lyongjian762-ops/edgedoc-cam/issues/3)
- [架构与数据流](https://github.com/lyongjian762-ops/edgedoc-cam/issues/4)
- [验收报告](https://github.com/lyongjian762-ops/edgedoc-cam/issues/5)

## 当前状态

只读审查已确认传感器驱动与媒体拓扑、NPU DRM 节点及 RKNN 运行库、CPU 推理程序与 GGUF 文件。尚未在本轮采集新图像或运行推理。旧日志的单次 33.67 秒不是正式性能基线。板卡时钟与本次审查日期不一致，后续实验需记录时间来源。

完整项目骨架和原始命令输出已在本地准备，待提交到本分支。未经批准不刷机、不修改设备树、内核、网络或安装依赖；模型权重和私人文档不提交到仓库。
