"""Filesystem write tooling constrained by visible_directories policy."""

from __future__ import annotations

from pathlib import Path

from sheaf.tools.simple_tool import tool
from sheaf.tools.visibility import ensure_writable, resolve_input_path


def _display_path(path: Path) -> str:
    try:
        resolved = path.resolve()
    except OSError:
        resolved = path
    home = Path.home().resolve()
    try:
        rel = resolved.relative_to(home)
    except ValueError:
        return str(resolved)
    return f"~/{rel.as_posix()}"


@tool("write_note")
def write_note_tool(relative_path: str, content: str, overwrite: bool = True) -> str:
    """Write UTF-8 text to a path allowed by visible_directories policy."""

    target = resolve_input_path(relative_path)
    ensure_writable(target)
    if target.exists() and target.is_dir():
        raise ValueError("Target path points to a directory, expected a file")
    if target.exists() and not overwrite:
        raise ValueError("Target file already exists and overwrite is false")

    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")
    bytes_written = len(content.encode("utf-8"))
    return f"Wrote {_display_path(target)} ({bytes_written} bytes)"
