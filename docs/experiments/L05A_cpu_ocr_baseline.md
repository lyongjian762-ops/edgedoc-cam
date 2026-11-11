# L05-A：Xiaomi-OCR-0 CPU 基线

## 范围、环境与证据边界

本实验使用合成图片和板卡本机 CPU，不使用摄像头、真实私人文档、NPU 或外部推理服务。执行前检查 `main` 的 `aab8c21393787e3e2567061b4873a76370b347e8`；PR #7、#8、#9、#10 均为 Draft，没有将其代码混入本分支。L02-R1 尚无真实摄像头回归，`imx415: write reg array error` 的根因仍未确认，详见 [L05-B/C 规划](L05B_C_stage_plan.md)。

2026-11-11 只读检查：LubanCat4，Linux `6.1.99-rk3588`，aarch64，Debian 12；Python 3.11.2，现有虚拟环境中 Pillow 13.0.0、openai 3.45.0。启动前可用内存约 3.6 GiB、无 swap，根文件系统可用约 38 GiB；仅 TCP 22 监听，8000 空闲。数值是该时刻观察，不代表其他时刻。板卡日期曾与项目日期不一致，服务返回的墙钟 `created` 不用于计时或实验日期判断。

服务器 `llama-server --version` 报 `0.8.0-dev (build 0, commit unknown)`，GNU 12.2.0 aarch64；构建缓存 `Release`、OpenMP 开启、CUDA/Vulkan 关闭。源码目录没有可读取的 Git 元数据，不能断言源码 commit 与二进制一一对应。ELF Build ID `827090eb407d0ea7c547841c836e476cb14be01f`；文件 SHA-256：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| llama-server | 71 KiB | `a4f7c117f272fc57be44ba39101599f9b32688a2ad6ba5b401e784044fa0b064` |
| Xiaomi-OCR-0.Q4_K_M.gguf | 505 MiB | `1e497ff06b91a3d822c543066df3e9ef69c62c5423be09d739dfa1db55630d3a` |
| Xiaomi-OCR-0.mmproj-bf16.gguf | 198 MiB | `634d6bc95781ebf4a3f06570a59859fa5082d488db540fd474bc6b4061aec90e` |

复用现有 `run-model.sh`：`127.0.0.1:8000`、alias `xiaomi-ocr-0`、`--threads 4 --ctx-size 2048 --batch-size 256 --ubatch-size 128 --parallel 1 --n-gpu-layers 0 --jinja`。不声称这些参数最优。启动前已获得负责人对单图验证及受控基准的批准；未安装依赖，未更改设备树、内核、网络或摄像头配置。服务不设置 API key，故严格限于板卡回环接口，停止后检查端口释放。

## 图像、协议和指标

`benchmarks/l05_cpu/fixtures/` 包含三张程序合成的英文印刷文本、对应参考文本及 SHA-256 清单；分别有 2、4、8 行，图像均为 1000×520 RGB PNG，无个人信息。固定文件及清单用于复现；生成脚本使用 Pillow 13.0.0 的默认字体，其他版本重新生成可能产生不同哈希。

本机只读审查的既有 `demo/server.py` 使用 `/v1/chat/completions`，内容数组先是 PNG base64 `image_url`，再是文本，非流式，temperature 0、top_p 0.8、presence_penalty 0、repetition_penalty 1、`enable_thinking=false`。本实验固定提示词 `Extract the text in the image.`（与既有 Demo 的 text-region 提示一致），输出上限 512，超时 180 秒。正式基准增加板上服务支持的 `cache_prompt=false`，避免相同图片的 KV 前缀缓存掩盖重复图像处理成本；须检查返回的 cached_tokens 验证实际行为。明确拒绝非回环 URL、空输出和非 `stop` 的 finish reason。所有请求串行，没有自动重试；若超时或连接中断，服务端可能仍在处理，停止后续请求并核对服务状态。

每张图单独启动服务，记录启动到 `/health` 返回 200 的时间；分别记录该图首次请求、1 次预热及 5 次热请求。请求耗时以客户端单调时钟计算，从 HTTP 发送到读取完整响应结束，不含 PNG 读取和 base64 编码；模型自身 timing 另保留在原始响应中。热请求统计使用均值、中位数、最小、最大，不对 5 个样本计算或声称可靠的 P99。

每秒读取进程 VmHWM、VmRSS、CPU tick，系统 MemAvailable 与可读 thermal zone；峰值是采样观察，VmHWM 为进程生命周期累计值，CPU 百分比以单核 100% 计，多核可以超过 100%。温度按 sysfs 毫摄氏度记录，具体标签以设备输出为准。低于 512 MiB 可用内存时守护进程停止自己启动的服务。CER 使用 Unicode NFKC 并去除空白后进行字符编辑距离计算，保留大小写和标点，参考文本分母不可为空。CER 不代替人工检查语义、遗漏和截断。

## 当前已完成的功能验证

在正式基准前，使用 sparse 图做一次独立服务的请求：HTTP 200，finish reason `stop`，文本与参考相符，去空白 CER 0；客户端耗时 105527.09 ms，prompt 517、completion 13 tokens；采样进程 VmHWM 991116 KiB，MemAvailable 最低 3291944 KiB。此请求用于功能验证，不混入正式热请求统计。

第一次停止时，终端中断与管理脚本重复发送信号，服务在清理阶段退出码 `-6`。确认端口释放后，隔离子进程会话并只转发一次中断信号；单独启动/停止复测服务退出码 `0`。两次真实记录均保留，不能把第一次异常写成成功。

发现缓存影响：使用旧客户端（未显式关闭默认缓存）重复 sparse 图片时，首次请求 106227 ms，5 次热请求中位数 768 ms；响应显示热请求 517 个 prompt token 中有 513 个被缓存。因此这些数字仅为缓存命中对照，不能当作完整图像处理延迟；正式基准改为 `cache_prompt=false`，并保持三张图片的其余参数一致。

## 正式 Benchmark 结果

**状态：未完成，未达到 L05-A 阶段门槛。** 稀疏和中等密度组各有完整的首次、一次预热、五次热请求；高密度组在测试期间因风扇没有转动而主动中止，尚缺五次有效热请求。历史 33.67 秒 smoke 和本次独立功能请求均不纳入正式热请求统计。

| 图片 | 服务就绪 ms | 首次请求 ms | 热请求数 | 热均值 / 中位数 ms | 热最小 / 最大 ms | 热请求 CER |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| sparse，2 行 | 3021.08 | 105894.26 | 5 | 106617.56 / 106620.83 | 106375.90 / 106946.01 | 5 次均为 0 |
| medium，4 行 | 3017.61 | 107820.22 | 5 | 107799.68 / 107799.76 | 107707.45 / 107923.97 | 5 次均为 0 |
| dense，8 行 | 3017.51 | 109625.96 | 0 | 未完成 | 未完成 | 未完成 |

已完成两组的全部 14 次响应均为 HTTP 200、`finish_reason=stop`，返回 `cached_tokens=0`；逐条对照参考文本，除空白差异外一致。每次 prompt 为 517 tokens；sparse 输出 13 tokens，medium 输出 31 tokens。dense 首次和预热分别 109625.96、109494.75 ms，均 HTTP 200、stop、cached_tokens=0、CER=0，输出 67 tokens；这两次不属于热统计。dense 第一次热请求因主动停服收到 `Remote end closed connection without response`，不可当作成功或热延迟样本。

| 组 | 进程峰值 VmHWM KiB | 最低 MemAvailable KiB | 采样峰值进程 CPU（单核 100%） | 采样最高 SoC / 大核 °C |
| --- | ---: | ---: | ---: | ---: |
| sparse | 1091416 | 3190676 | 401.36% | 64.692 / 71.153 |
| medium | 1091116 | 3193316 | 402.29% | 69.307 / 75.769 |
| dense，未完成 | 1025440 | 3260988 | 401.28% | 70.230 / 75.769 |

以上资源峰值反映该次采样与运行条件，不代表整机功耗或长期极限。测试过程中负责人观察风扇未转，软件只读检查显示 `pwm-fan` 启用，控制值由 200 升至 255，但没有转速反馈，无法确定插座电压及机械转动。dense 组运行中读取大核 75.769°C、SoC 70.230°C 时主动停止；这不是声称达到温度安全极限。停止后检查无本次 `llama-server` 进程、8000 端口无监听，随后大核约 54°C。风扇规格、接线和根因仍需断电检查，修复并确认散热后再讨论补测。该实验没有更改风扇配置。

停止 dense 时服务仍在处理请求：守护器先发送 SIGINT，等待后发送终止信号，服务日志出现第二次中断，服务最终退出码为 `1`，客户端记录连接关闭。这个退出码是**主动中断时的实际结果**；它不同于 sparse、medium 服务停止后退出码 `0`。所有日志均保留。

非敏感 [结果证据目录](../../benchmarks/l05_cpu/results/recorded/) 包含原始响应、设置、资源采样、服务与守护日志。服务日志仅将板卡用户目录替换为 `<HOME>/`；板卡时钟不准确，日志墙钟不作为延迟证据。服务多次输出 `find_slot: non-consecutive token position` 警告；现有样本输出完整，但成因未确认，不能据此断言运行时完全无问题。

结论只适用于这三张英文合成图片和当前 CPU 配置，尚未验证中文、真实摄像头图像、不同光照或持续稳定性。`llama-server` 的源码提交未知；缓存关闭已由响应实际字段核对，但配置和结果仍须由负责人/导师审查。风扇问题解决并完成 dense 五次热请求前，不进入 L05-B。

## 复现与后续阶段

完整复现需经负责人批准启动 CPU 服务，且先确认 8000 空闲、所用模型文件与哈希一致。服务和基准分别在板卡本机运行；将本目录复制到受控实验目录，确保保留 `fixtures/` 的相对路径。

```sh
python3 server_guard.py --output /tmp/l05a-service-sparse
# 另一个终端，从 guard 的 ready 事件取得 PID；不要手写或猜测已有服务 PID。
python3 run.py --case sparse --pid <本次服务PID> --output /tmp/l05a-sparse --warmups 1 --repeat 5 --timeout 180
# 首个终端 Ctrl-C，检查 guard 的 server_stopped 和端口；其他两张图按相同方法重新启动。
```

`requests.jsonl` 每行保存阶段、延迟、结果、token、完整原始响应或错误；`config.json` 固定参数；`resource_samples.json` 和 `summary.json` 保存资源采样和汇总。Mock 检查可在电脑运行 `python -m unittest discover -s tests -p test_l05_cpu.py -v`，只模拟 HTTP，不执行推理。

另外可运行 `python benchmarks/l05_cpu/audit_results.py benchmarks/l05_cpu/results/recorded/sparse-requests.jsonl benchmarks/l05_cpu/fixtures/sparse.txt` 检查次数、响应、参考文本和缓存字段；非零退出表示检查发现问题。该脚本不能替代人工质量审阅；对已中断的 dense 记录运行默认检查应报缺失热请求及中断错误。

L05-B、L05-C 必须分别达到前置阶段门槛并获得负责人同意才实施。当前文档中的计划不是实际验收。
