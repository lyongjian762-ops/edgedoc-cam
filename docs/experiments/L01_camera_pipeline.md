# Lesson 01：IMX415 媒体链路与单帧采集基线

状态：**只读检查完成；单帧采集等待项目负责人批准，尚未执行。** 关联 Issue #2。本次使用 SSH 执行只读命令，没有改变设备树、内核、网络、启动配置或安装软件。

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

## 采集计划：尚未批准，尚未执行

先在同一时刻重新查询格式与占用；若 video11 已被占用、符号链接变化或格式不再是下述 NV12 参数，则停止并重新评估，不抢占设备：

```sh
readlink -f /dev/video-camera0
v4l2-ctl -d /dev/video-camera0 --get-fmt-video
sudo -n fuser -v /dev/video11 /dev/video-camera0 /dev/media* /dev/video[0-9]* /dev/v4l-subdev* 2>&1
```

经负责人批准、检查条件仍满足后，计划只请求一帧，不使用 `--set-fmt-video` 修改格式。`mktemp` 建立私有、唯一的临时文件；`timeout` 限制阻塞。保存路径与退出码将记录，失败的空文件不视为成功：

```sh
umask 077
frame=$(mktemp /tmp/edgedoc-l01-XXXXXXXX.nv12) || exit 1
timeout 15s v4l2-ctl -d /dev/video-camera0 --stream-mmap=4 --stream-count=1 --stream-to="$frame"
rc=$?
printf 'capture_exit=%s\nframe=%s\n' "$rc" "$frame"
stat -c 'bytes=%s' "$frame"
```

仅有退出码或文件大小还不足以证明图像有效：应记录捕获命令完整输出、退出码、尺寸、像素格式及图像是否可辨认，必要时检查新内核日志。实际捕获文件可能包含私人内容，禁止提交到仓库。

## 图像复现说明（采集前的计划）

若采集成功，记录当时实际的像素格式、分辨率、stride、sizeimage、颜色范围和文件校验值；文件仅在板卡的临时目录本地保存。以本次观测参数为例，已有 FFmpeg 时可在本地尝试：

```sh
ffmpeg -f rawvideo -pixel_format nv12 -video_size 1920x1080 -color_range pc -i "$frame" -frames:v 1 l01-preview.png
```

该命令仅为转换说明，尚未执行；需根据采集时真实参数调整，特别是 stride、量化范围和色彩矩阵，且先确认 FFmpeg 已安装。预览图也可能包含私人内容，不上传。未经验证的图像转换不应冒充捕获结果。

## 下一轮待办

1. 项目负责人审批单帧命令；重新检查占用、格式、节点和可用空间。
2. 采集一帧并记录成功或失败的证据；复核字节数和实际图像内容。
3. 根据新日志定位跨 media 图的内部衔接、尺寸变化及 3A 服务作用；结果按证据更新本文件。
4. 若采集被占用或失败，保留原始错误，不停止其他进程或更改系统配置；先分析再提出下一步。
