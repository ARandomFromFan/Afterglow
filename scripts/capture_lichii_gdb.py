#!/usr/bin/env python3
"""Capture nn:: calls from Lichii via GDB breakpoints on main PLT entries."""
import hashlib
import json
import os
import pty
import re
import shutil
import subprocess
import sys
import threading
import time
import tomllib
import env

GAME_MAIN = os.path.join(env.GAME_DIR, "exefs", "main")
MAIN_JSON = os.path.join(env.WORK_DIR, "images", "main.json")
WORK_DIR = env.WORK_DIR
OUT_TRACE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(WORK_DIR, "lichii_nn_trace.txt")
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

    patch_lines = []
    patch_list_hash = "unknown"
    if LICHII_SOURCE_TOML and os.path.exists(LICHII_SOURCE_TOML):
        try:
            with open(LICHII_SOURCE_TOML, "rb") as f:
                data = tomllib.load(f)
            patches = source_table(data).get("patches", [])
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
        "patch_list_hash": patch_list_hash,
        "patch_lines": patch_lines,
    }

def main():
    prov = get_lichii_provenance()
    print(f"Lichii store path: {prov['store_path']}")
    print(f"Lichii rev: {prov['rev']}")

    with open(MAIN_JSON) as f:
        main_meta = json.load(f)
    jump_slots = [r for r in main_meta["relocs"] if r["type"] == "JUMP_SLOT"]
    print(f"Loaded {len(jump_slots)} JUMP_SLOT symbols from main.json")

    skip_substrings = ["Lock", "Unlock", "GetCurrentThread", "GetSystemTick", "GetTlsValue", "SetTlsValue"]

    targets = []
    for idx, r in enumerate(jump_slots):
        sym = r["sym"]
        if not ("nn" in sym.lower() or "nvn" in sym.lower()):
            continue
        if any(skip in sym for skip in skip_substrings):
            continue
        targets.append((idx, sym))

    print(f"Tracing {len(targets)} symbols on main thread up to nvnBootstrapLoader")

    tmp_dir = "/tmp/duskbloods_gdb_run"
    os.makedirs(f"{tmp_dir}/eden", exist_ok=True)
    shutil.copy(os.path.expanduser(os.environ.get("EDEN_CONFIG") or "~/.config/eden/qt-config.ini"), f"{tmp_dir}/eden/qt-config.ini")

    env = dict(os.environ)
    env.setdefault("XDG_RUNTIME_DIR", "/run/user/1000")
    env.setdefault("WAYLAND_DISPLAY", "wayland-1")
    env.setdefault("DISPLAY", ":1")
    env["EDEN_NX2_APP_POOL_PROBE"] = "1152"
    env["EDEN_NX2_DIAG_CODEC_CLIENT_REPLY"] = "1"
    env["EDEN_NX2_EXPERIMENTAL_CODEC_DECODE"] = "1"
    env["EDEN_NX2_EXPERIMENTAL_SYSMIX_OUTPUT"] = "1"
    env["EDEN_NX2_SYSMIX_DRAIN_MULTIPLIER"] = "1"
    env["EDEN_NX2_GPU_MAP_FLAG_PROBE"] = "1"
    env["EDEN_NX2_SILENT_BAD_OPUS"] = "1"
    env["EDEN_NX2_LOOPBACK_ONLY"] = "1"

    cmd = [
        "lichii-cli",
        "-c", f"{tmp_dir}/eden/qt-config.ini",
        "-d", "12345",
        "-g", GAME_MAIN
    ]

    print("Launching Lichii with PTY on port 12345...")
    master, slave = pty.openpty()
    proc = subprocess.Popen(cmd, stdout=slave, stderr=slave, stdin=subprocess.DEVNULL, env=env, close_fds=True)
    os.close(slave)

    main_base = None
    ready = False
    buf = ""

    try:
        while True:
            chunk = os.read(master, 1024).decode("utf-8", errors="replace")
            if not chunk:
                break
            buf += chunk
            if "Module main loaded at" in buf and not main_base:
                m = re.search(r"Module main loaded at (0x[0-9a-fA-F]+)", buf)
                if m:
                    main_base = int(m.group(1), 16)
                    print(f"Detected main load base: {hex(main_base)}")
            if "suspending guest process until a debugger continues" in buf:
                ready = True
                print("Lichii GDB stub ready!")
                break

        if not ready or main_base is None:
            raise RuntimeError("Failed to detect main load base or GDB readiness from Lichii stdout")

        def drain_output():
            while True:
                try:
                    c = os.read(master, 1024)
                    if not c:
                        break
                except Exception:
                    break

        drain_thread = threading.Thread(target=drain_output, daemon=True)
        drain_thread.start()

        gdb_script_path = f"{tmp_dir}/trace.py"
        bp_list_json = f"{tmp_dir}/bp_list.json"
        with open(bp_list_json, "w") as f:
            json.dump([{"idx": idx, "sym": sym} for idx, sym in targets], f)

        trace_out_tmp = f"{tmp_dir}/captured_calls.txt"

        gdb_py_content = f"""
import gdb, json, sys

gdb.execute("set architecture aarch64")
gdb.execute("target remote :12345")
gdb.execute("set pagination off")
gdb.execute("set confirm off")

main_base = {main_base}
plt_start = 0x4927690

with open("{bp_list_json}") as f:
    bp_data = json.load(f)

trace_file = open("{trace_out_tmp}", "w", buffering=1)
call_seq = 0

class PltBreakpoint(gdb.Breakpoint):
    def __init__(self, addr, sym):
        super().__init__(f"*{{hex(addr)}}")
        self.sym = sym

    def stop(self):
        global call_seq
        t = gdb.selected_thread()
        tname = t.name or ""
        if tname == "MainThread" or "Main" in tname or t.num == 1:
            call_seq += 1
            trace_file.write(f"[HLE TRACE {{call_seq:03d}}] [TID MainThread] {{self.sym}}\\n")
            if "nvnBootstrapLoader" in self.sym:
                trace_file.write("[hos MainThread] Reached first NVN2 call (nvnBootstrapLoader)\\n")
                return True
        return False

for item in bp_data:
    addr = main_base + plt_start + item["idx"] * 16
    PltBreakpoint(addr, item["sym"])

print(f"Registered {{len(bp_data)}} PLT breakpoints in GDB. Continuing execution...")
gdb.execute("continue")
trace_file.close()
print(f"Capture finished. Total main calls: {{call_seq}}")
gdb.execute("quit")
"""
        with open(gdb_script_path, "w") as f:
            f.write(gdb_py_content)

        print("Attaching GDB to capture trace...")
        gdb_cmd = f"nix-shell -p gdb --run \"gdb -batch -x {gdb_script_path}\""
        gdb_res = subprocess.run(gdb_cmd, shell=True, capture_output=True, text=True, timeout=180)
        print("GDB stdout:\n", gdb_res.stdout)
        if gdb_res.stderr:
            print("GDB stderr:\n", gdb_res.stderr)

        with open(OUT_TRACE, "w") as f_out:
            f_out.write(f"# Lichii NN Trace Capture\n")
            f_out.write(f"# Lichii Store Path: {prov['store_path']}\n")
            f_out.write(f"# Lichii Binary Rev: {prov['rev']}\n")
            f_out.write(f"# Lichii Patch List Hash: {prov['patch_list_hash']}\n")
            f_out.write(f"# Main Load Base: {hex(main_base)}\n")
            f_out.write(f"# PLT Mapping Method: AArch64 PLT disassembly at 0x4927690 + idx * 16 (ADRP+LDR to GOT)\n")
            f_out.write(f"# Target Range: main module imports up to nvnBootstrapLoader\n")
            f_out.write(f"# ------------------------------------------------------------\n")
            if os.path.exists(trace_out_tmp):
                with open(trace_out_tmp, "r") as f_in:
                    f_out.write(f_in.read())
        print(f"Trace saved to {OUT_TRACE}")

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except Exception:
            proc.kill()
        try:
            os.close(master)
        except Exception:
            pass
        shutil.rmtree(tmp_dir, ignore_errors=True)
        print("Lichii and GDB stopped, temporary files cleaned up.")

if __name__ == "__main__":
    main()
