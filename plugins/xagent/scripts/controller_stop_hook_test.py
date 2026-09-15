from __future__ import annotations

import fcntl
import json
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

from plugins.xagent.scripts import controller_stop_hook as hook


FIXTURE_ROOT = Path(__file__).with_name("fixtures") / "controller_stop_hooks"
SCRIPT = Path(__file__).with_name("controller_stop_hook.py")


def fixture(name: str) -> dict[str, object]:
    return json.loads((FIXTURE_ROOT / name).read_text(encoding="utf-8"))


def response_payload(
    *,
    session_id: str = "session-1",
    tool_name: str,
    result: dict[str, object],
    harness_shape: str = "codex",
) -> dict[str, object]:
    tool_response: object
    if harness_shape == "claude":
        tool_response = json.dumps(result)
    else:
        tool_response = {
            "content": [{"type": "text", "text": json.dumps(result)}],
            "structuredContent": result,
        }
    return {
        "hook_event_name": "PostToolUse",
        "session_id": session_id,
        "tool_name": tool_name,
        "tool_response": tool_response,
    }


def stop_payload(session_id: str = "session-1", **extra: object) -> dict[str, object]:
    payload: dict[str, object] = {
        "hook_event_name": "Stop",
        "session_id": session_id,
    }
    payload.update(extra)
    return payload


def state_files(root: Path) -> list[Path]:
    return sorted(path for path in root.glob("*.json") if path.is_file())


def read_state(root: Path) -> dict[str, object]:
    files = state_files(root)
    if len(files) != 1:
        raise AssertionError(f"expected one state file, found {files}")
    return json.loads(files[0].read_text(encoding="utf-8"))


def read_session_state(root: Path, harness: str, session_id: str) -> dict[str, object]:
    state_path, _ = hook.session_paths(root, harness, session_id)
    return json.loads(state_path.read_text(encoding="utf-8"))


class PayloadContractTests(unittest.TestCase):
    def test_captured_claude_and_codex_fixtures_have_subagent_discriminators(self):
        self.assertFalse(hook.is_native_subagent(fixture("claude_post_tool_use_start.json"), "claude"))
        self.assertFalse(hook.is_native_subagent(fixture("codex_post_tool_use_start.json"), "codex"))
        self.assertTrue(hook.is_native_subagent(fixture("claude_post_tool_use_subagent.json"), "claude"))
        self.assertTrue(hook.is_native_subagent(fixture("codex_post_tool_use_subagent.json"), "codex"))
        self.assertTrue(hook.is_native_subagent({"agent_id": "native-agent"}, "codex"))
        self.assertTrue(hook.is_native_subagent({"agent_type": "default"}, "codex"))

    def test_structured_content_is_preferred(self):
        tool_response = {
            "content": [{"type": "text", "text": "{\"run_id\":\"text\",\"sequence\":2}"}],
            "structuredContent": {"run_id": "structured", "sequence": 3},
        }
        self.assertEqual(
            {"run_id": "structured", "sequence": 3},
            hook.extract_success_result(tool_response),
        )

    def test_single_json_text_block_is_fallback(self):
        tool_response = {
            "content": [{"type": "text", "text": "{\"run_id\":\"text-run\",\"sequence\":4}"}],
        }
        self.assertEqual(
            {"run_id": "text-run", "sequence": 4},
            hook.extract_success_result(tool_response),
        )

    def test_error_envelope_never_uses_nested_identity(self):
        is_error = {
            "isError": True,
            "structuredContent": {"run_id": "nested-run", "sequence": 9},
        }
        direct_string_error = (
            "{\"error\":\"unknown_run\","
            "\"message\":\"Unknown xagent run\","
            "\"details\":{\"run_id\":\"nested-run\",\"sequence\":9}}"
        )
        text_error = {
            "content": [
                {
                    "type": "text",
                    "text": "{\"error\":\"unknown_run\",\"details\":{\"run_id\":\"nested-run\"}}",
                }
            ],
        }
        self.assertIsNone(hook.extract_success_result(is_error))
        self.assertIsNone(hook.extract_success_result(direct_string_error))
        self.assertIsNone(hook.extract_success_result(text_error))
        with tempfile.TemporaryDirectory() as tempdir:
            payload = {
                "hook_event_name": "PostToolUse",
                "session_id": "session-1",
                "tool_name": "xagent_start",
                "tool_response": direct_string_error,
            }

            hook.observe(payload, "claude", Path(tempdir))

            self.assertEqual([], state_files(Path(tempdir)))

    def test_only_the_xagent_mcp_namespace_is_an_xagent_tool(self):
        self.assertEqual("xagent_message", hook.normalize_tool_name("xagent_message"))
        self.assertEqual(
            "xagent_message",
            hook.normalize_tool_name("mcp__xagent__xagent_message"),
        )
        self.assertIsNone(hook.normalize_tool_name("mcp__other_server__xagent_message"))
        self.assertIsNone(hook.normalize_tool_name("other__xagent_close"))
        self.assertIsNone(hook.normalize_tool_name("mcp__xagent__xagent_unknown"))

    def test_foreign_mcp_namespace_never_creates_pending_state(self):
        # Installed observer groups carry no matcher, so every tool result
        # reaches the hook; a same-named tool from another MCP server is not
        # an xagent run and must not strand the controller on an await.
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)

            hook.observe(
                response_payload(
                    tool_name="mcp__other_server__xagent_message",
                    result={"run_id": "foreign-run", "sequence": 3},
                ),
                "claude",
                root,
            )

            self.assertEqual([], state_files(root))
            self.assertIsNone(hook.guard(stop_payload(), "claude", root))


class ObserverStateTests(unittest.TestCase):
    def test_start_followup_message_interrupt_await_and_close_transitions(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            hook.observe(
                response_payload(
                    tool_name="mcp__xagent__xagent_start",
                    result={"run_id": "run-1", "sequence": 7},
                ),
                "codex",
                root,
            )
            hook.observe(
                response_payload(
                    tool_name="xagent_message",
                    result={"run_id": "run-1", "sequence": 8},
                ),
                "codex",
                root,
            )
            hook.observe(
                response_payload(
                    tool_name="xagent_message",
                    result={"run_id": "run-1", "sequence": 9},
                ),
                "codex",
                root,
            )
            hook.observe(
                response_payload(
                    tool_name="xagent_interrupt",
                    result={"run_id": "run-1", "sequence": 10, "phase": "interrupted"},
                ),
                "codex",
                root,
            )
            hook.observe(
                response_payload(
                    tool_name="xagent_await",
                    result={"run_id": "run-1", "sequence": 11, "event": "worker.attention"},
                ),
                "codex",
                root,
            )

            state = read_state(root)
            self.assertEqual(5, state["revision"])
            self.assertEqual(
                [{"run_id": "run-1", "after_sequence": 11, "order": 1}],
                state["pending"],
            )

            hook.observe(
                response_payload(tool_name="xagent_close", result={"run_id": "run-1"}),
                "codex",
                root,
            )
            self.assertEqual([], state_files(root))

    def test_multiple_runs_preserve_insertion_order(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            for run_id, sequence in (("run-a", 4), ("run-b", 2), ("run-c", 9)):
                hook.observe(
                    response_payload(
                        tool_name="xagent_start",
                        result={"run_id": run_id, "sequence": sequence},
                    ),
                    "codex",
                    root,
                )

            state = read_state(root)
            self.assertEqual(
                [
                    {"run_id": "run-a", "after_sequence": 4, "order": 1},
                    {"run_id": "run-b", "after_sequence": 2, "order": 2},
                    {"run_id": "run-c", "after_sequence": 9, "order": 3},
                ],
                state["pending"],
            )
            self.assertIn('run_id="run-a"', hook.guard(stop_payload(), "codex", root)["reason"])

    def test_sessions_and_harnesses_are_isolated(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            hook.observe(
                response_payload(
                    session_id="shared",
                    tool_name="xagent_start",
                    result={"run_id": "claude-run", "sequence": 1},
                ),
                "claude",
                root,
            )
            hook.observe(
                response_payload(
                    session_id="shared",
                    tool_name="xagent_start",
                    result={"run_id": "codex-run", "sequence": 2},
                ),
                "codex",
                root,
            )
            hook.observe(
                response_payload(
                    session_id="other",
                    tool_name="xagent_start",
                    result={"run_id": "other-run", "sequence": 3},
                ),
                "codex",
                root,
            )

            self.assertEqual(3, len(state_files(root)))
            self.assertIn('run_id="claude-run"', hook.guard(stop_payload("shared"), "claude", root)["reason"])
            self.assertIn('run_id="codex-run"', hook.guard(stop_payload("shared"), "codex", root)["reason"])
            self.assertIn('run_id="other-run"', hook.guard(stop_payload("other"), "codex", root)["reason"])

    def test_native_subagent_event_is_ignored(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            payload = response_payload(
                tool_name="xagent_start",
                result={"run_id": "run-1", "sequence": 7},
            )
            payload["agent_id"] = "native-agent"
            payload["agent_type"] = "default"

            hook.observe(payload, "codex", root)

            self.assertEqual([], state_files(root))

    def test_await_deadline_with_same_cursor_is_noop(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            hook.observe(
                response_payload(
                    tool_name="xagent_start",
                    result={"run_id": "run-1", "sequence": 7},
                ),
                "codex",
                root,
            )
            hook.observe(
                response_payload(
                    tool_name="xagent_await",
                    result={
                        "run_id": "run-1",
                        "sequence": 7,
                        "event": "supervision.deadline",
                        "reason": "await_deadline",
                    },
                ),
                "codex",
                root,
            )

            state = read_state(root)
            self.assertEqual(1, state["revision"])
            self.assertEqual(7, state["pending"][0]["after_sequence"])

    def test_terminal_events_and_phases_clear_only_matching_run(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            for run_id in ("run-1", "run-2"):
                hook.observe(
                    response_payload(
                        tool_name="xagent_start",
                        result={"run_id": run_id, "sequence": 1},
                    ),
                    "codex",
                    root,
                )

            hook.observe(
                response_payload(
                    tool_name="xagent_await",
                    result={"run_id": "run-1", "sequence": 3, "event": "turn.completed"},
                ),
                "codex",
                root,
            )
            self.assertEqual(["run-2"], [item["run_id"] for item in read_state(root)["pending"]])

            for index, phase in enumerate(("completed", "failed", "cancelled", "abandoned"), start=1):
                session = f"phase-{index}"
                hook.observe(
                    response_payload(
                        session_id=session,
                        tool_name="xagent_start",
                        result={"run_id": f"run-{phase}", "sequence": 1},
                    ),
                    "codex",
                    root,
                )
                hook.observe(
                    response_payload(
                        session_id=session,
                        tool_name="xagent_await",
                        result={"run_id": f"run-{phase}", "sequence": 2, "phase": phase},
                    ),
                    "codex",
                    root,
                )
                self.assertIsNone(hook.guard(stop_payload(session), "codex", root))


class StopGuardTests(unittest.TestCase):
    def test_oldest_pending_run_blocks_with_exact_latest_cursor(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            for run_id, sequence in (("run-1", 7), ("run-2", 99)):
                hook.observe(
                    response_payload(
                        tool_name="xagent_start",
                        result={"run_id": run_id, "sequence": sequence},
                    ),
                    "codex",
                    root,
                )

            self.assertEqual(
                {
                    "decision": "block",
                    "reason": "Call xagent_await with run_id=\"run-1\" and after_sequence=7 before stopping.",
                },
                hook.guard(stop_payload(), "codex", root),
            )
            state = read_state(root)
            self.assertEqual(state["revision"], state["last_rejected_revision"])

    def test_same_revision_is_rejected_only_once_across_active_field_variants(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            hook.observe(
                response_payload(
                    tool_name="xagent_start",
                    result={"run_id": "run-1", "sequence": 7},
                ),
                "codex",
                root,
            )

            self.assertIsNotNone(hook.guard(stop_payload(stop_hook_active=False), "codex", root))
            self.assertIsNone(hook.guard(stop_payload(stop_hook_active=True), "codex", root))
            self.assertIsNone(hook.guard(stop_payload(), "codex", root))

    def test_new_revision_rearms_guard(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            hook.observe(
                response_payload(
                    tool_name="xagent_start",
                    result={"run_id": "run-1", "sequence": 7},
                ),
                "codex",
                root,
            )
            self.assertIsNotNone(hook.guard(stop_payload(), "codex", root))
            hook.observe(
                response_payload(
                    tool_name="xagent_message",
                    result={"run_id": "run-1", "sequence": 8},
                ),
                "codex",
                root,
            )

            block = hook.guard(stop_payload(), "codex", root)

            self.assertEqual(
                "Call xagent_await with run_id=\"run-1\" and after_sequence=8 before stopping.",
                block["reason"],
            )

    def test_new_record_after_empty_state_can_block_once(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            start = response_payload(
                tool_name="xagent_start",
                result={"run_id": "run-1", "sequence": 1},
            )
            hook.observe(start, "codex", root)
            self.assertIsNotNone(hook.guard(stop_payload(), "codex", root))
            hook.observe(response_payload(tool_name="xagent_close", result={"run_id": "run-1"}), "codex", root)
            hook.observe(start, "codex", root)

            self.assertIsNotNone(hook.guard(stop_payload(), "codex", root))

    def test_no_pending_malformed_or_unknown_schema_fails_open(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            self.assertIsNone(hook.guard(stop_payload("missing"), "codex", root))

            malformed_path, _ = hook.session_paths(root, "codex", "bad-json")
            malformed_path.write_text("{not-json", encoding="utf-8")
            self.assertIsNone(hook.guard(stop_payload("bad-json"), "codex", root))
            self.assertEqual("{not-json", malformed_path.read_text(encoding="utf-8"))

            unknown_path, _ = hook.session_paths(root, "codex", "old-schema")
            unknown_path.write_text(
                json.dumps({"schema_version": 999, "revision": 1, "pending": []}),
                encoding="utf-8",
            )
            self.assertIsNone(hook.guard(stop_payload("old-schema"), "codex", root))

    def test_cli_observe_is_silent_and_malformed_inputs_fail_open(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            observe = subprocess.run(
                [
                    "python3",
                    str(SCRIPT),
                    "--harness",
                    "codex",
                    "--state-root",
                    str(root),
                    "observe",
                ],
                input=json.dumps(
                    response_payload(
                        tool_name="xagent_start",
                        result={"run_id": "run-1", "sequence": 7},
                    )
                ),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(0, observe.returncode)
            self.assertEqual("", observe.stdout)
            self.assertEqual("", observe.stderr)
            self.assertTrue(state_files(root))

            bad_json = subprocess.run(
                [
                    "python3",
                    str(SCRIPT),
                    "--harness",
                    "codex",
                    "--state-root",
                    str(root),
                    "guard",
                ],
                input="{not-json",
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(0, bad_json.returncode)
            self.assertEqual("", bad_json.stdout)
            self.assertEqual("", bad_json.stderr)

            invalid_args = subprocess.run(
                [
                    "python3",
                    str(SCRIPT),
                    "--harness",
                    "clade",
                    "--state-root",
                    str(root),
                    "guard",
                ],
                input=json.dumps(stop_payload()),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(0, invalid_args.returncode)
            self.assertEqual("", invalid_args.stdout)
            self.assertEqual("", invalid_args.stderr)

    def test_claude_and_codex_emit_captured_compatible_stop_output(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            for harness, start_name, stop_name in (
                ("claude", "claude_post_tool_use_start.json", "claude_stop.json"),
                ("codex", "codex_post_tool_use_start.json", "codex_stop.json"),
            ):
                hook.observe(fixture(start_name), harness, root)
                result = subprocess.run(
                    [
                        "python3",
                        str(SCRIPT),
                        "--harness",
                        harness,
                        "--state-root",
                        str(root),
                        "guard",
                    ],
                    input=json.dumps(fixture(stop_name)),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                self.assertEqual(0, result.returncode)
                self.assertEqual("", result.stderr)
                self.assertEqual("block", json.loads(result.stdout)["decision"])


class ConcurrencyTests(unittest.TestCase):
    def test_overlapping_observer_and_guard_do_not_lose_updates(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            hook.observe(
                response_payload(
                    tool_name="xagent_start",
                    result={"run_id": "run-1", "sequence": 1},
                ),
                "codex",
                root,
            )

            for index in range(20):
                session_id = f"race-{index}"
                hook.observe(
                    response_payload(
                        session_id=session_id,
                        tool_name="xagent_start",
                        result={"run_id": "run-1", "sequence": 1},
                    ),
                    "codex",
                    root,
                )
                observer = threading.Thread(
                    target=hook.observe,
                    args=(
                        response_payload(
                            session_id=session_id,
                            tool_name="xagent_start",
                            result={"run_id": "run-2", "sequence": 2},
                        ),
                        "codex",
                        root,
                    ),
                )
                guard_result: list[dict[str, str] | None] = []
                guard_thread = threading.Thread(
                    target=lambda: guard_result.append(hook.guard(stop_payload(session_id), "codex", root))
                )
                observer.start()
                guard_thread.start()
                observer.join()
                guard_thread.join()

                pending_ids = {
                    item["run_id"]
                    for item in read_session_state(root, "codex", session_id)["pending"]
                }
                self.assertEqual({"run-1", "run-2"}, pending_ids)
                state = read_session_state(root, "codex", session_id)
                self.assertEqual(2, state["revision"])
                self.assertIn(state["last_rejected_revision"], (1, 2))
                self.assertEqual("block", guard_result[0]["decision"])

    def test_lock_timeout_fails_open_within_bound(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            hook.observe(
                response_payload(
                    session_id="locked",
                    tool_name="xagent_start",
                    result={"run_id": "run-1", "sequence": 1},
                ),
                "codex",
                root,
            )
            state_path, lock_path = hook.session_paths(root, "codex", "locked")
            with lock_path.open("a+", encoding="utf-8") as lock_file:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
                started = time.monotonic()
                try:
                    self.assertIsNone(hook.guard(stop_payload("locked"), "codex", root))
                    hook.observe(
                        response_payload(
                            session_id="locked",
                            tool_name="xagent_message",
                            result={"run_id": "run-1", "sequence": 9},
                        ),
                        "codex",
                        root,
                    )
                finally:
                    elapsed = time.monotonic() - started
                    fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)

            self.assertLess(elapsed, 2.5)
            self.assertEqual(1, json.loads(state_path.read_text(encoding="utf-8"))["revision"])

    def test_lock_file_survives_empty_state_cleanup(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            hook.observe(
                response_payload(
                    session_id="cleanup",
                    tool_name="xagent_start",
                    result={"run_id": "run-1", "sequence": 1},
                ),
                "codex",
                root,
            )
            state_path, lock_path = hook.session_paths(root, "codex", "cleanup")
            hook.observe(
                response_payload(
                    session_id="cleanup",
                    tool_name="xagent_close",
                    result={"run_id": "run-1"},
                ),
                "codex",
                root,
            )

            self.assertFalse(state_path.exists())
            self.assertTrue(lock_path.exists())


if __name__ == "__main__":
    unittest.main()
