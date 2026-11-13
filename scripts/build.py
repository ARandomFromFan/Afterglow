#!/usr/bin/env python3
"""Build duskbloods_game native executable from lifted C and host runtime."""
import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
import env

def parse_args():
    p = argparse.ArgumentParser(description="Compile and link duskbloods_game")
    p.add_argument("--work", default=env.WORK_DIR)
    p.add_argument("--host", default=str(env.REPO / "host"))
    p.add_argument("--jobs", "-j", type=int, default=12)
    p.add_argument("--force", action="store_true", help="Recompile all files")
    return p.parse_args()

def compile_file(cmd, src, obj):
    t0 = time.time()
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        return False, src, res.stderr or res.stdout, time.time() - t0
    return True, src, None, time.time() - t0

def main():
    args = parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import overload_check
    if not overload_check.check(args.host, args.work):
        print("Build stopped: fix the overload check above", file=sys.stderr)
        sys.exit(1)
    gen_dir = os.path.join(args.work, "sa", "gen")
    build_dir = os.path.join(args.work, "sa", "build")
    obj_dir = os.path.join(build_dir, "obj")
    os.makedirs(obj_dir, exist_ok=True)
    exe_path = os.path.join(args.work, "duskbloods_game")

    # Collect source files
    chunk_srcs = sorted([
        os.path.join(gen_dir, f) for f in os.listdir(gen_dir)
        if f.startswith("code_") and f.endswith(".c")
    ])
    gen_helper_srcs = [
        os.path.join(gen_dir, f) for f in [
            "a64_table.c", "a64_hooks.c", "a64_rt.c", "a64_suyu.c",
            "hos_image.c", "a64_stubs.c", "nvn2_exports.c"
        ]
        if os.path.exists(os.path.join(gen_dir, f))
    ]

    host_srcs = []
    for f in os.listdir(args.host):
        if f.endswith(".c"):
            host_srcs.append(os.path.join(args.host, f))
    posix_dir = os.path.join(args.host, "posix")
    if os.path.isdir(posix_dir):
        for f in os.listdir(posix_dir):
            if f.endswith(".c"):
                host_srcs.append(os.path.join(posix_dir, f))

    pkg_cflags = []
    pkg_libs = []
    # openssl is the host's TLS stack for nn::ssl (host/nn_ssl.c), the same "ssl"+"crypto" the SDK's ssl
    # service would have provided on the console
    pkgs = ["sdl2", "libavcodec", "libswscale", "libswresample", "libavutil", "opus", "openssl"]
    try:
        r = subprocess.run(["pkg-config", "--cflags"] + pkgs,
                           capture_output=True, text=True, check=True)
        pkg_cflags = r.stdout.split()
        r = subprocess.run(["pkg-config", "--libs"] + pkgs,
                           capture_output=True, text=True, check=True)
        pkg_libs = r.stdout.split()
    except Exception as e:
        print(f"Warning: pkg-config failed: {e}", file=sys.stderr)

    all_jobs = []
    # 1. Chunks
    for s in chunk_srcs:
        base = os.path.splitext(os.path.basename(s))[0]
        o = os.path.join(obj_dir, base + ".o")
        cmd = [
            "clang", "-O1", "-g0",
            "-fno-vectorize", "-fno-slp-vectorize",
            "-fno-unroll-loops",
            "-I" + gen_dir,
            "-c", s, "-o", o
        ]
        all_jobs.append((cmd, s, o))

    # 2. Gen helpers
    for s in gen_helper_srcs:
        base = os.path.splitext(os.path.basename(s))[0]
        o = os.path.join(obj_dir, base + ".o")
        cmd = [
            "clang", "-O1", "-g0",
            "-I" + gen_dir,
            "-c", s, "-o", o
        ]
        all_jobs.append((cmd, s, o))

    # 3. Host sources
    for s in host_srcs:
        base = os.path.splitext(os.path.basename(s))[0]
        if "posix" in s:
            base = "posix_" + base
        o = os.path.join(obj_dir, base + ".o")
        cmd = [
            "clang", "-O2", "-g0",
            "-I" + gen_dir,
            "-I" + args.host,
            "-I" + os.path.join(args.host, "posix"),
            "-I" + os.path.join(os.path.dirname(args.host), "third_party", "stb"),
            "-D_GNU_SOURCE",
        ] + pkg_cflags + [
            "-c", s, "-o", o
        ]
        all_jobs.append((cmd, s, o))

    # Filter needed compilations
    to_compile = []
    all_objs = []
    for cmd, s, o in all_jobs:
        all_objs.append(o)
        if args.force or not os.path.exists(o) or os.path.getmtime(s) > os.path.getmtime(o):
            to_compile.append((cmd, s, o))

    print(f"Total compilation targets: {len(all_jobs)} ({len(to_compile)} to rebuild, {len(all_jobs) - len(to_compile)} up-to-date)")

    if to_compile:
        t0 = time.time()
        completed = 0
        total = len(to_compile)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(compile_file, cmd, s, o): s for cmd, s, o in to_compile}
            for fut in concurrent.futures.as_completed(futs):
                ok, s, err, elap = fut.result()
                completed += 1
                bname = os.path.basename(s)
                if not ok:
                    print(f"\n[FAIL] {bname} failed ({elap:.1f}s):\n{err}", file=sys.stderr)
                    sys.exit(1)
                print(f"[{completed:3d}/{total:3d}] Compiled {bname:20s} in {elap:.1f}s")
        print(f"Compilation finished in {time.time() - t0:.1f}s")

    # Link executable
    print(f"Linking {exe_path} from {len(all_objs)} object files...")
    rsp_path = os.path.join(build_dir, "objs.rsp")
    with open(rsp_path, "w") as f:
        f.write("\n".join(f'"{o}"' for o in all_objs) + "\n")

    rpaths = [f"-Wl,-rpath,{arg[2:]}" for arg in pkg_libs if arg.startswith("-L")]
    # Resolve libatomic at build time without hardcoded store paths
    atomic_link = ["-latomic"]
    for comp in ["gcc", "clang", "cc"]:
        try:
            r = subprocess.run([comp, "-print-file-name=libatomic.a"], capture_output=True, text=True)
            p = r.stdout.strip()
            if r.returncode == 0 and os.path.isabs(p) and os.path.exists(p):
                atomic_link = [p]
                break
        except Exception:
            pass
    if atomic_link == ["-latomic"]:
        for comp in ["gcc", "clang", "cc"]:
            try:
                r = subprocess.run([comp, "-print-file-name=libatomic.so.1"], capture_output=True, text=True)
                p = r.stdout.strip()
                if r.returncode == 0 and os.path.isabs(p) and os.path.exists(p):
                    rpaths.append(f"-Wl,-rpath,{os.path.dirname(os.path.realpath(p))}")
                    break
            except Exception:
                pass

    link_cmd = [
        "clang",
        "-fuse-ld=lld",
        "@" + rsp_path,
        "-lSDL2", "-lpthread", "-ldl", "-lm",
        # win32_posix.c and afterglow_vk.so find these through dlsym: unexported, pool imports failed on RADV and
        # input read the window as never focused and every key as up
        "-Wl,--export-dynamic-symbol=hos_posix_*",
    ] + atomic_link + pkg_libs + rpaths + [
        "-o", exe_path
    ]
    t_link = time.time()
    res = subprocess.run(link_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        print(f"Link failed:\n{res.stderr or res.stdout}", file=sys.stderr)
        sys.exit(1)

    exe_size_mb = os.path.getsize(exe_path) / (1024 * 1024)
    print(f"Successfully linked {exe_path} ({exe_size_mb:.1f} MB) in {time.time() - t_link:.2f}s")

if __name__ == "__main__":
    main()
