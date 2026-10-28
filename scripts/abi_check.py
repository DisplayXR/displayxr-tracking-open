#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: BSL-1.0
"""Check that include/xrt has the runtime's ABI: every enumerator value, every struct size, and
the offset of every member of struct xrt_device equal what the runtime's own headers give.

    abi_check.py <runtime>/src/xrt/include [--ours include]

The neutral headers (scripts/neutralize_headers.py) drop vendor enumerators and rename two members;
this compiles one probe against each header set and diffs the output.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

HEADERS = ["xrt_device.h", "xrt_tracking.h", "xrt_plane_detector.h", "xrt_visibility_mask.h", "xrt_input_plugin.h", "xrt_tracking_plugin.h", "xrt_camera_pose.h",
           "xrt_component_settings.h"]
# Members the neutral headers rename: ours -> the runtime's.
RENAMED = {
    "reset_body_tracking_calibration": "reset_body_tracking_calibration_meta",
    "set_body_tracking_calibration_override": "set_body_tracking_calibration_override_meta",
}


def strip_comments(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def enumerators(inc):
    names = []
    for p in sorted((inc / "xrt").glob("*.h")):
        t = strip_comments(p.read_text())
        for body in re.findall(r"\benum\s+\w+\s*\{(.*?)\}", t, flags=re.S):
            names += re.findall(r"^\s*([A-Z][A-Z0-9_]*)\s*(?:=|,|$)", body, flags=re.M)
        names += re.findall(r"_\((XRT_INPUT_[A-Z0-9_]+)\s*,", t)
    return sorted(set(n for n in names if not n.endswith("MAX_ENUM")))


def structs(inc):
    found = []
    for p in sorted((inc / "xrt").glob("*.h")):
        found += re.findall(r"^struct\s+(\w+)\s*\{", strip_comments(p.read_text()), flags=re.M)
    return sorted(set(found))


def device_members(inc):
    t = strip_comments((inc / "xrt" / "xrt_device.h").read_text())
    body = re.search(r"^struct xrt_device\s*\{(.*?)^\};", t, flags=re.S | re.M).group(1)
    members = re.findall(r"\(\s*\*\s*(\w+)\s*\)\s*\(", body)
    members += re.findall(r"^\s*(?:const\s+)?(?:struct|enum)?\s*\w+\s*\**\s*(\w+)\s*(?:\[[^\]]*\])?;",
                          body, flags=re.M)
    return sorted(set(members))


def probe(inc, enums, structs_, members, rename):
    src = "#include <stdio.h>\n#include <stddef.h>\n"
    src += "".join(f'#include "xrt/{h}"\n' for h in HEADERS)
    src += "int main(void) {\n"
    src += "".join(f'\tprintf("enum {e} %lld\\n", (long long){e});\n' for e in enums)
    src += "".join(f'\tprintf("sizeof {s} %zu\\n", sizeof(struct {s}));\n' for s in structs_)
    for m in members:
        real = rename.get(m, m)
        src += f'\tprintf("offset xrt_device.{m} %zu\\n", offsetof(struct xrt_device, {real}));\n'
    src += "\treturn 0;\n}\n"
    with tempfile.TemporaryDirectory() as d:
        c = pathlib.Path(d) / "probe.c"
        exe = pathlib.Path(d) / "probe"
        c.write_text(src)
        subprocess.run(["cc", "-std=c11", "-w", "-I", str(inc), str(c), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runtime_include")
    ap.add_argument("--ours", default=str(pathlib.Path(__file__).resolve().parent.parent / "include"))
    a = ap.parse_args()
    ours, theirs = pathlib.Path(a.ours), pathlib.Path(a.runtime_include)
    enums, structs_, members = enumerators(ours), structs(ours), device_members(ours)
    mine = probe(ours, enums, structs_, members, {})
    real = probe(theirs, enums, structs_, members, RENAMED)
    if mine != real:
        a_, b_ = mine.splitlines(), real.splitlines()
        for x, y in zip(a_, b_):
            if x != y:
                print(f"ours: {x}\nruntime: {y}")
        return 1
    print(f"ABI matches: {len(enums)} enumerators, {len(structs_)} struct sizes, "
          f"{len(members)} xrt_device offsets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
