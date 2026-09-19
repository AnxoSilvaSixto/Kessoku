---
name: filesystem-security-boundary
description: Path-containment invariant and attack-surface checklist for this project's single-library-root security boundary. Load before writing or reviewing any code that opens, resolves, lists, watches, or validates a filesystem path.
license: MIT
compatibility: opencode
metadata:
  domain: filesystem-security
  risk: high
---

## The invariant

> A runtime path must resolve inside the user-approved library root before the operation is allowed.

This is a hard security boundary, not a UX nicety — see `CLAUDE.md` §1/§8. The application may only ever touch the one folder the user selected and its descendants.

## A string-prefix check is not enough

On Windows, comparing path strings with a prefix check is not sufficient containment. Resolve both the candidate path and the root to their canonical, final form (verify the current recommended API and its exact semantics against Microsoft Learn before relying on it — don't take this from memory) and compare *those*, so a junction, symlink, or reparse point that looks like it's inside the root but actually points outside it gets caught.

## Attack-surface checklist — think through each for any path-handling change

- `..` traversal
- Absolute paths supplied where a relative one was expected
- Drive letter changes
- UNC paths
- Junctions
- Symbolic links
- Windows reparse points generally
- Case and Unicode-normalization differences
- TOCTOU-style races (the check and the actual file operation aren't atomic)

## Testing priorities for anything in this area

- Library-root containment itself — a path outside the root must be rejected, not silently clamped or ignored
- File discovery correctly stays within the root
- Unsupported-format files are rejected, not partially processed
- At least one test per attack-surface item above that's relevant to the code being changed

## Stop conditions

Stop and flag rather than deciding if:
- A feature seems to require reading or writing outside the library root, for any reason
- A chosen library or API's path-handling behavior is unclear or under-documented
- A "convenience" shortcut comes up (scanning removable drives, Windows media libraries, network shares, cloud-storage folders) — the answer is no; flag it if it seems necessary
