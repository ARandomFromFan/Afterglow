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

