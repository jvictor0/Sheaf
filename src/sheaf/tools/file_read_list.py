"""Tools for listing directories and reading files under data/notes."""

from __future__ import annotations

from pathlib import Path

from langchain_core.tools import tool

from sheaf.config.settings import NOTES_DIR
from sheaf.tools.file_write import resolve_note_path


def _resolve_dir(relative_dir: str) -> Path:
    path_text = relative_dir.strip()
    root = NOTES_DIR.resolve()
    candidate = root if not path_text else resolve_note_path(path_text)
    if not candidate.exists():
        raise ValueError(f"Path does not exist: {candidate}")
    if not candidate.is_dir():
        raise ValueError(f"Path is not a directory: {candidate}")
    return candidate


@tool("list_notes")
def list_notes_tool(relative_dir: str = ".", recursive: bool = False) -> str:
    """List entries under data/notes.

    Set recursive=true to include files recursively from the selected subdirectory.
    """

    target = _resolve_dir("" if relative_dir == "." else relative_dir)
    root = NOTES_DIR.resolve()

    if recursive:
        entries = sorted(p.relative_to(root).as_posix() for p in target.rglob("*"))
    else:
        entries = sorted(p.relative_to(root).as_posix() for p in target.iterdir())

    if not entries:
        return f"No entries under {target.relative_to(root).as_posix() or '.'}"
    return "\n".join(entries)


@tool("read_note")
def read_note_tool(relative_path: str, start_line: int = 0, end_line: int = 0) -> str:
    """Read a UTF-8 note file under data/notes, optionally by line range.

    Line range semantics:
    - If start_line <= 0 and end_line <= 0: return whole file.
    - Otherwise, start_line is 1-based inclusive; end_line is 1-based exclusive.
    """

    target = resolve_note_path(relative_path)
    if not target.exists():
        raise ValueError(f"File does not exist: {target}")
    if not target.is_file():
        raise ValueError(f"Path is not a file: {target}")

    text = target.read_text(encoding="utf-8")
    if start_line <= 0 and end_line <= 0:
        return text

    lines = text.splitlines()
    total = len(lines)
    start_idx = max(0, start_line - 1)
    end_idx = total if end_line <= 0 else min(total, end_line - 1)
    if end_idx < start_idx:
        raise ValueError("end_line must be greater than or equal to start_line")
    return "\n".join(lines[start_idx:end_idx])

