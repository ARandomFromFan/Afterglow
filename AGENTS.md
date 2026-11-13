# AGENTS.md

Guide for coding agents working on Afterglow, a native port of *The Duskbloods* made by static recompilation. Read `README.md` first for what the project is and how the parts fit together.

## Rules

- New behaviour goes behind an environment knob that is off by default until it is proven, then becomes the default in its own change.
- Performance claims need interleaved A/B runs (at least 3 + 3) on the same build, with pooled fps and GPU time. One run against an older baseline is not a result.
- Verify visual claims against the Switch 2 original before calling a difference cosmetic or a bug.
- Third-party code: copy only GPL-3.0-compatible code, and add a `THIRD_PARTY_NOTICES.md` entry naming the project and the exact commit you read.
- Commits use Conventional Commits (`feat:`, `fix(vk):`, `docs:`, ...). Plans and scratch notes are not committed.
- Comments: one line where possible, at most two, and only for what the code cannot show.
