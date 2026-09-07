---
name: pico-tn160-linux-workspace
description: Safely operate pico_tn160 after its migration to /home/slam/dev, including Linux preflight, CMake preset builds, stale-path audits, old-copy deletion gates, and protection against unauthorized cache or toolchain changes. Use when resuming this repository, cleaning old Sipeed/home copies, fixing its CMake environment, or preparing thermal work in the Linux-native workspace. Do not use for thermal-model decisions themselves; pair those tasks with pico-tn160-thermal-drift.
---

# pico_tn160 Linux workspace

Keep repository location, build identity, authorization, and artifact naming deterministic. A path-cleanup request does not authorize a build, cache migration, toolchain change, or adjacent refactor.

## Frozen locations

- Sole project root: `/home/slam/dev/pico_tn160`.
- Linux preflight: `/home/slam/tools/agent_preflight.py --project pico-tn160`.
- Environment: `/home/slam/dev/env.sh`.
- Build preset: `pico2-release` from `CMakePresets.json`.
- Formal build directory: `/home/slam/dev/pico_tn160/build`.
- Formal UF2: `/home/slam/dev/pico_tn160/build/pico_tn160.elf.uf2`.
- `/home/slam/pico_tn160` and `/home/slam/Sipeed/pico_tn160` were intentionally deleted on 2026-09-03. Do not recreate them.
- Do not delete `/home/slam/Sipeed/tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi` until every consumer and `env.sh` selection is independently audited.

Linux-wide tools belong under `/home/slam/tools`. Windows-hosted projects should keep their own preflight under the repository's `tools/`; do not move or delete the remaining shared Windows preflight until all callers are migrated.

## Authorization boundary

Before any write, restate the exact user-authorized outcome and list each mutation class separately: reference/path edits, creation or movement of tools, deletion of exact directories, CMake/cache/build changes, and firmware/algorithm changes.

Authorization for one class is not authorization for another. Questions, observations, “看看”, or agreement with a diagnosis do not authorize deletion/building. If the user asks only to delete named old copies, audit first and delete only those copies; do not move `build/`, change the build system, or compile.

## Read-only intake

1. Read `AGENTS.md`, `HANDOFF.md`, `C_context/MEMORY.md`, and the task-specific Skill.
2. Run the Linux preflight and `git status --short`.
3. Confirm `pwd`, `realpath`, `stat`, and `findmnt` for every old/new path. A directory under `/home/slam/Sipeed` is a Windows 9p original, not merely an alias.
4. Before deletion, compare Git HEAD, tracked diff, untracked file lists, and content checksums while excluding `.git` and generated `build/` data. Scan active files for hardcoded old roots and check for running processes.
5. Preserve unrelated dirty work. Do not reset, restore, clean, commit, or push unless explicitly requested.

## Build contract

Use only these commands:

```bash
# Incremental
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --build --preset pico2-release --parallel "$(nproc)"

# Configure and build
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --preset pico2-release && cmake --build --preset pico2-release --parallel "$(nproc)"

# Fresh configure and build
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --fresh --preset pico2-release && cmake --build --preset pico2-release --parallel "$(nproc)"
```

`build.sh` was deleted and must not be restored. Never run a build merely to validate a path-only request. Before a fresh build, state that it rewrites generated files under `build/` and obtain explicit authorization.

## CMake invariants and warnings

- `pico_sdk_import` must be included before `project()` so CMake can find the SDK's `Platform/PICO.cmake` while enabling languages.
- The target deliberately keeps `OUTPUT_NAME "${PROJECT_NAME}.elf"` and `SUFFIX ""`; this preserves `pico_tn160.elf` and `pico_tn160.elf.uf2` after correct platform initialization.
- `System is unknown to cmake` indicates initialization/module-path ordering, not a generic harmless cross-compile message.
- The `.note.GNU-stack` warning comes from the selected ARM GNU Toolchain CRT object such as `crtn.o`. Do not patch binary toolchain objects, upgrade the shared toolchain, add `-z noexecstack`, or hide it with `--no-warn-execstack` without a concrete CI/security requirement and separate authorization.
- Build success proves compilation only. Report UF2 bytes/SHA and retain “未上板验证” until real hardware feedback exists.

## Closing the task

- Re-scan active AGENTS/README/Skill/preflight files for old paths and obsolete `build.sh` calls.
- Validate `cmake --list-presets`, `git diff --check`, task-specific tests, and changed Skills with `quick_validate.py`.
- If migration is the task, report exact deleted paths, recoverability, and deliberately untouched external caches/toolchains.
- For algorithm work, stop here and load `pico-tn160-thermal-drift`; this Skill does not authorize or choose a compensation model.
