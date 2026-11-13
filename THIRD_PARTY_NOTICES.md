# Third Party Notices

This project is licensed under the GNU General Public License, version 3 or (at your option) any later version (GPL-3.0-or-later); the full text of version 3 is in `LICENSE`. It references and adapts components from the following third party projects.


## Hookshot
- Source: https://git.notrexed.dev/Hookshot/Hookshot (github.com/NotRexed/Hookshot is gone)
- Pinned Commit: `8b2e1d3b895a5f890dcb886d904b62d6b38df2c7` (tag: `v2.0.8-rc.1`)
- Upstream issue 915 (driver fault in a Linux pipeline compile): fixed by commit `2079b41fec8aa3b153299cc545cf13fc02cdaf8f`, an ancestor of `8b2e1d3` (`git merge-base --is-ancestor`).
- License: GNU General Public License v3.0 (GPL-3.0)
- Components referenced / adapted:
  - `a64-kit` ARM64 lifter and tools vendored in `third_party/a64-kit` and `lifter/`.
  - Horizon OS runtime foundation (`host/hos.h`, `host/hos_main.c`, `host/hos_mem.c`, `host/hos_thread.c`, `host/hos_svc.c`).
  - NVN2 and Vulkan rendering engine vendored in `gpu/` (`gpu/render/`, `gpu/vk/`).
- Notice: Reusing or linking code from Hookshot or a64-kit subjects derivative binaries to GPL-3.0 terms.


## LASRecomp
- Source: https://github.com/peiikai/LASRecomp (references/LASRecomp)
- Commit: `579fdad6d736c8fbf1d21943593731eb926b6659`
- License: MIT License
- Components referenced:
  - Finished port startup sequence patterns (`runtime/main.c`, `runtime/loader.c`).
  - HLE import dispatch structure and explicit stub whitelisting (`runtime/hle/`).
  - Fiber and delegate null safety patterns (`runtime/hle/delegate_null.c`).
- Notice: Used as a secondary MIT reference. NVN1 assumptions from LASRecomp are not applied to the Switch 2 NVN2 path.


## Lichii
- Source: https://github.com/zortos293/Lichii
- Pinned Commit: `d2eaae4f3abca636b030b726f107bace06f61079` (source archive https://github.com/zortos293/Lichii/archive/d2eaae4f3abca636b030b726f107bace06f61079.tar.gz, the one the reference Lichii build uses)
- License: GNU General Public License v3.0 (GPL-3.0-or-later)
- Components copied from `src/shader_recompiler/frontend/ampere/` into `gpu/vk/eden/shader_recompiler/frontend/ampere/`:
  - Unchanged (byte-identical to d2eaae4, checked with `diff`): `geometry_exit.h`, `instruction.h`, `program_address_map.h`, `program_capture.h`, `program_override.h`, `register_pressure.h`, `trap_guards.h`.
  - Modified: `lower_to_maxwell.h`. Hookshot additions are marked `(hookshot)`; additions made in this repository are marked `(afterglow)`: the `native_matrix_multiply` flag (HMMA.16816 lowers to one HMMA_nx2 word when the cooperative matrix self-test passed) and a `register_map_words` field that this lowering leaves at 0 (Hookshot's REGMAP_nx2 register renaming is not ported).
  - This includes the lowering of opcode 0x34e ("native program counter load", geometry stage only, d2eaae4 `lower_to_maxwell.h` line 3870) and opcodes 0x845 / 0x906 ("native immediate integer-to-float conversion", 0x906 rounding up, d2eaae4 `lower_to_maxwell.h` line 3220), along with SULD 0x998, SUST 0x99c, ISBERD 0x923 and ALD 0x321.


## Suyu (Texture Decoders)
- Source: https://gitlab.com/suyu-emu/suyu
- License: GNU General Public License v2.0-or-later / v3.0-or-later
- Components referenced / adapted:
  - ASTC and BCN texture decompression and Tegra GOB swizzle routines vendored in `gpu/third_party/suyu/video_core/textures/` and `gpu/third_party/suyu/common/`.
  - The duplicate `gpu/third_party/suyu/shader_recompiler` was deleted in favour of the canonical Eden recompiler in `gpu/vk/eden/`.


## totk-pc-recomp
- Source: https://github.com/valentderah/totk-pc-recomp
- License: MIT License
- Components referenced:
  - Static recompilation architecture and pipeline design.


## SavageWindRecomp
- Source: https://github.com/Revivedx/SavageWindRecomp
- License: MIT License
- Components referenced:
  - Native Switch recompilation workflow.


## OpenSSL
- Source: https://github.com/openssl/openssl
- License: Apache License 2.0
- Components used: the host's `libssl`/`libcrypto` (found with `pkg-config openssl`, linked by `scripts/build.py`) as the TLS implementation behind the `nn::ssl` HLEs in `host/nn_ssl.c`.
- Notice: OpenSSL is linked, not vendored or modified. It is used only for the game's own client connections to loopback test servers, with this project's test CA; the system trust store is never loaded.

