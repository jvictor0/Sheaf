"""Helpers for applying unified diffs to a single text file."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Hunk:
    old_start: int
    old_count: int
    new_start: int
    new_count: int
    lines: list[str]


def _parse_range(text: str) -> tuple[int, int]:
    if "," in text:
        start_text, count_text = text.split(",", 1)
        return int(start_text), int(count_text)
    return int(text), 1


def parse_unified_diff(patch: str) -> list[Hunk]:
    lines = patch.splitlines()
    hunks: list[Hunk] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.startswith("--- ") or line.startswith("+++ "):
            index += 1
            continue
        if not line.startswith("@@ "):
            raise ValueError("Patch error: expected unified diff hunk header")
        try:
            header, _ = line[3:].split(" @@", 1)
            old_range, new_range = header.strip().split(" ")
            old_start, old_count = _parse_range(old_range[1:])
            new_start, new_count = _parse_range(new_range[1:])
        except Exception as exc:  # noqa: BLE001
            raise ValueError("Patch error: invalid unified diff hunk header") from exc
        index += 1
        hunk_lines: list[str] = []
        while index < len(lines):
            item = lines[index]
            if item.startswith("@@ "):
                break
            if not item or item[0] not in {" ", "+", "-"}:
                raise ValueError("Patch error: invalid hunk line prefix")
            hunk_lines.append(item)
            index += 1
        hunks.append(
            Hunk(
                old_start=old_start,
                old_count=old_count,
                new_start=new_start,
                new_count=new_count,
                lines=hunk_lines,
            )
        )
    if not hunks:
        raise ValueError("Patch error: no hunks found")
    return hunks


def apply_unified_diff(original: str, patch: str) -> str:
    original_lines = original.splitlines(keepends=True)
    hunks = parse_unified_diff(patch)
    output: list[str] = []
    cursor = 0
    for hunk in hunks:
        start_index = max(0, hunk.old_start - 1)
        if start_index < cursor:
            raise ValueError("Patch error: overlapping hunks")
        output.extend(original_lines[cursor:start_index])
        cursor = start_index
        for raw_line in hunk.lines:
            prefix = raw_line[0]
            payload = raw_line[1:]
            candidate = payload + "\n"
            if prefix == " ":
                if cursor >= len(original_lines) or original_lines[cursor] != candidate:
                    raise ValueError("Patch error: context mismatch")
                output.append(original_lines[cursor])
                cursor += 1
                continue
            if prefix == "-":
                if cursor >= len(original_lines) or original_lines[cursor] != candidate:
                    raise ValueError("Patch error: deletion mismatch")
                cursor += 1
                continue
            if prefix == "+":
                output.append(candidate)
                continue
        consumed = cursor - start_index
        if consumed != hunk.old_count:
            raise ValueError("Patch error: old hunk line count mismatch")
    output.extend(original_lines[cursor:])
    return "".join(output)
