# Kessoku

A minimal, native Windows 11 music player. Browse, play, pause, and seek FLAC
and WAV files from one user-selected library folder, played back bit-perfectly
through WASAPI Exclusive Mode. Nothing else, on purpose.

## Requirements

- Windows 11 (x64)
- Visual Studio 2026 with the C++ desktop workload
- CMake 3.31 or newer
- vcpkg, with `VCPKG_ROOT` pointing at the checkout
- Windows SDK 10.0.26100 or newer

Optional developer tooling: LLVM 23 (`clangd`, `clang-tidy`, `clang-format`)
and Ninja. The Visual Studio preset remains the build of record; the Ninja
preset exists only to emit `compile_commands.json` for clangd.

## Build

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

The executable lands in `build\Debug\Kessoku.exe`. Run it from
`build\Debug` so the vcpkg-deployed DLLs resolve.

Release and RelWithDebInfo builds are tuned for this machine's Zen 3 CPU:
MSVC uses `/arch:AVX2` (which also enables FMA and BMI), `/Ob3`, `/Gw`, and
`/Zc:inline`, with `/GL` + `/LTCG` whole-program optimization and a
non-incremental linker. The resulting binaries require an AVX2-capable CPU;
they are not portable to older x64 hardware. Release builds can additionally
consume a local profile-guided optimization database (see below).

`player_tests` opens the default output device in WASAPI Exclusive Mode. Run
one audio test suite at a time: two `ctest` invocations in parallel (for
example, Debug and Release) contend for the same endpoint and one will fail.

### Presets

| Preset | Purpose |
|---|---|
| `default` | Visual Studio 2026, Debug, native x64-windows dependencies |
| `release` | Build/test the Release configuration of the default preset |
| `asan` | Debug build with MSVC AddressSanitizer |
| `compile-commands` | Ninja build that emits `build/ninja/compile_commands.json` |
| `pgo-generate` | Instrumented Release build for PGO training (`/GENPROFILE`) |
| `pgo-use` | Profile-optimized Release build (`/USEPROFILE`) |

The `default` and `compile-commands` presets use the project-local
`triplets\x64-windows-native.cmake` vcpkg triplet, which builds Release
dependencies with the same AVX2 and maximum-speed profile as the app. That
triplet is specific to this machine's CPU; a different CPU needs its own
triplet (or the stock `x64-windows` one).

### Profile-guided optimization

Release builds can be trained on this machine's real scan/playback workload.
The workflow is opt-in and local to this machine:

```powershell
scripts\pgo.ps1
```

The script builds `build-pgo` with `/LTCG /GENPROFILE`, generates a small
FLAC/WAV training library under `build-pgo\training`, drives the real UI
(folder pick, scan, play, pause, resume, seek, natural end-of-stream) for both
codecs, merges the generated `.pgc` files into
`build-pgo\pgo\Kessoku.pgd`, then reconfigures and relinks the app with
`/LTCG /USEPROFILE`. It finishes with the Release CTest suite and a smoke test
of the optimized binary. Use `-SkipTests` or `-SkipSmokeTest` to run only part
of the workflow.

PGO needs an interactive desktop: the script controls the folder picker and
transport controls, so leave the mouse and keyboard alone while it runs, and
make sure no other audio test suite is using the endpoint. The `pgo-generate`
build deploys the MSVC `pgort140.dll` runtime next to the instrumented binary;
the final `/USEPROFILE` image does not import it. Only `Kessoku.exe` is
instrumented: the vcpkg DLLs keep their native-triplet `/GL` + `/LTCG` build,
without PGO. Profiles and training media stay under `build-pgo\` and
are not committed. Re-run the script after meaningful code changes; each run
forces a fresh `/GENPROFILE` link so stale counters are discarded.

`scripts\setup-ninja-env.bat` initializes the MSVC environment, configures the
Ninja preset, builds it, and leaves `compile_commands.json` ready for clangd.

### Static analysis

`clang-tidy` is advisory, not a build gate. Generate the Ninja compile
database once, then analyze individual translation units:

```powershell
scripts\setup-ninja-env.bat
clang-tidy -p build\ninja src\main.cpp
```

Add LLVM's `bin` directory to `PATH` if `clang-tidy` is not found. `.clang-tidy`
enables the analyzer plus the bugprone, modernize, performance, and readability
families; checks that conflict with Win32 APIs or established project style are
disabled so real findings stay visible. Prefer analyzing changed files over the
whole tree.

## Layout

```
src/core      Result/Error types, library-root containment, UTF-8 helpers
src/audio     WASAPI Exclusive playback, FLAC and WAV decoding
src/library   Library scanning and tag reading
src/main.cpp  Win32 + WTL window and transport controls
tests/        CTest executables (no external test framework)
```

## Invariants

- Offline at runtime: no network calls, telemetry, accounts, or update checks.
- Filesystem access is confined to the user-selected library root.
- WASAPI Exclusive Mode only: no shared-mode fallback, resampling, DSP, or
  volume processing.
- FLAC and WAV only. DSD is out of scope.
- Windows 11 native only.

`AGENTS.md` documents the working rules; `CLAUDE.md` holds the full
architecture rationale.
