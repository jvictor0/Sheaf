#!/usr/bin/env python3
"""Observe xagent tool results and guard controller stop events."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Sequence

import fcntl


STATE_SCHEMA_VERSION = 1
LOCK_TIMEOUT_SECONDS = 1.0
MCP_NAMESPACE_PREFIX = "mcp__xagent__"
TERMINAL_PHASES = frozenset({"completed", "failed", "cancelled", "abandoned"})
START_TOOLS = frozenset(
    {
        "xagent_start",
        "xagent_message",
    }
)
SUPPORTED_TOOLS = START_TOOLS | frozenset(
    {
        "xagent_interrupt",
        "xagent_await",
        "xagent_close",
    }
)
START_FIELDS = {
    "xagent_start": ("run_id", "sequence"),
    "xagent_message": ("run_id", "sequence"),
    "xagent_interrupt": ("run_id", "sequence"),
    "xagent_await": ("run_id", "sequence"),
    "xagent_close": ("run_id",),
}


class LockUnavailable(Exception):
    """Raised when a per-session hook lock cannot be acquired promptly."""


def normalize_tool_name(raw_name: object) -> str | None:
    """Return the xagent tool a hook payload names, or `None`.

    The installed observer groups carry no matcher, so every tool result
    reaches this program. Only the undecorated name and the captured
    `mcp__xagent__` decoration are ours: a same-named tool from another MCP
    server is a different run space, and accepting it would strand the
    controller on an await for a run xagent has never heard of.
    """
    if not isinstance(raw_name, str):
        return None
    tool_name = raw_name.removeprefix(MCP_NAMESPACE_PREFIX)
    return tool_name if tool_name in SUPPORTED_TOOLS else None


def _decoded_success_object(raw_text: str) -> dict[str, object] | None:
    try:
        decoded = json.loads(raw_text)
    except json.JSONDecodeError:
        return None
    if not isinstance(decoded, dict):
        return None
    if "error" in decoded or decoded.get("isError") is True:
        return None
    return decoded


def extract_success_result(tool_response: object) -> dict[str, object] | None:
    # Claude Code v2.1.220 captures MCP JSON text results as a bare JSON string.
    # Treat that as the same single-text fallback shape, not as nested identity.
    if isinstance(tool_response, str):
        return _decoded_success_object(tool_response)
    if not isinstance(tool_response, dict):
        return None
    if tool_response.get("isError") is True or "error" in tool_response:
        return None

    structured = tool_response.get("structuredContent")
    if isinstance(structured, dict):
        if structured.get("isError") is True or "error" in structured:
            return None
        return structured
    if structured is not None:
        return None

    content = tool_response.get("content")
    if not isinstance(content, list) or len(content) != 1:
        return None
    block = content[0]
    if not isinstance(block, dict):
        return None
    if block.get("type") != "text" or not isinstance(block.get("text"), str):
        return None
    return _decoded_success_object(block["text"])


def is_native_subagent(payload: dict[str, object], harness: str) -> bool:
    if harness not in {"claude", "codex"}:
        return False
    return isinstance(payload.get("agent_id"), str) or isinstance(payload.get("agent_type"), str)


def session_key(harness: str, session_id: str) -> str:
    raw = f"{harness}\0{session_id}".encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def session_paths(state_root: Path, harness: str, session_id: str) -> tuple[Path, Path]:
    key = session_key(harness, session_id)
    return state_root / f"{key}.json", state_root / f"{key}.lock"


@contextmanager
def _session_lock(lock_path: Path) -> Iterator[None]:
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+", encoding="utf-8") as lock_file:
        deadline = time.monotonic() + LOCK_TIMEOUT_SECONDS
        while True:
            try:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError as error:
                if time.monotonic() >= deadline:
                    raise LockUnavailable(str(lock_path)) from error
                time.sleep(0.01)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def _empty_state() -> dict[str, object]:
    return {
        "schema_version": STATE_SCHEMA_VERSION,
        "revision": 0,
        "last_rejected_revision": None,
        "next_order": 1,
        "pending": [],
    }


def _load_state(
    state_path: Path,
    missing_state: dict[str, object] | None,
) -> dict[str, object] | None:
    if not state_path.exists():
        return missing_state
    try:
        raw = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if _validate_state(raw):
        return raw
    return None


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _validate_state(raw: object) -> bool:
    if not isinstance(raw, dict):
        return False
    if raw.get("schema_version") != STATE_SCHEMA_VERSION:
        return False
    if not _is_int(raw.get("revision")) or raw["revision"] < 0:
        return False
    if not _is_int(raw.get("next_order")) or raw["next_order"] < 1:
        return False
    last_rejected = raw.get("last_rejected_revision")
    if last_rejected is not None and not _is_int(last_rejected):
        return False
    pending = raw.get("pending")
    if not isinstance(pending, list):
        return False
    seen_orders: set[int] = set()
    for item in pending:
        if not isinstance(item, dict):
            return False
        if not isinstance(item.get("run_id"), str) or not item["run_id"]:
            return False
        if not _is_int(item.get("after_sequence")) or item["after_sequence"] < 0:
            return False
        if not _is_int(item.get("order")) or item["order"] < 1:
            return False
        if item["order"] in seen_orders:
            return False
        seen_orders.add(item["order"])
    return True


def _write_state(state_path: Path, state: dict[str, object]) -> None:
    pending = state.get("pending")
    if pending == []:
        try:
            state_path.unlink()
        except FileNotFoundError:
            pass
        return
    state_path.parent.mkdir(parents=True, exist_ok=True)
    staged = state_path.with_name(f".{state_path.name}.stage-{os.getpid()}")
    staged.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    os.replace(staged, state_path)


def _pending_items(state: dict[str, object]) -> list[dict[str, object]]:
    pending = state["pending"]
    assert isinstance(pending, list)
    return pending


def _find_pending(state: dict[str, object], run_id: str) -> dict[str, object] | None:
    for item in _pending_items(state):
        if item["run_id"] == run_id:
            return item
    return None


def _add_or_update_pending(
    state: dict[str, object],
    *,
    run_id: str,
    after_sequence: int,
) -> bool:
    existing = _find_pending(state, run_id)
    if existing is not None:
        if existing["after_sequence"] == after_sequence:
            return False
        existing["after_sequence"] = after_sequence
        return True

    order = state["next_order"]
    assert isinstance(order, int)
    _pending_items(state).append(
        {"run_id": run_id, "after_sequence": after_sequence, "order": order}
    )
    state["next_order"] = order + 1
    return True


def _remove_pending(state: dict[str, object], run_id: str) -> bool:
    pending = _pending_items(state)
    remaining = [item for item in pending if item["run_id"] != run_id]
    if len(remaining) == len(pending):
        return False
    state["pending"] = remaining
    return True


def _string_field(result: dict[str, object], field: str) -> str | None:
    value = result.get(field)
    if isinstance(value, str) and value:
        return value
    return None


def _sequence_field(result: dict[str, object]) -> int | None:
    value = result.get("sequence")
    if _is_int(value) and value >= 0:
        return value
    return None


def _apply_transition(
    state: dict[str, object],
    tool_name: str,
    result: dict[str, object],
) -> bool:
    if tool_name in START_TOOLS or tool_name == "xagent_interrupt":
        identity_field = START_FIELDS[tool_name][0]
        run_id = _string_field(result, identity_field)
        sequence = _sequence_field(result)
        if run_id is None or sequence is None:
            return False
        return _add_or_update_pending(state, run_id=run_id, after_sequence=sequence)

    if tool_name == "xagent_await":
        run_id = _string_field(result, "run_id")
        sequence = _sequence_field(result)
        if run_id is None or sequence is None:
            return False
        pending = _find_pending(state, run_id)
        if pending is None:
            return False
        if (
            result.get("event") == "turn.completed"
            or result.get("reason") == "run_terminal"
            or result.get("phase") in TERMINAL_PHASES
        ):
            return _remove_pending(state, run_id)
        if (
            result.get("event") == "supervision.deadline"
            and result.get("reason") == "await_deadline"
            and pending["after_sequence"] == sequence
        ):
            return False
        if pending["after_sequence"] == sequence:
            return False
        pending["after_sequence"] = sequence
        return True

    if tool_name == "xagent_close":
        run_id = _string_field(result, "run_id")
        if run_id is None:
            return False
        return _remove_pending(state, run_id)

    return False


def _session_id(payload: dict[str, object]) -> str | None:
    session_id = payload.get("session_id")
    if isinstance(session_id, str) and session_id:
        return session_id
    return None


def observe(payload: dict[str, object], harness: str, state_root: Path) -> None:
    if not isinstance(payload, dict) or is_native_subagent(payload, harness):
        return
    session_id = _session_id(payload)
    tool_name = normalize_tool_name(payload.get("tool_name"))
    if session_id is None or tool_name is None:
        return
    result = extract_success_result(payload.get("tool_response"))
    if result is None:
        return

    state_path, lock_path = session_paths(state_root, harness, session_id)
    try:
        with _session_lock(lock_path):
            state = _load_state(state_path, _empty_state())
            if state is None:
                return
            if not _apply_transition(state, tool_name, result):
                return
            revision = state["revision"]
            assert isinstance(revision, int)
            state["revision"] = revision + 1
            _write_state(state_path, state)
    except (LockUnavailable, OSError):
        return


def _selected_pending(state: dict[str, object]) -> dict[str, object] | None:
    pending = _pending_items(state)
    if not pending:
        return None
    return min(pending, key=lambda item: item["order"])


def guard(payload: dict[str, object], harness: str, state_root: Path) -> dict[str, str] | None:
    if not isinstance(payload, dict) or is_native_subagent(payload, harness):
        return None
    session_id = _session_id(payload)
    if session_id is None:
        return None
    state_path, lock_path = session_paths(state_root, harness, session_id)
    try:
        with _session_lock(lock_path):
            state = _load_state(state_path, None)
            if state is None:
                return None
            selected = _selected_pending(state)
            if selected is None:
                return None
            revision = state["revision"]
            if state.get("last_rejected_revision") == revision:
                return None
            state["last_rejected_revision"] = revision
            _write_state(state_path, state)
            return {
                "decision": "block",
                "reason": (
                    f"Call xagent_await with run_id=\"{selected['run_id']}\" "
                    f"and after_sequence={selected['after_sequence']} before stopping."
                ),
            }
    except (LockUnavailable, OSError):
        return None


class _FailOpenArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise ValueError(message)

    def exit(self, status: int = 0, message: str | None = None) -> None:
        raise ValueError(message or "")


def _parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = _FailOpenArgumentParser(add_help=False)
    parser.add_argument("--harness", choices=("claude", "codex"), required=True)
    parser.add_argument("--state-root", type=Path, required=True)
    parser.add_argument("mode", choices=("observe", "guard"))
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parse_args(argv)
    except (ValueError, argparse.ArgumentError):
        return 0
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, UnicodeDecodeError, OSError):
        return 0
    if not isinstance(payload, dict):
        return 0

    if args.mode == "observe":
        observe(payload, args.harness, args.state_root)
        return 0

    decision = guard(payload, args.harness, args.state_root)
    if decision is not None:
        print(json.dumps(decision))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
