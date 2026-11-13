# AGENTS.md

Guide for coding agents working on Afterglow, a native port of *The Duskbloods* made by static recompilation. Read `README.md` first for what the project is and how the parts fit together.

## Layout

- `lifter/`, `third_party/a64-kit/`: AArch64 to C lifter. `scripts/lift.py` drives it.
- `host/`: HLE of the Switch 2 OS (`hos_*.c`) and its services (`nn_*.c`).
- `gpu/vk/`: NVN2 to Vulkan renderer, built as `afterglow_vk.so`. `gpu/vk/eden/` is Eden's shader recompiler, `gpu/vk/sirit/` the SPIR-V emitter. `gpu/vk/VULKAN.md` explains the design.
- `scripts/`: lift scripts. `scripts/env.py` resolves paths.
- `tests/`: `python3 -m unittest tests.test_pac` checks the lifter's PAC lowering.

## Setup

Copy `scripts/.env.example` to `scripts/.env` and set `WORK_DIR`, `GAME_DIR` and `LIFTER_KIT_DIR`. Never hardcode a machine-specific path in a tracked file. New scripts import `env` and use `env.WORK_DIR`, `env.GAME_DIR`, `env.LIFTER_KIT_DIR` or `env.REPO`.

You need clang, lld, pkg-config and Python 3 with the `lz4` package (the lifter reads compressed NSOs with it) on PATH, plus a Vulkan driver with `VK_EXT_mesh_shader` and `VK_EXT_external_memory_host`.

## Build

- `scripts/lift.py` regenerates all lifted C and is slow. Rerun it only after a lifter change.

## Shader program cache

The renderer caches translated programs in the work dir (`afterglow_programs.bin`, `.idx`, `afterglow_vk_pipelines.bin`). For a visual A/B after translator changes, move the three files aside first.

## Debugging

Most diagnostics are environment knobs, read with `getenv` and off by default. Grep `gpu/vk/` and `host/` for `RX_VK_` and `HOS_` to find them. Common ones: `RX_VK_DUMP` (frame window), `RX_VK_DUMP_PROGS`, `RX_VK_DUMP_ARGS`, `RX_VK_PASS_TIMING`, `RX_VK_SKIP_PROG`, `HOS_PROFILE`.

- Programs are named by a 16-hex key such as `87def4ee...`. Use the key in reports and knobs.
- Read numbers from logs over pictures. Dumped stencil PNGs are quantised and misleading.
- Do not use `HOS_PATCH_DRAWS=raw`.

## Rules

- Online stays local. Sockets are forced to 127.0.0.1 and port 443 maps to 18443 (`host/nn_socket.c`). Do not weaken either.
- New behaviour goes behind an environment knob that is off by default until it is proven, then becomes the default in its own change.
- Performance claims need interleaved A/B runs (at least 3 + 3) on the same build, with pooled fps and GPU time. One run against an older baseline is not a result.
- Verify visual claims against the Switch 2 original before calling a difference cosmetic or a bug.
- Third-party code: copy only GPL-3.0-compatible code, and add a `THIRD_PARTY_NOTICES.md` entry naming the project and the exact commit you read.
- Commits use Conventional Commits (`feat:`, `fix(vk):`, `docs:`, ...). Plans and scratch notes are not committed.
- Comments: one line where possible, at most two, and only for what the code cannot show.
