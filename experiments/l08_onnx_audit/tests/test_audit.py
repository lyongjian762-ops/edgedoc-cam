import json
import sys
import tempfile
import unittest
from pathlib import Path
import numpy as np
import onnx
import torch
from onnx import numpy_helper

ROOT=Path(__file__).resolve().parents[1]; sys.path.insert(0,str(ROOT))
from model import make_model, make_inputs, validate_input
from compare_outputs import compare, session
from controlled_graphs import PARAMS, EPSILON, expected_fold
from run_all import np_conv

OUT=ROOT/'results'/'recorded'

class AuditTests(unittest.TestCase):
    def test_parameter_count(self): self.assertEqual(sum(p.numel() for p in make_model().parameters()),1315)
    def test_input_contract(self):
        validate_input(make_inputs()['random'])
        with self.assertRaises(ValueError): validate_input(np.zeros((1,1,16,16),np.float32))
        with self.assertRaises(ValueError): validate_input(np.zeros((1,1,32,32),np.float64))
    def test_model_output_and_determinism(self):
        x=torch.from_numpy(make_inputs()['random'])
        with torch.inference_mode(): a=make_model()(x).numpy(); b=make_model()(x).numpy()
        self.assertEqual(a.shape,(1,3)); self.assertTrue(np.isfinite(a).all()); np.testing.assert_array_equal(a,b)
    def test_onnx_checker(self): onnx.checker.check_model(onnx.load(str(OUT/'tiny_original.onnx')))
    def test_pytorch_vs_ort(self):
        model=make_model(); s=session(OUT/'tiny_original.onnx','disabled')
        for x in make_inputs().values():
            with torch.inference_mode(): reference=model(torch.from_numpy(x)).numpy()
            self.assertTrue(compare(reference,s.run(None,{'x':x})[0])['pass'])
    def test_optimization_alignment(self):
        a=session(OUT/'tiny_original.onnx','disabled'); b=session(OUT/'tiny_original.onnx','basic')
        for x in make_inputs().values(): self.assertTrue(compare(a.run(None,{'x':x})[0],b.run(None,{'x':x})[0])['pass'])
    def test_fold_formula(self):
        w,b=expected_fold(); x=np.linspace(-1,1,64,dtype=np.float32).reshape(1,1,8,8)
        reference=np.maximum(np_conv(x,w,b),0)
        self.assertTrue(compare(reference,session(OUT/'conv_bn_original.onnx','disabled').run(None,{'x':x})[0])['pass'])
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'optimized.onnx'; session(OUT/'conv_bn_original.onnx','basic',path)
            graph=onnx.load(str(path)).graph
            if not any(n.op_type=='BatchNormalization' for n in graph.node):
                conv=next(n for n in graph.node if n.op_type=='Conv'); values={t.name:numpy_helper.to_array(t) for t in graph.initializer}
                self.assertTrue(compare(w,values[conv.input[1]])['pass']); self.assertTrue(compare(b,values[conv.input[2]])['pass'])
    def test_multi_consumers(self):
        x=np.linspace(-1,1,64,dtype=np.float32).reshape(1,1,8,8)
        raw=np_conv(x,PARAMS['W'],PARAMS['B']); a=PARAMS['scale']/np.sqrt(PARAMS['var']+EPSILON)
        norm=(raw-PARAMS['mean'][None,:,None,None])*a[None,:,None,None]+PARAMS['bias'][None,:,None,None]
        for level in ('disabled','basic'):
            outputs=session(OUT/'multi_original.onnx',level).run(None,{'x':x})
            self.assertTrue(compare(raw,outputs[0])['pass']); self.assertTrue(compare(norm,outputs[1])['pass'])
    def test_mutated_weight_detected(self):
        original=OUT/'conv_bn_original.onnx'; m=onnx.load(str(original)); a=numpy_helper.to_array(m.graph.initializer[0]).copy(); a.flat[0]+=2.0
        m.graph.initializer[0].CopyFrom(numpy_helper.from_array(a,m.graph.initializer[0].name))
        x=np.linspace(-1,1,64,dtype=np.float32).reshape(1,1,8,8)
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'mutated.onnx'; onnx.save(m,str(path)); a=session(original,'disabled').run(None,{'x':x})[0]; b=session(path,'disabled').run(None,{'x':x})[0]
        self.assertFalse(compare(a,b)['pass'])
    def test_incorrect_scaling_detected(self):
        x=make_inputs()['random']; s=session(OUT/'tiny_original.onnx','disabled')
        self.assertFalse(compare(s.run(None,{'x':x})[0],s.run(None,{'x':x*0.1})[0])['pass'])
    def test_nonfinite_fails(self): self.assertFalse(compare(np.array([np.nan],np.float32),np.array([np.nan],np.float32))['pass'])
    def test_not_run_not_pass(self):
        from run_all import all_stages_pass
        stages={k:'PASS' for k in ('environment','tiny_export','alignment','controlled','benchmark','profiling','tests')}
        self.assertTrue(all_stages_pass(stages)); stages['profiling']='NOT_RUN'; self.assertFalse(all_stages_pass(stages))

if __name__=='__main__': unittest.main()
