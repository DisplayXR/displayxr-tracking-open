#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: BSL-1.0
"""Make the vendored runtime headers in include/xrt vendor-neutral without changing the ABI.

The runtime's headers come from a lineage that names VR hardware: device and input enums for
specific controllers, face and body tracking types from vendor OpenXR extensions. This repo is
public and vendor-neutral, so the copies here carry none of it. The rewrite keeps every value the
provider exchanges with the runtime:

- ``enum xrt_device_name`` and ``enum xrt_tracking_type`` count implicitly, so every kept
  enumerator gets its value written out and the dropped ones leave a gap;
- the input and output name lists carry explicit values, so dropped entries change nothing;
- the face and body tracking types reach the provider only as pointers in ``struct xrt_device``,
  so they become incomplete declarations.

``tests/abi_check`` compares the result with the runtime's own headers (CI fetches them at the
pinned tag). Usage::

    neutralize_headers.py <runtime>/src/xrt/include/xrt include/xrt   # refresh from the runtime
    neutralize_headers.py --check include/xrt                          # the lint guard
"""

import argparse
import pathlib
import re
import sys

# Hardware, vendor and vendor-extension tokens. Matched case-insensitively against identifiers
# split on underscores and against comment words.
VENDOR = re.compile(
    r"(?<![a-z0-9])("
    r"vive|wmr|pico4?|pico_neo3|odyssey|samsung|oculus|valve|htc|quest(_[12])?|rift|hololens|knuckles|"
    r"hydra|psmv|pssense|daydream|ml2|meta|bytedance|varjo|reverb|g2|huawei|xbox|ultraleap|leap|"
    r"realsense|depthai|lighthouse|ligthhouse|blubur|oppo|fb|razer|hp|msft|index|go_controller|"
    r"touch|cosmos|focus3|neo3|tundra|sony|playstation|magic_leap|windows mixed"
    r")(?![a-z0-9])",
    re.IGNORECASE,
)

# Identifier parts that are vendor names only in identifiers ("index" is a word in comments).
ID_ONLY = {"index", "touch", "go_controller", "focus3", "cosmos", "neo3", "tundra", "g2", "hp"}


STRICT = {
    "vive", "vivepro", "wmr", "pico", "pico4", "neo3", "odyssey", "samsung", "oculus", "valve", "htc", "quest",
    "rift", "hololens", "knuckles", "hydra", "psmv", "pssense", "daydream", "ml2", "bytedance",
    "varjo", "reverb", "huawei", "xbox", "ultraleap", "leap", "realsense", "depthai", "lighthouse",
    "blubur", "oppo", "fb", "msft", "meta", "cosmos", "focus3", "tundra", "magicleap",
}
COMBOS = re.compile(
    r"(^|_)(index_(controller|haptic|system|a_|b_|squeeze|trigger|thumb|trackpad|grip|aim)|"
    r"touch_(controller|haptic|pro|plus|x_|y_|a_|b_|menu|system|squeeze|trigger|thumb|grip|aim)|"
    r"go_(controller|system|trigger|back|trackpad|grip|aim)|g2_|hp_|ml2_|mndx_)",
    re.IGNORECASE,
)


def vendor_in_identifier(name):
    parts = set(name.lower().split("_"))
    return bool(parts & STRICT) or bool(COMBOS.search(name))


def vendor_in_comment(text):
    for m in VENDOR.finditer(text.replace("_", " ")):
        if m.group(1).lower() in ID_ONLY:
            continue
        return True
    return False


def comment_part(line):
    """The comment text of a line, or None."""
    s = line.strip()
    if s.startswith(("//", "/*", "*")):
        return s
    i = line.find("//")
    return line[i:] if i >= 0 else None


def rewrite_implicit_enum(lines, start):
    """Pin the values of an implicitly counted enum, dropping vendor enumerators."""
    out = [lines[start], lines[start + 1]]  # "enum x" and "{"
    value = -1
    i = start + 2
    while not lines[i].startswith("};"):
        line = lines[i]
        m = re.match(r"\s*([A-Z][A-Z0-9_]*)\s*(=\s*([^,]+))?,?\s*(//.*)?$", line)
        if m:
            name = m.group(1)
            value = int(m.group(3), 0) if m.group(3) else value + 1
            if not vendor_in_identifier(name):
                trail = m.group(4) or ""
                if trail and vendor_in_comment(trail):
                    trail = ""
                out.append(f"\t{name} = {value},{(' ' + trail) if trail else ''}\n")
        elif comment_part(line) is not None and vendor_in_comment(line):
            pass
        else:
            out.append(line)
        i += 1
    return out, i


def drop_block(lines, start_pat, end_pat, replacement):
    s = next(i for i, l in enumerate(lines) if re.match(start_pat, l))
    e = next(i for i in range(s, len(lines)) if re.match(end_pat, lines[i]))
    return lines[:s] + replacement + lines[e:]


class _Gap(str):
    pass


DROPPED_GAP = _Gap("")


def neutralize(name, text):
    lines = text.splitlines(keepends=True)
    if name == "xrt_defines.h":
        lines = drop_block(
            lines,
            r"enum xrt_face_expression2_fb",
            r"/\*!\n",  # stops at the doc comment of enum xrt_output_name (checked below)
            [
                "/*\n",
                " * Face and body tracking. The provider never fills these; struct xrt_device\n",
                " * passes them by pointer, so incomplete types are enough here.\n",
                " */\n",
                "struct xrt_facial_expression_set;\n",
                "struct xrt_body_skeleton;\n",
                "struct xrt_body_joint_set;\n",
                "\n",
            ],
        )
        assert any(l.startswith("enum xrt_output_name") for l in lines)
    implicit = {"xrt_defines.h": ["enum xrt_device_name"], "xrt_tracking.h": ["enum xrt_tracking_type"]}
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if any(line.startswith(e) for e in implicit.get(name, [])):
            body, i = rewrite_implicit_enum(lines, i)
            out += body
            continue
        # List entries with explicit values: "_(XRT_INPUT_X, ...) \" and "XRT_OUTPUT_NAME_X = ...".
        m = re.match(r"\s*(?:_\()?(XRT_(?:INPUT|OUTPUT_NAME)_[A-Z0-9_]+)\s*[,=]", line)
        if m and vendor_in_identifier(m.group(1)[len("XRT_INPUT_"):]):
            out.append(DROPPED_GAP)
            i += 1
            continue
        if "body_tracking_calibration" in line:
            line = re.sub(r"_meta\b", "", line)
        c = comment_part(line)
        if c is not None and vendor_in_comment(c):
            line = neutral_comment(line)
            if line is None:
                out.append(DROPPED_GAP)
                i += 1
                continue
        out.append(line)
        i += 1
    return "".join(tidy(out))


def tidy(lines):
    """Fold the blank lines and bare list continuations that dropped entries leave behind."""
    out = []
    after_gap = False
    for line in lines:
        if line is DROPPED_GAP:
            after_gap = True
            continue
        s = line.strip()
        if after_gap and out and s in ("", "\\") and out[-1].strip() == s:
            continue
        if s not in ("", "\\"):
            after_gap = False
        out.append(line)
    return out


# Comment lines that carry a vendor word but whose point is neutral: rewritten, not dropped.
REWRITES = [
    (r"\(XR_FB_face_tracking,[ \t]*$", "(an extension's"),
    (r"XR_HTC_facial_tracking, etc\)\.", "data type)."),
    (r"\(XR_FB_body_tracking,[ \t]*$", "(an extension's"),
    (r"XR_META_body_tracking_full_body, etc\)\.", "joint set)."),
    (r"XR_META_body_tracking_calibration - body", "Body"),
    (r"A Leap Motion can be unplugged", "A tracker can be unplugged"),
    (r"behaviour \(ultraleap's ul_mount_offset_default \{0, 1\.45, -0\.10\} stays\)\.",
     "behaviour."),
    (r"by magnetic fields, eg\. Razer Hydra\.", "by magnetic fields."),
    (r"by Ligthhouse", "by base-station sweeps"),
    (r"not supported by Monado\.", "not supported by the runtime."),
]


def neutral_comment(line):
    for pat, rep in REWRITES:
        new = re.sub(pat, rep, line)
        if new != line:
            return new
    s = line.strip()
    if s.startswith("//") or (s.startswith("/**") and s.endswith("*/ \\")) or s.startswith("* ") or s == "*":
        return None  # a whole-line vendor comment: drop it
    return line


def scan(path):
    bad = []
    for n, line in enumerate(path.read_text().splitlines(), 1):
        for m in re.finditer(r"[A-Za-z_][A-Za-z0-9_]*", line):
            if "_" in m.group(0):
                if vendor_in_identifier(m.group(0)):
                    bad.append((n, line.strip()))
                    break
        else:
            c = comment_part(line)
            if c is not None and vendor_in_comment(c):
                bad.append((n, line.strip()))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    src = pathlib.Path(a.src)
    if a.check:
        failed = False
        for p in sorted(src.glob("*.h")):
            for n, l in scan(p):
                print(f"{p}:{n}: {l}")
                failed = True
        return 1 if failed else 0
    dst = pathlib.Path(a.dst)
    for p in sorted(dst.glob("*.h")):
        s = src / p.name
        p.write_text(neutralize(p.name, s.read_text()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
