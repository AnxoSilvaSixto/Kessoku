# mcp SDK pinned: mcp==2.2.0 (installed machine-level via `pip install "mcp==2.2.0"`).
# Official SDK: from mcp.server.mcpserver import MCPServer (mcp 2.x; FastMCP was renamed).
# Runtime imports: mcp + stdlib only (json, os, re, subprocess, sys, pathlib). No network, no writes.
"""Offline, read-only vcpkg registry query MCP server (stdio).

Reads local files only under C:\\vcpkg\\ports\\ (and allowlists C:\\vcpkg\\versions\\).
Never writes anywhere. Never makes network calls.

Containment (per repo skill `filesystem-security-boundary`):
- Strict allowlist: port names must match ^[a-z0-9]([a-z0-9-]*[a-z0-9])?$ (max 64 chars).
  This rejects `..` traversal, absolute paths, drive letters, UNC paths, slashes,
  backslashes, colons, and empty strings before any filesystem access.
- Canonicalize: both the allowlisted root and the candidate path are resolved via
  Path.resolve() (resolves junctions/symlinks/reparse points), then compared with
  os.path.normcase + Path.is_relative_to so a symlink inside ports/ pointing
  outside is still rejected. Case/Unicode differences are neutralised by normcase
  + resolve on Windows.
- Reject, don't clamp: any escape or unknown port raises a clean ToolError.
- TOCTOU note: check-then-use is best-effort here; the server is read-only so a
  race cannot cause a write outside the root, and every tool re-validates the
  name on each call.
- Read-only audit: only Path.read_text, Path.iterdir, Path.is_file/is_dir, and
  `git log --oneline` (read-only subprocess, fixed argv, no shell) are used.
  No open(..., 'w'), write_text, mkdir, unlink, rename, or git mutation anywhere.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError

VCPKG_ROOT = Path(r"C:\vcpkg")
PORTS_ROOT = VCPKG_ROOT / "ports"
VERSIONS_ROOT = VCPKG_ROOT / "versions"

# Strict allowlist for vcpkg port names (lowercase, digits, hyphens; no dots/underscores/slashes).
_PORT_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$")
_MAX_HISTORY = 50


def _canonical_root(root: Path) -> Path:
    """Resolve the allowlisted root to canonical form (best effort if missing)."""
    try:
        return root.resolve()
    except OSError:
        return root.absolute()


def _is_within(canonical_child: Path, canonical_root: Path) -> bool:
    """Case-aware containment check (Windows paths are case-insensitive)."""
    try:
        c = os.path.normcase(str(canonical_child))
        r = os.path.normcase(str(canonical_root))
    except OSError:
        return False
    if c == r:
        return True
    try:
        return canonical_child.is_relative_to(canonical_root)
    except (AttributeError, ValueError):
        # Fallback for very old Pythons (3.14 always has is_relative_to, kept for safety).
        return c.startswith(r.rstrip(os.sep) + os.sep)


def _resolve_port_dir(name: str) -> Path:
    """Validate `name` against the allowlist and return the canonical port dir.

    Raises ToolError (clean MCP error, no traceback leak) on any rejection.
    """
    if not isinstance(name, str) or not name:
        raise ToolError("Invalid port name: must be a non-empty string.")
    if len(name) > 64 or not _PORT_RE.match(name):
        raise ToolError(
            f"Invalid port name {name!r}: only [a-z0-9-] (max 64 chars) are allowed; "
            "'..', '/', '\\\\', ':', drive letters and UNC paths are rejected."
        )
    candidate = PORTS_ROOT / name
    try:
        canonical_root = _canonical_root(PORTS_ROOT)
        canonical_child = candidate.resolve()
    except OSError as exc:
        raise ToolError(f"Cannot resolve port path for {name!r}.") from None
    if not _is_within(canonical_child, canonical_root):
        raise ToolError(f"Rejected port name {name!r}: resolves outside the ports root.")
    return canonical_child


def _read_vcpkg_json(port_dir: Path, display_name: str) -> dict:
    manifest = port_dir / "vcpkg.json"
    try:
        text = manifest.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise ToolError(f"Unknown port {display_name!r}: no vcpkg.json found.") from None
    except OSError:
        raise ToolError(f"Cannot read vcpkg.json for port {display_name!r}.") from None
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        raise ToolError(f"Corrupt vcpkg.json for port {display_name!r}.") from None
    if not isinstance(data, dict):
        raise ToolError(f"Corrupt vcpkg.json for port {display_name!r}.") from None
    return data


server = MCPServer("vcpkg-query")


@server.tool()
def vcpkg_port_info(name: str) -> dict:
    """Return registry metadata for a vcpkg port.

    Parses C:\\vcpkg\\ports\\<name>\\vcpkg.json into version, port-version,
    description, dependencies, homepage, license. Unknown port -> clean error.
    """
    port_dir = _resolve_port_dir(name)
    try:
        if not port_dir.is_dir():
            raise ToolError(f"Unknown port {name!r}.")
    except ToolError:
        raise
    except OSError:
        raise ToolError(f"Cannot access port {name!r}.") from None
    data = _read_vcpkg_json(port_dir, name)
    return {
        "name": data.get("name", name),
        "version": data.get("version", ""),
        "port-version": data.get("port-version", 0),
        "description": data.get("description", ""),
        "dependencies": data.get("dependencies", []),
        "homepage": data.get("homepage", ""),
        "license": data.get("license", ""),
    }


@server.tool()
def vcpkg_port_files(name: str) -> dict:
    """List files in C:\\vcpkg\\ports\\<name>\\ and return `usage` text when present.

    The `usage` text carries the correct find_package incantation.
    """
    port_dir = _resolve_port_dir(name)
    try:
        if not port_dir.is_dir():
            raise ToolError(f"Unknown port {name!r}.")
        entries = sorted(p.name for p in port_dir.iterdir())
    except ToolError:
        raise
    except OSError:
        raise ToolError(f"Cannot list files for port {name!r}.") from None
    usage_text: str | None = None
    usage_path = port_dir / "usage"
    # Containment re-check for the usage file itself (defence in depth).
    try:
        canonical_usage = usage_path.resolve()
        if _is_within(canonical_usage, _canonical_root(PORTS_ROOT)):
            if usage_path.is_file():
                try:
                    usage_text = usage_path.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    raise ToolError(f"Cannot read usage file for port {name!r}.") from None
    except ToolError:
        raise
    except OSError:
        raise ToolError(f"Cannot access usage file for port {name!r}.") from None
    return {"port": name, "files": entries, "usage": usage_text}


@server.tool()
def vcpkg_port_history(name: str, n: int = 10) -> dict:
    """Return the last n `git log --oneline` entries for the port directory."""
    port_dir = _resolve_port_dir(name)
    try:
        if not port_dir.is_dir():
            raise ToolError(f"Unknown port {name!r}.")
    except ToolError:
        raise
    except OSError:
        raise ToolError(f"Cannot access port {name!r}.") from None
    try:
        count = int(n)
    except (TypeError, ValueError):
        raise ToolError("Parameter 'n' must be an integer.") from None
    count = max(1, min(count, _MAX_HISTORY))
    # Read-only git query: fixed argv, no shell, timeout; name already allowlisted.
    rel = f"ports/{name}"
    try:
        proc = subprocess.run(
            ["git", "-C", str(VCPKG_ROOT), "log", f"-n{count}", "--oneline", "--", rel],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        raise ToolError(f"Cannot query history for port {name!r}.") from None
    if proc.returncode != 0:
        raise ToolError(f"Cannot query history for port {name!r}.")
    entries = [line for line in proc.stdout.splitlines() if line.strip()]
    return {"port": name, "entries": entries}


def _print_help() -> None:
    print("vcpkg-query MCP server (offline, read-only, stdio).")
    print("Usage:")
    print("  python tools/vcpkg-query/server.py          # run MCP server on stdio")
    print("  python tools/vcpkg-query/server.py --help   # this message")
    print("  python tools/vcpkg-query/server.py --self-check  # offline self-test")
    print("Tools (max 3): vcpkg_port_info, vcpkg_port_files, vcpkg_port_history")


def _self_check() -> int:
    """Offline self-test against ground truth; returns exit code."""
    failures: list[str] = []
    # Call the underlying logic directly (tools are thin wrappers over helpers).
    try:
        port_dir = _resolve_port_dir("taglib")
        data = _read_vcpkg_json(port_dir, "taglib")
        version = data.get("version", "")
        if version != "2.3.1":
            failures.append(f"taglib version {version!r} != '2.3.1'")
        else:
            print(f"OK taglib version == {version!r}")
        wtl_dir = _resolve_port_dir("wtl")
        wtl_data = _read_vcpkg_json(wtl_dir, "wtl")
        if str(wtl_data.get("version", "")) != "10.0.10320":
            failures.append(f"wtl version {wtl_data.get('version')!r} != '10.0.10320'")
        else:
            print(f"OK wtl version == '10.0.10320' (port-version {wtl_data.get('port-version', 0)})")
        flac_dir = _resolve_port_dir("libflac")
        flac_data = _read_vcpkg_json(flac_dir, "libflac")
        if str(flac_data.get("version", "")) != "1.5.0":
            failures.append(f"libflac version {flac_data.get('version')!r} != '1.5.0'")
        else:
            print("OK libflac version == '1.5.0'")
        # Escape must be rejected cleanly (ToolError), never a traceback leak to caller.
        for evil in ("../../windows", "..", "/etc/passwd", "C:\\Windows", "\\\\server\\share"):
            try:
                _resolve_port_dir(evil)
                failures.append(f"escape {evil!r} was NOT rejected")
            except Exception as exc:
                if type(exc).__name__ != "ToolError":
                    failures.append(f"escape {evil!r} raised {type(exc).__name__}, want ToolError")
                else:
                    print(f"OK escape {evil!r} rejected cleanly")
    except Exception as exc:
        failures.append(f"self-check crashed: {type(exc).__name__}")
    if failures:
        print("SELF-CHECK FAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("SELF-CHECK PASSED")
    return 0


def main(argv: list[str] | None = None) -> None:
    args = list(sys.argv[1:] if argv is None else argv)
    if "--help" in args or "-h" in args:
        _print_help()
        return
    if "--self-check" in args:
        raise SystemExit(_self_check())
    server.run(transport="stdio")


if __name__ == "__main__":
    main()
