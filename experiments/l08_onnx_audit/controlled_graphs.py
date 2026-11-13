import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

EPSILON = 1e-5
PARAMS = {"W": np.array([[[[0.1, -0.2, 0.3], [0.0, 0.4, -0.1], [0.2, 0.1, -0.3]]],
                         [[[0.3, 0.2, -0.1], [-0.2, 0.1, 0.4], [0.0, -0.3, 0.2]]]], np.float32),
          "B": np.array([0.15, -0.12], np.float32), "scale": np.array([1.3, 0.7], np.float32),
          "bias": np.array([0.2, -0.3], np.float32), "mean": np.array([0.4, -0.2], np.float32),
          "var": np.array([0.6, 1.7], np.float32)}


def expected_fold():
    p = PARAMS
    a = p['scale'] / np.sqrt(p['var'] + EPSILON)
    return p['W'] * a[:, None, None, None], a * (p['B'] - p['mean']) + p['bias']


def build(path, multi=False):
    nodes = [helper.make_node('Conv', ['x', 'W', 'B'], ['raw'], name='conv', pads=[1, 1, 1, 1]),
             helper.make_node('BatchNormalization', ['raw', 'scale', 'bias', 'mean', 'var'], ['norm'], name='bn', epsilon=EPSILON, training_mode=0)]
    outs = [helper.make_tensor_value_info('raw', TensorProto.FLOAT, [1, 2, 8, 8]),
            helper.make_tensor_value_info('norm', TensorProto.FLOAT, [1, 2, 8, 8])] if multi else [helper.make_tensor_value_info('out', TensorProto.FLOAT, [1, 2, 8, 8])]
    if not multi: nodes.append(helper.make_node('Relu', ['norm'], ['out'], name='relu'))
    graph = helper.make_graph(nodes, 'multi' if multi else 'fold', [helper.make_tensor_value_info('x', TensorProto.FLOAT, [1, 1, 8, 8])], outs,
                              [numpy_helper.from_array(v, k) for k, v in PARAMS.items()])
    m = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 18)], producer_name='l08-audit')
    m.ir_version = 10
    onnx.checker.check_model(m)
    onnx.save(m, str(path))
