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

板卡这次使用了已有环境中不在默认 PATH 的 `/home/cat/ai/xiaomi-ocr-0/.venv/bin/cmake`。未安装依赖。`--device` 由使用者根据本机拓扑指定；`--api auto|single|multi` 可选择采集 API，默认优先多平面；`--save-index` 是从 1 开始计数的有效帧序号，必须不大于 `--count`。输出前缀用于创建 `PREFIX.plane0.bin`（多内存平面时依次编号）和 `PREFIX.json`；程序先写入同目录临时文件并在关闭后使用不覆盖的重命名发布，避免覆盖既有文件。输出包含环境画面，应保存在私有位置，不能上传仓库。

工具只读取当前格式，**不调用 `VIDIOC_S_FMT` 或设置摄像头控制项**。采集前应核对占用和格式；启动流会实际启动硬件，且 3A 服务可能动态调整曝光等。此程序不是只读诊断工具。

## ioctl 与缓冲区生命周期

1. `VIDIOC_QUERYCAP` 检查 `device_caps`（有效时）或 `capabilities`，确认视频采集 API 和 streaming 能力；单平面 API 与多平面 API 使用不同的 buffer type 和参数结构。
2. `VIDIOC_G_FMT` 报告驱动当前格式，包括 FourCC、宽高、field、各内存平面的 bytesperline/sizeimage、颜色空间及量化信息。程序不假设是 RGB，也不切换格式。
3. `VIDIOC_REQBUFS` 请求 MMAP 缓冲区；逐个 `VIDIOC_QUERYBUF` 查询每个 buffer 和 plane 的长度、偏移，再 `mmap` 到用户地址空间。多平面 API 可以只有一个内存平面；本板实际就是这种情况，NV12 仍包含 Y 和 UV 图像数据。
4. 首次 `VIDIOC_QBUF` 将所有映射缓冲区交给驱动；`VIDIOC_STREAMON` 启动采集。`poll` 等待数据，`VIDIOC_DQBUF` 将已完成缓冲区交给应用。打印 sequence、timestamp、flags、各 plane 的 bytesused/data_offset；目标帧在 DQBUF 后、重新 QBUF 前复制到独立 vector。
5. 完成处理后 `VIDIOC_QBUF` 归还缓冲区，成功归还后才计入有效循环。达到目标后 `VIDIOC_STREAMOFF` 停止流，然后写出独立副本和 JSON；析构时 `munmap` 所有成功映射区域并关闭 fd。异常路径也尝试 STREAMOFF、unmap 和 close，若有清理错误会记录。

每个保存平面从 `data_offset` 起写入到 `bytesused` 为止；JSON 同时保留原始 offset、bytesused、保存长度、stride 和 sizeimage，便于解释数据。保存的数据可能含行填充或压缩格式；应以驱动实际参数和格式规范还原，不能通用地假设是紧密排列的 RGB。保存失败的残留策略见下方 L02-R1 记录；单个文件原子发布不等于所有文件构成原子事务。

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
- 采集两次启动的内核日志均出现 `imx415 6-001a: write reg array error`，随后仍取得画面。对板上候选 BSP 源码的只读定位见下方 L02-R1；运行内核与该源码尚未证明完全一致。

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

## L02-R1：事件等待、保存和可控测试

- `poll` 仅订阅 `POLLIN`，表示视频数据可取；`POLLPRI` 是事件通知，不触发 DQBUF。若意外仅收到 `POLLPRI`，程序明确报错；若同时有 `POLLIN` 与 `POLLPRI`，只有 `POLLIN` 是取帧依据。`POLLERR`、`POLLHUP` 和 `POLLNVAL` 优先作为错误处理，即使同次也有 `POLLIN`。本程序没有订阅事件，也不调用 `VIDIOC_DQEVENT`。
- 输出每个 plane 和 JSON 前，先在目标目录用 `mkstemp` 创建独立临时文件，写入并关闭成功后，使用 Linux `renameat2(RENAME_NOREPLACE)` 发布；文件系统或内核不支持该操作时返回失败，不回退为可能覆盖的重命名。
- 所有临时文件写完并关闭后才开始发布，JSON 最后发布。写入、关闭或重命名异常时，未发布的临时文件由析构清理；清理失败会报告残留路径。已经成功发布的完整 plane 文件保留，不自动删除；已有目标不会覆盖。若原有 JSON 与本次输出同名，它也会原样保留。多个文件不是整体原子事务，**只能依据进程退出码 0 判断此次保存成功**，不能单凭某个文件或 JSON 存在判断。异常终止可能遗留 `.tmp.*`；不调用 `fsync`，不保证断电后的持久性。
- 时间戳保存原始秒、微秒和 flags，同时解释时钟和来源；`interval_us` 只对相邻有效帧、同一来源且均标记为 monotonic 的非倒退时间戳输出，否则为 unavailable。若 sequence 有跳号，这个间隔可能跨越多帧，不可自动等同于一帧周期。

### 原始 300 帧日志的时间戳分析

原始 L02 日志 300 帧 flags 均为 `0x2001`：其中 `0x00002000` 是 `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`，`0x00000001` 是 `V4L2_BUF_FLAG_MAPPED`；来源掩码为 0，对应 `V4L2_BUF_FLAG_TSTAMP_SRC_EOF`。这是驱动标志所声明的时钟和来源，并非墙钟日期。不能把 `3180.866459` 等时间值当作日历时间，也不能直接与板卡错误的系统日期拼接。

对原始日志中 sequence 0–299 的 299 个相邻时间戳差进行计算：最小 16,505 µs、最大 16,819 µs、平均 16,666.562 µs、中位数 16,667 µs；无非正间隔。首末间隔总计 4,983,302 µs。这是一次采集的描述性统计，不是长期稳定性测试；该统计来自整改前的原始日志，整改后尚未重新启动摄像头验证新增实时输出。

### IMX415 BSP 源码的只读定位与局限

在板卡 `/home/cat/kernel/drivers/media/i2c/imx415.c` 候选源码中，`imx415_write_array()` 第 2016–2018 行仅在 `regs == NULL` 时打印 `write reg array error`，此时 `ret` 初始化为 0 并直接返回，尚未执行该函数内的寄存器写入循环。`__imx415_start_stream()` 第 3128、3131 行先传入 `cur_mode->global_reg_list`，再传入 `cur_mode->reg_list`。该源码中与观察到的 4 lane、1944×1096、10 bit、非 HDR 模式相符的表项将 `global_reg_list` 设为 NULL，另一个 `reg_list` 非空。因此，**若运行内核使用相同实现和该模式**，这条日志很可能来自空的全局表，并不代表某一寄存器 I²C 写失败。

候选源码的 `imx415_write_reg()` 第 2005–2006 行将 `i2c_master_send()` 未返回期望长度的情况统一转为 `-EIO`，不会在该路径保留原始 I²C 返回值；`imx415_write_array()` 的这条 NULL 日志也没有失败寄存器地址或 I²C 返回码。当前证据无法给出本次失败寄存器或原始 I²C 返回码，不能据此断言总线一定正常。

证据边界：候选源码树 commit 为 `1bc19c520831b6d0ca7d7afb4639189a2064e9bc`，`imx415.c` SHA-256 为 `27a0a79f9cdce31f15e1ef5b0793b499d3008bc4a8af497885c74cb04c76b556`，源码声明版本 00.01.08 与运行日志一致；两边配置均为 `CONFIG_VIDEO_IMX415=y`。但运行中的 `/boot/Image-6.1.99-rk3588` 与该源码树的构建 Image 大小及 SHA-256 不同，构建环境也不同，**不能证明候选源码就是当前运行内核的精确源码**。完整只读证据见 [L02_R1_readonly_and_tests.txt](L02_R1_readonly_and_tests.txt)。

### 测试与未完成项

板卡 g++ 12.2.0 与已有 CMake 4.4.4 在新临时构建目录编译，CTest 1/1 通过。受控测试覆盖参数解析、`POLLIN/POLLPRI/POLLERR` 及混合标志、真实管道超时和信号中断重试、模拟 DQBUF EAGAIN/EIO、异常帧标志及越界长度、时间戳解释和间隔、成功发布、已有目标不覆盖、缺失目录、受限文件大小造成的写入失败、部分发布残留策略。测试不访问摄像头；受限文件大小只应用于测试子进程。

增量构建曾出现系统头文件时间在未来、`Clock skew detected` 警告；随后在新的构建目录重新配置、编译并测试成功，没有调整板卡时间或系统配置。尚未实测真实驱动的 POLLPRI、EAGAIN、损坏帧注入，多内存平面、单平面成功采集，以及整改后的 300 帧运行。上面的 300 帧仍是整改前程序的实测，不能当作整改后硬件测试结果。
