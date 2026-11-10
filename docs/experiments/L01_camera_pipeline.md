# Lesson 01：IMX415 媒体链路与单帧采集基线

状态：**单帧采集和本地预览已完成；稳定性及寄存器写入错误待排查。** 关联 Issue #2。本次通过 SSH 完成只读检查及经批准的单帧采集，没有改变设备树、内核、网络、启动配置或安装软件。

## 环境与证据

- 检查日期：执行端 2026-11-10（中国时区）；板卡 `date -Is` 显示 2026-04-28，应先校准时间再用于事件关联。
- 板卡识别为 Embedfire LubanCat-4 V1；Debian GNU/Linux 12；Linux `6.1.99-rk3588`，aarch64。
- 完整命令输出及返回码见 [L01_camera_pipeline_commands.txt](L01_camera_pipeline_commands.txt)。该日志仅对构建主机标识做了脱敏。
- `/dev/media0` 为 `rkcif-mipi-lvds2`；`/dev/media1` 为 `rkisp1-vir0`。设备编号是本次观察，不保证重启后保持。

## 媒体拓扑及证据边界

```text
/dev/media0（RKCIF 图）：
m02_b_imx415 6-001a (/dev/v4l-subdev2), pad0
  --[ENABLED]--> rockchip-csi2-dphy0 (/dev/v4l-subdev1)
  --[ENABLED]--> rockchip-mipi-csi2 (/dev/v4l-subdev0)
  --[ENABLED，例如 pad1]--> stream_cif_mipi_id0 (/dev/video0)

跨媒体图的关联：rkcif-mipi-lvds2 在 media0 是设备名称，
                 在 media1 又以 source entity 出现；具体内部转接未由这些图直接展示。
/dev/media1（RKISP 图）：
rkcif-mipi-lvds2 (/dev/v4l-subdev4)
  --[ENABLED]--> rkisp-isp-subdev (/dev/v4l-subdev3)
  --[ENABLED]--> rkisp_mainpath (/dev/video11)
                      ↑ /dev/video-camera0 为指向 video11 的符号链接
```

`media-ctl` 两张图分别证明各自显示的链接启用；两图之间的内部数据转接是基于同名 RKCIF 实体的合理关联，**并非已完整观察到的跨图 link**。不能把 `/dev/video0` 画成已证实的 `/dev/video11` 上游转接节点。启用链接也不等于已有有效图像。

Sensor 和 D-PHY 显示 `SGBRG10_1X10/1944x1096`；ISP 输入实体显示 `SGBRG10_1X10/1920x1080`，ISP 输出 pad 显示 `YUYV8_2X8/1920x1080`。最终 video11 的采集格式为 `NV12`、1920×1080、单平面、每行 1920 字节、sizeimage 3,110,400 字节、量化 Full Range。各层格式、尺寸不同，需要进一步确认实际裁剪/转换过程，不能假定仅凭图就完全知道实现细节。

原理：Sensor 输出 Bayer 原始数据；CSI/D-PHY 接收并交给采集链路；ISP 将传感器数据处理为适合后续使用的图像；V4L2 设备向应用提供采集接口。`NV12` 是 Y 平面及交错 UV 平面的 4:2:0 格式，不是 JPEG。

## 占用与错误分析

- 普通用户 `fuser`/`lsof` 均返回 1 且无结果，不能据此认定设备空闲。随后的 `sudo -n fuser` 显示 root 的 `rkaiq_3A_server` PID 404 持有 video0、1、2、15–19 和 subdev0、2、3；未列出 video11。PID 和占用均是检查瞬间的快照，采集前必须再次确认。3A 服务可能参与曝光、白平衡等，未停止或修改该服务。
- 内核日志有启动早期 `get remote sensor_sd failed`、`Failed to get power-gpios` 等信息，也有稍后探测到 IMX415、异步子设备注册完成的记录。仅凭这些日志不能判定当前采集失败或无影响；应以采集及新日志核验。
- 第一次日志脚本通过 Windows 管道传入时混入 CR，造成最后一项 shell 语法错误和远程退出 127；经远端 `tr -d '\r'` 后重新运行，SSH 退出 0，各项返回码见日志。不能把失败误记成摄像头失败。

## 单帧采集：已获项目负责人批准并执行

先在同一时刻重新查询格式与占用；若 video11 已被占用、符号链接变化或格式不再是下述 NV12 参数，则停止并重新评估，不抢占设备：

```sh
readlink -f /dev/video-camera0
v4l2-ctl -d /dev/video-camera0 --get-fmt-video
sudo -n fuser -v /dev/video11 /dev/video-camera0 /dev/media* /dev/video[0-9]* /dev/v4l-subdev* 2>&1
```

经负责人批准，实际只请求一帧，未使用 `--set-fmt-video` 修改格式。`mktemp` 建立私有、唯一的临时文件；`timeout` 限制阻塞。保存路径与退出码将记录，失败的空文件不视为成功：

```sh
umask 077
frame=$(mktemp /tmp/edgedoc-l01-XXXXXXXX.nv12) || exit 1
timeout 15s v4l2-ctl -d /dev/video-camera0 --stream-mmap=4 --stream-count=1 --stream-to="$frame"
rc=$?
printf 'capture_exit=%s\nframe=%s\n' "$rc" "$frame"
stat -c 'bytes=%s' "$frame"
```

仅有退出码或文件大小还不足以证明图像有效：应记录捕获命令完整输出、退出码、尺寸、像素格式及图像是否可辨认，必要时检查新内核日志。实际捕获文件可能包含私人内容，禁止提交到仓库。

## 图像复现说明

复现时需记录实际的像素格式、分辨率、stride、sizeimage 和颜色范围；不要公开原始帧。以本次观测参数为例，使用已有 FFmpeg 可转换：

```sh
ffmpeg -f rawvideo -pixel_format nv12 -video_size 1920x1080 -color_range pc -i "$frame" -frames:v 1 l01-preview.png
```

本次已使用板卡现有 FFmpeg 按该参数转换并人工检查（输出为唯一临时文件）；需根据采集时真实参数调整，特别是 stride、量化范围和色彩矩阵，且先确认 FFmpeg 已安装。预览图也可能包含私人内容，不上传。未经验证的图像转换不应冒充捕获结果。

## 下一轮待办

1. 排查本次采集时出现的 `write reg array error`，结合更完整日志确认其时序与影响。
2. 后续经批准，用测试文档和受控光照验证文字清晰度和重复采集稳定性。
3. 根据新日志定位跨 media 图的内部衔接、尺寸变化及 3A 服务作用；结果按证据更新本文件。
4. 若采集被占用或失败，保留原始错误，不停止其他进程或更改系统配置；先分析再提出下一步。

## 本次实际采集结果（2026-11-10）

项目负责人已批准单帧命令。采集前复查：别名指向 `/dev/video11`，NV12 1920×1080、stride 1920、sizeimage 3,110,400、Full Range；`sudo -n fuser` 没有列出 video11，但 `rkaiq_3A_server` 仍持有若干其他媒体节点。`/tmp` 可用约 2.0 GiB；检查与采集并非原子操作。

`v4l2-ctl` 输出 `<`、返回码 0，文件为 3,110,400 字节，与事前报告的 sizeimage 一致。使用板卡现有 FFmpeg 以 NV12、1920×1080 和 Full Range 转换后，预览能辨认天花板与灯具，证明这次获得了有效画面；这张图不是文档，不足以评估文档识别质量、色彩准确性或持续运行稳定性。原始帧和预览图保留在私有临时位置，不提交仓库。

采集后的内核日志记录了 CSI/D-PHY、IMX415 和 RKCIF 的流启动与停止，也出现 `imx415 6-001a: write reg array error`，随后仍完成本次出帧。错误根因和影响未确认；不能仅凭返回码为 0 忽略它，也不能仅凭一条消息判定硬件故障。需结合完整日志及后续经批准的实验排查。证据见 [L01_capture_commands.txt](L01_capture_commands.txt)。