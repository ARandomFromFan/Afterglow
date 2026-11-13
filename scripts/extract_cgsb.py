#!/usr/bin/env python3
"""Extract raw CGSB containers and individual shader stages from FromSoftware DCX archives."""
import sys
import os
import zlib
import struct
import argparse

def decompress_dcx(data):
    dca_pos = data.find(b'DCA\x00')
    if dca_pos < 0:
        return None
    return zlib.decompress(data[dca_pos + 8:])

def parse_bnd4(decomp):
    if not decomp.startswith(b'BND4'):
        raise ValueError("Not a valid BND4 archive")
    file_count = struct.unpack('<I', decomp[0x0c:0x10])[0]
    headers_start = 0x40
    entry_sz = 36
    files = []
    for i in range(file_count):
        entry = decomp[headers_start + i * entry_sz : headers_start + (i + 1) * entry_sz]
        flags, unk, comp_sz, uncomp_sz, data_off, name_off = struct.unpack('<iiqqqi', entry)
        file_bytes = decomp[data_off : data_off + uncomp_sz]
        # Read name if available
        name = f"file_{i:03d}"
        if name_off > 0 and name_off < len(decomp):
            end_name = decomp.find(b'\x00', name_off)
            if end_name > 0:
                name = decomp[name_off:end_name].decode('latin-1', errors='replace')
        files.append((name, file_bytes))
    return files

def main():
    p = argparse.ArgumentParser(description="Extract CGSB containers from DCX archives")
    p.add_argument("input_dcx", help="Path to .shaderbnd.dcx file")
    p.add_argument("--out-dir", default=None, help="Directory to dump raw CGSB files")
    p.add_argument("--sample", default=None, help="Dump first CGSB container to this path")
    args = p.parse_args()

    with open(args.input_dcx, "rb") as f:
        data = f.read()

    decomp = decompress_dcx(data)
    if not decomp:
        print("Failed to decompress DCX archive", file=sys.stderr)
        sys.exit(1)

    files = parse_bnd4(decomp)
    cgsb_files = [f for f in files if f[1].startswith(b'CGSB')]
    print(f"Archive: {args.input_dcx} ({len(files)} files, {len(cgsb_files)} CGSB containers)")

    for idx, (name, buf) in enumerate(cgsb_files):
        # Offset 0x04: constant in every CGSB file (0x00112233), purpose unknown
        magic, const_marker, unk1, unk2, api_tag_off = struct.unpack('<4sIIII', buf[:20])
        code_off, code_sz, ctrl_off, ctrl_sz = struct.unpack('<QQQQ', buf[0x28:0x48])
        print(f"  [{idx:03d}] {name}: const_marker=0x{const_marker:08x}, code=0x{code_off:x} (+{code_sz}), ctrl=0x{ctrl_off:x} (+{ctrl_sz})")
        if args.sample and idx == 0:
            with open(args.sample, "wb") as sf:
                sf.write(buf)
            print(f"Dumped sample CGSB container to {args.sample}")

    if args.out_dir:
        os.makedirs(args.out_dir, exist_ok=True)
        for name, buf in cgsb_files:
            out_path = os.path.join(args.out_dir, os.path.basename(name))
            with open(out_path, "wb") as f:
                f.write(buf)

if __name__ == "__main__":
    main()
