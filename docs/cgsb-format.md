# CGSB shader container

Measured on all 2,675 CGSB files in the nine `romfs/shader/*.shaderbnd.dcx` archives of the Network Test (title
0400d1d02399a000 v0) with `scripts/extract_all_cgsb.py`. Fields not listed here were not decoded. All values are
little-endian.

## Header (0x68 bytes)

| Offset | Size | Value | Notes |
|---|---|---|---|
| 0x00 | 4 | `CGSB` | magic |
| 0x04 | 4 | 0x00112233 | same in every file |
| 0x08 | 4 | bits 15:0 = 1, bits 31:16 = stage | stage in pipeline order (0 VS, 1 TCS, 2 TES, 3 GS, 4 FS, 5 CS, 6 task, 7 mesh); it maps one to one onto the control section's NVN stage in every file |
| 0x0c | 4 | 0 | |
| 0x10 | 8 | 0x18 | offset of the API block |
| 0x18 | 4 | `NVN ` | API tag |
| 0x1c | 4 | 1 | |
| 0x20 | 8 | usually 0 | nonzero in 19 files (0x48000, 0x90000, 0x240000, 0x288000, 0xf30000); meaning unknown |
| 0x28 | 4 x 16 | (offset, size) pairs, u64 each | four sections |

## Sections

| # | Offset | Size | Contents |
|---|---|---|---|
| 0 | always 0x68 | equals the control section's +0x44 (code area end) in every file | NVN2 code area: for graphics stages a 128-byte program header, then Ampere instructions up to the control section's +0x38 length; c1 constants from +0x40 to +0x44 |
| 1 | right after section 0 | 0xe0 (2,318 files), 0xf8, 0x108, 0x118, 0x128 or 0x168 | NVN2 control section: magic 0x98761234, version byte 0xc9, NVN stage u16 at +0x56 |
| 2 | right after section 1 | 332 bytes in every file | not decoded |
| 3 | after section 2, or (0, 0) | 12 x n bytes, vertex stage only (246 of 253 vertex shaders) | not decoded |

The file ends at the end of the last present section (no trailing bytes in any file).

## Where the NVN2 shader binary is

The binary the NVN2 driver consumes is section 1 (control) plus section 0 (code area). `extract_all_cgsb.py` writes
them as `<name>.ctl` and `<name>.code`, which `gpu/vk/tests/shader_pipeline_test.cpp` and `gpu/shadertool.cpp`
read. `<name>.bin` holds the instructions alone (after the 128-byte header for graphics stages, up to +0x38).

## Comparison with Hookshot

Hookshot (8b2e1d3b, `gpu/render/rx_shader.cpp` line 957 comment, `gpu/vk/vk_shader.cpp` `control_end` and
`parse_code`) has no CGSB reader. It reads a stage as the control section (0xd8 bytes, magic 0x98761234) and the code
area (`end` = +0x44 bytes) at the addresses the game passes to `nvnProgramSetShaders`, skips a 128-byte header for
stages other than 5, and reads instructions up to +0x38. CGSB sections 1 and 0 have that shape in all 2,675 files, so
the corpus test feeds the recompiler the same input Hookshot gets at runtime. Every CGSB control section is longer
than the 0xd8 bytes Hookshot reads (0xe0 to 0x168); the extra bytes are not decoded.
