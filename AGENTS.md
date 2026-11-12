# AGENTS.md

> Instructions for opencode when working in this repository. Loaded automatically every session. This is the implementation-facing counterpart to `CLAUDE.md`, which instructs Claude (the remote architecture reviewer) and is not read by you. Where the two disagree, `CLAUDE.md` is authoritative — flag the conflict, don't pick one.

## What this is

A minimal, native Windows 11 music player. Browse/play/pause/seek FLAC and WAV files from one user-selected library folder, played back bit-perfectly through WASAPI Exclusive Mode. Nothing else, on purpose — see "Scope discipline" below.

## Hard invariants — never weaken, work around, or silently reinterpret these

- **Offline only.** No runtime network calls, telemetry, analytics, crash uploads, cloud sync, accounts, license checks, or update checks. A dependency that phones home at runtime is disqualifying, not a config detail.
- **Filesystem boundary.** Only the one user-selected library root and its descendants. Every runtime path must resolve *inside* that root before any file operation. See the `filesystem-security-boundary` skill before touching anything that opens, lists, or validates a path.
- **Audio.** WASAPI Exclusive Mode only. No silent fallback to shared mode. No resampling, normalization, DSP, mixing, crossfade, or ReplayGain. Bit-perfect is a requirement, not an aspiration. See the `audio-engine-wasapi` skill before touching anything under the audio engine.
- **Formats.** FLAC and WAV only. DSD is deferred — do not add it, even partially, even behind a flag.
- **Platform.** Windows 11 native only. Don't add cross-platform abstractions "just in case."

If a task seems to require crossing one of these, stop and flag it — see "Your autonomy boundary" below.

## Current architecture (decided — don't re-litigate)

| | |
|---|---|
| Language | C++ |
| UI | Win32 API + WTL |
| Build | CMake + vcpkg (project-local `x64-windows-native` triplet) |
| Audio decode | libFLAC |
| Tagging | TagLib |
| Indexing | No database. In-memory index built by scanning the library root at launch. |
| v1 scope | Browse, play, pause, seek only. No queue, shuffle, search, playlists, or gapless yet. |

Full reasoning for each choice is in `CLAUDE.md` §16.1–16.2 if you want it — you don't need to re-derive or second-guess these.

## Build tuning and PGO (decided — don't re-litigate)

Release and RelWithDebInfo use MSVC's maximum-speed profile for this machine: `/O2 /Ob3 /Oi /Ot /Oy /GF /Gy /Gw /Zc:inline /arch:AVX2 /fp:fast`, with `/GL` and `/LTCG /OPT:REF /OPT:ICF`. Debug and ASan stay unoptimized. The project-local `triplets\x64-windows-native.cmake` builds Release vcpkg dependencies with the same tuning, so the app and its DLLs all target this AVX2/Zen 3 machine and are not portable to older x64 CPUs. The PGO presets use `triplets\x64-windows-native-pgo.cmake`, which adds `/GENPROFILE`.

Profile-guided optimization is opt-in and local: `scripts\pgo.ps1` trains the real `Kessoku.exe` and the dependency DLLs, and all profiles and training media stay under the gitignored `build-pgo\` tree. Normal `default`/`release` builds neither produce nor consume profiles. Don't commit PGD/PGC files, and don't claim PGO is active for a normal build.

**Still open, not yours to decide:** installer/distribution format, exact metadata field editing surface, future DSD implementation. If a task nudges toward deciding one of these, stop and flag it instead.

## Your autonomy boundary

Proceed without asking for: implementation within the architecture above, refactors scoped to the task at hand, tests, docs, bug fixes that don't touch an invariant.

Stop and flag (don't decide, don't guess) before: adding a dependency, adding any networking, touching a hard invariant above, expanding scope beyond what was asked, or choosing between the still-open items above.

## Workflow

1. **Investigate** — read the relevant files, existing tests, and current conventions before changing anything. Don't guess at code you can open.
2. **Plan** — the smallest change that fully satisfies the request. No opportunistic cleanup, renaming, or "while I'm in here" refactors.
3. **Implement** — minimal surface area, preserve behavior outside what was asked.
4. **Verify** — see below.
5. **Report** — see below.

## Verification

- Build: `cmake --preset default` then `cmake --build --preset default` (Visual Studio 2026, Debug, x64-windows-native via the project-local triplet). `cmake --build --preset release` covers Release.
- Tests: `ctest --preset default` (all six CTest executables). ASan: `cmake --preset asan`, `cmake --build --preset asan`, `ctest --preset asan`. Release: `ctest --preset release`.
- Optimization: Release is expected to compile with the flags above and link with `/LTCG`. Don't claim optimization or PGO is active without build/link output or `dumpbin` evidence.
- PGO: `scripts\pgo.ps1` is the only supported workflow. It needs an interactive desktop and must finish with 6/6 CTest, smoke exit code 0, and no `pgort140.dll` imports in `Kessoku.exe` or the deployed DLLs. It deletes and rebuilds `build-pgo` plus the `x64-windows-native-pgo` vcpkg trees, so don't run it alongside another build or audio test. When claiming PGO work, report the app and per-DLL coverage from `build-pgo\pgo\summary*.txt`.
- Compile commands for clangd: `scripts\setup-ninja-env.bat` writes `build/ninja/compile_commands.json`; this is a developer aid, not the build of record.
- Smoke test: run the built `Kessoku.exe` from its own output directory (`build\Debug`, `build\Release`, or `build-pgo\Release`), confirm the window opens, and confirm exit code 0 after closing.
- For anything touching audio, filesystem paths, or metadata writes: test the failure paths explicitly (device loss, path-escape attempts, malformed tags), not just the happy path. These are correctness/security-critical, not places to spot-check.
- If LSP diagnostics are available (clangd, auto-detected for this C++ codebase once `lsp` is turned on in `opencode.json`), treat them as a fast first-pass signal, not a substitute for an actual build. A clean build is what "done" means, not a clean diagnostics pane.
- Never report a build, test, or check as passing unless you actually ran it and it actually passed. "Should work" is not a verification result.

## Domain skills — load before you touch these areas

| If you're touching... | Load this first |
|---|---|
| Anything under the audio engine / WASAPI code | `audio-engine-wasapi` skill |
| Anything that opens, resolves, lists, or validates a filesystem path | `filesystem-security-boundary` skill |
| FLAC or WAV tag/metadata read or write code | `metadata-tagging` skill |

These aren't optional background reading — invoke the matching skill before writing or reviewing code in that area. They exist because these three domains are where a plausible-looking change is most likely to be subtly wrong.

## Using MCP, plugins, and LSP

- Prefer a connected MCP tool over shelling out when it does the same job more safely or legibly. Dev-time research network is allowed (e.g. `context7` fetching library docs) per `CLAUDE.md` §6 — but the shipped app itself must never make a runtime network call. No MCP tool, plugin, or LSP action may reach outside the library root/repo for filesystem access, regardless of what the tool is technically capable of.
- Keep enabled MCP servers limited to the ones actually in use for this project. Each one adds to context on every turn, and that cost matters more on a local model with a smaller context budget than on a hosted one.
- Enabled MCP servers: `context7` (global remote `https://mcp.context7.com/mcp`, dev-time library docs only — allowed network exception, never app runtime) and `vcpkg-query` (project local `python tools/vcpkg-query/server.py`, offline read-only vcpkg registry queries).

## Scope discipline

Default answer to a feature nobody asked for is *no*. If a task would add a capability, a dependency, or a persistent architectural commitment beyond what was requested, stop and flag it rather than including it "since you're in there."

## Git hygiene

Focused commits. Check `git diff` and `git status` before considering anything finished. Never force-push or rewrite shared history. Never commit secrets, build output, caches, or machine-specific files.

## Definition of done

- The requested behavior works, and you verified it — not assumed it.
- No hard invariant was weakened, worked around, or reinterpreted.
- If build flags, dependency builds, or PGO were touched, an actual Release/PGO build backs the claim: applied flags, profile coverage, and no PGO runtime import, not a Debug-only build.
- No unrelated file changed.
- Anything still uncertain is stated as uncertain, not glossed over.
- No feature or dependency was added beyond what was asked.

## When something breaks

Characterize the failure before fixing it. Determine whether it's caused by this change or pre-existing. Prefer the smallest root-cause fix over a workaround that hides the symptom. If the only way to preserve a feature is to compromise an invariant above, don't — revert or isolate the change and flag it instead.

## Priority order when requirements pull against each other

1. Security and privacy (offline, filesystem boundary)
2. Audio correctness (WASAPI Exclusive, bit-perfect)
3. Confirmed product requirements
4. Metadata compatibility and data preservation
5. Simplicity
6. Maintainability
7. Convenience / polish

## Reporting

State what changed, what you verified and how, and what's still uncertain. Don't claim you ran something you didn't. A claim that something is "correct," "safe," or "bit-perfect" isn't itself evidence — show the check that supports it.
