# gpu/: the standalone build's renderer

`render/`, `compat/` and `third_party/suyu/` are TOTKPort's renderer (`a64-kit/gpu/`, commit `e88744a`,
2026-10-31): the native NVN layer's replay on WebGPU (wgpu-native on the PC), its texture, buffer, pipeline and
present code, and suyu's shader recompiler with TOTKPort's WGSL backend. `host/nvn_native.c` and `host/display.c`
are the API side and the window, from the same commit (see `host/PROVENANCE.md`).

Licensing as in TOTKPort's `NOTICE.md`: suyu's parts are GPL-2.0-or-later / GPL-3.0-or-later per file (texts in
`third_party/suyu/`); `render/nvn_api.h` (h1k421, as adapted by jhaizhou), the NVN layer's design (jhaizhou's
d2r-mobile / MacNativeGames) and the FSR 1 port in `render/rx_present_fx.cpp` are MIT; `render/renderdoc_app.h` is
RenderDoc's MIT header. wgpu-native (MIT or Apache-2.0) is not vendored: `nx2/build.py` takes TOTKPort's build of it
(`WGPU_NATIVE` for another).

mkw-recomp's changes, for the Switch 2:
- **Ampere shaders.** `third_party/suyu/shader_recompiler/frontend/ampere/` is Eglantine's Ampere-to-Maxwell
  lowering (header-only; Eden Emulator Project, GPL-3.0-or-later, from the `work/eg` clone). `environment.h` gains
  `TranslateProgramAddress` and the control flow passes indirect branch targets through it, as in Eglantine.
- **NVN2 shader packages** (`render/rx_shader.cpp` `read_nvn_code`): the control section's layout (magic
  `0x98761234`, code length `+0x38`, c1 constants `+0x40..+0x44`, stage `+0x56`, workgroup `+0x58`), the 128-byte
  graphics program header, and the lowering before the Maxwell frontend.
- **The Switch 2's GOB swizzle** (the Tegra X1's with offset bits 5 and 6 swapped, from Eglantine's native tiling):
  `third_party/suyu/video_core/textures/decoders.h` (the CPU swizzle masks), the two WGSL unswizzles in
  `render/rx_texture_gpu.cpp`, and `bl_offset` in `render/rx_nvn.cpp`.
- **The 64-bit guest window**: per-page tables in `render/rx_buffer.cpp` are committed untouched (`PageTable`).
- **The window crop** (`rx_present_crop`): MKW renders at a dynamic size into a 3840x2160 window texture and crops
  it; the crop is copied out and presented.

`mkw_gpu.cpp` / `mkw_gpu.h` / `build-gpu.bat` are the earlier Vulkan stand-in (a window and a swapchain; no longer
loaded). `shadertool.cpp` runs dumped NVN2 programs through Eglantine's recompiler offline (SPIR-V; it prints what
each program reads from the constant banks).

## Modifications to vendored Hookshot and Eden files

- `gpu/vk/vk_replay.cpp`: added periodic screenshot export guarded by `HOS_SCREENSHOT` and `HOS_SCREENSHOT_INTERVAL` environment variables for milestone verification, and logged unrendered present image zero-initialization.
- `gpu/vk/vk_texture.cpp`: logged zero-initialization of newly cleared non-block-compressed render target VkImages.
- `gpu/vk/eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp`: guarded `cbufs[ssbo.cbuf_index]` with `Sirit::ValidId()` checks in `DefineGlobalMemoryFunctions` to avoid null ID assertion aborts on unused constant buffer indices.
- `gpu/vk/vk_main.cpp`: enabled `VK_EXT_debug_utils` to track validation message counts, and selected `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` to prevent invalid swapchain colorspace requests.
- `gpu/vk/vk_formats.cpp`: added runtime device format check falling back from unsupported D24S8/D24 to D32S8/D32 on AMD RADV.
- `gpu/vk/vk_shader.cpp`: inlined no-op `GuardFragmentOutputs` to remove dependency on deleted `oot_visual_workaround.h`.
- `gpu/vk/vk_menu.cpp`: replaced deleted `oot_cheat_items.h` with empty stubs to allow menu compilation without OoT files.
- `gpu/vk/vk_internal.h`: declared `validation_report()` for diagnostic message reporting.
- `gpu/vk/eden/shader_recompiler/frontend/ampere/`: all eight headers replaced by Lichii's `src/shader_recompiler/frontend/ampere/` at commit `d2eaae4f3abca636b030b726f107bace06f61079` (GPL-3.0-or-later). Seven are unchanged; `lower_to_maxwell.h` adds Hookshot's `native_matrix_multiply` HMMA_nx2 path and a `register_map_words` field that stays 0 (REGMAP_nx2 renaming not ported), marked `(afterglow)`. Lichii defines 0x34e as a native program counter load (d2eaae4 `lower_to_maxwell.h` line 3870) and 0x845 / 0x906 as an immediate integer-to-float conversion (line 3220); SULD 0x998, SUST 0x99c, ISBERD 0x923 and ALD 0x321 come from the same files.
- `gpu/third_party/suyu/shader_recompiler/`: deleted unused duplicate shader recompiler in favour of canonical Eden recompiler in `gpu/vk/eden/`.
