---
name: kessoku-build
description: Canonical configure-build-smoke-test-commit loop for this repo (CMake preset, /W4-clean MSVC build, window smoke test, minimal staged commits pushed for diff review). Load before configuring, building, smoke-testing, committing, or reviewing build claims.
license: MIT
compatibility: opencode
metadata:
  domain: build
  risk: low
---

## The invariant

> A build claim is proven by log output, never asserted. "Warning-free" means the
> build log shows zero warnings -- not that none were noticed.

## The loop

Run from the repo root (`C:\dev\Kessoku`). Read current values from
`CMakeLists.txt` / `CMakePresets.json` / `vcpkg.json` first -- do not hardcode the
C++ standard, dependency versions, or preset names from memory; the commands below
are canonical, the versions are whatever the files say today.

1. Clean configure:
   - `Remove-Item -LiteralPath ".\build" -Recurse -Force` (if it exists)
   - `cmake --preset default` -- must finish with no errors.
2. Build: `cmake --build --preset default` -- zero errors, zero warnings at `/W4`.
   Confirm the emitted standard flag in the build log matches the
   `CMAKE_CXX_STANDARD` in `CMakeLists.txt`.
3. Smoke test `.\build\Debug\Kessoku.exe` (working directory `.\build\Debug` so the
   vcpkg-deployed DLLs resolve): a window titled "Kessoku" appears, no console
   window, close it and confirm exit code 0.
4. Spot-check with `dumpbin /headers` when the subsystem or architecture is in
   question: expect `subsystem (Windows GUI)`, `machine (x64)`.
5. Git hygiene: `git status --short` must show only intended files;
   `git show --stat` on the commit must prove it. Stage exact paths
   (`git add <paths>`, never `git add -A`), commit, `git push` for diff review.

## Non-negotiables

- MSVC is the compiler of record. Advisory tools (clangd, `/analyze`) are second
  opinions -- their diagnostics never override or weaken a clean MSVC `/W4` build,
  and chasing their noise is out of scope for a build session.
- No warning suppressions to achieve a clean build. Ever.
- One logical change per commit; build-file changes (`CMakeLists.txt`,
  `CMakePresets.json`) and dependency changes (`vcpkg.json`) never share a commit
  with source changes unless the task explicitly says so.
- `build/` and `vcpkg_installed/` are gitignored -- they must never appear in
  `git status` as anything but ignored.

## Stop conditions

Stop and flag rather than deciding if:
- Achieving zero warnings requires disabling a warning category -- report which
  warning and why instead of suppressing it.
- `git status` shows unexpected files -- don't sweep them into the commit.
- The smoke test fails (no window, wrong title, console appears, nonzero exit) --
  don't commit a broken build.
- The emitted standard flag doesn't match `CMakeLists.txt` (e.g. a moving-target
  flag where a pinned one is expected) -- don't standardize on it silently.
- A build, SDK, or dependency change is needed that the task at hand didn't
  authorize -- flag it; don't expand scope to fix the toolchain mid-task.
