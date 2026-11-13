#!/usr/bin/env python3
"""Compare Duskbloods host memory calls against Lichii ground truth capture."""
import argparse
import re
import sys

def parse_args():
    p = argparse.ArgumentParser(description="Compare host memory calls with capture")
    p.add_argument("--host-log", required=True, help="Duskbloods run log")
    p.add_argument("--capture", required=True, help="Lichii memory capture file")
    return p.parse_args()

def parse_host_memory_calls(path):
    calls = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if "SetMemoryHeapSize(" in line:
                m = re.search(r"SetMemoryHeapSize\((0x[0-9a-fA-F]+)", line)
                if m:
                    calls.append(("SetMemoryHeapSize", int(m.group(1), 16)))
            elif "AllocateMemoryBlock(" in line:
                m = re.search(r"AllocateMemoryBlock\([^,]+,\s*(0x[0-9a-fA-F]+)", line)
                if m:
                    calls.append(("AllocateMemoryBlock", int(m.group(1), 16)))
            elif "GetInfo 31" in line or "Nx2RemoteMemorySize" in line:
                m = re.search(r"0x([0-9a-fA-F]+)", line)
                if m:
                    calls.append(("GetInfo31", int(m.group(1), 16)))
    return calls

def parse_capture_memory_calls(path):
    calls = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if "SetMemoryHeapSize" in line:
                m = re.search(r"Heap size set to (0x[0-9a-fA-F]+)", line)
                if m:
                    calls.append(("SetMemoryHeapSize", int(m.group(1), 16)))
            elif "GetInfo 31" in line:
                m = re.search(r"Remote memory size: (0x[0-9a-fA-F]+)", line)
                if m:
                    calls.append(("GetInfo31", int(m.group(1), 16)))
    return calls

def main():
    args = parse_args()
    host_calls = parse_host_memory_calls(args.host_log)
    cap_calls = parse_capture_memory_calls(args.capture)

    print("Memory call comparison:")
    print(f"  Host calls parsed:    {len(host_calls)}")
    print(f"  Capture calls parsed: {len(cap_calls)}")

    # Check primary heap sizing
    host_heap = next((val for name, val in host_calls if name == "SetMemoryHeapSize"), None)
    cap_heap = next((val for name, val in cap_calls if name == "SetMemoryHeapSize"), None)

    if host_heap is None:
        print("[FAIL] Host log missing SetMemoryHeapSize call", file=sys.stderr)
        sys.exit(1)
    if cap_heap is None:
        print("[FAIL] Capture missing SetMemoryHeapSize call", file=sys.stderr)
        sys.exit(1)

    print(f"  Host SetMemoryHeapSize:    {hex(host_heap)} ({host_heap >> 20} MB)")
    print(f"  Capture SetMemoryHeapSize: {hex(cap_heap)} ({cap_heap >> 20} MB)")

    if host_heap != cap_heap:
        print(f"[FAIL] Heap size mismatch: host {hex(host_heap)} != capture {hex(cap_heap)}", file=sys.stderr)
        sys.exit(2)

    # Check AllocateMemoryBlock
    host_block = next((val for name, val in host_calls if name == "AllocateMemoryBlock"), None)
    expected_block = cap_heap - (7304 * 1024 * 1024)
    if host_block:
        print(f"  Host AllocateMemoryBlock:  {hex(host_block)} ({host_block >> 20} MB)")
        print(f"  Expected initial block:    {hex(expected_block)} ({expected_block >> 20} MB)")
        if host_block != expected_block:
            print(f"[FAIL] Block size mismatch: host {hex(host_block)} != expected {hex(expected_block)}", file=sys.stderr)
            sys.exit(3)

    print("[PASS] Host memory configuration matches Lichii ground truth.")

if __name__ == "__main__":
    main()
