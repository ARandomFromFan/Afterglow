#!/usr/bin/env python3
"""2% sample lift and resource extrapolation across main, sdk, and subsdk0."""
import argparse
import os
import pickle
import resource
import struct
import sys
import time
import subprocess
import shutil
import env

def parse_args():
    p = argparse.ArgumentParser(description="2% sample lift and resource extrapolation")
    p.add_argument("--exefs", default=os.path.join(env.GAME_DIR, "exefs"))
    p.add_argument("--work", default=os.path.join(env.WORK_DIR, "sample_modules"))
    p.add_argument("--lifter-kit", default=env.LIFTER_KIT_DIR)
    return p.parse_args()

def main():
    args = parse_args()
    sys.path.insert(0, os.path.join(args.lifter_kit, "third_party/a64-kit/tools"))
    sys.path.insert(0, os.path.join(args.lifter_kit, "third_party/a64-kit/hos"))
    sys.path.insert(0, os.path.join(args.lifter_kit, "nx2"))
    sys.path.insert(0, args.lifter_kit)

    import kit, nso2, nso, insn, lift_fixes, nx_image, a64_lift, a64emit, a64_rtgen, rtgen, standalone

    insn.install()
    lift_fixes.install()

    os.makedirs(args.work, exist_ok=True)
    t0 = time.time()
    print("Loading modules: main, subsdk0, sdk...")

    class MultiImage(nx_image.NxImage):
        def __init__(self, exefs, hle=()):
            base = 0x80000000
            nx_image.MAIN_BASE = base
            all_mods = nso.load_exefs(exefs)
            # Exclude subsdk1 (CUDA/DLSS replaced by Vulkan FSR2/XeSS)
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

    host_hle = {n[4:] for n in standalone.implemented()}
    img = MultiImage(args.exefs, hle=host_hle)
    print(f"Loaded image: {len(img.modules)} modules ({[m.name for m in img.modules]}), skipped {img.skipped}")

    L = a64_lift.Lifter(img)
    cache_path = os.path.join(os.path.dirname(args.work), "discovered_funcs.pkl")
    if os.path.exists(cache_path):
        print(f"Loading cached discovered functions from {cache_path}...")
        with open(cache_path, "rb") as f:
            cache_data = pickle.load(f)
            L.funcs = cache_data["funcs"]
            L.jt = cache_data["jt"]
            L.plt = cache_data["plt"]
            L.stats = cache_data.get("stats", {})
        total_funcs = len(L.funcs)
        print(f"Loaded {total_funcs} discovered functions from cache")
    else:
        t_disc = time.time()
        print("Discovering functions across all 3 modules...")
        L.discover()
        total_funcs = len(L.funcs)
        print(f"Discovered {total_funcs} functions in {time.time() - t_disc:.2f}s")
        print(f"Saving discovered functions cache to {cache_path}...")
        with open(cache_path, "wb") as f:
            pickle.dump({"funcs": L.funcs, "jt": L.jt, "plt": L.plt, "stats": L.stats}, f)

    # Sample 2% of functions (stride 50)
    all_entries = sorted(L.funcs.keys())
    sample_entries = all_entries[::50]
    sample_funcs = {e: L.funcs[e] for e in sample_entries}
    sample_ratio = len(sample_funcs) / total_funcs
    print(f"Selected {len(sample_funcs)} sample functions ({sample_ratio*100:.2f}% of total)")

    L_sample = a64_lift.Lifter(img)
    L_sample.funcs = sample_funcs
    L_sample.jt = {k: v for k, v in L.jt.items() if any(k in body for body, _ in sample_funcs.values())}
    L_sample.plt = {k: v for k, v in L.plt.items() if k in sample_funcs}

    a64_rtgen.write(args.work)
    rtgen.write(args.work)
    a64_h_path = os.path.join(args.work, "a64.h")
    with open(a64_h_path, "r") as f:
        a64_h_text = f.read()
    if "#include <math.h>" not in a64_h_text:
        a64_h_text = a64_h_text.replace("#include <string.h>", "#include <string.h>\n#include <math.h>")
        with open(a64_h_path, "w") as f:
            f.write(a64_h_text)

    t_write = time.time()
    chunk_files = L_sample.write(args.work)
    c_size = sum(os.path.getsize(f) for f in chunk_files)
    print(f"Emitted {len(chunk_files)} chunk file(s), total {c_size / (1024*1024):.2f} MB of sample C in {time.time() - t_write:.2f}s")

    obj_files = []
    print("Compiling sample with clang -O1...")
    t_comp = time.time()
    for cf in chunk_files:
        obj_f = cf.rsplit(".", 1)[0] + ".o"
        compile_cmd = [
            "clang", "-O1", "-g0",
            "-fno-vectorize", "-fno-slp-vectorize",
            "-fno-unroll-loops",
            "-I" + args.work,
            "-c", cf,
            "-o", obj_f
        ]
        res = subprocess.run(compile_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if res.returncode != 0:
            print(f"Clang failed on {cf}:\n{res.stderr}", file=sys.stderr)
            sys.exit(1)
        obj_files.append(obj_f)
    comp_time = time.time() - t_comp

    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    peak_rss_mb = usage.ru_maxrss / 1024.0
    obj_size = sum(os.path.getsize(f) for f in obj_files)

    extrap_factor = 1.0 / sample_ratio
    total_c_bytes = c_size * extrap_factor
    total_obj_bytes = obj_size * extrap_factor
    total_footprint_bytes = total_c_bytes + total_obj_bytes
    total_footprint_gb = total_footprint_bytes / (1024**3)
    total_cpu_time_sec = comp_time * extrap_factor
    total_cpu_time_min = total_cpu_time_sec / 60.0

    print("\n" + "=" * 60)
    print("RESOURCE EXTRAPOLATION REPORT (main + subsdk0 + sdk)")
    print("=" * 60)
    print(f"Total functions across 3 modules: {total_funcs}")
    print(f"Sample functions measured:         {len(sample_funcs)} ({sample_ratio*100:.2f}%)")
    print(f"Sample C source size:              {c_size / (1024*1024):.2f} MB")
    print(f"Sample object file size:           {obj_size / (1024*1024):.2f} MB")
    print(f"Sample compile time:               {comp_time:.2f} s")
    print(f"Sample peak RSS:                   {peak_rss_mb:.1f} MB")
    print("-" * 60)
    print(f"Projected 100% C source:           {total_c_bytes / (1024**3):.2f} GB")
    print(f"Projected 100% object files:       {total_obj_bytes / (1024*1024):.1f} MB")
    print(f"Projected total build footprint:   {total_footprint_gb:.2f} GB")
    print(f"Projected single-core CPU time:    {total_cpu_time_min:.1f} minutes")
    print(f"Projected 12-thread wall time:     ~{total_cpu_time_min / 12:.1f} minutes")
    print(f"Status: {'PASS (within 30 GB)' if total_footprint_gb <= 30.0 else 'FAIL (exceeds 30 GB)'}")
    print("=" * 60 + "\n")

    # Clean up sample files
    shutil.rmtree(args.work, ignore_errors=True)

if __name__ == "__main__":
    main()
