#!/usr/bin/env python3
"""Extract all CGSB shaders and control sections from the 9 FromSoftware archives."""
import os
import sys
import zlib
import struct
import json
from collections import Counter
import env

SHADER_DIR = os.path.join(env.GAME_DIR, "romfs", "shader")
OUT_DIR = os.path.join(env.WORK_DIR, "all_shaders")

ARCHIVES = [
    "gameface.shaderbnd.dcx",
    "gxdecal.shaderbnd.dcx",
    "gxffxshader.shaderbnd.dcx",
    "gxflvershader.shaderbnd.dcx",
    "gxgui.shaderbnd.dcx",
    "gxposteffect.shaderbnd.dcx",
    "gxrenderershader.shaderbnd.dcx",
    "gxshader.shaderbnd.dcx",
    "title.shaderbnd.dcx"
]

STAGE_NAMES = {
    0: "Vertex",
    1: "Fragment",
    2: "Geometry",
    3: "TessControl",
    4: "TessEval",
    5: "Compute",
    6: "Task",
    7: "Mesh",
}

# Header +0x08 bits 31:16 count stages in pipeline order; the control section uses NVN's order
PIPELINE_TO_NVN_STAGE = {0: 0, 1: 3, 2: 4, 3: 2, 4: 1, 5: 5, 6: 6, 7: 7}


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    bad = 0
    total_cgsb = 0
    stage_counts = Counter()
    archive_counts = {}

    manifest = []

    for arch_name in ARCHIVES:
        arch_path = os.path.join(SHADER_DIR, arch_name)
        with open(arch_path, "rb") as f:
            raw_data = f.read()

        dca_pos = raw_data.find(b"DCA\x00")
        if dca_pos < 0:
            print(f"Error: {arch_name} not DCA compressed", file=sys.stderr)
            continue
        decomp = zlib.decompress(raw_data[dca_pos + 8:])
        file_count = struct.unpack("<I", decomp[0x0c:0x10])[0]

        base_name = arch_name.split(".")[0]
        arch_cgsb = 0
        arch_stages = Counter()

        for i in range(file_count):
            entry = decomp[0x40 + i * 36 : 0x40 + (i + 1) * 36]
            csize, usize, raw_doff = struct.unpack("<QQQ", entry[8:32])
            doff = raw_doff & 0xFFFFFFFF
            noff = struct.unpack("<I", entry[32:36])[0]
            sname = decomp[noff:noff+80].split(b"\x00\x00")[0].decode("utf-16le", errors="ignore").replace("\x00", "")
            # Sanitize sname for filesystem
            safe_sname = "".join(c if c.isalnum() or c in "._-" else "_" for c in sname)

            cgsb = decomp[doff : doff + usize]
            if not cgsb.startswith(b"CGSB"):
                continue

            arch_cgsb += 1
            total_cgsb += 1

            # Layout: docs/cgsb-format.md. Section 0 is the code area, section 1 the NVN2 control section
            hdr_word, = struct.unpack("<I", cgsb[0x08:0x0c])
            s0_off, s0_sz = struct.unpack("<QQ", cgsb[0x28:0x38])
            s1_off, s1_sz = struct.unpack("<QQ", cgsb[0x38:0x48])

            code_area = cgsb[s0_off : s0_off + s0_sz]
            ctl = cgsb[s1_off : s1_off + s1_sz]

            magic, = struct.unpack("<I", ctl[:4])
            st, = struct.unpack("<H", ctl[0x56:0x58])
            code_len, = struct.unpack("<I", ctl[0x38:0x3c])
            c1o, = struct.unpack("<I", ctl[0x40:0x44])
            end, = struct.unpack("<I", ctl[0x44:0x48])
            problems = []
            if magic != 0x98761234 or ctl[4] != 0xC9:
                problems.append(f"control magic {magic:#x} version {ctl[4]:#x}")
            if s0_sz != end or code_len > end or c1o > end:
                problems.append(f"code area {s0_sz:#x} vs control len {code_len:#x} c1 {c1o:#x} end {end:#x}")
            if PIPELINE_TO_NVN_STAGE.get(hdr_word >> 16) != st:
                problems.append(f"header stage {hdr_word >> 16} vs control stage {st}")
            if problems:
                print(f"[{arch_name}] {sname}: " + "; ".join(problems), file=sys.stderr)
                bad += 1

            arch_stages[st] += 1
            stage_counts[st] += 1

            # Extract raw Ampere SASS instructions
            # st == 5 (Compute): no 128-byte SPH, instructions start at 0
            # st != 5 (Graphics/Task/Mesh): 128-byte SPH header, instructions start at 128
            hdr_sz = 0 if st == 5 else 128
            insns = code_area[hdr_sz:code_len] if code_len > hdr_sz else b""
            # whole 16-byte instructions only
            if len(insns) % 16 != 0:
                insns = insns[:len(insns) - (len(insns) % 16)]

            file_stem = f"{base_name}_{i:04d}_{safe_sname}"
            cgsb_path = os.path.join(OUT_DIR, f"{file_stem}.cgsb")
            code_path = os.path.join(OUT_DIR, f"{file_stem}.code")
            ctl_path = os.path.join(OUT_DIR, f"{file_stem}.ctl")
            bin_path = os.path.join(OUT_DIR, f"{file_stem}.bin")

            with open(cgsb_path, "wb") as f_out:
                f_out.write(cgsb)
            with open(code_path, "wb") as f_out:
                f_out.write(code_area)
            with open(ctl_path, "wb") as f_out:
                f_out.write(ctl)
            with open(bin_path, "wb") as f_out:
                f_out.write(insns)

            meta = {
                "archive": arch_name,
                "index": i,
                "name": sname,
                "stem": file_stem,
                "stage": st,
                "stage_name": STAGE_NAMES.get(st, f"Unknown/Stage{st}"),
                "ctl_magic": hex(magic),
                "code_len": code_len,
                "c1o": c1o,
                "end": end,
                "s0_sz": s0_sz,
                "insn_bytes": len(insns),
            }
            manifest.append(meta)

        archive_counts[arch_name] = {"files": file_count, "cgsb": arch_cgsb, "stages": dict(arch_stages)}
        print(f"[{arch_name}] Extracted {arch_cgsb} CGSB shaders (stages: {dict(arch_stages)})")

    manifest_path = os.path.join(OUT_DIR, "manifest.json")
    with open(manifest_path, "w") as f_out:
        json.dump(manifest, f_out, indent=2)

    print("\n--- Whole Corpus Summary ---")
    print(f"Total CGSB shaders extracted: {total_cgsb} ({bad} failed layout checks)")
    for st in sorted(stage_counts.keys()):
        print(f"  Stage {st} ({STAGE_NAMES.get(st, 'Unknown')}): {stage_counts[st]}")

if __name__ == "__main__":
    main()
