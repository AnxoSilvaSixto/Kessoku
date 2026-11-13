# Kessoku

A minimal, native Windows 11 music player. Browse, play, pause, and seek FLAC
and WAV files from one user-selected library folder, played back bit-perfectly
through WASAPI Exclusive Mode. Nothing else, on purpose.

## Downloads

No packaged builds are published yet. The installer/distribution format is
still undecided, so build from source with the steps below. This section will
be updated when binaries are available.

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

### Tests and benchmarks

`ctest` runs eight executables: `library_root_tests`, `utf8_tests`,
`scanner_tests`, `metadata_tests`, `player_tests`, `transport_position_tests`,
`format_tests`, and `performance_tests`.

`format_tests` feeds malformed, truncated, and deterministically random
WAV/FLAC/metadata inputs through the parsers and checks that content/extension
mismatches fail cleanly instead of reporting empty metadata. It also covers
boundary conditions such as case-sensitive RIFF chunk IDs, zero-size chunks,
the exact 1 MB `fmt` allocation cap, and FLAC STREAMINFO sample-rate limits.

`player_tests` includes device-free rejection checks for malformed input,
unsupported bit depths and channel layouts, unknown FLAC sample counts, and
WAV files with no data chunk, so those cases fail before touching audio
hardware.

`performance_tests` times complete user-visible workloads: scanning 2,000
files, scanning a mixed 2,000-file library that contains only 500 audio
tracks, scanning and reading metadata for a 500-track library at launch, 2,000
containment checks, 100 WAV and FLAC metadata reads, opening and decoding a
10-second FLAC, rejecting 200 malformed WAV files, transport conversions, time
formatting, and UTF-8 path round-trips. Each scenario is warmed up, measured
three times, and held to the 400 ms Doherty Threshold. AddressSanitizer builds
execute the same workloads but do not enforce wall-clock budgets, because
sanitizer instrumentation changes timing rather than application behavior.

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
codecs, merges the generated `.pgc` files into the matching profile databases,
and relinks with `/LTCG /USEPROFILE`: first each vcpkg dependency DLL in its
build tree, then the app. It finishes with the Release CTest suite and a smoke
test of the optimized binary. Use `-SkipTests` or `-SkipSmokeTest` to run only
part of the workflow.

The `pgo-generate`/`pgo-use` presets use the separate
`triplets\x64-windows-native-pgo.cmake` triplet, which adds `/GENPROFILE` to
the same Zen 3 flags as the normal native triplet. Training exercises
`FLAC.dll` (FLAC decode) and `tag.dll` (metadata); `z.dll` is loaded by
TagLib and records whatever counters its scan path touches. Every dependency
DLL is then relinked with `/USEPROFILE`, including untrained ones, which keeps
the PGO runtime out of the deployed binaries. The script verifies with
`dumpbin` that neither `Kessoku.exe` nor any deployed DLL imports
`pgort140.dll`, then removes the runtime from the build tree.

PGO needs an interactive desktop: the script controls the folder picker and
transport controls, so leave the mouse and keyboard alone while it runs, and
make sure no other audio test suite is using the endpoint. Each run deletes
`build-pgo` and the `x64-windows-native-pgo` vcpkg build trees and clears the
binary cache for the run, so instrumented packages are always rebuilt from
source and stale counters cannot be reused. Profiles and training media stay
under `build-pgo\` and are not committed. Re-run the script after meaningful
code or dependency changes.

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
