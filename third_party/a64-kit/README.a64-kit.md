# a64-kit

NotRexed's AArch64 kit, vendored as the core of this project's lifter (`lifter/`, driven by `scripts/lift.py`). See `PROVENANCE.md` for where it came from and `NOTICE.TOTKPort.md` for its credits.

**Licensing:** the instruction translator is suyu's (GPL-2.0-or-later, `lift/suyu/`), so this kit and everything built with it (the translator library, the generated runtime, all lifted C) are GPL.

## Layout

| Path | What |
|---|---|
| `tools/nso.py` | NSO loader: LZ4 segments, MOD0, dynamic section, symbols, relocations, init_array, `.eh_frame_hdr` function starts |
| `tools/survey.py` | Instruction census of a module's `.text` |
| `tools/toolchain.py` | clang/lld lookup, native target flags |
| `hos/nx_image.py` | The image builder (rtld's job at build time: modules at fixed addresses, relocations applied, imports bound) |
| `lift/a64_lift.py` | The lifter: functions, control flow, jump tables, PLT, calls |
| `lift/a64emit.cpp`, `build_emit.py`, `a64emit.py` | suyu's per-instruction translator as a shared library and its ctypes binding |
| `lift/a64_rtgen.py` | Writes suyu's FP helpers and runtime pieces, adapted to `rt/a64.h`, next to the lifted code |
| `lift/suyu/` | `arm64_to_c.h` with its licenses, plus this project's hardware FMA path (`HOS_FAST_FMA`) |
| `rt/a64.h` | The guest model the lifted code sees |
| `rt/a64_rt.c` | Dispatch, indirect calls and jumps, traps, exclusive monitor, CNTPCT |
| `tests/insn_test.py` | Every instruction form a game uses, Unicorn vs lifted C |

## The guest model

| | |
|---|---|
| Context | suyu's `GuestContext` shape: `x[32]` (x[31] = SP), NZCV bytes, `vreg[32][2]`, `fpcr`/`fpsr`, TPIDR(RO)_EL0, plus the exclusive monitor and thread pointer |
| Functions | `void f_XXXXXXXX(Ctx *restrict c)`: an entry plus everything reachable without crossing another entry. Branches and fallthrough into another entry are `TAILCALL`s |
| Calls | `bl` writes LR and calls; `blr` → `a64_icall` (binary search over the function table); `ret` returns |
| `br` | Recovered jump tables → `switch` on the target; PLT stubs resolved through the relocated GOT; anything else (virtual tail calls) → `a64_ijump` |
| Exclusives | `ldxr` remembers (address, size, value); `stxr` compare-and-swaps against it, so real host threads are safe |
| Entries | `.eh_frame_hdr` starts, exports, init_array, RELATIVE/ABS64 targets in code, `bl` targets |
