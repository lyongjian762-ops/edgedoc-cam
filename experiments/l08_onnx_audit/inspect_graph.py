import hashlib
from collections import Counter
import numpy as np
import onnx
from onnx import helper, numpy_helper, shape_inference


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize(path):
    m = onnx.load(str(path))
    onnx.checker.check_model(m)
    inferred = shape_inference.infer_shapes(m)
    def value_info(v):
        t = v.type.tensor_type
        return {"name": v.name, "dtype": onnx.TensorProto.DataType.Name(t.elem_type),
                "shape": [d.dim_value if d.HasField('dim_value') else d.dim_param or '?' for d in t.shape.dim]}
    def attr(a):
        v = helper.get_attribute_value(a)
        if isinstance(v, bytes): return v.decode('utf-8', 'replace')
        if isinstance(v, (int, float, str)): return v
        if isinstance(v, (list, tuple)): return [x.decode('utf-8', 'replace') if isinstance(x, bytes) else x for x in v]
        return str(v)
    return {"sha256": sha(path), "ir_version": m.ir_version,
            "opsets": [{"domain": x.domain, "version": x.version} for x in m.opset_import],
            "inputs": [value_info(x) for x in m.graph.input], "outputs": [value_info(x) for x in m.graph.output],
            "inferred_values": [value_info(x) for x in inferred.graph.value_info],
            "nodes": [{"name": n.name, "op_type": n.op_type, "inputs": list(n.input), "outputs": list(n.output),
                       "attributes": {a.name: attr(a) for a in n.attribute}} for n in m.graph.node],
            "operator_counts": dict(Counter(n.op_type for n in m.graph.node)),
            "initializers": [{"name": t.name, "shape": list(t.dims), "dtype": str(numpy_helper.to_array(t).dtype),
                              "sha256": hashlib.sha256(numpy_helper.to_array(t).tobytes()).hexdigest()} for t in m.graph.initializer]}
