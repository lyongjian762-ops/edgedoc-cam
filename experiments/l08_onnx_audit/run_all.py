import json
import os
import platform
import subprocess
import sys
import time
import unittest
from pathlib import Path
HERE = Path(__file__).resolve().parent
OUT = HERE / 'results' / 'recorded'
try:
    import numpy as np
    import onnx
    import onnxruntime as ort
    import onnxscript
    import torch
    from onnx import numpy_helper
except ImportError as exc:
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT/'manifest.json').write_text(json.dumps({'python':sys.version,'platform':platform.platform(),'dependency_error':str(exc)},indent=2),encoding='utf-8')
    (OUT/'verification.json').write_text(json.dumps({'overall':'BLOCKED','stages':{'environment':'BLOCKED','tiny_export':'NOT_RUN','alignment':'NOT_RUN','controlled':'NOT_RUN','benchmark':'NOT_RUN','profiling':'NOT_RUN','tests':'NOT_RUN'},'error':str(exc)},indent=2),encoding='utf-8')
    raise SystemExit(f'BLOCKED: missing dependency: {exc}')
from model import make_model, make_inputs, validate_input, SEED
from inspect_graph import sha, summarize
from controlled_graphs import build, PARAMS, EPSILON, expected_fold
from compare_outputs import session, compare, ATOL, RTOL
from benchmark import run as benchmark
from profile_ort import profile

def all_stages_pass(stages):
    required = ('environment','tiny_export','alignment','controlled','benchmark','profiling','tests')
    return all(stages.get(key) == 'PASS' for key in required)


def dump(name, value):
    (OUT / name).write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False), encoding='utf-8')


def np_conv(x, w, b):
    padded = np.pad(x, ((0,0),(0,0),(1,1),(1,1)))
    y = np.empty((1,2,8,8), np.float32)
    for c in range(2):
        for i in range(8):
            for j in range(8): y[0,c,i,j] = np.sum(padded[0,0,i:i+3,j:j+3] * w[c,0]) + b[c]
    return y


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    stages = {k: 'NOT_RUN' for k in ['environment','tiny_export','alignment','controlled','benchmark','profiling','tests']}
    def stage(name, fn):
        try:
            result = fn(); stages[name] = 'PASS'; dump('verification.json', {'stages': stages}); return result
        except Exception as exc:
            stages[name] = 'FAIL'; dump('verification.json', {'stages': stages, 'error': repr(exc)}); raise
    dump('verification.json', {'stages': stages})
    torch.set_num_threads(1); torch.set_num_interop_threads(1)
    def environment():
        assert 'CPUExecutionProvider' in ort.get_available_providers()
        def git(*args): return subprocess.check_output(['git', *args], cwd=HERE, text=True, encoding='utf-8').strip()
        cpu=platform.processor()
        if os.name == 'nt':
            try: cpu=subprocess.check_output(['powershell','-NoProfile','-Command','(Get-CimInstance Win32_Processor).Name'],text=True,encoding='utf-8').strip()
            except (OSError,subprocess.SubprocessError): pass
        return {'platform': platform.platform(), 'architecture': platform.machine(), 'cpu': cpu, 'python': sys.version,
                'numpy': np.__version__, 'torch': torch.__version__, 'onnx': onnx.__version__, 'onnxruntime': ort.__version__, 'onnxscript': onnxscript.__version__,
                'providers_available': ort.get_available_providers(), 'provider_used': 'CPUExecutionProvider',
                'git_branch': git('branch','--show-current'), 'base_commit': git('merge-base','HEAD','origin/main'), 'git_head_at_run': git('rev-parse','HEAD'), 'worktree_status_at_start': git('status','--short'),
                'input_shape': [1,1,32,32], 'dtype': 'float32', 'normal_range': [0,1], 'negative_stress_range': [-1,1],
                'seed': SEED, 'input_generator': 'numpy.default_rng(42)', 'torch_rng': 'fork_rng; manual_seed(42)',
                'export': {'dynamo': True, 'optimize': False, 'opset': 18, 'external_data': False, 'export_params': True, 'keep_initializers_as_inputs': False, 'verify': False, 'report': False, 'profile': False, 'input_names':['x'], 'output_names':['output'], 'dynamic_shapes': None, 'do_constant_folding_default': True, 'eval': True, 'inference_mode': True, 'torch_threads': 1},
                'ort': {'provider': 'CPUExecutionProvider', 'intra_op_num_threads': 1, 'inter_op_num_threads': 1, 'execution_mode': 'ORT_SEQUENTIAL', 'levels': ['disabled','basic','all']},
                'benchmark': {'warmup': 30, 'blocks': 3, 'calls_per_block': 100, 'order': 'seeded shuffle per block', 'timer': 'perf_counter_ns', 'timer_resolution_s': time.get_clock_info('perf_counter').resolution, 'profile_separate': True}}
    manifest = stage('environment', environment)
    dump('manifest.json', manifest)
    model = make_model(); inputs = make_inputs()
    assert sum(p.numel() for p in model.parameters()) == 1315
    for x in inputs.values(): validate_input(x)
    tiny = OUT/'tiny_original.onnx'
    def tiny_export():
        np.savez(OUT/'inputs.npz', **inputs)
        with torch.inference_mode():
            refs = {k: model(torch.from_numpy(v)).numpy() for k,v in inputs.items()}
            np.savez(OUT/'pytorch_outputs.npz', **refs)
            torch.onnx.export(model, (torch.from_numpy(inputs['random']),), str(tiny), input_names=['x'], output_names=['output'], opset_version=18, dynamo=True, optimize=False, external_data=False)
        onnx.checker.check_model(onnx.load(str(tiny)))
        return refs
    refs = stage('tiny_export', tiny_export)
    graphs = {'tiny_original': summarize(tiny)}
    def alignment():
        rows = {}
        outputs = {}
        for level in ('disabled','basic','all'):
            opt = OUT/f'tiny_{level}.onnx'
            s = session(tiny, level, opt)
            graphs[f'tiny_{level}'] = summarize(opt)
            outputs[level] = {k: s.run(None, {'x': x})[0] for k,x in inputs.items()}
        for k in inputs:
            rows[k] = {'distribution': 'out_of_distribution' if k == 'negative_stress' else 'normal',
                       'pytorch_vs_disabled': compare(refs[k], outputs['disabled'][k]),
                       'disabled_vs_basic': compare(outputs['disabled'][k], outputs['basic'][k]),
                       'disabled_vs_all': compare(outputs['disabled'][k], outputs['all'][k])}
        dump('alignment.json', rows)
        assert all(v['pass'] for item in rows.values() for v in item.values() if isinstance(v, dict))
        return rows
    stage('alignment', alignment)
    b, multi = OUT/'conv_bn_original.onnx', OUT/'multi_original.onnx'
    bx = np.linspace(-1,1,64,dtype=np.float32).reshape(1,1,8,8)
    def controlled():
        build(b); build(multi, True)
        np.savez(OUT/'controlled_parameters.npz', **PARAMS, x=bx)
        for label,path in [('conv_bn',b),('multi',multi)]:
            graphs[label+'_original'] = summarize(path)
            for level in ('disabled','basic'):
                opt=OUT/f'{label}_{level}.onnx'; session(path,level,opt); graphs[f'{label}_{level}']=summarize(opt)
        disabled=session(b,'disabled').run(None,{'x':bx})[0]
        basic=session(b,'basic').run(None,{'x':bx})[0]
        wf,bf=expected_fold(); manual=np.maximum(np_conv(bx,wf,bf),0)
        optimized=onnx.load(str(OUT/'conv_bn_basic.onnx'))
        init={t.name:numpy_helper.to_array(t) for t in optimized.graph.initializer}
        conv=next(n for n in optimized.graph.node if n.op_type=='Conv')
        observed = 'BatchNormalization' not in [n.op_type for n in optimized.graph.node]
        weight_check = compare(wf, init[conv.input[1]]) if conv.input[1] in init else {'pass':False,'reason':'weights not initializer'}
        bias_check = compare(bf, init[conv.input[2]]) if len(conv.input)>2 and conv.input[2] in init else {'pass':False,'reason':'bias not initializer'}
        mo={level: session(multi,level).run(None,{'x':bx}) for level in ('disabled','basic')}
        raw=np_conv(bx,PARAMS['W'],PARAMS['B'])
        scale=PARAMS['scale']/np.sqrt(PARAMS['var']+EPSILON)
        norm=(raw-PARAMS['mean'][None,:,None,None])*scale[None,:,None,None]+PARAMS['bias'][None,:,None,None]
        result={'epsilon': EPSILON, 'parameters': {k:v.tolist() for k,v in PARAMS.items()}, 'expected_fold_W': wf.tolist(), 'expected_fold_B': bf.tolist(),
                'fusion_observed': observed, 'optimized_conv_inputs': list(conv.input), 'optimized_W': init[conv.input[1]].tolist() if conv.input[1] in init else None,
                'optimized_B': init[conv.input[2]].tolist() if len(conv.input)>2 and conv.input[2] in init else None,
                'weight_formula_vs_ort':weight_check, 'bias_formula_vs_ort':bias_check,
                'manual_vs_disabled':compare(manual,disabled), 'disabled_vs_basic':compare(disabled,basic),
                'multi_raw_reference':compare(raw,mo['disabled'][0]), 'multi_norm_reference':compare(norm,mo['disabled'][1]),
                'multi_raw_disabled_vs_basic':compare(mo['disabled'][0],mo['basic'][0]), 'multi_norm_disabled_vs_basic':compare(mo['disabled'][1],mo['basic'][1])}
        dump('controlled.json',result)
        assert all(result[k]['pass'] for k in ['manual_vs_disabled','disabled_vs_basic','multi_raw_reference','multi_norm_reference','multi_raw_disabled_vs_basic','multi_norm_disabled_vs_basic'])
        if observed: assert weight_check['pass'] and bias_check['pass']
        return result
    controlled_result=stage('controlled', controlled)
    dump('graph_summary.json',graphs)
    manifest.update({'model_sha256':sha(tiny), 'inputs_sha256':sha(OUT/'inputs.npz'), 'pytorch_outputs_sha256':sha(OUT/'pytorch_outputs.npz'),
                     'controlled_model_sha256':sha(b), 'multi_model_sha256':sha(multi), 'actual_graph':graphs['tiny_original'],
                     'bn_parameters': {'weight':model.bn1.weight.detach().tolist(), 'bias':model.bn1.bias.detach().tolist(), 'running_mean':model.bn1.running_mean.tolist(), 'running_var':model.bn1.running_var.tolist(), 'eps':model.bn1.eps}})
    dump('manifest.json',manifest)
    stats=stage('benchmark',lambda: benchmark({'tiny':tiny,'conv_bn':b},{'tiny':inputs['random'],'conv_bn':bx},OUT))
    dump('benchmark_summary.json',stats)
    def profiling():
        return {f'{track}_{level}':profile(path,x,level,OUT/f'profile_{track}_{level}.json') for track,path,x in [('tiny',tiny,inputs['random']),('conv_bn',b,bx)] for level in ('disabled','basic')}
    profiles=stage('profiling',profiling); dump('profile_summary.json',profiles)
    def tests():
        suite=unittest.defaultTestLoader.discover(str(HERE/'tests'),pattern='test_*.py')
        result=unittest.TextTestRunner(verbosity=2).run(suite)
        counts={'run':result.testsRun,'pass':result.testsRun-len(result.failures)-len(result.errors)-len(result.skipped), 'fail':len(result.failures)+len(result.errors), 'skipped':len(result.skipped)}
        dump('test_results.json',counts)
        if not result.wasSuccessful(): raise AssertionError(counts)
        return counts
    counts=stage('tests',tests)
    dump('verification.json',{'overall':'PASS' if all_stages_pass(stages) else 'NOT_PASS', 'stages':stages,'tests':counts,'fusion_observed':controlled_result['fusion_observed']})
    print(json.dumps({'stages':stages,'tests':counts,'fusion_observed':controlled_result['fusion_observed']},indent=2))


if __name__ == '__main__': main()
