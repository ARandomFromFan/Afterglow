#!/usr/bin/env python3
"""Standalone lift driver for Duskbloods Network Test (main, subsdk0, sdk)."""
import argparse
import collections
import json
import os
import pickle
import struct
import sys
import time
import env

def parse_args():
    p = argparse.ArgumentParser(description="Lift Duskbloods modules into C")
    p.add_argument("--exefs", default=os.path.join(env.GAME_DIR, "exefs"))
    p.add_argument("--work", default=env.WORK_DIR)
    p.add_argument("--lifter-kit", default=env.LIFTER_KIT_DIR)
    p.add_argument("--host", default=None, help="Host runtime directory for HLE detection")
    p.add_argument("--rechunk", action="store_true", help="Recompute chunk boundaries")
    return p.parse_args()

# V8 embedded builtins: reached only through this layout table of 16-byte entries (instruction offset, length, metadata offset, length)
V8_BLOB = 0x8001ba80
V8_LAYOUT = 0x84af8790

def v8_builtins(img):
    out, p, last = set(), V8_LAYOUT, -1
    while True:
        off, length = struct.unpack_from("<2I", img.mem, p - 0x80000000)
        if off <= last or not length or length > 0x100000:
            return out
        out.add(V8_BLOB + off)
        last, p = off, p + 16

def main():
    args = parse_args()
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    local_a64 = os.path.join(repo_root, "third_party/a64-kit")
    local_lifter = os.path.join(repo_root, "lifter")

    if os.path.isdir(local_a64) and os.path.isdir(local_lifter):
        sys.path.insert(0, os.path.join(local_a64, "tools"))
        sys.path.insert(0, os.path.join(local_a64, "hos"))
        sys.path.insert(0, os.path.join(local_a64, "lift"))
        sys.path.insert(0, local_lifter)
    else:
        kit = args.lifter_kit
        sys.path.insert(0, os.path.join(kit, "third_party/a64-kit/tools"))
        sys.path.insert(0, os.path.join(kit, "third_party/a64-kit/hos"))
        sys.path.insert(0, os.path.join(kit, "third_party/a64-kit/lift"))
        sys.path.insert(0, os.path.join(kit, "nx2"))
        sys.path.insert(0, kit)

    import kit, nso2, nso, insn, lift_fixes, nx_image, a64_lift, a64emit, a64_rtgen, rtgen, standalone, chunks

    insn.install()
    lift_fixes.install()

    t0 = time.time()
    print("Initializing MultiImage (main, subsdk0, sdk)...")

    host_dir = args.host or os.path.join(repo_root, "host")

    class MultiImage(nx_image.NxImage):
        def __init__(self, exefs, hle=()):
            base = 0x80000000
            nx_image.MAIN_BASE = base
            all_mods = nso.load_exefs(exefs)
            self.modules = [m for m in all_mods if m.name in ("main", "subsdk0", "sdk")]
            self.skipped = [m.name for m in all_mods if m.name not in ("main", "subsdk0", "sdk")]
            for m in self.modules:
                m.base = base
                size = (m.segments[2][0] + m.segments[2][1] + m.bss_size + 0xFFF) & ~0xFFF
                if len(m.image) < size:
                    m.image.extend(bytes(size - len(m.image)))
                base += size
            self.end = base
            self.mem = bytearray(self.end - nx_image.MAIN_BASE)
            for m in self.modules:
                self.mem[m.base - nx_image.MAIN_BASE : m.base - nx_image.MAIN_BASE + len(m.image)] = m.image
            self.hle = set(hle)
            self.exports = {}
            for m in self.modules:
                for s in m.symbols:
                    if s["defined"] and s["name"] and s["name"] not in self.exports:
                        self.exports[s["name"]] = (m, s)
            self.hooks, self.hook_syms, self.tramp, self.unresolved = {}, {}, {}, {}
            for sym in sorted(self.hle):
                hit = self.exports.get(sym)
                if hit:
                    m, s = hit
                    name = nx_image.c_ident(sym)
                    self.hooks[m.base + s["value"]] = name
                    self.hook_syms[name] = sym
            self._relocate()
            self.code = [(m.base + lo, m.base + hi, m.name) for m in self.modules for lo, hi in [m.code_range()]]
            self._entries()
            self.names = self.symbol_names()

        def _entries(self):
            super()._entries()
            for i in range(880):
                self.entries.add(0x84927690 + i * 16)
            self.entries |= v8_builtins(self)

        def _relocate(self):
            for m in self.modules:
                bind = {s["name"]: s["bind"] for s in m.symbols}
                for r in m.relocs:
                    at = m.base + r["offset"] - nx_image.MAIN_BASE
                    if r["type"] == "RELATIVE":
                        v = m.base + r["addend"]
                    elif r["type"] in ("ABS64", "GLOB_DAT", "JUMP_SLOT"):
                        v = self.resolve(r["sym"], bind.get(r["sym"]) == "weak") + (r["addend"] if r["type"] == "ABS64" else 0)
                    elif r["type"] in (1031, "TLSDESC"):
                        v = 0
                    else:
                        raise SystemExit(f"{m.name}: relocation type {r['type']} at {r['offset']:#x} not supported")
                    struct.pack_into("<Q", self.mem, at, v & 0xFFFFFFFFFFFFFFFF)

    all_mods = nso.load_exefs(args.exefs)
    load_mods = [m for m in all_mods if m.name in ("main", "subsdk0", "sdk")]
    hle, stubbed = standalone.hle_set(load_mods)
    print(f"Standalone HLE: {len(hle)} implemented functions, {len(stubbed)} OS-facing imports to stub")
    hle_all = hle | stubbed

    img = MultiImage(args.exefs, hle=hle_all)
    out_dir = os.path.join(args.work, "sa")
    img_dir = os.path.join(out_dir, "image")
    gen_dir = os.path.join(out_dir, "gen")
    os.makedirs(img_dir, exist_ok=True)
    os.makedirs(gen_dir, exist_ok=True)

    meta = img.write(img_dir)
    layout_str = ", ".join(f"{m['name']}@{m['base']:#x}+{m['size']:#x}" for m in meta["modules"])
    print(f"Image layout: {layout_str}")
    print(f"Skipped modules: {img.skipped}")

    L = a64_lift.Lifter(img)
    cache_path = os.path.join(args.work, "discovered_funcs.pkl")
    if os.path.exists(cache_path):
        print(f"Loading cached discovered functions from {cache_path}...")
        with open(cache_path, "rb") as f:
            cache_data = pickle.load(f)
            L.funcs = cache_data["funcs"]
            L.jt = cache_data["jt"]
            L.plt = cache_data["plt"]
            L.stats = cache_data.get("stats", collections.Counter())
        print(f"Loaded {len(L.funcs)} functions from cache in {time.time() - t0:.2f}s")
    else:
        print("Discovering functions across all 3 modules...")
        t_disc = time.time()
        L.discover()
        print(f"Discovered {len(L.funcs)} functions in {time.time() - t_disc:.2f}s")
        print(f"Saving discovery cache to {cache_path}...")
        with open(cache_path, "wb") as f:
            pickle.dump({"funcs": L.funcs, "jt": L.jt, "plt": L.plt, "stats": L.stats}, f)

    # entries added after the discovery cache was made: walk them, and re-walk the functions they now cut short
    new = {e for e in v8_builtins(img) if e not in L.funcs and e not in img.hooks}
    if new:
        L.entries |= new
        affected = {e for e, (body, _) in L.funcs.items() if not new.isdisjoint(body.keys())}
        for e in sorted(new | affected):
            L.funcs[e] = L.walk(e)
        print(f"Walked {len(new)} new entries, re-walked {len(affected)} functions")
        with open(cache_path, "wb") as f:
            pickle.dump({"funcs": L.funcs, "jt": L.jt, "plt": L.plt, "stats": L.stats}, f)

    overlap = set(L.funcs) & set(img.hooks)
    if overlap:
        print(f"Removing {len(overlap)} hooked functions from lifted functions set...")
        for a in overlap:
            del L.funcs[a]

    # Runtime headers
    a64_rtgen.write(gen_dir)
    rtgen.write(gen_dir)
    a64_h_path = os.path.join(gen_dir, "a64.h")
    with open(a64_h_path, "r") as f:
        a64_h_text = f.read()
    if "#include <math.h>" not in a64_h_text:
        a64_h_text = a64_h_text.replace("#include <string.h>", "#include <string.h>\n#include <math.h>")
    if "hos_trace_plt" not in a64_h_text:
        a64_h_text += "\nvoid hos_trace_plt(struct GuestContext *c, unsigned int idx);\n"
    with open(a64_h_path, "w") as f:
        f.write(a64_h_text)

    # Standalone runtime helpers
    start_hook, exit_hook = standalone.write_image_c(gen_dir, img, meta, args.exefs)
    print(f"Generated hos_image.c (thread hooks {start_hook:#x}, {exit_hook:#x})")
    num_stubs = standalone.gen_stubs(gen_dir, meta)
    print(f"Generated a64_stubs.c ({num_stubs} OS-facing stubs)")
    nvn = standalone.nvn_exports(img.modules)
    standalone.write_nvn_exports(gen_dir, nvn)
    print(f"Generated nvn2_exports.c ({len(nvn)} NVN exports routed to host)")

    # Parallel chunk emission
    print("Emitting C chunks in parallel...")
    t_chunks = time.time()
    files = chunks.write(L, gen_dir, img_dir, rechunk=args.rechunk)
    c_bytes = sum(os.path.getsize(f) for f in files)
    print(f"Wrote {len(files)} chunk files ({c_bytes / (1024*1024):.1f} MB) in {time.time() - t_chunks:.2f}s")
    print(f"Lift complete in {time.time() - t0:.2f}s")

if __name__ == "__main__":
    main()
