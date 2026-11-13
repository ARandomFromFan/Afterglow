#!/usr/bin/env python3
"""Checks that the program cache's stamp covers every env knob the shader translator reads.

disk_stamp() (gpu/vk/vk_shader.cpp) hashes the value of every environment variable whose name starts with one of
the prefixes in its translation_env[] list, and the translator reads its knobs with getenv() as it translates. A knob
that changes what it emits is therefore only kept apart if a line of that list covers its name: one that is missing
makes a cache written under one value be read back under the other, and an A/B of the knob runs both arms on the
first arm's modules.

This fails when a knob name read in the translator's sources is covered by neither
  - a prefix in translation_env[],
  - a mode that disk_stamp() hashes by value (the backend flags vk_coopmat.cpp coopmat_init() sets), or
  - RUNTIME_ONLY below (with the reason it needs no line).

It also reports prefixes in the list that no name matches (not an error: a prefix is meant to cover names it does
not spell out, and some are kept for knobs a build has not been made with yet), so a list entry left behind by a
knob that was removed shows up.

Usage: scripts/check_translation_env.py [--src SRC_TREE] [--verbose]
"""
import argparse
import pathlib
import re
import sys

# (the sources the stamp is about: the host's translation path, the backend flags it sets, and the translator itself.
# Only these are scanned: a getenv in gpu/vk/vk_draw.cpp or vk_texture.cpp is a run-time knob of the renderer, and no
# program in the cache was translated differently for it)
SCANNED = [
    "gpu/vk/vk_shader.cpp",      # (translate(), disk_stamp(), the per-program Optimization globals it sets)
    "gpu/vk/vk_coopmat.cpp",     # (coopmat_init(): the backend's modes, and Getenv with names passed in)
    "gpu/vk/eden/shader_recompiler",
]

# (a knob whose effect disk_stamp() hashes by value: the mode flags, not the name. Named here rather than in
# translation_env[] because the mode is the card's too, and a prefix of the name would not keep a cache carried to
# another card apart)
MODE_HASHED = {
    "RX_VK_NO_NAN_GUARD": "modes[] frag_nan_guard (a NaN on a colour output -> 0, the rest clamped)",
    "RX_VK_NO_INVARIANT": "modes[] position_invariant (Invariant on the position output)",
}

# (read where the cache is not: logging, dumps, and what the renderer does with a program once it is made or picked.
# A name here is one whose value cannot change the SPIR-V the translator emits)
RUNTIME_ONLY = {
    "RX_VK_CAPTURE": "which frames are captured (vk_memory.cpp), not what is emitted",
    "RX_VK_DUMP_FAILED": "what is written out when a translation fails, after it failed",
    "RX_VK_DUMP_NATIVE": "the guest code and the IR written beside a translation: a dump, not an input",
    "RX_VK_DUMP_SPV": "the emitted SPIR-V written to a folder: the folder or nothing, the module the same",
    "RX_VK_DUMP_STAGES": "the stages' code written out as the game has it: a dump",
    "RX_VK_HANDLES_LIVE": "where a handle program reads the guest's memory from at pick time, not emitted code",
    "RX_VK_IR": "the IR dumped, and a workgroup log line: a dump",
    "RX_VK_LOG_BRX": "one log line in the branch table tracker",
    "RX_VK_NO_PROGRAM_CACHE": "the cache is not opened at all (disk_init), so no stamp and no modules from it",
    "RX_VK_PICK_BIND_GEN": "which program a draw picks, not what the programs are",
    "RX_VK_PTR_DIAG": "the pointer pass's counts, printed and reset per translation: a dump",
    "RX_VK_REGISTER_AT_ONCE": "whether descriptors are registered as the program is picked (canonical_answer)",
    "RX_VK_REJECT_KEYS": "which programs are refused at pick time, for bisecting a bad translation",
    "RX_VK_REJECT_STAGES": "the stages of RX_VK_REJECT_KEYS",
    "RX_VK_SYNC_SHADERS": "translating on the call's thread or on the job pool",
}

NAME_IN_getenv = re.compile(r'getenv\s*\(\s*"((?:RX_VK|EDEN|HOS)_[A-Z0-9_]+)"')
ANY_NAME = re.compile(r'"((?:RX_VK|EDEN|HOS)_[A-Z0-9_]+)"')


def sources(root):
    for entry in SCANNED:
        p = root / entry
        if p.is_dir():
            for f in sorted(p.rglob("*")):
                if f.suffix in (".cpp", ".h", ".hpp", ".cc"):
                    yield f
        elif p.is_file():
            yield p
        else:
            sys.exit("check_translation_env: %s: no such file or directory (wrong --src?)" % p)


def stamp_prefixes(vk_shader):
    """The prefixes in disk_stamp()'s translation_env[] list, in their order."""
    text = vk_shader.read_text(errors="replace")
    m = re.search(r"translation_env\[\]\s*=\s*\{(.*?)\n\s*\};", text, re.S)
    if not m:
        sys.exit("check_translation_env: no translation_env[] list in %s (was disk_stamp() rewritten?)" % vk_shader)
    body = re.sub(r"//[^\n]*", "", m.group(1))   # (the per-entry comments name other files: not prefixes)
    return re.findall(r'"([^"]+)"', body)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", default=None, help="the tree holding gpu/vk (default: ../../src beside this script)")
    ap.add_argument("--verbose", action="store_true", help="print every name and where it is covered")
    args = ap.parse_args()
    root = pathlib.Path(args.src) if args.src else pathlib.Path(__file__).resolve().parents[2] / "src"
    if not (root / "gpu/vk/vk_shader.cpp").is_file():
        sys.exit("check_translation_env: %s: no gpu/vk/vk_shader.cpp (wrong --src?)" % root)

    vk_shader = root / "gpu/vk/vk_shader.cpp"
    prefixes = stamp_prefixes(vk_shader)
    # (a name that is covered by a hashed mode is only covered while disk_stamp() hashes that mode)
    modes_hashed = "const uint32_t modes[]" in vk_shader.read_text(errors="replace")
    where = {}   # name -> the files it is read in
    for f in sources(root):
        text = f.read_text(errors="replace")
        rel = str(f.relative_to(root))
        for name in set(ANY_NAME.findall(text)):   # (any literal, so a helper taking the name as an argument counts)
            where.setdefault(name, set()).add(rel)

    missing, covered = [], {}
    for name in sorted(where):
        pre = next((p for p in prefixes if name.startswith(p)), None)
        if pre:
            covered[name] = "translation_env: %s" % pre
        elif name in MODE_HASHED and modes_hashed:
            covered[name] = "mode: %s" % MODE_HASHED[name]
        elif name in RUNTIME_ONLY:
            covered[name] = "run time: %s" % RUNTIME_ONLY[name]
        else:
            missing.append(name)

    unused = [p for p in prefixes if not any(n.startswith(p) for n in where)]

    if args.verbose:
        for name in sorted(where):
            print("  %-32s %s" % (name, covered.get(name, "MISSING")))
        print()
    print("%d names in %d files; %d through translation_env[], %d through a hashed mode, %d run time only" %
          (len(where), len(list(sources(root))), sum(1 for c in covered.values() if c.startswith("translation_env")),
           sum(1 for c in covered.values() if c.startswith("mode")), sum(1 for c in covered.values() if c.startswith("run time"))))
    if unused:
        print("translation_env[] prefixes no name in these sources starts with (kept for something else, or left behind):")
        for p in unused:
            print("  %s" % p)
    if missing:
        print("NOT IN THE STAMP: the translator reads these, and a cache made under one value is read back under the other:")
        for name in missing:
            print("  %-32s %s" % (name, ", ".join(sorted(where[name]))))
        print("add a line to translation_env[] in gpu/vk/vk_shader.cpp (or, if the value cannot change what is emitted, to")
        print("RUNTIME_ONLY in this file with the reason)")
        return 1
    print("ok: every name read in the translator is in the stamp or cannot change what it emits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
