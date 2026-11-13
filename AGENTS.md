# AGENTS.md

Guide for coding agents working on Afterglow, a native port of *The Duskbloods* made by static recompilation. Read `README.md` first for what the project is and how the parts fit together.

## Layout

- `lifter/`, `third_party/a64-kit/`: AArch64 to C lifter. `scripts/lift.py` drives it.
- `scripts/`: lift scripts. `scripts/env.py` resolves paths.
- `tests/`: `python3 -m unittest tests.test_pac` checks the lifter's PAC lowering.

## Setup

Copy `scripts/.env.example` to `scripts/.env` and set `WORK_DIR`, `GAME_DIR` and `LIFTER_KIT_DIR`. Never hardcode a machine-specific path in a tracked file. New scripts import `env` and use `env.WORK_DIR`, `env.GAME_DIR`, `env.LIFTER_KIT_DIR` or `env.REPO`.

You need clang, lld, pkg-config and Python 3 with the `lz4` package (the lifter reads compressed NSOs with it) on PATH.

## Build

- `scripts/lift.py` regenerates all lifted C and is slow. Rerun it only after a lifter change.

## Rules

- New behaviour goes behind an environment knob that is off by default until it is proven, then becomes the default in its own change.
- Performance claims need interleaved A/B runs (at least 3 + 3) on the same build, with pooled fps and GPU time. One run against an older baseline is not a result.
- Verify visual claims against the Switch 2 original before calling a difference cosmetic or a bug.
- Third-party code: copy only GPL-3.0-compatible code, and add a `THIRD_PARTY_NOTICES.md` entry naming the project and the exact commit you read.
- Commits use Conventional Commits (`feat:`, `fix(vk):`, `docs:`, ...). Plans and scratch notes are not committed.
- Comments: one line where possible, at most two, and only for what the code cannot show.
