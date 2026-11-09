# Sprint 0：板卡只读基线

审查日期：2026-11-09（会话时区 Asia/Shanghai）。本次通过 SSH 只读检查；没有采集新图像、运行 OCR、安装依赖或修改板卡配置。完整原始命令输出已保存在本地项目包，待提交。

## 系统与存储

- 设备树型号：Embedfire LubanCat-4 V1；Debian 12；内核 6.1.99-rk3588，aarch64。
- 内存：3.8 GiB，总可用约 3.6 GiB（检查时），无 swap；这是瞬时值，不是推理峰值。
- 根文件系统为 /dev/mmcblk1p3，58 GiB；另有 29.1 GiB 的 mmcblk0。尚未确认节点与 eMMC/TF 的对应关系。

## 摄像头

- I²C 6-001a 已绑定 imx415 驱动，内核日志报告检测到传感器。
- /dev/media0 的启用链路显示 IMX415 → csi2-dphy0 → rockchip-mipi-csi2 → CIF；/dev/media1 是 ISP 拓扑。
- /dev/video-camera0 指向 /dev/video11（rkisp_mainpath）；当前格式 1920×1080 NV12，报告支持 streaming。/dev/video0 是 10-bit Bayer CIF 节点。
- 以上是驱动、拓扑和格式枚举；本轮没有执行实际采集或检查画面。

## NPU 和模型

- /dev/dri/renderD129 的 sysfs 设备对应 fdab0000.npu 和 RKNPU 驱动；启动日志显示 RKNPU DRM 0.9.8 初始化。
- /usr/lib/librknnrt.so 存在，内嵌版本字符串为 2.3.0；项目目录还有另一份运行库。没有执行 RKNN 推理或证明模型兼容。
- 板上存在 llama.cpp 的 llama-server、Xiaomi-OCR-0 Q4_K_M GGUF（约 505 MiB）及 mmproj BF16 GGUF（约 198 MiB）。
- 旧 smoke 结果记录 latency_ms=33670；输入、重复次数和计时边界未形成正式基线，不能作为优化前成绩。

## 时钟与后续验证

板卡 date -Is 显示 2026-04-28，与本次会话日期不一致。先记录该偏差，未经批准不更改时间配置。后续通过 Issue #2 检查真实图像，通过 Issue #3 使用固定输入、明确计时边界、重复运行、内存和质量检查建立 CPU 基线。

主要只读命令：uname -a、cat /etc/os-release、free -h、df -hT、lsblk、media-ctl -d /dev/media0 -p、media-ctl -d /dev/media1 -p、v4l2-ctl --all、v4l2-ctl --list-formats-ext、sysfs DRM 驱动映射、dpkg-query 和运行库文件检查。
