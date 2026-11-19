# AGENTS.md

Guide for coding agents working on Afterglow, a native port of *The Duskbloods* made by static recompilation. Read `README.md` first for what the project is and how the parts fit together.

## Layout

- `lifter/`, `third_party/a64-kit/`: AArch64 to C lifter. `scripts/lift.py` drives it.
- `host/`: HLE of the Switch 2 OS (`hos_*.c`) and its services (`nn_*.c`).
- `gpu/vk/`: NVN2 to Vulkan renderer, built as `afterglow_vk.so`. `gpu/vk/eden/` is Eden's shader recompiler, `gpu/vk/sirit/` the SPIR-V emitter. `gpu/vk/VULKAN.md` explains the design.
- `scripts/`: lift, build and analysis scripts. `scripts/env.py` resolves paths.
- `tests/`: `python3 -m unittest tests.test_pac` checks the lifter's PAC lowering.

## Setup

Copy `scripts/.env.example` to `scripts/.env` and set `WORK_DIR`, `GAME_DIR` and `LIFTER_KIT_DIR`. Never hardcode a machine-specific path in a tracked file. New scripts import `env` and use `env.WORK_DIR`, `env.GAME_DIR`, `env.LIFTER_KIT_DIR` or `env.REPO`.

You need clang, lld, pkg-config and Python 3 with the `lz4` package (the lifter reads compressed NSOs with it) on PATH, plus a Vulkan driver with `VK_EXT_mesh_shader` and `VK_EXT_external_memory_host`.

## Build

```sh
python3 scripts/build_vk.py   # afterglow_vk.so
python3 scripts/build.py      # duskbloods_game
```

- "Successfully linked" is the only success signal. A failed link prints `*** LINK FAILED ***` and leaves the old binary in place, so a run after it tests stale code. Check the banner every time.
- Without lld the error is `invalid linker name in argument '-fuse-ld=lld'`; do not drop `-fuse-ld=lld`.
- Touching the shader translator (`gpu/vk/eden/`, `gpu/vk/sirit/`) rebuilds most of the renderer's objects.
- `scripts/lift.py` regenerates all lifted C and is slow. Rerun it only after a lifter change.

## Shader program cache

The renderer caches translated programs in the work dir (`afterglow_programs.bin`, `.idx`, `afterglow_vk_pipelines.bin`). The cache stamp hashes the translator sources and the code-changing flags, and the log says when a cache is refused. When a new environment knob changes translation, add it to the stamp in `gpu/vk/vk_shader.cpp`, or an old cache will load as current. For a visual A/B after translator changes, move the three files aside first.

## Running

```sh
$WORK_DIR/duskbloods_game $WORK_DIR/sa/image/image.bin --romfs $GAME_DIR/romfs --user $WORK_DIR/user
```

- The game never exits on its own. Run it in the background with a time budget (about 5 minutes), never in the foreground, and never stream its stdout into your context. Kill it when the budget runs out.
- `HOS_AUDIO=0` runs without sound. Headless runs work under `gamescope --backend headless --expose-wayland -W 1920 -H 1080 --`.
- One game per GPU at a time, and do not build on a machine while a run there is being measured.
- Renderer screenshots are `RX_VK_SHOT_W`x`RX_VK_SHOT_H` (default 1280x720) whatever the window size; check the `vk: swapchain` log line for the real size.

## Debugging

Most diagnostics are environment knobs, read with `getenv` and off by default. Grep `gpu/vk/` and `host/` for `RX_VK_` and `HOS_` to find them. Common ones: `RX_VK_DUMP` (frame window), `RX_VK_DUMP_PROGS`, `RX_VK_DUMP_ARGS`, `RX_VK_PASS_TIMING`, `RX_VK_SKIP_PROG`, `HOS_PROFILE`.

- Programs are named by a 16-hex key such as `87def4ee...`. Use the key in reports and knobs.
- Read numbers from logs over pictures. Dumped stencil PNGs are quantised and misleading.
- Do not use `HOS_PATCH_DRAWS=raw`.

## Rules

- No game code, game data, keys or generated C in git. Run `scripts/check_content.sh` before committing.
- Online stays local. Sockets are forced to 127.0.0.1 and port 443 maps to 18443 (`host/nn_socket.c`). Do not weaken either.
- New behaviour goes behind an environment knob that is off by default until it is proven, then becomes the default in its own change.
- Performance claims need interleaved A/B runs (at least 3 + 3) on the same build, with pooled fps and GPU time. One run against an older baseline is not a result.
- Verify visual claims against the Switch 2 original before calling a difference cosmetic or a bug.
- Third-party code: copy only GPL-3.0-compatible code, and add a `THIRD_PARTY_NOTICES.md` entry naming the project and the exact commit you read.
- Commits use Conventional Commits (`feat:`, `fix(vk):`, `docs:`, ...). Plans and scratch notes are not committed.
- Comments: one line where possible, at most two, and only for what the code cannot show.
