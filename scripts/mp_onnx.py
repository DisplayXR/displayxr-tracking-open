# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Graph passes that turn tf2onnx output of the MediaPipe face models into the
pack's ONNX blobs, byte for byte reproducibly.

Every pass is numerically a no-op (the CPU execution provider's output is
bit-identical before and after); together they make the bytes depend only on
the pinned upstream .task and the pinned converter versions.

  fix_dims        every symbolic dim on a graph input / output -> 1 (the same
                  rewrite as onnxruntime.tools.make_dynamic_shape_fixed, but for
                  whatever name tf2onnx gave the batch dim, which is random)
  fix_for_coreml  explicit zero 'pads' on Conv / MaxPool (Core ML ML Program
                  refuses a missing 'pad'), PRelu slope [1,C,1,1] -> [C,1,1]
                  (Core ML wants [C,1,1] or a scalar), explicit Pad value
  canonicalize    tf2onnx names constants and intermediate tensors from a
                  hash-ordered walk, so two runs name them differently; rename
                  every node / intermediate tensor / initializer by its position
                  (inputs and outputs keep their names), order initializers by
                  first use, drop value_info and doc strings
"""

import numpy as np
import onnx
from onnx import helper
from onnx import numpy_helper as nh
from onnxruntime.tools.onnx_model_utils import make_dim_param_fixed


def fix_dims(model):
    params = set()
    for vi in list(model.graph.input) + list(model.graph.output):
        for d in vi.type.tensor_type.shape.dim:
            if d.HasField("dim_param"):
                params.add(d.dim_param)
    for p in sorted(params):
        make_dim_param_fixed(model.graph, p, 1)
    return len(params)


def fix_for_coreml(model):
    g = model.graph
    init = {i.name: i for i in g.initializer}
    n_pad = n_prelu = n_padv = 0
    zero = None
    for n in g.node:
        if n.op_type in ("Conv", "MaxPool"):
            names = [a.name for a in n.attribute]
            if "pads" not in names and "auto_pad" not in names:
                n.attribute.append(helper.make_attribute("pads", [0, 0, 0, 0]))
                n_pad += 1
        elif n.op_type == "PRelu" and n.input[1] in init:
            t = init[n.input[1]]
            a = nh.to_array(t)
            if a.ndim == 4 and a.shape[0] == 1:
                t.CopyFrom(nh.from_array(a.reshape(a.shape[1:]), t.name))
                n_prelu += 1
        elif n.op_type == "Pad" and len(n.input) == 2:
            if zero is None:
                zero = nh.from_array(np.array(0, np.float32), "coreml_fix_pad_zero")
                g.initializer.append(zero)
            n.input.append(zero.name)
            n_padv += 1
    return n_pad, n_prelu, n_padv


def canonicalize(model):
    g = model.graph
    keep = {i.name for i in g.input} | {o.name for o in g.output}
    init = {i.name: i for i in g.initializer}
    rename = {}
    order = []
    for ni, n in enumerate(g.node):
        for x in n.input:
            if x in init and x not in rename:
                rename[x] = "c%d" % len(order)
                order.append(x)
        for j, o in enumerate(n.output):
            if o and o not in keep:
                rename[o] = "t%d_%d" % (ni, j)
        n.name = "n%d" % ni
        n.doc_string = ""
    for n in g.node:
        n.input[:] = [rename.get(x, x) for x in n.input]
        n.output[:] = [rename.get(x, x) for x in n.output]
    used = []
    for x in order:
        t = init[x]
        t.name = rename[x]
        used.append(t)
    del g.initializer[:]
    g.initializer.extend(used)
    del g.value_info[:]
    model.doc_string = ""
    g.doc_string = ""
    return len(used)


def finish(src_path, dst_path):
    """tf2onnx output -> pack blob. Returns a one-line summary."""
    m = onnx.load(src_path)
    dims = fix_dims(m)
    pads, prelu, padv = fix_for_coreml(m)
    inits = canonicalize(m)
    onnx.checker.check_model(m)
    onnx.save(m, dst_path)
    return "dims fixed %d, pads +%d, prelu slopes %d, pad values %d, initializers %d" % (dims, pads, prelu, padv, inits)
