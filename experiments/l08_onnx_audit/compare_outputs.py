import numpy as np
import onnxruntime as ort

LEVELS = {'disabled': ort.GraphOptimizationLevel.ORT_DISABLE_ALL, 'basic': ort.GraphOptimizationLevel.ORT_ENABLE_BASIC, 'all': ort.GraphOptimizationLevel.ORT_ENABLE_ALL}
ATOL, RTOL = 1e-5, 1e-4


def session(path, level, optimized=None, profile=False):
    o = ort.SessionOptions()
    o.graph_optimization_level = LEVELS[level]
    o.intra_op_num_threads = o.inter_op_num_threads = 1
    o.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    if optimized: o.optimized_model_filepath = str(optimized)
    o.enable_profiling = profile
    return ort.InferenceSession(str(path), sess_options=o, providers=['CPUExecutionProvider'])


def compare(a, b):
    a, b = np.asarray(a), np.asarray(b)
    if a.shape != b.shape or a.dtype != b.dtype or not np.isfinite(a).all() or not np.isfinite(b).all():
        return {'pass': False, 'reason': 'shape, dtype or non-finite', 'shape_a': list(a.shape), 'shape_b': list(b.shape), 'dtype_a': str(a.dtype), 'dtype_b': str(b.dtype)}
    delta = np.abs(a.astype(np.float64) - b.astype(np.float64))
    index = np.unravel_index(np.argmax(delta), delta.shape)
    return {'pass': bool(np.allclose(a, b, atol=ATOL, rtol=RTOL)), 'shape': list(a.shape), 'dtype': str(a.dtype),
            'finite': True, 'max_abs': float(delta[index]), 'rmse': float(np.sqrt(np.mean(delta ** 2))),
            'index': [int(i) for i in index], 'a_value': float(a[index]), 'b_value': float(b[index]),
            'argmax_equal': bool(np.argmax(a) == np.argmax(b)), 'atol': ATOL, 'rtol': RTOL}
