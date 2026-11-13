#!/usr/bin/env python3
"""Align the recomp main-PLT trace with the Lichii trace and report every divergence and stub hit."""
import argparse
import difflib
import json
import re
import sys
from collections import Counter

TRACE_RE = re.compile(r"\[HLE TRACE (\d+)\]\s*(?:\[TID\s*([^\]]+)\]\s*)?([^\s\(]+)")
SYNC_LR_RE = re.compile(r"\[hos MainThread\] \[SYNC\].*?lr=(0x[0-9a-fA-F]+)")
STUB_RE = re.compile(r"\[STUB #(\d+)\] (Whitelisted non-aborting stub|FATAL: Unimplemented imported symbol called): (\S+)(.*)")
COLLAPSE = ("BroadcastConditionVariable", "SignalConditionVariable")
END_SYM = "nvnBootstrapLoader"


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--actual", required=True, help="recomp log (HOS_TRACE=1)")
    p.add_argument("--expected", required=True, help="Lichii trace from capture_lichii_gdb.py")
    p.add_argument("--explain", default=None, help="JSON list of known nondeterministic call groups")
    return p.parse_args()


def extract_calls(path):
    """Returns [(sym, lr or None)], with runs of identical Broadcast/SignalConditionVariable collapsed to one."""
    calls, pending = [], None
    with open(path, "r", errors="replace") as f:
        for line in f:
            if line.lstrip().startswith("#"):
                continue
            m = TRACE_RE.search(line)
            if m:
                sym = m.group(3)
                pending = None
                if calls and calls[-1][0] == sym and any(k in sym for k in COLLAPSE):
                    continue
                calls.append([sym, None])
                pending = calls[-1]
                continue
            if pending is not None:
                m = SYNC_LR_RE.search(line)
                if m:
                    pending[1] = int(m.group(1), 16)
                    pending = None
    return [tuple(c) for c in calls]


def extract_stubs(path):
    stubs = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = STUB_RE.search(line)
            if m and m.group(3) not in stubs:
                stubs[m.group(3)] = (int(m.group(1)), m.group(2), m.group(4).strip())
    return stubs


def load_explain(path):
    if not path:
        return []
    with open(path) as f:
        groups = json.load(f)
    for g in groups:
        g["lrs"] = {int(x, 16) for x in g.get("lrs", [])}
    return groups


def explain_part(entries, groups, has_lr):
    """Name of the first group that covers every entry, or None. Lichii entries carry no lr."""
    for g in groups:
        if all(any(s in sym for s in g["symbols"]) and (not has_lr or not g["lrs"] or lr in g["lrs"])
               for sym, lr in entries):
            return g["name"]
    return None


def short(sym):
    return re.sub(r"^_ZN2nn2os\d+", "nn::os::", sym)[:70]


def main():
    args = parse_args()
    act = extract_calls(args.actual)
    exp = extract_calls(args.expected)
    groups = load_explain(args.explain)

    print("Traces (runs of identical Broadcast/SignalConditionVariable collapsed):")
    print(f"  recomp entries: {len(act)}")
    print(f"  Lichii entries: {len(exp)}")
    if not act or not exp:
        print("ERROR: a trace has no [HLE TRACE] entries", file=sys.stderr)
        return 1

    a_syms = [s for s, _ in act]
    e_syms = [s for s, _ in exp]
    # autojunk off: with it, symbols above 1% frequency (InitializeMutex, SignalLightEvent) are ignored as junk
    sm = difflib.SequenceMatcher(None, e_syms, a_syms, autojunk=False)
    matched = sum(b.size for b in sm.get_matching_blocks())
    print(f"\nMatched entries: {matched} ({100.0 * matched / len(exp):.2f}% of Lichii, "
          f"{100.0 * matched / len(act):.2f}% of recomp, ratio {100.0 * sm.ratio():.2f}%)")

    print("\nNon-matching blocks (start index:length on each side):")
    unexplained = 0
    blocks = [op for op in sm.get_opcodes() if op[0] != "equal"]
    if not blocks:
        print("  none")
    for tag, i1, i2, j1, j2 in blocks:
        a_part, e_part = act[j1:j2], exp[i1:i2]
        names = [explain_part(p, groups, lr) for p, lr in ((a_part, True), (e_part, False)) if p]
        why = names[0] if names and all(names) else None
        unexplained += why is None
        counts = Counter(short(s) for s, _ in a_part + e_part).most_common(3)
        lrs = sorted({hex(lr) for _, lr in a_part if lr is not None})
        print(f"  {tag:7s} Lichii {i1}:{i2 - i1}  recomp {j1}:{j2 - j1}  [{why or 'UNEXPLAINED'}]")
        print(f"          {counts}  recomp lr {lrs[:6]}")

    # Second pass without the explained groups: what is left must match one to one
    a_rest = [s for s, lr in act if not explain_part([(s, lr)], groups, True)]
    e_rest = [s for s, lr in exp if not explain_part([(s, lr)], groups, False)]
    sm2 = difflib.SequenceMatcher(None, e_rest, a_rest, autojunk=False)
    rest_blocks = [op for op in sm2.get_opcodes() if op[0] != "equal"]
    print(f"\nWithout explained groups: recomp {len(a_rest)} entries, Lichii {len(e_rest)} entries, "
          f"{len(rest_blocks)} non-matching blocks")
    for tag, i1, i2, j1, j2 in rest_blocks:
        print(f"  {tag:7s} Lichii {i1}:{i2 - i1} {[short(s) for s in e_rest[i1:i2]][:4]}  "
              f"recomp {j1}:{j2 - j1} {[short(s) for s in a_rest[j1:j2]][:4]}")

    diff = Counter(a_syms)
    diff.subtract(Counter(e_syms))
    print("\nTop 10 symbols over-represented in the recomp:")
    for sym, d in [x for x in diff.most_common() if x[1] > 0][:10]:
        print(f"  {d:+5d}  {short(sym)}  (recomp {a_syms.count(sym)}, Lichii {e_syms.count(sym)})")
    under = [x for x in diff.most_common() if x[1] < 0]
    if under:
        print("Symbols under-represented in the recomp:")
        for sym, d in under:
            print(f"  {d:+5d}  {short(sym)}")

    stubs = extract_stubs(args.actual)
    print(f"\nStub hits in the recomp log ({len(stubs)} distinct symbols, first hit number shown):")
    for sym, (n, kind, rest) in sorted(stubs.items(), key=lambda x: x[1][0]):
        print(f"  #{n:<3d} {'FATAL ' if kind.startswith('FATAL') else ''}{sym} {rest}")

    if groups:
        print("\nExplanation groups:")
        for g in groups:
            print(f"  {g['name']}: {g['reason']}")

    ends = (a_syms[-1] == END_SYM, e_syms[-1] == END_SYM)
    fatal = any(k.startswith("FATAL") for _, k, _ in stubs.values())
    print(f"\nrecomp ends at {END_SYM}: {ends[0]}; Lichii ends at {END_SYM}: {ends[1]}")
    print(f"First-pass blocks not covered by a group: {unexplained}; blocks left after removing groups: "
          f"{len(rest_blocks)}; fatal stubs: {int(fatal)}")
    ok = all(ends) and not rest_blocks and not fatal
    print(f"M2 trace gate: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
