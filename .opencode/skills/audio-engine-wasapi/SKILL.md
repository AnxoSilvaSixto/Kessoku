---
name: audio-engine-wasapi
description: WASAPI Exclusive Mode playback invariants, verification checklist, and research pointers for this project's bit-perfect audio engine (C++, libFLAC). Load before writing or reviewing any code that touches audio device initialization, format negotiation, buffering, device loss/recovery, or playback lifecycle.
license: MIT
compatibility: opencode
metadata:
  domain: audio
  risk: high
---

## Why this exists

Audio playback is correctness-critical for this project. Bit-perfect WASAPI Exclusive Mode output is a confirmed product requirement, not a preference — see `CLAUDE.md` §1/§7 for the full rationale.

## Non-negotiable rules

- WASAPI Exclusive Mode only. If `Initialize` fails, surface the failure to the caller — do **not** fall back to shared mode, silently or otherwise.
- No resampling, no normalization, no DSP, no software mixing, no volume-semantics changes, no channel alteration. If a requested change would introduce any of these, stop and flag it instead of implementing it.
- Don't hide sample-format negotiation behind an abstraction that makes it harder to audit.
- Make COM initialization and ownership explicit — don't rely on implicit apartment/threading assumptions.
- Respect `IAudioClient`/`IAudioClient3` lifetime and threading contracts as currently documented, not as remembered.

## Before implementing, verify against primary sources

These are exactly the kind of details that are easy to misremember and expensive to get wrong. Check current Microsoft Learn documentation rather than pattern-matching to a remembered WASAPI example:

- Current `IAudioClient` / `IAudioClient3` initialization and threading semantics
- Exclusive-mode device format negotiation behavior
- Event-driven buffering setup
- Device invalidation and recovery behavior
- Buffer sizing and stream shutdown ordering

## Testing priorities for anything in this area

- The normal Exclusive Mode init path
- The initialization-*failure* path (this is the one most likely to get skipped — don't skip it)
- Device loss / invalidation and recovery
- Format negotiation
- Full playback lifecycle: start, pause, seek, stop, teardown

## Stop conditions

Stop and flag rather than deciding if:
- Exclusive Mode cannot be initialized for a case the current design doesn't already handle
- A requirement here conflicts with what libFLAC or the current buffering design actually does
- The only way to make a symptom go away is to touch one of the non-negotiable rules above
