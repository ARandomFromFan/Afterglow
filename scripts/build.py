#!/usr/bin/env python3
"""Build duskbloods_game native executable from lifted C and host runtime."""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time
import env

def parse_args():
    env_opt = os.environ.get("BUILD_CHUNK_OPT", "O1").upper()
    default_chunk_opt = env_opt if env_opt in ("O1", "O2") else "O1"
    p = argparse.ArgumentParser(description="Compile and link duskbloods_game")
    p.add_argument("--work", default=env.WORK_DIR)
    p.add_argument("--host", default=str(env.REPO / "host"))
    p.add_argument("--jobs", "-j", type=int, default=12)
    p.add_argument("--force", action="store_true", help="Recompile all files")
    p.add_argument("--chunk-opt", choices=["O1", "O2", "o1", "o2"], default=default_chunk_opt,
                   help="Optimization level for lifted chunks: O1 (default) or O2 (adds -march=x86-64-v3)")
    p.add_argument("--dry-run", "-n", action="store_true", help="Print compilation commands without running them")
    return p.parse_args()

def compile_file(cmd, src, obj):
    t0 = time.time()
    # drop the old depfile first so a failed compile cannot leave a stale one behind
    try:
        os.remove(obj + ".d")
    except OSError:
        pass
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        return False, src, res.stderr or res.stdout, time.time() - t0
    return True, src, None, time.time() - t0

def read_depfile(depfile):
    """Parse a Makefile-style depfile written by clang -MD -MF."""
    try:
        with open(depfile, "r") as f:
            text = f.read()
    except OSError:
        return None
    text = text.replace("\\\n", " ")
    if ":" not in text:
        return None
    text = text.split(":", 1)[1]
    text = text.replace("\\ ", "\x00")  # keep escaped spaces inside a path
    deps = []
    for dep in text.split():
        dep = dep.replace("\x00", " ")
        if len(dep) > 1 and dep[0] == '"' and dep[-1] == '"':
            dep = dep[1:-1]
        deps.append(dep)
    return deps


def needs_build(src, obj, force=False):
    """True when obj must be recompiled: source newer, object missing, depfile
    missing, or any header listed in the depfile missing or newer than obj."""
    if force or not os.path.exists(obj):
        return True
    if not os.path.exists(src) or os.path.getmtime(src) > os.path.getmtime(obj):
        return True
    depfile = obj + ".d"
    if not os.path.exists(depfile):
        return True
    obj_mt = os.path.getmtime(obj)
    for dep in read_depfile(depfile) or ():
        try:
            if os.path.getmtime(dep) > obj_mt:
                return True
        except OSError:
            return True
    return False


def find_static_libatomic(pkg_libs):
    # Locate static libatomic.a from wt/env.sh toolchain; fail if missing, no store scans
    gcc_dir = os.environ.get("HOS_GCC_DIR", "/nix/store/rp1zq1p0rjfdg99rb2cfs14871y6cqaj-gcc-16.2.0")
    atomic_a = os.path.join(gcc_dir, "lib", "libatomic.a")
    if os.path.exists(atomic_a):
        return atomic_a
    if shutil.which("gcc"):
        try:
            r = subprocess.run(["gcc", "-print-file-name=libatomic.a"], capture_output=True, text=True)
            p = r.stdout.strip()
            if r.returncode == 0 and os.path.isabs(p) and os.path.exists(p):
                return p
        except Exception:
            pass
    for arg in pkg_libs:
        if arg.startswith("-L"):
            cand = os.path.join(arg[2:], "libatomic.a")
            if os.path.exists(cand):
                return cand
    raise RuntimeError(
        f"libatomic.a not found in toolchain ({atomic_a}). "
        "Source wt/env.sh or sync toolchain closures."
    )


def main():
    args = parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import overload_check
    if not overload_check.check(args.host, args.work):
        print("Build stopped: fix the overload check above", file=sys.stderr)
        sys.exit(1)
    args.chunk_opt = args.chunk_opt.upper()
    gen_dir = os.path.join(args.work, "sa", "gen")
    build_dir = os.path.join(args.work, "sa", "build")
    # soft CPU write watch (A64_WRITE_TRACK), default on; AFTERGLOW_WRITE_TRACK=0 builds without it in obj_<opt>
    wt_flags = ["-DA64_WRITE_TRACK"] if os.environ.get("AFTERGLOW_WRITE_TRACK", "1") != "0" else []
    opt_tag = args.chunk_opt + ("_wt" if wt_flags else "")
    obj_subdir = f"obj_{opt_tag}"
    obj_dir = os.path.join(build_dir, obj_subdir)
    if not args.dry_run:
        # Link legacy obj to obj_O1 when available to preserve seeded caches
        if opt_tag == "O1" and not os.path.exists(obj_dir) and os.path.exists(os.path.join(build_dir, "obj")):
            try:
                os.symlink("obj", obj_dir)
            except OSError:
                pass
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
    # hos_sdl.c dlopens SDL3 first (window icon, newer input); its -L only becomes an rpath, nothing links it
    if shutil.which("pkg-config"):
        r = subprocess.run(["pkg-config", "--libs-only-L", "sdl3"], capture_output=True, text=True)
        if r.returncode == 0:
            pkg_libs += r.stdout.split()

    all_jobs = []
    # 1. Chunks
    chunk_cflags = ["-O2", "-march=x86-64-v3"] if args.chunk_opt == "O2" else ["-O1"]
    chunk_cflags += [
        "-g0",
        "-fno-vectorize", "-fno-slp-vectorize",
        "-fno-unroll-loops",
        "-I" + gen_dir,
    ] + wt_flags
    fma3_on = os.environ.get("A64_FPX_FMA3", "1") != "0"
    if fma3_on:
        print("Build: A64_FPX_FMA3 enabled (-mfma -ffp-contract=off)")
        chunk_cflags += ["-mfma", "-ffp-contract=off"]
    for s in chunk_srcs:
        base = os.path.splitext(os.path.basename(s))[0]
        o = os.path.join(obj_dir, base + ".o")
        cmd = ["clang"] + chunk_cflags + [
            "-MD", "-MF", o + ".d",
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
        ] + wt_flags + [
            "-MD", "-MF", o + ".d",
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
        ] + wt_flags + pkg_cflags + [
            "-MD", "-MF", o + ".d",
            "-c", s, "-o", o
        ]
        all_jobs.append((cmd, s, o))

    # Filter needed compilations
    to_compile = []
    all_objs = []
    for cmd, s, o in all_jobs:
        all_objs.append(o)
        if needs_build(s, o, args.force):
            to_compile.append((cmd, s, o))

    if args.dry_run:
        print(f"Dry run ({args.chunk_opt}): {len(all_jobs)} targets, obj_dir={obj_dir}")
        for cmd, s, o in all_jobs:
            print(" ".join(cmd))
        return

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
    print(f"Linking {exe_path} from {len(all_objs)} object files ({args.chunk_opt})...")
    rsp_path = os.path.join(build_dir, f"objs_{opt_tag}.rsp")
    with open(rsp_path, "w") as f:
        f.write("\n".join(f'"{o}"' for o in all_objs) + "\n")

    rpaths = [f"-Wl,-rpath,{arg[2:]}" for arg in pkg_libs if arg.startswith("-L")]
    # Link libatomic.a statically from the fixed toolchain
    atomic_link = [find_static_libatomic(pkg_libs)]

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
        # A failed link leaves the previous binary in place: say so loudly, on stdout as well as stderr, and
        # name the usual cause. 98% of these are a shell without the toolchain (see $D/wt/env.sh).
        detail = (res.stderr or "") + (res.stdout or "")
        note = ""
        try:
            st = os.stat(exe_path)
            note = f"\nthe previous {os.path.basename(exe_path)} is still in place, unchanged since {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(st.st_mtime))}"
        except OSError:
            note = f"\n{os.path.basename(exe_path)} does not exist: nothing was produced"
        banner = (f"\n*** LINK FAILED ***  {os.path.basename(exe_path)} was NOT produced.{note}\n"
                  f"{detail.strip()}\n"
                  f"Hint: put clang, lld and pkg-config on PATH first (and "
                  f"LIBRARY_PATH); without lld the error is \"invalid linker name in argument '-fuse-ld=lld'\", "
                  f"without pkg-config it is '-lSDL2' or ffmpeg undefined symbols.\n"
                  f"Exit code 1.\n")
        print(banner, file=sys.stderr)
        print(banner)
        sys.exit(1)

    exe_size_mb = os.path.getsize(exe_path) / (1024 * 1024)
    print(f"Successfully linked {exe_path} ({exe_size_mb:.1f} MB) in {time.time() - t_link:.2f}s")
    write_icon(os.path.dirname(exe_path))

def write_icon(out_dir):
    """afterglow.png beside the game: the window icon display_sdl.c loads (SDL 3.4+)."""
    svg = str(env.REPO / "branding" / "afterglow-sigil.svg")
    png = os.path.join(out_dir, "afterglow.png")
    if not os.path.exists(svg) or (os.path.exists(png) and os.path.getmtime(png) >= os.path.getmtime(svg)):
        return
    for tool in (["rsvg-convert", "-w", "256", "-h", "256", "-o", png, svg],
                 ["magick", "-background", "none", svg, "-resize", "256x256", png]):
        if shutil.which(tool[0]) and subprocess.run(tool).returncode == 0:
            print(f"Window icon: {png}")
            return
    print("Window icon skipped: needs rsvg-convert or magick on PATH")

if __name__ == "__main__":
    main()
