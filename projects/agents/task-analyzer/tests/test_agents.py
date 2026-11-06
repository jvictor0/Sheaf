"""Tests for agents.claude_runner (Task 7's real agent_runner).

Every test monkeypatches ``agents.subprocess.run`` with a fake that never
touches Claude/an LLM: it inspects the batch's ``items.json``/``prompt.md``
(found via the fake's ``cwd`` kwarg, which ``agents.py`` sets to the
per-batch scratch work dir) and writes canned JSON into ``output/`` --
mimicking what a real claude subagent run would leave behind. Zero real
dispatches happen anywhere in this file.
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import agents  # noqa: E402


def _kind_of_entry(entry: dict) -> str:
    if "brief_file" in entry:
        return "complexity"
    if "review_files" in entry:
        return "grading"
    return "phase_labeling"


def _canned_valid(kind: str, entity_key: str) -> dict:
    if kind == "complexity":
        return {
            "task_key": entity_key,
            "C1": 2, "C2": 2, "C3": 2, "C4": 2, "C5": 2, "C6": 2, "C7": 1,
            "composite": 2.0,
            "rationale": {k: "n/a" for k in ("C1", "C2", "C3", "C4", "C5", "C6", "C7")},
        }
    if kind == "grading":
        return {
            "task_key": entity_key,
            "G1": 5, "G2": 5, "G3": 5, "G4": 5, "G5": 5,
            "n_critical": 0, "n_important": 0, "n_minor": 0,
            "verdict_sequence": "PASS", "rounds_to_accept": 1,
            "final_grade": "A", "evidence": "clean review",
            "reviewer_models": ["claude-sonnet-5"], "excluded_reviews": [],
        }
    return {"session_key": entity_key, "labels": {"1": "green"}}


class FakeClaude:
    """Stand-in for ``subprocess.run`` -- records every call (cmd + kwargs)
    and, for each item in the batch it's handed (read from cwd/items.json),
    writes an output JSON file per ``behavior(entity_key, attempt_number)``.

    ``behavior`` defaults to always-valid; tests override it to simulate
    malformed/missing output for specific keys on specific attempts.

    ``rc_and_log`` defaults to always-succeed (exit 0, empty log); tests
    override it to simulate a crashed/misconfigured claude process -- it's
    given the 1-based call index across the whole fake's lifetime and
    returns ``(returncode, log_text)``; ``log_text`` is written to the
    ``stdout`` file object the real code passes (mirroring how a real
    subprocess's output lands in the log file agents.py reads back).
    """

    def __init__(self, behavior=None, rc_and_log=None):
        self.calls = []  # list of (cmd, kwargs)
        self.batch_sizes = []
        self.call_index = 0
        self._attempts = {}  # entity_key -> attempt count so far
        self.behavior = behavior or (lambda kind, key, attempt: ("valid", _canned_valid(kind, key)))
        self.rc_and_log = rc_and_log or (lambda call_index: (0, ""))

    def __call__(self, cmd, **kwargs):
        self.calls.append((list(cmd), kwargs))
        self.call_index += 1
        cwd = Path(kwargs["cwd"])
        manifest = json.loads((cwd / "items.json").read_text())
        self.batch_sizes.append(len(manifest))
        output_dir = cwd / "output"

        returncode, log_text = self.rc_and_log(self.call_index)
        kwargs["stdout"].write(log_text.encode("utf-8"))
        kwargs["stdout"].flush()

        for entry in manifest:
            kind = _kind_of_entry(entry)
            entity_key = entry.get("task_key") or entry.get("session_key")
            self._attempts[entity_key] = self._attempts.get(entity_key, 0) + 1
            attempt = self._attempts[entity_key]
            action, payload = self.behavior(kind, entity_key, attempt)
            out_path = output_dir / f"{entity_key}.json"
            if action == "valid":
                out_path.write_text(json.dumps(payload), encoding="utf-8")
            elif action == "invalid":
                out_path.write_text(json.dumps(payload), encoding="utf-8")
            elif action == "missing":
                pass  # no file written at all
            else:
                raise AssertionError(f"unknown behavior action {action!r}")
        return subprocess.CompletedProcess(cmd, returncode)


class AgentsTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.staging_dir = Path(self._tmp.name) / "staging"

        self._real_run = agents.subprocess.run
        self.addCleanup(setattr, agents.subprocess, "run", self._real_run)

        # Executable fixture for resolution; subprocess.run is faked below.
        self._dummy_claude_bin = Path(self._tmp.name) / "dummy_claude"
        self._dummy_claude_bin.write_text("#!/bin/sh\nexit 0\n")
        self._dummy_claude_bin.chmod(0o755)
        self._orig_claude_bin = agents._CLAUDE_BIN
        agents._CLAUDE_BIN = self._dummy_claude_bin
        self.addCleanup(setattr, agents, "_CLAUDE_BIN", self._orig_claude_bin)

        # Isolate TASK_ANALYZER_CLAUDE_BIN so host-environment leakage
        # never affects resolution-order tests.
        had_env = agents._CLAUDE_BIN_ENV_VAR in os.environ
        orig_env = os.environ.get(agents._CLAUDE_BIN_ENV_VAR)
        os.environ.pop(agents._CLAUDE_BIN_ENV_VAR, None)

        def _restore_env():
            if had_env:
                os.environ[agents._CLAUDE_BIN_ENV_VAR] = orig_env
            else:
                os.environ.pop(agents._CLAUDE_BIN_ENV_VAR, None)

        self.addCleanup(_restore_env)

    def _patch(self, fake):
        agents.subprocess.run = fake


class TestExecutableDispatch(unittest.TestCase):
    def test_cli_on_path_reads_brief_and_writes_staged_output(self):
        # Exercise real process launch and file handoff without contacting an LLM.
        with tempfile.TemporaryDirectory(prefix="claude scorer ") as tempdir:
            root = Path(tempdir)
            cli = root / "claude"
            cli.write_text(
                f"#!{sys.executable}\n"
                "import json, pathlib, sys\n"
                "assert sys.argv[1] == '-p'\n"
                "assert sys.argv[sys.argv.index('--model') + 1] == 'haiku'\n"
                "assert sys.argv[sys.argv.index('--permission-mode') + 1] == 'acceptEdits'\n"
                "prompt = pathlib.Path.cwd() / 'prompt.md'\n"
                "assert str(prompt) in sys.argv[2]\n"
                "assert prompt.is_file()\n"
                "for item in json.loads(pathlib.Path('items.json').read_text()):\n"
                "    assert pathlib.Path(item['timeline_file']).read_text() == 'turn 1: wrote test'\n"
                "    key = item['session_key']\n"
                "    (pathlib.Path('output') / (key + '.json')).write_text(\n"
                "        json.dumps({'session_key': key, 'labels': {'1': 'red'}}))\n"
            )
            cli.chmod(0o755)
            items = [{"session_key": "claude:s1", "version": "1", "input_sha256": "a" * 64,
                      "timeline": "turn 1: wrote test"}]
            with mock.patch.dict(os.environ, {"PATH": str(root), "TASK_ANALYZER_CLAUDE_BIN": ""}):
                written = agents.claude_runner("phase_labeling", items, root / "staging")
            self.assertEqual(len(written), 1)
            self.assertEqual(json.loads(written[0].read_text()),
                             {"session_key": "claude:s1", "labels": {"1": "red"}})


class TestBatching(AgentsTestBase):
    def test_25_items_yields_3_invocations_of_at_most_12(self):
        fake = FakeClaude()
        self._patch(fake)

        items = [
            {
                "session_key": f"claude:sess-{i}",
                "version": "1",
                "input_sha256": f"{'a' * 63}{i % 10}",
                "timeline": f"turn 1: did thing {i}",
            }
            for i in range(25)
        ]

        written = agents.claude_runner("phase_labeling", items, self.staging_dir)

        self.assertEqual(len(fake.calls), 3)
        self.assertTrue(all(n <= 12 for n in fake.batch_sizes))
        self.assertEqual(sum(fake.batch_sizes), 25)
        self.assertEqual(len(written), 25)
        for p in written:
            self.assertTrue(p.exists())
            self.assertTrue(p.name.endswith(".json"))
            self.assertEqual(p.parent, self.staging_dir / "phase_labeling")

    def test_empty_items_returns_empty_without_dispatch(self):
        fake = FakeClaude()
        self._patch(fake)

        written = agents.claude_runner("complexity", [], self.staging_dir)

        self.assertEqual(written, [])
        self.assertEqual(len(fake.calls), 0)

    def test_unknown_kind_raises(self):
        fake = FakeClaude()
        self._patch(fake)
        with self.assertRaises(ValueError):
            agents.claude_runner("bogus", [{"task_key": "x"}], self.staging_dir)


class TestPromptRendering(AgentsTestBase):
    def test_rendered_prompt_includes_rubric_and_staging_paths(self):
        fake = FakeClaude()
        self._patch(fake)

        items = [
            {
                "task_key": "add-widget--task-1",
                "version": "1",
                "input_sha256": "b" * 64,
                "brief_text": "Implement the widget core module.",
            },
            {
                "task_key": "add-widget--task-2",
                "version": "1",
                "input_sha256": "c" * 64,
                "brief_text": "Implement the gadget core module.",
            },
        ]

        agents.claude_runner("complexity", items, self.staging_dir)

        self.assertEqual(len(fake.calls), 1)
        cwd = Path(fake.calls[0][1]["cwd"])
        prompt_text = (cwd / "prompt.md").read_text(encoding="utf-8")

        # The rubric is copied into the work dir (so scorers never read
        # outside their sandbox cwd); the prompt must point at the copy.
        self.assertIn(str(cwd / "rubric.md"), prompt_text)
        self.assertEqual(
            (cwd / "rubric.md").read_text(encoding="utf-8"),
            Path(agents._RUBRIC_PATH["complexity"]).read_text(encoding="utf-8"),
        )
        self.assertIn(str(cwd / "items.json"), prompt_text)
        self.assertIn(str(cwd / "output"), prompt_text)
        # placeholders must be fully substituted, none left over.
        self.assertNotIn("{{RUBRIC_PATH}}", prompt_text)
        self.assertNotIn("{{ITEMS_JSON_PATH}}", prompt_text)
        self.assertNotIn("{{OUTPUT_DIR}}", prompt_text)

        manifest = json.loads((cwd / "items.json").read_text())
        self.assertEqual(len(manifest), 2)
        brief_file = Path(manifest[0]["brief_file"])
        self.assertEqual(brief_file.read_text(encoding="utf-8"), "Implement the widget core module.")

    def test_stdin_closed_and_stdout_is_a_file_not_a_pipe(self):
        fake = FakeClaude()
        self._patch(fake)

        items = [{"session_key": "claude:s1", "version": "1", "input_sha256": "d" * 64, "timeline": "t"}]
        agents.claude_runner("phase_labeling", items, self.staging_dir)

        kwargs = fake.calls[0][1]
        self.assertNotEqual(kwargs["stdin"], subprocess.PIPE)
        self.assertNotEqual(kwargs["stdout"], subprocess.PIPE)
        self.assertEqual(kwargs["stdin"].name, os.devnull)
        # stdout is a real file on disk (the log), not an in-memory pipe.
        self.assertTrue(Path(kwargs["stdout"].name).exists())


class TestValidationAndRetry(AgentsTestBase):
    def test_malformed_output_is_retried_once_then_succeeds(self):
        def behavior(kind, key, attempt):
            if key == "add-widget--task-1" and attempt == 1:
                return "invalid", {"task_key": key}  # missing required keys
            return "valid", _canned_valid(kind, key)

        fake = FakeClaude(behavior=behavior)
        self._patch(fake)

        items = [
            {"task_key": "add-widget--task-1", "version": "1", "input_sha256": "e" * 64, "brief_text": "brief one"},
            {"task_key": "add-widget--task-2", "version": "1", "input_sha256": "f" * 64, "brief_text": "brief two"},
        ]

        written = agents.claude_runner("complexity", items, self.staging_dir)

        self.assertEqual(len(fake.calls), 2)  # initial batch + one retry batch
        self.assertEqual(fake.batch_sizes, [2, 1])  # retry batch has only the invalid item
        self.assertEqual(len(written), 2)

        names = sorted(p.name for p in written)
        self.assertTrue(any("add-widget--task-1" in n for n in names))
        self.assertTrue(any("add-widget--task-2" in n for n in names))

    def test_still_invalid_after_retry_is_dropped_not_written(self):
        def behavior(kind, key, attempt):
            if key == "add-widget--task-1":
                return "missing", None  # never produces output, even on retry
            return "valid", _canned_valid(kind, key)

        fake = FakeClaude(behavior=behavior)
        self._patch(fake)

        items = [
            {"task_key": "add-widget--task-1", "version": "1", "input_sha256": "1" * 64, "brief_text": "brief one"},
            {"task_key": "add-widget--task-2", "version": "1", "input_sha256": "2" * 64, "brief_text": "brief two"},
        ]

        written = agents.claude_runner("complexity", items, self.staging_dir)

        self.assertEqual(len(fake.calls), 2)
        self.assertEqual(len(written), 1)
        self.assertIn("add-widget--task-2", written[0].name)
        self.assertFalse(any("add-widget--task-1" in p.name for p in written))


class TestModelHint(AgentsTestBase):
    def test_model_hint_honored_by_default(self):
        fake = FakeClaude()
        self._patch(fake)

        # phase_labeling's prompt frontmatter declares model_hint: haiku.
        items = [{"session_key": "claude:s1", "version": "1", "input_sha256": "3" * 64, "timeline": "t"}]
        agents.claude_runner("phase_labeling", items, self.staging_dir)
        cmd = fake.calls[0][0]
        self.assertEqual(cmd[cmd.index("--model") + 1], "haiku")

        fake.calls.clear()
        # complexity/grading declare model_hint: sonnet.
        items = [{"task_key": "t1", "version": "1", "input_sha256": "4" * 64, "brief_text": "b"}]
        agents.claude_runner("complexity", items, self.staging_dir)
        cmd = fake.calls[0][0]
        self.assertEqual(cmd[cmd.index("--model") + 1], "sonnet")

    def test_model_override_wins_over_hint(self):
        fake = FakeClaude()
        self._patch(fake)

        items = [{"session_key": "claude:s2", "version": "1", "input_sha256": "5" * 64, "timeline": "t"}]
        agents.claude_runner("phase_labeling", items, self.staging_dir, model="opus")
        cmd = fake.calls[0][0]
        self.assertEqual(cmd[cmd.index("--model") + 1], "opus")

    def test_headless_cli_receives_prompt_file_pointer_and_write_permissions(self):
        fake = FakeClaude()
        self._patch(fake)
        items = [{"task_key": "t1", "version": "1", "input_sha256": "6" * 64, "brief_text": "b"}]
        agents.claude_runner("complexity", items, self.staging_dir)
        cmd, kwargs = fake.calls[0]
        self.assertEqual(cmd[:2], [str(self._dummy_claude_bin.resolve()), "-p"])
        prompt_path = Path(kwargs["cwd"]) / "prompt.md"
        self.assertEqual(cmd[2], f"Read your full task prompt at {prompt_path} and follow it exactly.")
        self.assertEqual(cmd[cmd.index("--permission-mode") + 1], "acceptEdits")
        self.assertTrue(prompt_path.is_file())


class TestGradingReviewFiles(AgentsTestBase):
    def test_grading_item_writes_review_text_and_uses_review_files_list(self):
        fake = FakeClaude()
        self._patch(fake)

        items = [
            {
                "task_key": "add-widget--task-1",
                "version": "1",
                "input_sha256": "7" * 64,
                "review_text": "PASS: looks correct, no issues found.",
            }
        ]
        written = agents.claude_runner("grading", items, self.staging_dir)

        self.assertEqual(len(written), 1)
        cwd = Path(fake.calls[0][1]["cwd"])
        manifest = json.loads((cwd / "items.json").read_text())
        self.assertIsInstance(manifest[0]["review_files"], list)
        review_file = Path(manifest[0]["review_files"][0])
        self.assertEqual(review_file.read_text(encoding="utf-8"), "PASS: looks correct, no issues found.")

        data = json.loads(written[0].read_text(encoding="utf-8"))
        self.assertEqual(data["final_grade"], "A")


def _canned_ungradeable(entity_key: str, reason: str = "all reviews mis-joined; nothing gradeable") -> dict:
    """followup-7, defect 2: prompts/grading.md's "Ungradeable" output
    shape -- same required keys as a normal grade, G1-G5/verdict_sequence/
    rounds_to_accept/final_grade/evidence null, reviewer_models empty,
    plus a non-empty ``reason`` string."""
    return {
        "task_key": entity_key,
        "G1": None, "G2": None, "G3": None, "G4": None, "G5": None,
        "n_critical": 0, "n_important": 0, "n_minor": 0,
        "verdict_sequence": None, "rounds_to_accept": None,
        "final_grade": None, "evidence": None,
        "reviewer_models": [], "excluded_reviews": ["reviewer-1", "reviewer-2"],
        "reason": reason,
    }


class TestGradingUngradeableShape(AgentsTestBase):
    """followup-7, defect 2: a grading item where every joined review is
    mis-joined (or grading is otherwise impossible) must not crash the
    dispatcher or get treated as a validation failure/retried -- the
    ungradeable shape (see prompts/grading.md) validates and stages exactly
    like a normal grade, first attempt, no retry."""

    def test_ungradeable_shape_validates_directly(self):
        self.assertTrue(agents._validate_output("grading", _canned_ungradeable("add-widget--task-1")))

    def test_ungradeable_output_is_accepted_first_attempt_no_retry(self):
        def behavior(kind, key, attempt):
            return "valid", _canned_ungradeable(key)

        fake = FakeClaude(behavior=behavior)
        self._patch(fake)

        items = [
            {
                "task_key": "add-widget--task-1",
                "version": "1",
                "input_sha256": "8" * 64,
                "review_text": "unrelated content, mis-joined",
            }
        ]
        written = agents.claude_runner("grading", items, self.staging_dir)

        self.assertEqual(len(fake.calls), 1)  # no retry -- accepted first attempt
        self.assertEqual(len(written), 1)
        data = json.loads(written[0].read_text(encoding="utf-8"))
        self.assertIsNone(data["final_grade"])
        self.assertIsNone(data["G1"])
        self.assertEqual(data["reason"], "all reviews mis-joined; nothing gradeable")


class TestGradingShapeDiscrimination(AgentsTestBase):
    """fix-round-1, finding 2: `_grading_shape` (and `_validate_output`
    built on it) discriminates on SUBSTANCE -- whether G1-G5 are null --
    never on the mere presence of a `reason` key. Covers all three
    reachable shapes: a real grade (reason optional and harmless when
    present), the ungradeable shape, and a rejected half-null hybrid."""

    def test_populated_grade_with_reason_is_graded_not_ungradeable(self):
        data = _canned_valid("grading", "add-widget--task-1")
        data["reason"] = "borderline call, but clean"
        self.assertEqual(agents._grading_shape(data), "graded")
        self.assertTrue(agents._validate_output("grading", data))

    def test_ungradeable_shape_is_ungradeable(self):
        data = _canned_ungradeable("add-widget--task-1")
        self.assertEqual(agents._grading_shape(data), "ungradeable")
        self.assertTrue(agents._validate_output("grading", data))

    def test_half_null_hybrid_is_rejected(self):
        data = _canned_valid("grading", "add-widget--task-1")
        data["G1"] = None  # only G1 null -- the rest stay real ints
        self.assertIsNone(agents._grading_shape(data))
        self.assertFalse(agents._validate_output("grading", data))

    def test_all_null_without_reason_is_rejected(self):
        data = _canned_ungradeable("add-widget--task-1")
        data["reason"] = None
        self.assertIsNone(agents._grading_shape(data))
        self.assertFalse(agents._validate_output("grading", data))

    def test_populated_grade_with_reason_is_accepted_end_to_end_no_retry(self):
        def behavior(kind, key, attempt):
            data = _canned_valid(kind, key)
            data["reason"] = "harmless note"
            return "valid", data

        fake = FakeClaude(behavior=behavior)
        self._patch(fake)

        items = [
            {
                "task_key": "add-widget--task-1",
                "version": "1",
                "input_sha256": "9" * 64,
                "review_text": "clean review",
            }
        ]
        written = agents.claude_runner("grading", items, self.staging_dir)

        self.assertEqual(len(fake.calls), 1)  # no retry
        self.assertEqual(len(written), 1)
        data = json.loads(written[0].read_text(encoding="utf-8"))
        self.assertEqual(data["final_grade"], "A")
        self.assertIsNotNone(data["G1"])

    def test_half_null_hybrid_is_retried_then_dropped(self):
        def behavior(kind, key, attempt):
            data = _canned_valid(kind, key)
            data["G1"] = None
            return "valid", data  # "valid" JSON, but an invalid grading shape

        fake = FakeClaude(behavior=behavior)
        self._patch(fake)

        items = [
            {
                "task_key": "add-widget--task-1",
                "version": "1",
                "input_sha256": "a" * 64,
                "review_text": "clean review",
            }
        ]
        written = agents.claude_runner("grading", items, self.staging_dir)

        self.assertEqual(len(fake.calls), 2)  # initial + one retry
        self.assertEqual(len(written), 0)  # still invalid after retry -- dropped


class TestClaudeBinResolution(AgentsTestBase):
    """Review fix: the claude entry point must be resolvable without editing
    source -- explicit param beats env var beats the PATH default."""

    def test_param_beats_env_beats_default(self):
        fake = FakeClaude()
        self._patch(fake)

        tmp = Path(self._tmp.name)
        env_bin = tmp / "env_claude"
        param_bin = tmp / "param_claude"
        for p in (env_bin, param_bin):
            p.write_text("#!/bin/sh\nexit 0\n")
            p.chmod(0o755)

        item = [{"task_key": "t1", "version": "1", "input_sha256": "a" * 64, "brief_text": "b"}]

        # Default (no override) -- setUp already pointed agents._CLAUDE_BIN
        # at a dummy existing file.
        agents.claude_runner("complexity", item, self.staging_dir)
        cmd = fake.calls[-1][0]
        self.assertEqual(cmd[0], str(self._dummy_claude_bin.resolve()))

        # Env var beats the default.
        os.environ[agents._CLAUDE_BIN_ENV_VAR] = str(env_bin)
        agents.claude_runner("complexity", item, self.staging_dir)
        cmd = fake.calls[-1][0]
        self.assertEqual(cmd[0], str(env_bin.resolve()))

        # Explicit param beats the env var.
        agents.claude_runner("complexity", item, self.staging_dir, claude_bin=str(param_bin.resolve()))
        cmd = fake.calls[-1][0]
        self.assertEqual(cmd[0], str(param_bin.resolve()))


class TestClaudeBinMissing(AgentsTestBase):
    """Review fix: a missing entry point must raise loudly, before any
    dispatch is attempted, naming the resolved path and all three
    resolution sources -- not silently produce zero staged files."""

    def test_missing_binary_raises_before_any_dispatch(self):
        fake = FakeClaude()
        self._patch(fake)

        missing = Path(self._tmp.name) / "does-not-exist" / "claude"
        item = [{"task_key": "t1", "version": "1", "input_sha256": "a" * 64, "brief_text": "b"}]

        with self.assertRaises(RuntimeError) as ctx:
            agents.claude_runner("complexity", item, self.staging_dir, claude_bin=str(missing))

        msg = str(ctx.exception)
        self.assertIn(str(missing), msg)
        self.assertIn("claude_bin", msg)
        self.assertIn(agents._CLAUDE_BIN_ENV_VAR, msg)
        self.assertEqual(len(fake.calls), 0)  # never dispatched


class TestNonzeroExit(AgentsTestBase):
    """Review fix: a subprocess that exits nonzero must not be silently
    treated the same as "the model produced bad JSON" -- if it's still
    failing after the one retry, raise with the exit code and log tail
    rather than quietly returning fewer staged files than requested."""

    def test_nonzero_exit_after_retry_raises_with_exit_code_and_log_tail(self):
        def rc_and_log(call_index):
            return 1, f"call {call_index}: CLI crash TAIL_MARKER_XYZ\n"

        def behavior(kind, key, attempt):
            return "missing", None  # process crashed -- no output ever produced

        fake = FakeClaude(behavior=behavior, rc_and_log=rc_and_log)
        self._patch(fake)

        items = [{"task_key": "t1", "version": "1", "input_sha256": "a" * 64, "brief_text": "b"}]

        with self.assertRaises(RuntimeError) as ctx:
            agents.claude_runner("complexity", items, self.staging_dir)

        msg = str(ctx.exception)
        self.assertIn("1", msg)  # exit code
        self.assertIn("TAIL_MARKER_XYZ", msg)
        self.assertIn("t1", msg)
        self.assertEqual(len(fake.calls), 2)  # initial dispatch + exactly one retry, never more

    def test_nonzero_exit_on_first_attempt_recovers_on_successful_retry(self):
        def rc_and_log(call_index):
            return (1, "boom\n") if call_index == 1 else (0, "")

        def behavior(kind, key, attempt):
            if attempt == 1:
                return "missing", None
            return "valid", _canned_valid(kind, key)

        fake = FakeClaude(behavior=behavior, rc_and_log=rc_and_log)
        self._patch(fake)

        items = [{"task_key": "t1", "version": "1", "input_sha256": "a" * 64, "brief_text": "b"}]
        written = agents.claude_runner("complexity", items, self.staging_dir)

        self.assertEqual(len(written), 1)
        self.assertEqual(len(fake.calls), 2)

    def test_genuine_validation_failure_with_zero_exit_still_just_drops(self):
        """Not every retry failure is a crash -- if claude exits 0 both times
        but the model's output never validates, that item is still silently
        dropped (an accepted, pre-existing behavior; only nonzero-exit
        failures must raise)."""
        def behavior(kind, key, attempt):
            return "invalid", {"task_key": key}  # always missing required keys

        fake = FakeClaude(behavior=behavior)  # rc_and_log defaults to always-0
        self._patch(fake)

        items = [{"task_key": "t1", "version": "1", "input_sha256": "a" * 64, "brief_text": "b"}]
        written = agents.claude_runner("complexity", items, self.staging_dir)

        self.assertEqual(written, [])
        self.assertEqual(len(fake.calls), 2)  # initial + 1 retry, then dropped -- no raise


if __name__ == "__main__":
    unittest.main()
