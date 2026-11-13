# Lesson 08.5：ONNX 导出与图优化审计

两条互相独立的路线：TinyDocNet 的 PyTorch → ONNX → ORT 对齐，以及手工 ONNX Conv–BN 折叠和多消费者语义。TinyDocNet 权重是固定种子的未训练参数，不具有 OCR 分类能力。

本次电脑环境为 Windows x64、Python 3.10.20、PyTorch 2.12.0+cpu；隔离 venv 继承已有 CPU 版 PyTorch/NumPy，其余依赖安装在 venv。完整实际版本、导出参数和输入 hash 见 `results/recorded/manifest.json`。结果仅描述本机 CPU，不能推断板卡性能。

## 复现

使用 Python 3.10 隔离环境并按 `requirements.txt` 安装依赖（安装前遵守所在环境的下载/安装审批）。本机复现命令，PowerShell：

```powershell
$env:PYTHONIOENCODING='utf-8'
& 'D:\learn_linux\.venvs\l08_onnx\Scripts\python.exe' run_all.py
& 'D:\learn_linux\.venvs\l08_onnx\Scripts\python.exe' -m unittest discover -s tests -p 'test_*.py' -v
```

其他机器替换 Python 路径；先运行 `run_all.py` 生成 `.onnx` 模型与记录，再运行独立测试。仓库按项目规则忽略 `.onnx` 模型文件；JSON 图结构、随机输入/参考输出、参数、Profiler 和时延样本提供审查证据。若流程中途失败，`verification.json` 将保留 FAIL/BLOCKED 和未运行状态；不要把上次留存的文件当作本次成功。

`model.py` 定义模型及输入；`run_all.py` 导出与编排；`inspect_graph.py` 检查图；`controlled_graphs.py` 构造模型并给出折叠公式；`compare_outputs.py` 定义比较器和 Session；`benchmark.py` 做独立计时；`profile_ort.py` 单独采集 profiler；`tests/test_audit.py` 包含正反向测试。详见 `docs/experiments/L08_onnx_graph_audit.md`。
