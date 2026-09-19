---
name: metadata-tagging
description: FLAC and WAV/ID3v2 metadata rules and Picard-compatibility notes for this project's TagLib-based tagging. Load before writing or reviewing any code that reads or writes track metadata.
license: MIT
compatibility: opencode
metadata:
  domain: metadata
  risk: medium-high
---

## Ground rules

- The metadata model is MusicBrainz/Picard-compatible, applied **locally only** — never query MusicBrainz at runtime.
- FLAC and WAV are not metadata-equivalent. Don't assume one format's semantics carry over to the other.
- WAV's only editable metadata target is ID3v2. RIFF INFO is not an editing target unless explicitly added later — don't add it opportunistically.
- Preserve metadata the change didn't intend to touch, whenever the library in use can do that safely.
- Don't invent MusicBrainz/Picard fields that aren't already part of the established mapping.
- Avoid destructive full-file rewrites when a safer, more targeted write is available.
- Never alter audio payload data to change metadata unless it's technically unavoidable — and if it seems unavoidable, that's a stop-and-flag, not a go-ahead.

## Before implementing field-level editing

Verify current Picard field mappings and TagLib's actual preservation behavior for the specific field and format involved — don't assume based on a similar field already implemented. FLAC and WAV/ID3v2 can behave differently for what looks like the same logical field.

## Testing priorities for anything in this area

- Metadata preservation across an edit (fields not touched by the change should survive unchanged)
- Correct editing of the field(s) actually in scope
- Malformed or unexpected existing metadata (don't assume every file's current tags are well-formed)
- FLAC-specific and WAV/ID3v2-specific cases tested separately, not just one representative format

## Stop conditions

Stop and flag rather than deciding if:
- A field's Picard mapping is ambiguous or can't be confirmed
- The chosen approach would require rewriting audio payload data
- A task implies adding RIFF INFO support or any DSD metadata handling
