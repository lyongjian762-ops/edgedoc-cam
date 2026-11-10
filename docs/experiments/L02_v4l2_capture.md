# Lesson 02：V4L2 MMAP 采集及独立快照

## 前置与范围

已读取 [Lesson 01 报告](https://github.com/lyongjian762-ops/edgedoc-cam/blob/lesson-01/camera-pipeline/docs/experiments/L01_camera_pipeline.md)：L01 的 PR 仍为 Draft，但已在真实板卡取得并检查一帧。其链路图与本实验相符；跨 media 图的内部转接和 IMX415 日志错误仍待解释。L02 使用采集前重新确认的 `/dev/video-camera0`（当时指向 `/dev/video11`），不硬编码节点：运行时必须指定 `--device`。

测试机：LubanCat-4 V1、Debian 12、Linux 6.1.99-rk3588 aarch64、g++ 12.2.0、既有 CMake 4.4.4。执行端日期 2026-11-10；板卡时钟显示 2026-04-28，不能用板卡墙钟和真实日期直接关联。

## 构建与使用

在 Linux 上使用现有 CMake 和 C++17 编译器：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
./build/edgedoc-v4l2-capture --device /dev/video-camera0 \
  --count 300 --save-index 150 --output /tmp/edgedoc-frame --timeout-ms 5000
```

板卡这次使用了已有环境中不在默认 PATH 的 `/home/cat/ai/xiaomi-ocr-0/.venv/bin/cmake`。未安装依赖。`--device` 由使用者根据本机拓扑指定；`--api auto|single|multi` 可选择采集 API，默认优先多平面；`--save-index` 是从 1 开始计数的有效帧序号，必须不大于 `--count`。输出前缀用于创建 `PREFIX.plane0.bin`（多内存平面时依次编号）和 `PREFIX.json`；程序采用独占创建，避免覆盖既有文件。输出包含环境画面，应保存在私有位置，不能上传仓库。

工具只读取当前格式，**不调用 `VIDIOC_S_FMT` 或设置摄像头控制项**。采集前应核对占用和格式；启动流会实际启动硬件，且 3A 服务可能动态调整曝光等。此程序不是只读诊断工具。

## ioctl 与缓冲区生命周期

1. `VIDIOC_QUERYCAP` 检查 `device_caps`（有效时）或 `capabilities`，确认视频采集 API 和 streaming 能力；单平面 API 与多平面 API 使用不同的 buffer type 和参数结构。
2. `VIDIOC_G_FMT` 报告驱动当前格式，包括 FourCC、宽高、field、各内存平面的 bytesperline/sizeimage、颜色空间及量化信息。程序不假设是 RGB，也不切换格式。
3. `VIDIOC_REQBUFS` 请求 MMAP 缓冲区；逐个 `VIDIOC_QUERYBUF` 查询每个 buffer 和 plane 的长度、偏移，再 `mmap` 到用户地址空间。多平面 API 可以只有一个内存平面；本板实际就是这种情况，NV12 仍包含 Y 和 UV 图像数据。
4. 首次 `VIDIOC_QBUF` 将所有映射缓冲区交给驱动；`VIDIOC_STREAMON` 启动采集。`poll` 等待数据，`VIDIOC_DQBUF` 将已完成缓冲区交给应用。打印 sequence、timestamp、flags、各 plane 的 bytesused/data_offset；目标帧在 DQBUF 后、重新 QBUF 前复制到独立 vector。
5. 完成处理后 `VIDIOC_QBUF` 归还缓冲区，成功归还后才计入有效循环。达到目标后 `VIDIOC_STREAMOFF` 停止流，然后写出独立副本和 JSON；析构时 `munmap` 所有成功映射区域并关闭 fd。异常路径也尝试 STREAMOFF、unmap 和 close，若有清理错误会记录。

每个保存平面从 `data_offset` 起写入到 `bytesused` 为止；JSON 同时保留原始 offset、bytesused、保存长度、stride 和 sizeimage，便于解释数据。保存的数据可能含行填充或压缩格式；应以驱动实际参数和格式规范还原，不能通用地假设是紧密排列的 RGB。若写入途中失败，已创建的部分输出可能保留，需要人工判断，不得将其视为完整结果。

## 错误处理

- `ioctl` 遇 EINTR 重试；非阻塞 DQBUF 的 EAGAIN 返回 poll，在同一帧的 deadline 内重试；poll 超时、设备错误、其他 ioctl 错误均返回非零。
- `V4L2_BUF_FLAG_ERROR`、空 payload 或不合法的 plane 长度/偏移被拒绝并尝试重新入队，不计入有效帧；超过 100 个无效帧则停止。
- QUERYBUF/DQBUF 平面数或索引与已映射缓冲区不一致时停止，避免越界；部分映射或后续错误均由资源清理路径释放已有映射并关闭 fd。
- 程序不会安装信号处理器；这里的清理保证适用于正常退出和 C++ 异常路径。外部强制终止时不保证用户态 STREAMOFF 和日志完整，操作系统会回收进程资源。

## 实测结果（一次 300 帧运行）

- 采集前别名指向 `/dev/video11`；特权 `fuser` 对 video11 返回 1、未列出占用者；其他媒体节点仍由 `rkaiq_3A_server` 持有。占用检查和启动不是原子操作。
- `QUERYCAP`：driver `rkisp_v6`、card `rkisp_mainpath`、有效 capability `0x4201000`。当前为 **多平面 Capture API，1 个内存平面**，`NV12`，1920×1080，stride 1920，sizeimage 3,110,400，量化 Full Range。映射了 4 个缓冲区。
- 300 个有效 DQBUF/QBUF 循环，0 个拒绝帧；sequence 0–299 连续，每帧 bytesused 3,110,400、offset 0。第 150 个有效帧的 sequence 为 149，已复制并保存；STREAMOFF 正常，进程退出码 0。
- 保存文件 3,110,400 字节，JSON 与格式相符。板卡既有 FFmpeg 将该 NV12 数据还原为 1920×1080 PNG，退出码 0；人工检查预览可辨认天花板和灯具。没有拍摄文档，因此未验证文本识别质量。
- 正常退出后，特权 fuser 未报告 video11 占用；第二次打开同一节点并成功采集 1 帧、STREAMOFF 正常、退出码 0。
- 参数 `--count 0`、不存在的设备、`/dev/null` 的 QUERYCAP，以及在仅支持多平面 API 的当前节点上指定 `--api single`，均按预期返回非零。单平面设备成功采集、多内存平面设备、EINTR/EAGAIN/timeout 和错误标志注入**未实测**。
- 采集两次启动的内核日志均出现 `imx415 6-001a: write reg array error`，随后仍取得画面。错误原因、后果和长期稳定性未确认；不能用本次成功掩盖该问题。

完整输出见 [L02_v4l2_capture_log.txt](L02_v4l2_capture_log.txt)。测试日志中的临时路径及内核标识已脱敏；不包含图像数据。

## 按本次格式还原图像

确认 JSON 显示 NV12、1920×1080、stride 为 1920、offset 为 0，且本地已有 FFmpeg 时，可以把本次保存的 plane0 恢复为预览：

```sh
ffmpeg -f rawvideo -pixel_format nv12 -video_size 1920x1080 -color_range pc \
  -i /tmp/edgedoc-frame.plane0.bin -frames:v 1 /tmp/edgedoc-preview.png
```

上述命令只适用于这些实际参数；若 stride 存在填充或格式不同，需要相应处理。原始数据、JSON 和预览图均应按场景隐私保存。

## 后续

排查寄存器写入错误；在合适的测试设备上验证单平面 API 和多个内存平面；增加可控的 EINTR/EAGAIN/timeout/损坏帧故障注入测试；经批准后使用测试文档和受控光照评估实际采集质量。
