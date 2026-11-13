# Lesson 08.5：ONNX 导出与图优化审计

## 结果与边界

**状态：实验流程 PASS；真实性能结论有限。** 2026-11-13 在 Windows x64 / AMD Ryzen 7 5800H 上完成，使用 CPUExecutionProvider。TinyDocNet 是 1315 参数的未训练教学模型，不具备 OCR 分类能力；未连接 RK3588S，不能把结果解释为板端或 NPU 性能。

本次 PyTorch 2.12.0+cpu、NumPy 2.2.6、ONNX 1.23.2、ONNX Runtime 1.23.2、ONNX Script 0.7.2、Python 3.10.20。导出使用 `dynamo=True, optimize=False, opset_version=18, external_data=False`，模型 `eval()`，参考推理使用 `torch.inference_mode()`；ORT 设置 CPU provider、intra/inter 各 1 线程和顺序执行。实际版本、输入规则、BN 参数、hash 和其他选项见 [manifest](../../experiments/l08_onnx_audit/results/recorded/manifest.json)。

仓库和工作区中未找到任务书所列教材实验包的三个脚本；模型按任务书给出的具体层结构实现，参数量与预期吻合，但未能与教材文件逐行对照。

## Track A：导出与对齐

原始 ONNX 的 SHA-256 为 `7e80155721f15bf2614f7c48b6abffc9752a7a51a276d698bb38d2ece3815479`；实际 IR 10、opset 18，输入 `float32 [1,1,32,32]`，输出 `float32 [1,3]`。checker 通过；shape inference 为 24 个中间值推断形状，本次无未知维度。

| 图 | 关键算子计数 |
|---|---|
| PyTorch 原始导出（`optimize=False`） | Conv 2、BatchNormalization 1、Relu 2、AveragePool 1、ReduceMean 1、Gemm 1，另有 Constant/Reshape/Concat |
| ORT_DISABLE_ALL 保存快照 | Conv 2、BatchNormalization 1、Relu 2；部分常量表现形式发生变化 |
| ORT_ENABLE_BASIC 保存快照 | Conv 2、Relu 2，独立 BN 不再出现 |
| ORT_ENABLE_ALL 保存快照 | Conv 2、AveragePool、ReorderOutput、ReduceMean、Reshape、Gemm；含硬件相关布局变换 |

原始图明确保留 BN，故不能把导出前的图变换归因给 ORT。ORT 的 disabled 快照与原始 ONNX 的常量表示也有差异；仅凭快照不应把这些表示差异全部归为启用的图优化。完整节点、属性、initializer、形状与 hash 见 [graph_summary.json](../../experiments/l08_onnx_audit/results/recorded/graph_summary.json)。`ENABLE_ALL` 保存图可能依赖本机 CPU 布局，不能视为通用部署模型。

零、一、随机、脉冲、棋盘和分布外负值六种输入均通过 `atol=1e-5, rtol=1e-4`。PyTorch 与禁用优化 ORT 的最大绝对误差跨输入为 **1.49e-8**；禁用优化与 Basic 的最大误差为 **2.98e-8**。每项的 RMSE、形状、dtype、有限性、最大误差位置和数值见 [alignment.json](../../experiments/l08_onnx_audit/results/recorded/alignment.json)。输入与参考结果分别保存为 NPZ，种子为 42；负值输入单独标为分布外。

## Track B：折叠与多消费者

**实际观察到融合。** 受控图原始为 `Conv → BatchNormalization → Relu`；Basic 快照为 `Conv → Relu`。使用两个输出通道的固定 W、b、gamma、beta、mean、var 和 epsilon=1e-5，独立计算 `a=gamma/sqrt(var+epsilon)`、`W'=aW`、`b'=a(b-mean)+beta`。ORT Basic 图中 Conv 的 W'、b' 与独立公式的最大差异均为 **0**；独立 NumPy 卷积与禁用优化输出的最大误差为 **1.19e-7**。原始/优化参数及逐项指标见 [controlled.json](../../experiments/l08_onnx_audit/results/recorded/controlled.json)。

多消费者图以原始 Conv 输出和 BN 输出作为两个图输出。此版本 ORT Basic 保留了 `Conv + BatchNormalization`；禁用与 Basic 两条输出的最大误差均为 **0**。测试的成功条件是两条可观察输出均与独立参考及优化前对齐，不依赖“必须保留 BN”这一实现选择。

## 性能：只比较本次 PC CPU 测量

相同原始模型和输入、CPUExecutionProvider、单线程、每配置 30 次预热，3 个随机顺序区块各 100 次；`perf_counter_ns()` 测量 `session.run()` 调用，包含 Python 调用开销。初始化单独记录一次；下表时间单位为微秒，初始化为毫秒。全部 1500 条样本和顺序在 [benchmark_raw.csv](../../experiments/l08_onnx_audit/results/recorded/benchmark_raw.csv)，统计见 [benchmark_summary.json](../../experiments/l08_onnx_audit/results/recorded/benchmark_summary.json)。

| 图 / 优化 | 初始化 ms | 热态中位数 µs | P90 µs | 标准差 µs | 三个区块中位数 µs |
|---|---:|---:|---:|---:|---|
| Tiny / disabled | 1.999 | 61.4 | 82.17 | 13.50 | 61.7 / 61.1 / 61.25 |
| Tiny / basic | 1.606 | 54.3 | 78.62 | 15.20 | 54.4 / 54.2 / 54.3 |
| Tiny / all | 2.251 | 38.8 | 67.41 | 13.24 | 38.75 / 38.4 / 43.2 |
| Conv–BN / disabled | 1.070 | 19.8 | 39.71 | 9.13 | 19.8 / 19.5 / 23.8 |
| Conv–BN / basic | 0.861 | 17.9 | 23.01 | 4.90 | 18.2 / 17.9 / 17.8 |

最终一轮 Tiny Basic 三个区块的中位数均低于 disabled；但之前一轮同模型、同配置测得 Basic 78.55 µs、disabled 62.6 µs，方向相反。之前的 [原始样本](../../experiments/l08_onnx_audit/results/recorded/previous_run_benchmark_raw.csv)、[汇总](../../experiments/l08_onnx_audit/results/recorded/previous_run_benchmark_summary.json)和 [manifest](../../experiments/l08_onnx_audit/results/recorded/previous_run_manifest.json)也予保留。因此，**目前未观察到跨运行稳定可区分的 Basic 加速**。最终一轮受控图 Basic 中位数低 1.9 µs，差异小于样本波动，不足以证明稳定收益。`ALL` 在此单进程样本中中位数更低，但有布局优化和区块变化；它不是多进程或真实 OCR 工作负载的性能结论。计时器声明分辨率 100 ns，不等于测量精度；微型模型容易受 Python、调度和频率变化影响。每种初始化只测一次，不作统计推断；没有报告重复同一输入的 P99。

## Profiler 与接口依据

Profiler 在正式 benchmark 之后另建 Session，每配置执行 5 次。原始 trace 和 [profile_summary.json](../../experiments/l08_onnx_audit/results/recorded/profile_summary.json) 记录节点耗时、算子和可见 provider，所有观察到的节点均为 CPUExecutionProvider。受控图 profiling 中 disabled 的 Conv / BN / Relu 事件均值分别约 10.6 / 6.4 / 3.4 µs，Basic 的 Conv / Relu 约 12.0 / 3.8 µs。Profiler 本身引入开销，这些值不能与正式计时混用；它也不能证明硬件内部指令执行细节或板端执行位置。

接口依据：[PyTorch ONNX exporter 文档](https://docs.pytorch.org/docs/stable/onnx_export.html)说明 `dynamo` 与 `optimize`；[ORT 图优化文档](https://onnxruntime.ai/docs/performance/model-optimizations/graph-optimizations.html)说明优化等级和 `optimized_model_filepath`；[ORT profiling 文档](https://onnxruntime.ai/docs/performance/tune-performance/profiling-tools.html)说明 `enable_profiling` 和 JSON trace。文档为通用接口依据；本次具体观察以已安装版本和实验文件为准。

## 自动化与未解决项

`run_all.py` 中阶段结果和 [verification.json](../../experiments/l08_onnx_audit/results/recorded/verification.json) 为 PASS；独立 `unittest` **12 PASS / 0 FAIL / 0 SKIP**，输出见 [test_output.txt](../../experiments/l08_onnx_audit/results/recorded/test_output.txt)。测试包含参数量、输入契约、checker、数值对齐、折叠、多消费者、修改权重、错误缩放、NaN 和 NOT_RUN 不得算 PASS。首次尝试因 GBK 控制台无法打印导出器符号失败；UTF-8 重试后又暴露 NumPy 索引 JSON 序列化问题。两者修正与简要记录见 [first_attempt_failure.txt](../../experiments/l08_onnx_audit/results/recorded/first_attempt_failure.txt)。

仍待负责人审查：教材实验包源码未取得；真实应用输入、多个进程/设备、长时间温度与频率影响均未测。目录及复现命令见 [实验 README](../../experiments/l08_onnx_audit/README.md)。
