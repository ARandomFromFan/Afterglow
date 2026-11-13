# Afterglow

A native PC port of *The Duskbloods*, made by static recompilation. The game's AArch64 code is lifted to C, compiled for x86-64 and run on top of a high-level emulation (HLE) of the Switch 2 OS, with NVN2 graphics translated to Vulkan.

Linux comes first. Windows is planned once Linux is in good shape.

**This repository contains no game code or data.** You need your own dump of the game.

## How it works

| Part | Where | What it does |
|------|-------|--------------|
| Lifter | `lifter/`, `third_party/a64-kit/` | Turns the game's executables into C |
| Host runtime | `host/` | HLE of the Switch 2 OS services: memory, threads, filesystem |

## Requirements

- Linux on x86-64
- clang, lld, Python 3, pkg-config
- Python 3 with the `lz4` package
- Your own dump of the game: `exefs` and `romfs`

## Lifting

```sh
python3 scripts/lift.py
```

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
