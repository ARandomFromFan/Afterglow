#!/usr/bin/env python3
"""Fail when the host HLEs one overload or template instance of a main PLT import but not another.

The missing variant would fall through to lifted SDK code, as BroadcastConditionVariable and
GetTouchScreenStates<1> once did.
"""
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
import env

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = env.WORK_DIR


def plt_symbols(host):
    text = open(os.path.join(host, "main_plt_syms.h")).read()
    return re.findall(r'^\s*"([^"]+)",', text, re.M)


def host_hles(host):
    """Mangled names the host implements: `HLE(x)` macros and `void imp_x(Ctx` definitions in host/*.c"""
    names = set()
    for d in (host, os.path.join(host, "posix")):
        for f in os.listdir(d):
            if f.endswith(".c"):
                src = open(os.path.join(d, f), errors="replace").read()
                names |= set(re.findall(r"^HLE\((\w+)\)", src, re.M))
                names |= set(re.findall(r"^void imp_(\w+)\(Ctx", src, re.M))
    return names


def demangle(syms):
    tool = shutil.which("llvm-cxxfilt") or shutil.which("c++filt")
    if not tool:
        raise SystemExit("overload check: no llvm-cxxfilt or c++filt on PATH")
    out = subprocess.run([tool], input="\n".join(syms) + "\n", capture_output=True, text=True, check=True)
    lines = out.stdout.splitlines()
    if len(lines) != len(syms):
        raise SystemExit(f"overload check: {tool} returned {len(lines)} lines for {len(syms)} symbols")
    return dict(zip(syms, lines))


def strip_top(s, open_c, close_c):
    """s without its top-level open_c...close_c groups"""
    out, depth = [], 0
    for ch in s:
        if ch == open_c:
            depth += 1
        elif ch == close_c and depth:
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def qualified_name(demangled):
    """nn::hid::GetTouchScreenStates from `int nn::hid::GetTouchScreenStates<1>(nn::hid::TouchScreenState<1>*, int)`"""
    s = demangled.strip()
    if "(" not in s or "operator" in s:
        return s
    s = re.sub(r"\s*(const|volatile|&&|&)\s*$", "", s)
    depth, start = 0, None
    for i in range(len(s) - 1, -1, -1):   # the parameter list is the group closed by the last ')'
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                start = i
                break
    name = strip_top(s[:start], "<", ">") if start is not None else s
    return name.split(" ")[-1]   # (a template instance's demangled name starts with its return type)


def lifted_hooks(work):
    """Symbols the last lift hooked to host functions, or None without a lift"""
    path = os.path.join(work, "sa", "image", "image.json")
    if not os.path.exists(path):
        return None
    return {h["sym"] for h in json.load(open(path))["hooks"]}


def check(host=os.path.join(REPO, "host"), work=WORK):
    syms = plt_symbols(host)
    hles = host_hles(host)
    dm = demangle(syms)
    groups = defaultdict(list)
    for s in syms:
        groups[qualified_name(dm[s])].append(s)
    missing = []
    for name, variants in sorted(groups.items()):
        have = [v for v in variants if v in hles]
        if have and len(have) != len(variants):
            missing.append((name, have, [v for v in variants if v not in hles]))
    hooked = lifted_hooks(work)
    unwired = sorted(s for s in syms if s in hles and hooked is not None and s not in hooked)
    multi = sum(1 for v in groups.values() if len(v) > 1)
    print(f"Overload check: {len(syms)} main PLT imports, {len(groups)} qualified names, "
          f"{multi} with more than one variant, {len(missing)} partly implemented")
    for name, have, lack in missing:
        print(f"  {name}")
        for v in have:
            print(f"    HLE      {dm[v]}  [{v}]")
        for v in lack:
            print(f"    MISSING  {dm[v]}  [{v}]")
    if hooked is None:
        print("  (no sa/image/image.json: the lifted hooks were not checked)")
    else:
        print(f"Lifted hooks: {len(unwired)} main PLT imports with a host HLE that the last lift did not hook "
              f"(run scripts/lift.py)")
        for s in unwired:
            print(f"    UNWIRED  {dm[s]}  [{s}]")
    return not missing and not unwired


if __name__ == "__main__":
    sys.exit(0 if check(*sys.argv[1:3]) else 1)
