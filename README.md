# Afterglow

A native PC port of *The Duskbloods*, made by static recompilation. The game's AArch64 code is lifted to C, compiled for x86-64 and run on top of a high-level emulation (HLE) of the Switch 2 OS, with NVN2 graphics translated to Vulkan.

Linux comes first. Windows is planned once Linux is in good shape.

**This repository contains no game code or data.** You need your own dump of the game.

## Status

Work in progress. The game boots, reaches the title menu and plays in-game.

Known issues:

- Hatching-like stripes on some walls.
- Menu text shows overlapping ghost letters after the language is changed while the game runs. Text is correct when the game starts in that language.
- For now performance is CPU-bound, around 40 fps in heavy scenes; the target is 60.

## How it works

| Part | Where | What it does |
|------|-------|--------------|
| Lifter | `lifter/`, `third_party/a64-kit/` | Turns the game's executables into C |
| Host runtime | `host/` | HLE of the Switch 2 OS services: memory, threads, filesystem, audio, input, network |
| Renderer | `gpu/vk/` | NVN2 to Vulkan, with Eden's shader recompiler for Maxwell/Ampere shaders to SPIR-V |

## Requirements

- Linux on x86-64
- A Vulkan driver with `VK_EXT_mesh_shader` and `VK_EXT_external_memory_host`
- clang, lld, Python 3, pkg-config
- Your own dump of the game: `exefs` and `romfs`

## Contributing

Help is welcome and really appreciated, whether it's code, testing or bug reports.

Most needed:

- **Windows port.** Windows is planned but needs people to work on it.
- **Testing on hardware we lack**: NVIDIA GPUs on Linux, NVIDIA and AMD GPUs on Windows, and Intel CPUs and GPUs on both. Reports with your CPU, GPU, driver version and a log help a lot.

Coding agents should read `AGENTS.md` first.

## Legal

You must own the game and dump it yourself. Do not open issues or pull requests that contain game files, keys or generated C.

Licensed under GPL-3.0-or-later; see `LICENSE`. Third-party code and its licenses are listed in `THIRD_PARTY_NOTICES.md`.

*The Duskbloods* is a trademark of its owners. This project is not affiliated with or endorsed by them.
