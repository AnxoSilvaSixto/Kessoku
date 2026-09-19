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
| Build | CMake + vcpkg |
| Audio decode | libFLAC |
| Tagging | TagLib |
| Indexing | No database. In-memory index built by scanning the library root at launch. |
| v1 scope | Browse, play, pause, seek only. No queue, shuffle, search, playlists, or gapless yet. |

Full reasoning for each choice is in `CLAUDE.md` §16.1 if you want it — you don't need to re-derive or second-guess these.

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

- Build: `<TODO: fill in the actual CMake configure/build invocation and preset name>`
- Tests: `<TODO: fill in the test runner/command>`
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

- Prefer a connected MCP tool over shelling out when it does the same job more safely or legibly — but the offline and filesystem invariants above apply to tool use too. No MCP tool, plugin, or LSP action may reach outside the library root/repo or make a network call, regardless of what the tool is technically capable of.
- Keep enabled MCP servers limited to the ones actually in use for this project. Each one adds to context on every turn, and that cost matters more on a local model with a smaller context budget than on a hosted one.
- `<TODO: name the specific MCP servers and plugins enabled here, and what each is for, so future sessions don't have to guess>`

## Scope discipline

Default answer to a feature nobody asked for is *no*. If a task would add a capability, a dependency, or a persistent architectural commitment beyond what was requested, stop and flag it rather than including it "since you're in there."

## Git hygiene

Focused commits. Check `git diff` and `git status` before considering anything finished. Never force-push or rewrite shared history. Never commit secrets, build output, caches, or machine-specific files.

## Definition of done

- The requested behavior works, and you verified it — not assumed it.
- No hard invariant was weakened, worked around, or reinterpreted.
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
