#!/usr/bin/env python3
"""Build Afterglow Vulkan renderer shared library (afterglow_vk.so)."""
import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
import env

def parse_args():
    p = argparse.ArgumentParser(description="Compile Afterglow Vulkan backend (afterglow_vk.so)")
    p.add_argument("--repo", default=str(env.REPO))
    p.add_argument("--work", default=env.WORK_DIR)
    p.add_argument("--jobs", "-j", type=int, default=12)
    p.add_argument("--force", action="store_true")
    return p.parse_args()

def get_nix_paths():
    try:
        r = subprocess.run([
            "nix-build", "<nixpkgs>",
            "-A", "vulkan-headers",
            "-A", "spirv-headers",
            "-A", "vulkan-memory-allocator",
            "-A", "vulkan-loader",
            "--no-out-link"
        ], capture_output=True, text=True, check=True)
        lines = r.stdout.strip().split()
        return lines[0], lines[1], lines[2], lines[3]
    except Exception:
        return (
            "/nix/store/700al9wsgzdn2zpv33q6c3jhqygfs0pn-vulkan-headers-1.4.363.0",
            "/nix/store/vhsii3ii633nik980shdb7gm04466jpn-spirv-headers-1.4.363.0",
            "/nix/store/ac9ig6sj4l2r2yd2wfm0wz1qphlkzvq8-vulkan-memory-allocator-3.5.0",
            "/nix/store/1l0a4cq8ahhpgwx0byq2zcsi316mlzm1-vulkan-loader-1.4.363.0"
        )

def compile_file(cmd, src, obj):
    t0 = time.time()
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        return False, src, res.stderr or res.stdout, time.time() - t0
    return True, src, None, time.time() - t0

def main():
    args = parse_args()
    vk_dir = os.path.join(args.repo, "gpu", "vk")
    obj_dir = os.path.join(args.work, "vk_build", "obj")
    os.makedirs(obj_dir, exist_ok=True)
    out_so = os.path.join(args.work, "afterglow_vk.so")

    vk_headers, spirv_headers, vma, vk_loader = get_nix_paths()

    includes = [
        "-I" + os.path.join(vk_dir, "compat"),
        "-I" + os.path.join(args.repo, "gpu", "compat"),
        "-I" + os.path.join(vk_dir, "eden"),
        "-I" + os.path.join(args.repo, "gpu", "third_party", "suyu"),
        "-I" + os.path.join(vk_dir, "sirit", "include"),
        "-I" + os.path.join(vk_dir, "sirit", "src"),
        "-I" + os.path.join(spirv_headers, "include"),
        "-I" + os.path.join(spirv_headers, "include", "spirv", "unified1"),
        "-I" + os.path.join(vk_headers, "include"),
        "-I" + os.path.join(vma, "include"),
        "-I" + vk_dir,
        "-I" + os.path.join(args.repo, "host", "posix"),
        "-I" + os.path.join(args.repo, "third_party", "stb"),
    ]

    cxx_flags = [
        "clang++", "-std=c++20", "-O2", "-g0", "-fPIC",
        "-D_GNU_SOURCE", "-DVK_NO_PROTOTYPES", "-DAGVK_BUILDING",
        "-DAGVK_INCLUDES=2", "-DAFTERGLOW_SHADER_STAMP=0x12345678ull",
        "-DNDEBUG", "-Wno-everything"
    ] + includes

    c_flags = [
        "clang", "-O2", "-g0", "-fPIC", "-D_GNU_SOURCE", "-Wno-everything",
        "-I" + os.path.join(args.repo, "host", "posix"),
        "-I" + os.path.join(args.repo, "third_party", "stb"),
    ]

    cpp_srcs = []
    for sub in ("eden", os.path.join("sirit", "src")):
        for d, _, files in os.walk(os.path.join(vk_dir, sub)):
            cpp_srcs += [os.path.join(d, f) for f in files if f.endswith(".cpp")]
    cpp_srcs += [os.path.join(vk_dir, f) for f in os.listdir(vk_dir) if f.endswith(".cpp")]

    suyu_tex = os.path.join(args.repo, "gpu", "third_party", "suyu", "video_core", "textures")
    for f in ("astc.cpp", "decoders.cpp"):
        p = os.path.join(suyu_tex, f)
        if os.path.exists(p):
            cpp_srcs.append(p)

    c_srcs = [
        os.path.join(args.repo, "host", "posix", "win32_posix.c"),
        os.path.join(args.repo, "host", "posix", "gdi_posix.c"),
    ]

    all_jobs = []
    for s in sorted(cpp_srcs):
        name = os.path.relpath(s, args.repo).replace(os.sep, "_")[:-4]
        obj = os.path.join(obj_dir, "vk_" + name + ".o")
        cmd = cxx_flags + ["-c", s, "-o", obj]
        all_jobs.append((cmd, s, obj))

    for s in c_srcs:
        name = os.path.splitext(os.path.basename(s))[0]
        obj = os.path.join(obj_dir, "vk_posix_" + name + ".o")
        cmd = c_flags + ["-c", s, "-o", obj]
        all_jobs.append((cmd, s, obj))

    to_compile = []
    all_objs = []
    for cmd, s, obj in all_jobs:
        all_objs.append(obj)
        if args.force or not os.path.exists(obj) or os.path.getmtime(s) > os.path.getmtime(obj):
            to_compile.append((cmd, s, obj))

    print(f"Targets: {len(all_jobs)} ({len(to_compile)} to compile, {len(all_jobs) - len(to_compile)} up-to-date)")

    if to_compile:
        t0 = time.time()
        completed = 0
        total = len(to_compile)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(compile_file, cmd, s, obj): s for cmd, s, obj in to_compile}
            for fut in concurrent.futures.as_completed(futs):
                ok, s, err, elap = fut.result()
                completed += 1
                bname = os.path.basename(s)
                if not ok:
                    print(f"\n[FAIL] {bname} failed ({elap:.1f}s):\n{err}", file=sys.stderr)
                    sys.exit(1)
                print(f"[{completed:3d}/{total:3d}] Compiled {bname:25s} in {elap:.1f}s")
        print(f"Compilation finished in {time.time() - t0:.1f}s")

    print(f"Linking {out_so} from {len(all_objs)} objects...")
    rsp = os.path.join(args.work, "vk_build", "vk_objs.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in all_objs) + "\n")

    loader_lib = os.path.join(vk_loader, "lib")
    link_cmd = [
        "clang++", "-fuse-ld=lld", "-shared",
        "-Wl,-Bsymbolic", "-Wl,--gc-sections",
        "-static-libstdc++", "-static-libgcc",
        "-Wl,-rpath," + loader_lib,
        "-L" + loader_lib,
        "@" + rsp,
        "-ldl", "-lpthread", "-lm",
        "-o", out_so
    ]
    t_link = time.time()
    res = subprocess.run(link_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        print(f"Link failed:\n{res.stderr or res.stdout}", file=sys.stderr)
        sys.exit(1)

    mb = os.path.getsize(out_so) / (1024 * 1024)
    print(f"Successfully linked {out_so} ({mb:.1f} MB) in {time.time() - t_link:.2f}s")

if __name__ == "__main__":
    main()
