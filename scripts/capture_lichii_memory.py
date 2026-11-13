#!/usr/bin/env python3
"""Capture real memory values from Lichii during Duskbloods boot."""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path
import env

GAME_MAIN = os.path.join(env.GAME_DIR, "exefs", "main")
CONFIG_FILE = os.path.expanduser(os.environ.get("EDEN_CONFIG") or "~/.config/eden/qt-config.ini")
WORK_DIR = env.WORK_DIR
OUT_CAPTURE = os.path.join(WORK_DIR, "lichii_memory_trace.txt")
LICHII_SOURCE_TOML = os.path.expanduser(os.environ.get("LICHII_SOURCE_TOML", ""))


def source_table(data):
    for k in os.environ.get("LICHII_SOURCE_KEY", "source").split("."):
        data = data.get(k, {})
    return data

def get_lichii_provenance():
    bin_path = shutil.which("lichii-cli") or shutil.which("lichii")
    store_path = os.path.realpath(bin_path) if bin_path else "unknown"
    rev = "unknown"
    m = re.search(r"lichii-nx2-([0-9a-fA-F]+)", store_path)
    if m:
        rev = m.group(1)

    config_rev = "unknown"
    patch_lines = []
    patches = []
    patch_list_hash = "unknown"
    rev_mismatch = False

    if LICHII_SOURCE_TOML and os.path.exists(LICHII_SOURCE_TOML):
        try:
            with open(LICHII_SOURCE_TOML, "rb") as f:
                data = tomllib.load(f)
            source_info = source_table(data)
            config_rev = source_info.get("rev", "unknown")
            if rev != "unknown" and config_rev != "unknown" and not config_rev.startswith(rev):
                rev_mismatch = True

            patches = source_info.get("patches", [])
            ordered_patch_bytes = []
            for idx, p in enumerate(patches, 1):
                url = p.get("url", "")
                h = p.get("hash", "")
                patch_lines.append(f"#   [{idx:02d}] {url} ({h})")
                ordered_patch_bytes.append(f"{url}|{h}\n".encode("utf-8"))
            patch_list_hash = hashlib.sha256(b"".join(ordered_patch_bytes)).hexdigest()
        except Exception as e:
            patch_list_hash = f"error: {e}"

    return {
        "store_path": store_path,
        "rev": rev,
        "config_rev": config_rev,
        "rev_mismatch": rev_mismatch,
        "patches": patches,
        "patch_lines": patch_lines,
        "patch_list_hash": patch_list_hash,
    }

def format_provenance_header(prov):
    lines = [
        "# Lichii Capture Header",
        f"# Lichii Store Path: {prov['store_path']}",
        f"# Lichii Binary Rev: {prov['rev']}",
        f"# Config Rev: {prov['config_rev']}",
    ]
    if prov.get("rev_mismatch"):
        lines.append(f"# WARNING: Config rev ({prov['config_rev']}) does not match binary rev ({prov['rev']})")
    lines.append(f"# Lichii Patch List Hash (ordered): {prov['patch_list_hash']}")
    lines.append(f"# Patches ({len(prov.get('patches', []))} in config order):")
    for p_line in prov.get("patch_lines", []):
        lines.append(p_line)
    lines.append("# ------------------------------------------------------------\n")
    return "\n".join(lines)

def run_lichii_capture(timeout_sec=15):
    raw_log = os.path.join(WORK_DIR, "lichii_capture_raw.log")
    env = dict(os.environ)
    env.update({
        "EDEN_NX2_APP_POOL_PROBE": "1152",
        "EDEN_NX2_MEM_TRACE": "1",
        "EDEN_NX2_SVC_TRACE": "1",
        "EDEN_NX2_DIAG_CODEC_CLIENT_REPLY": "1",
        "EDEN_NX2_EXPERIMENTAL_CODEC_DECODE": "1",
        "EDEN_NX2_EXPERIMENTAL_SYSMIX_OUTPUT": "1",
        "EDEN_NX2_SYSMIX_DRAIN_MULTIPLIER": "1",
        "EDEN_NX2_GPU_MAP_FLAG_PROBE": "1",
        "EDEN_NX2_SILENT_BAD_OPUS": "1",
        "EDEN_NX2_LOOPBACK_ONLY": "1",
    })

    prov = get_lichii_provenance()
    header_text = format_provenance_header(prov)

    cmd = [
        "timeout", str(timeout_sec),
        "lichii-cli",
        "-c", CONFIG_FILE,
        "-g", GAME_MAIN
    ]
    print(f"Running Lichii capture ({prov['rev']}) for {timeout_sec}s...")
    with open(raw_log, "wb") as f:
        f.write(header_text.encode("utf-8"))
        f.flush()
        subprocess.run(cmd, cwd=WORK_DIR, env=env, stdout=f, stderr=subprocess.STDOUT)
    return raw_log, prov

def parse_capture(raw_log):
    events = []
    re_mem_trace = re.compile(r"mem trace:\s*([^\n\r]+)")
    re_svc_in = re.compile(r"svc (0x[0-9a-fA-F]+)\s+in\s+t(\d+)\s+\[0\]=([^\s]+)\s+\[1\]=([^\s]+)\s+\[2\]=([^\s]+)\s+\[3\]=([^\s]+)")
    re_svc_out = re.compile(r"svc (0x[0-9a-fA-F]+)\s+out\s+t(\d+)\s+\[0\]=([^\s]+)")

    with open(raw_log, "rb") as f:
        for line in f:
            line_str = line.decode("utf-8", errors="replace").strip()
            m_mem = re_mem_trace.search(line_str)
            if m_mem:
                events.append(("MEM_TRACE", m_mem.group(1)))
                continue

            m_in = re_svc_in.search(line_str)
            if m_in:
                svc_num = int(m_in.group(1), 16)
                tid = m_in.group(2)
                args = (m_in.group(3), m_in.group(4), m_in.group(5), m_in.group(6))
                events.append(("SVC_IN", (svc_num, tid, args)))
                continue

            m_out = re_svc_out.search(line_str)
            if m_out:
                svc_num = int(m_out.group(1), 16)
                tid = m_out.group(2)
                res = m_out.group(3)
                events.append(("SVC_OUT", (svc_num, tid, res)))
                continue

    return events

def extract_target_sequence(events):
    results = []
    # State tracking
    last_info_type = None

    for ev_type, data in events:
        if ev_type == "MEM_TRACE":
            text = data
            if text.startswith("GetInfo type"):
                # Format: GetInfo type X sub 0x0 -> 0x0 = 0xVALUE
                m = re.match(r"GetInfo type (\d+) sub (0x[0-9a-fA-F]+) -> (0x[0-9a-fA-F]+) = (0x[0-9a-fA-F]+)", text)
                if m:
                    itype = int(m.group(1))
                    sub = m.group(2)
                    res = m.group(3)
                    val = int(m.group(4), 16)
                    if itype == 16:
                        # IsVirtualAddressMemoryEnabled checks if type 16 > 0
                        enabled = val > 0
                        results.append({
                            "call": "IsVirtualAddressMemoryEnabled",
                            "raw_svc": f"GetInfo type 16 -> {hex(val)}",
                            "value": enabled,
                            "desc": f"Returns {enabled} (system resource total {hex(val)}, {val >> 20} MB)"
                        })
                    elif itype == 17:
                        # GetVirtualAddressMemoryResourceUsage
                        results.append({
                            "call": "GetVirtualAddressMemoryResourceUsage",
                            "raw_svc": f"GetInfo type 17 -> {hex(val)}",
                            "value": val,
                            "desc": f"Used system resource: {hex(val)} ({val // 1024} KB)"
                        })
                    elif itype == 21:
                        results.append({
                            "call": "QueryMemoryInfo (TotalNonSystemMemorySize)",
                            "raw_svc": f"GetInfo type 21 -> {hex(val)}",
                            "value": val,
                            "desc": f"Total available user memory: {hex(val)} ({val >> 20} MB)"
                        })
                    elif itype == 22:
                        results.append({
                            "call": "QueryMemoryInfo (TotalNonSystemMemorySizeUsed)",
                            "raw_svc": f"GetInfo type 22 -> {hex(val)}",
                            "value": val,
                            "desc": f"Total used user memory: {hex(val)} ({val >> 20} MB)"
                        })
                    elif itype == 31:
                        results.append({
                            "call": "GetInfo 31 (Nx2RemoteMemorySize)",
                            "raw_svc": f"GetInfo type 31 -> {hex(val)}",
                            "value": val,
                            "desc": f"Remote memory size: {hex(val)} ({val >> 20} MB)"
                        })
                    elif itype == 32:
                        results.append({
                            "call": "GetInfo 32 (Nx2RemoteMemoryUsed)",
                            "raw_svc": f"GetInfo type 32 -> {hex(val)}",
                            "value": val,
                            "desc": f"Remote memory used: {hex(val)}"
                        })
            elif text.startswith("SetHeapSize"):
                # SetHeapSize 0x299000000 -> 0x0 at 0x210ce00000
                m = re.match(r"SetHeapSize (0x[0-9a-fA-F]+) -> (0x[0-9a-fA-F]+) at (0x[0-9a-fA-F]+)", text)
                if m:
                    size = int(m.group(1), 16)
                    res = m.group(2)
                    addr = m.group(3)
                    results.append({
                        "call": "SetMemoryHeapSize",
                        "raw_svc": text,
                        "value": size,
                        "desc": f"Heap size set to {hex(size)} ({size >> 20} MB) at base {addr}"
                    })
            elif text.startswith("MapPhysicalMemory"):
                results.append({
                    "call": "AllocateMemoryPages",
                    "raw_svc": text,
                    "value": text,
                    "desc": f"Physical memory mapped: {text}"
                })
            elif text.startswith("UnmapPhysicalMemory"):
                results.append({
                    "call": "FreeMemoryPages",
                    "raw_svc": text,
                    "value": text,
                    "desc": f"Physical memory unmapped: {text}"
                })

        elif ev_type == "SVC_IN":
            svc_num, tid, args = data
            if svc_num == 0x02:
                # SetMemoryPermission(addr, size, perm)
                addr = args[0]
                size = args[1]
                perm = args[2]
                results.append({
                    "call": "SetMemoryPermission",
                    "raw_svc": f"svcSetMemoryPermission addr={addr} size={size} perm={perm}",
                    "value": (addr, size, perm),
                    "desc": f"SetMemoryPermission addr={addr} size={size} perm={perm}"
                })

    return results

def main():
    if len(sys.argv) > 1 and os.path.exists(sys.argv[1]):
        raw_log = sys.argv[1]
        prov = None
    else:
        raw_log, prov = run_lichii_capture(timeout_sec=15)

    events = parse_capture(raw_log)
    sequence = extract_target_sequence(events)

    print(f"\nCaptured {len(sequence)} target memory operations up to engine initialization:\n")
    if prov is None:
        prov = get_lichii_provenance()

    with open(OUT_CAPTURE, "w") as out_f:
        out_f.write("# Lichii Memory Call Sequence Ground Truth\n")
        out_f.write(format_provenance_header(prov))
        out_f.write("# Environment: EDEN_NX2_APP_POOL_PROBE=1152 EDEN_NX2_MEM_TRACE=1 EDEN_NX2_SVC_TRACE=1\n\n")
        for i, item in enumerate(sequence, 1):
            line = f"[{i:03d}] {item['call']}: {item['desc']}"
            print(line)
            out_f.write(line + "\n")
            out_f.write(f"      RAW: {item['raw_svc']}\n")

    print(f"\nSaved capture to {OUT_CAPTURE}")

if __name__ == "__main__":
    main()
