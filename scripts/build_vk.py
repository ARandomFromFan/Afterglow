#!/usr/bin/env python3
"""Build Afterglow Vulkan renderer shared library (afterglow_vk.so)."""
import argparse
import concurrent.futures
import hashlib
import os
import subprocess
import sys
import time
import env

# (AFTERGLOW_SHADER_STAMP keys the on-disk program cache; hashed from the translator so a stale cache is refused)
TRANSLATOR_DIRS = (("gpu", "vk"), ("gpu", "compat"), ("host", "posix"), ("third_party", "stb"),
                   ("third_party", "suyu", "video_core", "textures"))
TRANSLATOR_SUFFIXES = (".cpp", ".cc", ".c", ".h", ".hpp", ".hh", ".inl", ".inc", ".hxx")

def translator_sources(repo):
    """Translator sources and headers, sorted by repo-relative path."""
    files = []
    for sub in TRANSLATOR_DIRS:
        for d, dirs, names in os.walk(os.path.join(repo, *sub)):
            dirs[:] = sorted(x for x in dirs if x != "__pycache__")
            files += [os.path.join(d, f) for f in names if f.endswith(TRANSLATOR_SUFFIXES)]
    files.sort(key=lambda f: os.path.relpath(f, repo).replace(os.sep, "/"))
    return files

def translator_stamp(repo, flags=()):
    """(stamp, files, bytes): first 8 bytes of sha256 over the flags and each source's path, size and contents."""
    files = translator_sources(repo)
    h = hashlib.sha256()
    h.update(b"flags\x00" + "\n".join(flags).encode() + b"\x00")
    total = 0
    for f in files:
        with open(f, "rb") as fh:
            data = fh.read()
        h.update(os.path.relpath(f, repo).replace(os.sep, "/").encode())
        h.update(b"\x00" + len(data).to_bytes(8, "little") + b"\x00")
        h.update(data)
        total += len(data)
    return int.from_bytes(h.digest()[:8], "little"), len(files), total

def stamp_flags(flags, repo):
    """Code-changing flags only: drop include paths, the prefix map and the stamp itself, so tree location does not matter."""
    root = os.path.abspath(repo)
    return [f.replace(root, "src") for f in flags
            if not f.startswith(("-I", "-DAFTERGLOW_SHADER_STAMP", "-ffile-prefix-map"))]

def parse_args():
    p = argparse.ArgumentParser(description="Compile Afterglow Vulkan backend (afterglow_vk.so)")
    p.add_argument("--repo", default=str(env.REPO))
    p.add_argument("--work", default=env.WORK_DIR)
    p.add_argument("--jobs", "-j", type=int, default=12)
    p.add_argument("--force", action="store_true")
    p.add_argument("--stamp", default=None,
                   help="shader stamp to build with, hex or decimal (default: hashed from the translator's sources)")
    return p.parse_args()

def get_nix_paths():
    given = [os.environ.get(k) for k in ("VULKAN_HEADERS_DIR", "SPIRV_HEADERS_DIR", "VMA_DIR", "VULKAN_LOADER_DIR")]
    if all(given):   # (set by flake.nix: the sandbox has no nix-build)
        return tuple(given)
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
        "-ffile-prefix-map=" + os.path.abspath(args.repo) + "=src",
        "-D_GNU_SOURCE", "-DVK_NO_PROTOTYPES", "-DAGVK_BUILDING",
        "-DAGVK_INCLUDES=2",
        "-DNDEBUG", "-Wno-everything"
    ] + includes
    stamp, stamp_files, stamp_bytes = translator_stamp(args.repo, stamp_flags(cxx_flags, args.repo))
    if args.stamp:
        print(f"shader stamp 0x{int(args.stamp, 0):016x} (given; the translator's {stamp_files} sources and its flags "
              f"hash to 0x{stamp:016x})")
        stamp = int(args.stamp, 0)
    else:
        print(f"shader stamp 0x{stamp:016x} (the translator's {stamp_files} sources, {stamp_bytes} bytes, and its flags)")
    cxx_flags.append(f"-DAFTERGLOW_SHADER_STAMP=0x{stamp:016x}ull")

    c_flags = [
        "clang", "-O2", "-g0", "-fPIC", "-D_GNU_SOURCE", "-Wno-everything",
        "-ffile-prefix-map=" + os.path.abspath(args.repo) + "=src",
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
        cmd = cxx_flags + ["-MD", "-MF", obj + ".d", "-c", s, "-o", obj]
        all_jobs.append((cmd, s, obj))

    for s in c_srcs:
        name = os.path.splitext(os.path.basename(s))[0]
        obj = os.path.join(obj_dir, "vk_posix_" + name + ".o")
        cmd = c_flags + ["-MD", "-MF", obj + ".d", "-c", s, "-o", obj]
        all_jobs.append((cmd, s, obj))

    to_compile = []
    all_objs = []
    for cmd, s, obj in all_jobs:
        all_objs.append(obj)
        if needs_build(s, obj, args.force):
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
        # A failed link leaves the previous binary in place: say so loudly, on stdout as well as stderr, and
        # name the usual cause. 98% of these are a shell without the toolchain (see $D/wt/env.sh).
        detail = (res.stderr or "") + (res.stdout or "")
        note = ""
        try:
            st = os.stat(out_so)
            note = f"\nthe previous {os.path.basename(out_so)} is still in place, unchanged since {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(st.st_mtime))}"
        except OSError:
            note = f"\n{os.path.basename(out_so)} does not exist: nothing was produced"
        banner = (f"\n*** LINK FAILED ***  {os.path.basename(out_so)} was NOT produced.{note}\n"
                  f"{detail.strip()}\n"
                  f"Hint: put clang, lld and pkg-config on PATH first (and "
                  f"LIBRARY_PATH); without lld the error is \"invalid linker name in argument '-fuse-ld=lld'\", "
                  f"without pkg-config it is '-lSDL2' or ffmpeg undefined symbols.\n"
                  f"Exit code 1.\n")
        print(banner, file=sys.stderr)
        print(banner)
        sys.exit(1)

    mb = os.path.getsize(out_so) / (1024 * 1024)
    print(f"Successfully linked {out_so} ({mb:.1f} MB) in {time.time() - t_link:.2f}s")

if __name__ == "__main__":
    main()
