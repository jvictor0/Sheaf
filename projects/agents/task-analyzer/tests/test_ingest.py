"""Tests for the idempotent ingest driver (ingest.py).

Builds a throwaway git repo fixture (mirroring test_discovery.py's pattern)
under tmp_path with one landed change (``add-widget``), its SDD task brief
(``.superpowers/sdd/add-widget/task-1-brief.md``), and hand-written claude
transcript fixtures (an implementer session + a reviewer session) under a
separate fake "sessions corpus" directory, pointed at via the
``claude_projects_root``/``codex_sessions_root`` kwargs so the real
``~/.claude/projects`` on this machine is never touched.

``fake_agent_runner`` is a zero-LLM stand-in for Task 7's real
``agents.claude_runner``: it just writes canned, schema-valid JSON straight
into the staging dir per the ``kind``/``entity_key``/``version``/
``input_sha256`` cache key ingest.py passes it, and records every call it
receives so tests can assert dispatch counts/idempotency directly.
"""
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import assets  # noqa: E402
import costs  # noqa: E402
import db  # noqa: E402
import extractors  # noqa: E402
import ingest  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


def _run_git(repo, *args):
    subprocess.run(["git", "-C", str(repo)] + list(args), check=True, capture_output=True, text=True)


CHANGE_NAME = "add-widget"
TASK_KEY = "task-1"

IMPLEMENTER_PROMPT = (
    "Read your task brief at .superpowers/sdd/add-widget/task-1-brief.md and implement it."
)
REVIEWER_PROMPT = (
    "You are a READ-ONLY task reviewer. Read the brief at "
    ".superpowers/sdd/add-widget/task-1-brief.md and review the implementation "
    "for spec compliance. Do not edit any files."
)


def _make_repo(root: Path):
    """A git repo with one landed change (``add-widget``) on branch `main`,
    plus its SDD task brief under .superpowers/sdd/."""
    repo = root / "repo"
    repo.mkdir()
    _run_git(repo, "init", "-q")
    _run_git(repo, "checkout", "-q", "-b", "main")
    _run_git(repo, "config", "user.email", "test@example.com")
    _run_git(repo, "config", "user.name", "Test")

    archive = repo / "openspec" / "changes" / "archive" / "2026-07-10-add-widget"
    archive.mkdir(parents=True)
    (archive / "proposal.md").write_text("proposal\n")

    sdd = repo / ".superpowers" / "sdd" / CHANGE_NAME
    sdd.mkdir(parents=True)
    (sdd / f"{TASK_KEY}-brief.md").write_text("Implement the widget core module.\n")
    # A sibling review-brief file must NOT be mistaken for a task brief.
    (sdd / f"{TASK_KEY}-review-brief.md").write_text("Review guidance.\n")

    _run_git(repo, "add", "-A")
    _run_git(repo, "commit", "-q", "-m", "seed")
    return repo


def _write_claude_session(path: Path, session_id: str, prompt: str, last_message: str,
                           ts_start: str = "2026-07-10T00:00:00Z", ts_end: str = "2026-07-10T00:05:00Z"):
    lines = [
        json.dumps({
            "type": "user", "message": {"role": "user", "content": prompt},
            "sessionId": session_id, "timestamp": ts_start,
        }),
        json.dumps({
            "type": "assistant",
            "message": {
                "model": "claude-sonnet-5", "role": "assistant",
                "content": [{"type": "text", "text": last_message}],
                "usage": {"input_tokens": 100, "cache_read_input_tokens": 10,
                          "cache_creation_input_tokens": 0, "output_tokens": 50},
            },
            "sessionId": session_id, "timestamp": ts_end,
        }),
    ]
    path.write_text("\n".join(lines) + "\n")


def _make_sessions(root: Path):
    """One implementer session + one reviewer session, both referencing the
    add-widget/task-1 brief path (so they join unambiguously)."""
    claude_root = root / "claude_projects" / "proj"
    claude_root.mkdir(parents=True)
    _write_claude_session(
        claude_root / "impl-session.jsonl", "impl-1", IMPLEMENTER_PROMPT,
        "Implemented the widget core.",
        ts_start="2026-07-10T00:00:00Z", ts_end="2026-07-10T00:05:00Z",
    )
    _write_claude_session(
        claude_root / "review-session.jsonl", "review-1", REVIEWER_PROMPT,
        "PASS: looks correct, no issues found.",
        ts_start="2026-07-10T01:00:00Z", ts_end="2026-07-10T01:10:00Z",
    )
    return claude_root.parent


class FakeAgentRunner:
    """Writes canned, schema-valid staged JSON for every item it's handed.
    Records every call (kind, [item keys]) for idempotency assertions.
    ``fail_on_call`` (1-based) makes the runner stage its first item's file
    then raise, simulating a crash mid-dispatch."""

    def __init__(self, fail_on_call=None):
        self.calls = []
        self.fail_on_call = fail_on_call
        self._n = 0

    def __call__(self, kind, items, staging_dir):
        self._n += 1
        self.calls.append((kind, [dict(i) for i in items]))
        written = []
        for item in items:
            data = self._canned(kind, item)
            entity_key = item.get("task_key") or item.get("session_key")
            fname = ingest._staging_filename(entity_key, item["version"], item["input_sha256"])
            out_dir = Path(staging_dir) / kind
            out_dir.mkdir(parents=True, exist_ok=True)
            tmp = out_dir / (fname + ".tmp")
            tmp.write_text(json.dumps(data), encoding="utf-8")
            final = out_dir / fname
            os.replace(tmp, final)
            written.append(final)
            if self.fail_on_call == self._n:
                raise RuntimeError("simulated crash after staging one file")
        return written

    def _canned(self, kind, item):
        if kind == "complexity":
            return {
                "task_key": item["task_key"],
                "C1": 2, "C2": 2, "C3": 2, "C4": 2, "C5": 2, "C6": 2, "C7": 1,
                "composite": 2.0,
                "rationale": {k: "n/a" for k in ("C1", "C2", "C3", "C4", "C5", "C6", "C7")},
            }
        if kind == "grading":
            return {
                "task_key": item["task_key"],
                "G1": 5, "G2": 5, "G3": 5, "G4": 5, "G5": 5,
                "n_critical": 0, "n_important": 0, "n_minor": 0,
                "verdict_sequence": "PASS", "rounds_to_accept": 1,
                "final_grade": "A", "evidence": "clean review",
                "reviewer_models": ["claude-sonnet-5"], "excluded_reviews": [],
            }
        # phase_labeling
        return {"session_key": item["session_key"], "labels": {"1": "green"}}


class IngestTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.repo = _make_repo(self.root)
        self.claude_root = _make_sessions(self.root)
        self.staging_dir = self.root / "staging"
        self.db_path = self.root / "t.sqlite"

    def _conn(self):
        return db.connect(self.db_path)

    def _run(self, conn, agent_runner=None, **kwargs):
        return ingest.run(
            conn, self.repo,
            agent_runner=agent_runner,
            staging_dir=self.staging_dir,
            codex_sessions_root=self.root / "no-codex-here",
            claude_projects_root=self.claude_root,
            **kwargs,
        )

    def _plan(self, conn, **kwargs):
        return ingest.plan_work(
            conn, self.repo,
            staging_dir=self.staging_dir,
            codex_sessions_root=self.root / "no-codex-here",
            claude_projects_root=self.claude_root,
            **kwargs,
        )


class TestIdempotency(IngestTestBase):
    def test_fresh_run_writes_rows_then_second_run_is_a_no_op(self):
        conn = self._conn()
        fake = FakeAgentRunner()

        report1 = self._run(conn, agent_runner=fake)

        self.assertEqual(report1.changes_written, 1)
        self.assertEqual(report1.tasks_written, 1)
        self.assertEqual(report1.sessions_written, 2)
        self.assertEqual(report1.rows_written.get("complexity"), 1)
        self.assertEqual(report1.rows_written.get("grading"), 1)
        self.assertEqual(report1.rows_written.get("phase_labeling"), 1)
        self.assertEqual(len(fake.calls), 3)  # complexity, grading, phase_labeling

        self.assertEqual(conn.execute("SELECT COUNT(*) FROM changes").fetchone()[0], 1)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM tasks").fetchone()[0], 1)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM sessions").fetchone()[0], 2)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM complexity").fetchone()[0], 1)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 1)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM phase_tokens").fetchone()[0], 1)
        # brief_text and review_text archived verbatim.
        brief_text = conn.execute("SELECT brief_text FROM tasks").fetchone()[0]
        self.assertIn("widget core module", brief_text)
        review_text = conn.execute("SELECT review_text FROM grades").fetchone()[0]
        self.assertIn("PASS", review_text)

        dump_path = self.db_path.parent / (self.db_path.stem + ".dump.jsonl")
        dump_after_run1 = dump_path.read_bytes()

        # Second run: nothing new, fake runner not invoked again.
        report2 = self._run(conn, agent_runner=fake)
        self.assertEqual(report2.total_writes, 0)
        self.assertEqual(report2.rows_written, {})
        self.assertEqual(report2.dispatched, {})
        self.assertEqual(len(fake.calls), 3)  # unchanged from run 1

        # A true no-op run does NOT append to ingest_log (it would otherwise
        # bloat the audit trail on every repeated no-op run); still exactly
        # the 1 row from run 1's real work.
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM ingest_log").fetchone()[0], 1)

        # And the JSONL dump -- which excludes ingest_log entirely -- is
        # byte-identical across the no-op re-run.
        self.assertEqual(dump_path.read_bytes(), dump_after_run1)


class TestDryRun(IngestTestBase):
    def test_dry_run_leaves_db_byte_identical_and_plan_nonempty(self):
        conn = self._conn()
        conn.close()
        before = self.db_path.read_bytes()

        conn = self._conn()
        report = self._run(conn, agent_runner=FakeAgentRunner(), dry_run=True)
        conn.close()
        after = self.db_path.read_bytes()

        self.assertEqual(before, after)
        self.assertTrue(report.dry_run)
        self.assertFalse(report.plan.is_empty())
        self.assertIn(f"{CHANGE_NAME}/{TASK_KEY}", report.plan.new_tasks)
        self.assertEqual(len(report.plan.missing_complexity), 1)
        self.assertEqual(len(report.plan.missing_grades), 1)
        self.assertEqual(len(report.plan.missing_phase_labels), 1)

    def test_scoped_dry_run_lists_only_in_scope_sessions(self):
        # fix-round-1, finding 3: disc.joined spans the FULL landed
        # universe always (evidence is computed unscoped -- see
        # _has_change_evidence), but run() only ever WRITES sessions for
        # (change, task_key) keys in disc.briefs, which --change DOES
        # narrow. A scoped dry-run's new_sessions must match that -- not
        # overstate it with a real, evidenced join to an out-of-scope
        # change.
        sdd2 = self.repo / ".superpowers" / "sdd" / "add-gadget"
        sdd2.mkdir(parents=True)
        (sdd2 / "task-1-brief.md").write_text("Implement the gadget core module.\n")
        archive2 = self.repo / "openspec" / "changes" / "archive" / "2026-07-11-add-gadget"
        archive2.mkdir(parents=True)
        (archive2 / "proposal.md").write_text("proposal\n")
        _run_git(self.repo, "add", "-A")
        _run_git(self.repo, "commit", "-q", "-m", "add second change")

        _write_claude_session(
            self.claude_root / "gadget-impl-session.jsonl", "gadget-impl-1",
            "Read your task brief at .superpowers/sdd/add-gadget/task-1-brief.md and implement it.",
            "Implemented the gadget core.",
        )

        conn_unscoped = self._conn()
        plan_unscoped = self._plan(conn_unscoped)
        self.assertIn("claude:gadget-impl-1", plan_unscoped.new_sessions)
        self.assertIn("claude:impl-1", plan_unscoped.new_sessions)

        conn_scoped = db.connect(self.root / "scoped.sqlite")
        plan_scoped = ingest.plan_work(
            conn_scoped, self.repo, change=CHANGE_NAME,
            staging_dir=self.staging_dir,
            codex_sessions_root=self.root / "no-codex-here",
            claude_projects_root=self.claude_root,
        )
        self.assertIn("claude:impl-1", plan_scoped.new_sessions)
        self.assertIn("claude:review-1", plan_scoped.new_sessions)
        self.assertNotIn("claude:gadget-impl-1", plan_scoped.new_sessions)

        # The scoped plan must match what a scoped real run actually writes.
        self._run(conn_scoped, agent_runner=FakeAgentRunner(), change=CHANGE_NAME)
        row = conn_scoped.execute(
            "SELECT session_id FROM sessions WHERE session_id = ?", ("claude:gadget-impl-1",)
        ).fetchone()
        self.assertIsNone(row)


class TestNoAgents(IngestTestBase):
    def test_no_agents_writes_mechanical_rows_only(self):
        conn = self._conn()
        fake = FakeAgentRunner()

        report = self._run(conn, agent_runner=fake, no_agents=True)

        self.assertEqual(report.tasks_written, 1)
        self.assertEqual(report.sessions_written, 2)
        self.assertEqual(report.rows_written, {})
        self.assertEqual(len(fake.calls), 0)  # never invoked

        self.assertEqual(conn.execute("SELECT COUNT(*) FROM complexity").fetchone()[0], 0)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 0)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM phase_tokens").fetchone()[0], 0)

        # Gaps are still visible in the report's plan, just not dispatched.
        self.assertEqual(len(report.plan.missing_complexity), 1)
        self.assertEqual(len(report.plan.missing_grades), 1)
        self.assertEqual(len(report.plan.missing_phase_labels), 1)


class TestCrashResume(IngestTestBase):
    def test_crash_after_staging_resumes_without_reinvoking(self):
        conn = self._conn()
        fake = FakeAgentRunner(fail_on_call=1)

        with self.assertRaises(RuntimeError):
            self._run(conn, agent_runner=fake)

        # The crash rolled back the whole task's transaction.
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM tasks").fetchone()[0], 0)
        self.assertEqual(len(fake.calls), 1)

        # But its staged output survived the crash on disk.
        staged = list((self.staging_dir / "complexity").glob("*.json"))
        self.assertEqual(len(staged), 1)

        # Re-run completes, using the staged file for complexity without
        # re-invoking the runner for that key; the runner IS invoked again
        # for grading/phase_labeling (never attempted before the crash).
        report2 = self._run(conn, agent_runner=fake)

        self.assertEqual(report2.rows_written.get("complexity"), 1)
        self.assertEqual(report2.staged_used.get("complexity"), 1)
        self.assertNotIn("complexity", report2.dispatched)
        self.assertEqual(len(fake.calls), 1 + 2)  # +grading, +phase_labeling only

        self.assertEqual(conn.execute("SELECT COUNT(*) FROM complexity").fetchone()[0], 1)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 1)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM phase_tokens").fetchone()[0], 1)


class SelectiveAgentRunner:
    """Like ``FakeAgentRunner`` for complexity/phase_labeling (always canned
    valid output), but "grading" items are handled per ``grading_mode``
    (followup-7, defect 2):

    - ``"valid"`` (default): canned normal grade, same as FakeAgentRunner.
    - ``"decline"``: writes NO output file at all for grading items --
      simulating a scorer that correctly found nothing gradeable but (the
      pre-fix bug) never wrote a file, which used to crash the whole run.
    - ``"ungradeable"``: writes prompts/grading.md's Ungradeable shape (all
      G-fields null, a non-empty ``reason``) -- the fixed contract for the
      same "nothing gradeable" situation.
    - ``"valid_with_reason"`` (fix-round-1, finding 2): a fully populated,
      real grade (all G-fields real ints) that ALSO carries a harmless
      explanatory ``reason`` -- must still ingest as a real grade, never
      be mistaken for the ungradeable shape.
    - ``"half_null_hybrid"`` (fix-round-1, finding 2): SOME G-fields null,
      some not -- invalid under either shape; must be rejected, never
      silently accepted as either "graded" or "ungradeable".
    """

    def __init__(self, grading_mode="valid", reason="all reviews mis-joined; nothing gradeable"):
        self.calls = []
        self.grading_mode = grading_mode
        self.reason = reason
        self._canned_runner = FakeAgentRunner()

    def __call__(self, kind, items, staging_dir):
        self.calls.append((kind, [dict(i) for i in items]))
        written = []
        for item in items:
            if kind == "grading" and self.grading_mode == "decline":
                continue  # write nothing at all for this item
            data = self._canned(kind, item)
            entity_key = item.get("task_key") or item.get("session_key")
            fname = ingest._staging_filename(entity_key, item["version"], item["input_sha256"])
            out_dir = Path(staging_dir) / kind
            out_dir.mkdir(parents=True, exist_ok=True)
            tmp = out_dir / (fname + ".tmp")
            tmp.write_text(json.dumps(data), encoding="utf-8")
            final = out_dir / fname
            os.replace(tmp, final)
            written.append(final)
        return written

    def _canned(self, kind, item):
        if kind == "grading" and self.grading_mode == "ungradeable":
            return {
                "task_key": item["task_key"],
                "G1": None, "G2": None, "G3": None, "G4": None, "G5": None,
                "n_critical": 0, "n_important": 0, "n_minor": 0,
                "verdict_sequence": None, "rounds_to_accept": None,
                "final_grade": None, "evidence": None,
                "reviewer_models": [], "excluded_reviews": ["review-1", "review-2"],
                "reason": self.reason,
            }
        if kind == "grading" and self.grading_mode == "valid_with_reason":
            data = dict(self._canned_runner._canned(kind, item))
            data["reason"] = self.reason
            return data
        if kind == "grading" and self.grading_mode == "half_null_hybrid":
            data = dict(self._canned_runner._canned(kind, item))
            data["G1"] = None  # only G1 null -- the rest stay real ints
            return data
        return self._canned_runner._canned(kind, item)


class TestScorerDeclineRobustness(IngestTestBase):
    """followup-7, defect 2: a scorer that correctly declines (nothing
    gradeable, e.g. every joined review turned out to be mis-joined) must
    not crash the run; a genuinely missing/invalid output (decline against
    the prompt's own contract, or broken infra -- indistinguishable from
    here) fails only that one item by default, aborting the whole run only
    under --strict."""

    def test_grading_decline_without_output_fails_only_that_item_by_default(self):
        conn = self._conn()
        runner = SelectiveAgentRunner(grading_mode="decline")

        report = self._run(conn, agent_runner=runner)  # strict defaults to False

        # The task's OTHER work still landed -- one broken item doesn't
        # abort the whole task's transaction, let alone the whole run.
        self.assertEqual(report.tasks_written, 1)
        self.assertEqual(report.rows_written.get("complexity"), 1)
        self.assertEqual(report.rows_written.get("phase_labeling"), 1)
        self.assertNotIn("grading", report.rows_written)

        self.assertEqual(report.failed.get("grading"), 1)
        self.assertEqual(len(report.failed_items), 1)
        self.assertEqual(report.failed_items[0]["kind"], "grading")
        self.assertIn(f"{CHANGE_NAME}--{TASK_KEY}", report.failed_items[0]["entity_key"])
        self.assertNotIn("grading", report.ungradeable)

        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 0)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM complexity").fetchone()[0], 1)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM tasks").fetchone()[0], 1)

    def test_grading_decline_without_output_raises_with_strict(self):
        conn = self._conn()
        runner = SelectiveAgentRunner(grading_mode="decline")

        with self.assertRaises(RuntimeError):
            self._run(conn, agent_runner=runner, strict=True)

        # strict restores the pre-fix behavior: the whole task's transaction
        # rolls back (same as TestCrashResume's raise-from-runner case).
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM tasks").fetchone()[0], 0)

    def test_ungradeable_result_recorded_durably_without_grades_row_and_run_continues(self):
        conn = self._conn()
        runner = SelectiveAgentRunner(grading_mode="ungradeable", reason="both reviews mis-joined")

        report = self._run(conn, agent_runner=runner)

        self.assertEqual(report.tasks_written, 1)
        self.assertEqual(report.rows_written.get("complexity"), 1)
        self.assertEqual(report.rows_written.get("phase_labeling"), 1)
        self.assertNotIn("grading", report.rows_written)  # no grades row written

        self.assertEqual(report.ungradeable.get("grading"), 1)
        self.assertEqual(len(report.ungradeable_items), 1)
        self.assertEqual(report.ungradeable_items[0]["reason"], "both reviews mis-joined")
        self.assertIn(f"{CHANGE_NAME}--{TASK_KEY}", report.ungradeable_items[0]["entity_key"])
        self.assertNotIn("grading", report.failed)

        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 0)

        # Durable in the audit trail too.
        log_row = conn.execute(
            "SELECT actions_json FROM ingest_log ORDER BY run_id DESC LIMIT 1"
        ).fetchone()
        actions = json.loads(log_row[0])
        self.assertEqual(actions["ungradeable"].get("grading"), 1)
        self.assertEqual(len(actions["ungradeable_items"]), 1)

    def test_ungradeable_result_is_idempotent_across_reruns(self):
        conn = self._conn()
        runner = SelectiveAgentRunner(grading_mode="ungradeable")
        self._run(conn, agent_runner=runner)
        self.assertEqual(len(runner.calls), 3)  # complexity, grading, phase_labeling

        # Second run: the staged ungradeable file satisfies the cache key
        # -- no re-dispatch, still no grades row, still recorded.
        report2 = self._run(conn, agent_runner=runner)
        self.assertEqual(len(runner.calls), 3)  # unchanged -- nothing new to dispatch
        self.assertEqual(report2.total_writes, 0)  # no NEW rows this run
        self.assertEqual(report2.ungradeable.get("grading"), 1)
        self.assertEqual(report2.staged_used.get("grading"), 1)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 0)

    def test_ungradeable_rescore_supersedes_stale_grade(self):
        # fix-round-1, finding 1: a real grades row already exists (from a
        # normal first run). A second reviewer session then changes the
        # joined review text (REASON_INPUT_CHANGED), and THIS TIME the
        # scorer correctly finds nothing gradeable. The stale row's
        # input_sha256 no longer matches any real input -- it must not
        # survive, or a downstream reader would keep consuming a grade for
        # review text that isn't what's actually joined anymore.
        conn = self._conn()
        self._run(conn, agent_runner=FakeAgentRunner())
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 1)

        _write_claude_session(
            self.claude_root / "review-session-2.jsonl", "review-2", REVIEWER_PROMPT,
            "REVISE: actually this whole review is about the wrong module.",
            ts_start="2026-07-10T02:00:00Z", ts_end="2026-07-10T02:10:00Z",
        )

        runner = SelectiveAgentRunner(grading_mode="ungradeable", reason="the new review is mis-joined")
        report2 = self._run(conn, agent_runner=runner)

        self.assertEqual(report2.ungradeable.get("grading"), 1)
        self.assertEqual(report2.superseded_grades, 1)
        self.assertTrue(report2.ungradeable_items[0].get("superseded_grades_row"))
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 0)

        log_row = conn.execute(
            "SELECT actions_json FROM ingest_log ORDER BY run_id DESC LIMIT 1"
        ).fetchone()
        actions = json.loads(log_row[0])
        self.assertEqual(actions["superseded_grades"], 1)

    def test_ungradeable_first_dispatch_does_not_touch_grades_table(self):
        # The DELETE in the grading branch must be a harmless no-op (0
        # rows) in the far more common REASON_MISSING case -- no prior
        # grades row exists at all yet.
        conn = self._conn()
        runner = SelectiveAgentRunner(grading_mode="ungradeable")
        report = self._run(conn, agent_runner=runner)
        self.assertEqual(report.superseded_grades, 0)
        self.assertNotIn("superseded_grades_row", report.ungradeable_items[0])
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 0)

    def test_populated_grade_with_harmless_reason_ingests_as_a_grade(self):
        # fix-round-1, finding 2: a `reason` key alone must not
        # discriminate -- a fully populated, real grade (all G fields
        # non-null) that also happens to carry an explanatory `reason`
        # must ingest as a grade, not be silently suppressed.
        conn = self._conn()
        runner = SelectiveAgentRunner(grading_mode="valid_with_reason", reason="fyi, borderline but clean")

        report = self._run(conn, agent_runner=runner)

        self.assertEqual(report.rows_written.get("grading"), 1)
        self.assertNotIn("grading", report.ungradeable)
        self.assertEqual(report.superseded_grades, 0)
        row = conn.execute("SELECT g1, final_grade FROM grades").fetchone()
        self.assertEqual(row[0], 5)
        self.assertEqual(row[1], "A")

    def test_half_null_hybrid_grading_shape_is_rejected(self):
        # fix-round-1, finding 2: a shape with SOME G fields null and some
        # not is neither a valid grade nor a valid ungradeable result --
        # _validate_staged rejects it outright (same as any other
        # malformed staged file), so _resolve_gap sees "no valid staged
        # file resulted" and fails just this item (report.failed) rather
        # than guessing at either shape; the run continues for other kinds.
        conn = self._conn()
        runner = SelectiveAgentRunner(grading_mode="half_null_hybrid")

        report = self._run(conn, agent_runner=runner)

        self.assertNotIn("grading", report.rows_written)
        self.assertNotIn("grading", report.ungradeable)
        self.assertEqual(report.failed.get("grading"), 1)
        self.assertEqual(conn.execute("SELECT COUNT(*) FROM grades").fetchone()[0], 0)
        self.assertEqual(report.rows_written.get("complexity"), 1)


class TestGradingShapeDiscrimination(unittest.TestCase):
    """fix-round-1, finding 2: direct unit coverage of ingest.py's
    ``_grading_shape``/``_validate_staged`` for grading -- the three
    shapes a staged grading JSON can take."""

    def test_graded_shape(self):
        data = {
            "task_key": "t", "G1": 5, "G2": 4, "G3": 5, "G4": 5, "G5": 4,
            "n_critical": 0, "n_important": 1, "n_minor": 0,
            "verdict_sequence": "PASS", "rounds_to_accept": 1,
            "final_grade": "A", "evidence": "e",
            "reviewer_models": ["m"], "excluded_reviews": [],
        }
        self.assertEqual(ingest._grading_shape(data), "graded")
        self.assertTrue(ingest._validate_staged("grading", data))

    def test_graded_shape_with_harmless_reason_is_still_graded(self):
        data = {
            "task_key": "t", "G1": 5, "G2": 4, "G3": 5, "G4": 5, "G5": 4,
            "n_critical": 0, "n_important": 1, "n_minor": 0,
            "verdict_sequence": "PASS", "rounds_to_accept": 1,
            "final_grade": "A", "evidence": "e",
            "reviewer_models": ["m"], "excluded_reviews": [],
            "reason": "harmless note",
        }
        self.assertEqual(ingest._grading_shape(data), "graded")
        self.assertTrue(ingest._validate_staged("grading", data))

    def test_ungradeable_shape(self):
        data = {
            "task_key": "t", "G1": None, "G2": None, "G3": None, "G4": None, "G5": None,
            "n_critical": 0, "n_important": 0, "n_minor": 0,
            "verdict_sequence": None, "rounds_to_accept": None,
            "final_grade": None, "evidence": None,
            "reviewer_models": [], "excluded_reviews": ["r1"],
            "reason": "all reviews mis-joined",
        }
        self.assertEqual(ingest._grading_shape(data), "ungradeable")
        self.assertTrue(ingest._validate_staged("grading", data))

    def test_all_null_without_reason_is_invalid(self):
        data = {
            "task_key": "t", "G1": None, "G2": None, "G3": None, "G4": None, "G5": None,
            "n_critical": 0, "n_important": 0, "n_minor": 0,
            "verdict_sequence": None, "rounds_to_accept": None,
            "final_grade": None, "evidence": None,
            "reviewer_models": [], "excluded_reviews": [],
        }
        self.assertIsNone(ingest._grading_shape(data))
        self.assertFalse(ingest._validate_staged("grading", data))

    def test_half_null_hybrid_is_invalid(self):
        data = {
            "task_key": "t", "G1": None, "G2": 4, "G3": 5, "G4": 5, "G5": 4,
            "n_critical": 0, "n_important": 1, "n_minor": 0,
            "verdict_sequence": "PASS", "rounds_to_accept": 1,
            "final_grade": "A", "evidence": "e",
            "reviewer_models": ["m"], "excluded_reviews": [],
        }
        self.assertIsNone(ingest._grading_shape(data))
        self.assertFalse(ingest._validate_staged("grading", data))


class TestRescore(IngestTestBase):
    def test_rubric_bump_reported_stale_then_rescored_with_flag(self):
        conn = self._conn()
        fake = FakeAgentRunner()
        self._run(conn, agent_runner=fake)
        self.assertEqual(len(fake.calls), 3)

        real_version = assets.read_asset_version
        complexity_path = str(ingest.KIND_RUBRIC_PATH["complexity"])

        def bumped_version(path):
            if str(path) == complexity_path:
                return "2"
            return real_version(path)

        original = assets.read_asset_version
        assets.read_asset_version = bumped_version
        try:
            # Without --rescore: untouched, reported stale.
            report_no_rescore = self._run(conn, agent_runner=fake)
            self.assertEqual(report_no_rescore.stale.get("complexity"), 1)
            self.assertNotIn("complexity", report_no_rescore.dispatched)
            self.assertEqual(len(fake.calls), 3)  # not invoked again
            rows = conn.execute("SELECT rubric_version FROM complexity").fetchall()
            self.assertEqual([r[0] for r in rows], ["1"])  # v1 retained, untouched

            # With --rescore complexity: re-dispatched, new version row added
            # alongside the retained v1 row.
            report_rescore = self._run(conn, agent_runner=fake, rescore="complexity")
            self.assertEqual(report_rescore.rows_written.get("complexity"), 1)
            self.assertEqual(report_rescore.dispatched.get("complexity"), 1)
            self.assertEqual(len(fake.calls), 4)

            rows = conn.execute(
                "SELECT rubric_version FROM complexity ORDER BY rubric_version"
            ).fetchall()
            self.assertEqual([r[0] for r in rows], ["1", "2"])
        finally:
            assets.read_asset_version = original


class TestQuarantine(IngestTestBase):
    """Beyond the five required scenarios: a session whose task key ("task-1")
    matches more than one landed change's brief (no change_dir/openspec_change
    in its prompt to disambiguate) must be quarantined -- stored as a
    `sessions` row with task_id NULL, never guessed onto either task."""

    def setUp(self):
        super().setUp()
        # A second landed change, also with a task-1 brief.
        sdd2 = self.repo / ".superpowers" / "sdd" / "add-gadget"
        sdd2.mkdir(parents=True)
        (sdd2 / "task-1-brief.md").write_text("Implement the gadget core module.\n")
        archive2 = self.repo / "openspec" / "changes" / "archive" / "2026-07-11-add-gadget"
        archive2.mkdir(parents=True)
        (archive2 / "proposal.md").write_text("proposal\n")
        _run_git(self.repo, "add", "-A")
        _run_git(self.repo, "commit", "-q", "-m", "add second change")

        _write_claude_session(
            self.claude_root / "ambiguous-session.jsonl", "ambiguous-1",
            "Continue implementing task 1 as directed.",
            "Made progress.",
        )

    def test_ambiguous_task_key_is_quarantined_not_guessed(self):
        conn = self._conn()
        plan = self._plan(conn)
        self.assertEqual(len(plan.quarantined), 1)
        q = plan.quarantined[0]
        self.assertEqual(q.session_id, "claude:ambiguous-1")
        self.assertIn("add-widget/task-1", q.candidates)
        self.assertIn("add-gadget/task-1", q.candidates)

        report = self._run(conn, agent_runner=FakeAgentRunner())
        row = conn.execute(
            "SELECT task_id FROM sessions WHERE session_id = ?", ("claude:ambiguous-1",)
        ).fetchone()
        self.assertIsNotNone(row)
        self.assertIsNone(row[0])
        self.assertEqual(len(report.quarantined), 1)

    def test_reason_is_no_evidence_not_ambiguity_when_prompt_names_neither_change(self):
        # followup-7, defect 1: this session's prompt names NEITHER change
        # (no change_dir/openspec_change/plan/brief-dir evidence at all)
        # -- it must quarantine as "no change evidence", distinct from the
        # "task key matches >1 task" reason reserved for a session that
        # DOES give evidence, just for more than one change at once.
        conn = self._conn()
        plan = self._plan(conn)
        self.assertEqual(plan.quarantined[0].reason, "no change evidence for task key")

    def test_scoped_and_unscoped_join_outcomes_are_identical(self):
        # followup-7, defect 1 (the actual canary bug): --change must only
        # filter which JOINED sessions get ingested, never alter the join
        # decision itself. Before the fix, `--change add-widget` collapsed
        # brief_index to a single candidate, so this same ambiguous session
        # would have joined add-widget/task-1 outright instead of
        # quarantining -- exactly what happened to ~75 real sessions
        # against add-sdd-task-analyzer in the first canary run.
        conn_unscoped = self._conn()
        plan_unscoped = self._plan(conn_unscoped)

        conn_scoped = db.connect(self.root / "scoped.sqlite")
        plan_scoped = ingest.plan_work(
            conn_scoped, self.repo, change=CHANGE_NAME,
            staging_dir=self.staging_dir,
            codex_sessions_root=self.root / "no-codex-here",
            claude_projects_root=self.claude_root,
        )

        self.assertEqual(len(plan_unscoped.quarantined), 1)
        self.assertEqual(len(plan_scoped.quarantined), 1)
        self.assertEqual(plan_unscoped.quarantined[0].session_id, plan_scoped.quarantined[0].session_id)
        self.assertEqual(plan_unscoped.quarantined[0].reason, plan_scoped.quarantined[0].reason)
        self.assertEqual(
            sorted(plan_unscoped.quarantined[0].candidates),
            sorted(plan_scoped.quarantined[0].candidates),
        )
        # Sanity: --change still discovers add-widget's own (unambiguous)
        # task normally -- only the AMBIGUOUS session's outcome is what
        # must match between scoped and unscoped runs.
        self.assertIn(f"{CHANGE_NAME}/{TASK_KEY}", plan_scoped.new_tasks)
        report_scoped = self._run(conn_scoped, agent_runner=FakeAgentRunner(), change=CHANGE_NAME)
        row = conn_scoped.execute(
            "SELECT task_id FROM sessions WHERE session_id = ?", ("claude:ambiguous-1",)
        ).fetchone()
        self.assertIsNotNone(row)
        self.assertIsNone(row[0])  # quarantined, not guessed onto add-widget/task-1
        self.assertEqual(len(report_scoped.quarantined), 1)

    def test_reason_is_ambiguity_when_prompt_gives_evidence_for_both_changes(self):
        # The complementary case to the "no evidence" test above: a
        # session whose prompt affirmatively names BOTH changes' SDD
        # directories (e.g. a coordinator session comparing them) still
        # correctly quarantines, but with the "task key matches >1 task"
        # reason -- real evidence for more than one candidate, not an
        # absence of evidence.
        _write_claude_session(
            self.claude_root / "evidenced-ambiguous-session.jsonl", "evidenced-ambiguous-1",
            "Compare .superpowers/sdd/add-widget/task-1-brief.md against "
            ".superpowers/sdd/add-gadget/task-1-brief.md for task 1.",
            "Compared both.",
        )
        conn = self._conn()
        plan = self._plan(conn)
        self.assertEqual(len(plan.quarantined), 2)
        q = next(q for q in plan.quarantined if q.session_id == "claude:evidenced-ambiguous-1")
        self.assertEqual(q.reason, "task key matches >1 task")
        self.assertIn("add-widget/task-1", q.candidates)
        self.assertIn("add-gadget/task-1", q.candidates)

    def test_healing_pass_corrects_pre_fix_residue_and_is_idempotent(self):
        # followup-7, defect 1 amendment: simulate exactly the canary's
        # residue shape -- a session row already committed with a task_id
        # from the OLD scope-narrowing bug (it would have joined
        # "claude:ambiguous-1" onto add-widget/task-1 under `--change
        # add-widget`, despite giving zero real evidence for either
        # candidate). A re-run under the fixed evidence-based logic must
        # heal it back to quarantined (task_id NULL), and a further re-run
        # must find nothing left to heal.
        conn = self._conn()
        report1 = self._run(conn, agent_runner=FakeAgentRunner())
        row = conn.execute(
            "SELECT task_id FROM sessions WHERE session_id = ?", ("claude:ambiguous-1",)
        ).fetchone()
        self.assertIsNone(row[0])  # correctly quarantined by the fixed logic already

        widget_task_id = conn.execute(
            "SELECT t.task_id FROM tasks t JOIN changes c ON c.change_id = t.change_id "
            "WHERE c.name = ? AND t.task_key = ?", (CHANGE_NAME, TASK_KEY),
        ).fetchone()[0]

        # Corrupt it to look like pre-fix residue: wrongly joined to
        # add-widget/task-1, exactly what the old scope-narrowing bug did.
        conn.execute(
            "UPDATE sessions SET task_id = ? WHERE session_id = ?",
            (widget_task_id, "claude:ambiguous-1"),
        )
        conn.commit()
        corrupted = conn.execute(
            "SELECT task_id FROM sessions WHERE session_id = ?", ("claude:ambiguous-1",)
        ).fetchone()[0]
        self.assertEqual(corrupted, widget_task_id)

        report2 = self._run(conn, agent_runner=FakeAgentRunner())
        self.assertEqual(report2.healed_sessions, 1)
        self.assertEqual(report2.healed_session_ids, ["claude:ambiguous-1"])
        healed = conn.execute(
            "SELECT task_id FROM sessions WHERE session_id = ?", ("claude:ambiguous-1",)
        ).fetchone()[0]
        self.assertIsNone(healed)

        # Idempotent: nothing left to heal on a further re-run.
        report3 = self._run(conn, agent_runner=FakeAgentRunner())
        self.assertEqual(report3.healed_sessions, 0)
        self.assertEqual(report3.healed_session_ids, [])


class TestChangeEvidenceJoining(IngestTestBase):
    """followup-7, defect 1: the two ADDITIONAL evidence sources beyond
    change_dir/openspec_change (which the pre-existing fixtures already
    exercise via their `.superpowers/sdd/<change>/task-N-brief.md`
    prompts) -- a mention of the change's own plan file, and a mention of
    its SDD brief directory via a file `discovery.task_keys` doesn't
    itself extract a change_dir from."""

    def setUp(self):
        super().setUp()
        # A second landed change with a task-1 brief AND a committed plan
        # file, disambiguating add-widget/task-1 vs add-gadget/task-1 via
        # evidence OTHER than an exact brief-path match.
        sdd2 = self.repo / ".superpowers" / "sdd" / "add-gadget"
        sdd2.mkdir(parents=True)
        (sdd2 / "task-1-brief.md").write_text("Implement the gadget core module.\n")
        archive2 = self.repo / "openspec" / "changes" / "archive" / "2026-07-11-add-gadget"
        archive2.mkdir(parents=True)
        (archive2 / "proposal.md").write_text("proposal\n")
        plans_dir = self.repo / "docs" / "superpowers" / "plans"
        plans_dir.mkdir(parents=True, exist_ok=True)
        (plans_dir / "2026-07-11-add-gadget.md").write_text(
            "## Task 1: Gadget Core\n\nBuild the gadget.\n"
        )
        _run_git(self.repo, "add", "-A")
        _run_git(self.repo, "commit", "-q", "-m", "add second change with a plan file")

    def test_joins_via_plan_path_mention_alone(self):
        # No ".superpowers/sdd/..." path anywhere in the prompt -- only a
        # mention of add-gadget's own plan file.
        _write_claude_session(
            self.claude_root / "plan-evidence-session.jsonl", "plan-evidence-1",
            "See docs/superpowers/plans/2026-07-11-add-gadget.md for context. "
            "Implement task 1.",
            "Implemented.",
        )
        conn = self._conn()
        plan = self._plan(conn)
        self.assertEqual(plan.quarantined, [])
        self.assertIn("add-gadget/task-1", plan.new_tasks)

        self._run(conn, agent_runner=FakeAgentRunner())
        row = conn.execute(
            "SELECT t.task_key, c.name FROM sessions s "
            "JOIN tasks t ON t.task_id = s.task_id JOIN changes c ON c.change_id = t.change_id "
            "WHERE s.session_id = ?",
            ("claude:plan-evidence-1",),
        ).fetchone()
        self.assertIsNotNone(row)
        self.assertEqual((row["task_key"], row["name"]), ("task-1", "add-gadget"))

    def test_joins_via_sdd_directory_mention_without_exact_brief_suffix(self):
        # References a sibling file under the change's SDD directory that
        # discovery.task_keys does NOT extract a change_dir from (only
        # "-brief.md"/"-implementer-prompt.md" suffixes populate
        # change_dir) -- the broader substring check must still catch it.
        _write_claude_session(
            self.claude_root / "dir-evidence-session.jsonl", "dir-evidence-1",
            "Read .superpowers/sdd/add-gadget/task-1-review-package.md and "
            "address task 1 feedback.",
            "Addressed.",
        )
        conn = self._conn()
        plan = self._plan(conn)
        self.assertEqual(plan.quarantined, [])
        self.assertIn("add-gadget/task-1", plan.new_tasks)


class TestLandedOnlyBriefs(IngestTestBase):
    """Review finding 1: brief content (and existence) must come from the
    resolved archive-ref git tree, never the working tree filesystem --
    otherwise `--dry-run`/`plan_work` could report work driven by
    uncommitted edits, violating D3's "discovery is a pure diff against a
    landed git ref" rule."""

    def test_brief_added_only_to_working_tree_is_not_ingested(self):
        # A second task brief written to disk but never `git add`/committed.
        sdd = self.repo / ".superpowers" / "sdd" / CHANGE_NAME
        (sdd / "task-2-brief.md").write_text("Uncommitted, working-tree-only brief.\n")

        conn = self._conn()
        plan = self._plan(conn)
        self.assertNotIn(f"{CHANGE_NAME}/task-2", plan.new_tasks)

        report = self._run(conn, agent_runner=FakeAgentRunner())
        self.assertEqual(report.tasks_written, 1)  # only the committed task-1
        rows = conn.execute("SELECT task_key FROM tasks").fetchall()
        self.assertEqual([r[0] for r in rows], [TASK_KEY])

    def test_brief_content_read_from_committed_ref_not_working_tree_edit(self):
        # Edit the already-committed brief on disk without committing the
        # edit -- the working-tree version must never be read.
        brief_path = self.repo / ".superpowers" / "sdd" / CHANGE_NAME / f"{TASK_KEY}-brief.md"
        brief_path.write_text("UNCOMMITTED EDIT should never be seen.\n")

        conn = self._conn()
        self._run(conn, agent_runner=FakeAgentRunner())
        brief_text = conn.execute("SELECT brief_text FROM tasks").fetchone()[0]
        self.assertIn("widget core module", brief_text)
        self.assertNotIn("UNCOMMITTED EDIT", brief_text)


class TestArchiveRefRecorded(IngestTestBase):
    """Review finding 3: the resolved archive ref name (not just its SHA)
    must be recoverable from ingest_log for audit purposes."""

    def test_archive_ref_recorded_in_ingest_log_actions(self):
        conn = self._conn()
        self._run(conn, agent_runner=FakeAgentRunner(), archive_ref="refs/heads/main")
        row = conn.execute("SELECT git_sha, actions_json FROM ingest_log").fetchone()
        self.assertIsNotNone(row)
        actions = json.loads(row[1])
        self.assertEqual(actions["archive_ref"], "refs/heads/main")
        self.assertTrue(row[0])  # git_sha (resolved commit) also present


class TestTaskSectionsFromPlan(unittest.TestCase):
    """followup-6, defect 2 (fix-round-1): ``ingest._task_sections_from_plan``
    accepts ``Task N:`` headings (colon required -- every one of the 362
    real task headings surveyed across ``docs/superpowers/plans/*.md`` has
    one; it's what excludes a plan's own "Task N Review" sub-headings from
    being mistaken for a task boundary) at any consistent heading level,
    skipping headings inside fenced code blocks. A section runs from its
    own heading through the line before the NEXT heading at the SAME
    level -- not "the next Task heading of any number" (the original,
    looser rule the `subagent-driven-development` skill's own
    `task-brief` script uses, which folded a real plan's "Task 1 Review"
    section into Task 1's brief_text, polluting brief_sha256 and every
    downstream agentic cache key -- see followup-6-report.md's "Fix round
    1")."""

    def test_heading_level_two_consistent_throughout(self):
        plan = "## Task 1: First\n\nBody one.\n\n## Task 2: Second\n\nBody two.\n"
        sections = ingest._task_sections_from_plan(plan)
        self.assertEqual(set(sections), {1, 2})
        self.assertIn("Body one.", sections[1])
        self.assertNotIn("Body two.", sections[1])
        self.assertIn("Body two.", sections[2])

    def test_heading_level_three_consistent_throughout(self):
        # Real plans use "## Task N" as often as "### Task N" -- but never
        # mixed levels for task headings within one plan (surveyed on
        # main: zero exceptions) -- so this is a SEPARATE plan example,
        # not a level mixed into the same one.
        plan = "### Task 1: First\n\nBody one.\n\n### Task 2: Second\n\nBody two.\n"
        sections = ingest._task_sections_from_plan(plan)
        self.assertEqual(set(sections), {1, 2})
        self.assertIn("Body one.", sections[1])
        self.assertNotIn("Body two.", sections[1])
        self.assertIn("Body two.", sections[2])

    def test_review_subheading_at_the_same_level_closes_the_task_without_becoming_one(self):
        # The exact real-corpus shape (docs/superpowers/plans/2026-06-26-
        # add-synth-midi-controller-io.md): "### Task 1 Review" at the SAME
        # level as "### Task 1:" must close task 1's section -- and must
        # NOT itself become a phantom "task 1 review" task, since it has
        # no colon right after the number.
        plan = (
            "### Task 1: First\n\n"
            "Body one.\n\n"
            "### Task 1 Review\n\n"
            "Reviewer notes that must never appear in task 1's brief.\n\n"
            "### Task 2: Second\n\n"
            "Body two.\n"
        )
        sections = ingest._task_sections_from_plan(plan)
        self.assertEqual(set(sections), {1, 2})  # no phantom task for "1 Review"
        self.assertNotIn("Reviewer notes", sections[1])
        self.assertNotIn("Task 1 Review", sections[1])
        self.assertIn("Body one.", sections[1])
        self.assertIn("Body two.", sections[2])

    def test_section_runs_through_next_same_level_heading_not_next_task_heading_of_any_kind(self):
        # A DEEPER subsection heading within a task (not at the task's own
        # level) must stay part of that task's section.
        plan = (
            "## Task 1: First\n\n"
            "#### Verification\n\n"
            "Run the tests.\n\n"
            "## Task 2: Second\n\n"
            "Body two.\n"
        )
        sections = ingest._task_sections_from_plan(plan)
        self.assertIn("#### Verification", sections[1])
        self.assertIn("Run the tests.", sections[1])
        self.assertNotIn("Body two.", sections[1])

    def test_non_task_heading_at_the_same_level_still_closes_the_task(self):
        # Not just "Task N Review" specifically -- ANY heading at the same
        # level closes the currently open task's section.
        plan = (
            "## Task 1: First\n\n"
            "Body one.\n\n"
            "## Notes\n\n"
            "Some unrelated notes that must not leak into task 1.\n\n"
            "## Task 2: Second\n\n"
            "Body two.\n"
        )
        sections = ingest._task_sections_from_plan(plan)
        self.assertNotIn("unrelated notes", sections[1])
        self.assertIn("Body two.", sections[2])

    def test_task_heading_inside_a_fenced_code_block_is_not_a_real_heading(self):
        plan = (
            "## Task 1: First\n\n"
            "Example plan snippet:\n\n"
            "```\n"
            "## Task 99: not a real task heading\n"
            "```\n\n"
            "Real body continues.\n"
        )
        sections = ingest._task_sections_from_plan(plan)
        self.assertEqual(set(sections), {1})
        self.assertIn("Real body continues.", sections[1])

    def test_no_task_headings_yields_empty_dict(self):
        plan = "# Just a plan\n\nNo task sections here at all.\n"
        self.assertEqual(ingest._task_sections_from_plan(plan), {})

    def test_task_heading_without_colon_is_not_a_task_heading(self):
        # Every real task heading on main has a colon right after the
        # number -- a bare "Task N" with no colon (and no real corpus
        # example of this shape) is deliberately not accepted, since
        # relaxing it is exactly what let "Task N Review" masquerade as a
        # task heading before this fix.
        plan = "## Task 1 no colon here\n\nBody.\n"
        self.assertEqual(ingest._task_sections_from_plan(plan), {})


class TestPlanDerivedBriefs(IngestTestBase):
    """followup-6, defect 2: the SDD workflow never commits
    ``.superpowers/`` (it's uncommitted scratch), so a landed change's
    task briefs are typically absent at the archive ref in steady state --
    only the historical migrated_v0 corpus happened to have them (that
    extraction read live worktree scratch). When a landed change has ZERO
    committed briefs, its tasks are derived from its own committed
    Superpowers plan file instead."""

    def _commit_plan_only_change(self, change_name, plan_text, date="2026-07-11"):
        archive = self.repo / "openspec" / "changes" / "archive" / f"{date}-{change_name}"
        archive.mkdir(parents=True)
        (archive / "proposal.md").write_text("proposal\n")
        plans_dir = self.repo / "docs" / "superpowers" / "plans"
        plans_dir.mkdir(parents=True, exist_ok=True)
        plan_path = plans_dir / f"{date}-{change_name}.md"
        plan_path.write_text(plan_text)
        _run_git(self.repo, "add", "-A")
        _run_git(self.repo, "commit", "-q", "-m", f"add {change_name}")
        return f"docs/superpowers/plans/{date}-{change_name}.md"

    def test_tasks_derived_from_plan_when_no_briefs_committed(self):
        change_name = "add-planonly"
        plan_rel_path = self._commit_plan_only_change(
            change_name,
            "# Add Plan Only\n\n"
            "## Task 1: First Task\n\n"
            "Do the first thing.\n\n"
            "## Task 2: Second Task\n\n"
            "Do the second thing.\n",
        )
        _write_claude_session(
            self.claude_root / "planonly-impl-1.jsonl", "planonly-impl-1",
            f"Read your task brief at .superpowers/sdd/{change_name}/task-1-brief.md and implement it.",
            "Implemented task 1.",
        )

        conn = self._conn()
        plan = self._plan(conn, change=change_name)
        self.assertIn(f"{change_name}/task-1", plan.new_tasks)
        self.assertIn(f"{change_name}/task-2", plan.new_tasks)

        report = self._run(conn, agent_runner=FakeAgentRunner(), change=change_name)
        self.assertEqual(report.tasks_written, 2)
        row = conn.execute(
            "SELECT brief_text, brief_path, brief_sha256, brief_bytes FROM tasks t "
            "JOIN changes c ON c.change_id = t.change_id "
            "WHERE c.name = ? AND t.task_key = 'task-1'",
            (change_name,),
        ).fetchone()
        self.assertIn("## Task 1: First Task", row["brief_text"])
        self.assertIn("Do the first thing.", row["brief_text"])
        self.assertNotIn("Second Task", row["brief_text"])
        self.assertEqual(row["brief_path"], f"{plan_rel_path}#task-1")
        # Cache-key fields derive from brief_text exactly as for a real
        # brief -- no special-casing (followup-6 brief's own requirement).
        self.assertEqual(row["brief_sha256"], db.sha256_text(row["brief_text"]))
        self.assertEqual(row["brief_bytes"], len(row["brief_text"].encode("utf-8")))

        # The joined implementer session actually attached to the
        # plan-derived task.
        session_row = conn.execute(
            "SELECT task_id FROM sessions WHERE session_id = ?", ("claude:planonly-impl-1",)
        ).fetchone()
        self.assertIsNotNone(session_row["task_id"])

    def test_real_briefs_win_over_plan_fallback(self):
        change_name = "add-bothsources"
        self._commit_plan_only_change(
            change_name,
            "## Task 1: Plan Version\n\nPLAN TEXT should never be used.\n",
        )
        sdd = self.repo / ".superpowers" / "sdd" / change_name
        sdd.mkdir(parents=True)
        (sdd / "task-1-brief.md").write_text("REAL BRIEF: build the thing.\n")
        _run_git(self.repo, "add", "-A")
        _run_git(self.repo, "commit", "-q", "-m", "add committed brief")

        conn = self._conn()
        self._run(conn, agent_runner=FakeAgentRunner(), change=change_name)
        row = conn.execute(
            "SELECT brief_text, brief_path FROM tasks t JOIN changes c ON c.change_id = t.change_id "
            "WHERE c.name = ? AND t.task_key = 'task-1'",
            (change_name,),
        ).fetchone()
        self.assertIn("REAL BRIEF", row["brief_text"])
        self.assertNotIn("PLAN TEXT", row["brief_text"])
        self.assertTrue(row["brief_path"].endswith("task-1-brief.md"))
        self.assertNotIn("#task-1", row["brief_path"])

    def test_plan_with_no_task_headers_yields_zero_tasks_no_crash(self):
        change_name = "add-notasks"
        self._commit_plan_only_change(change_name, "# Just a plan\n\nNo task sections at all.\n")

        conn = self._conn()
        report = self._run(conn, agent_runner=FakeAgentRunner(), change=change_name)
        self.assertEqual(report.tasks_written, 0)
        task_count = conn.execute(
            "SELECT COUNT(*) FROM tasks t JOIN changes c ON c.change_id = t.change_id WHERE c.name = ?",
            (change_name,),
        ).fetchone()[0]
        self.assertEqual(task_count, 0)
        # The change itself still ingests as a row (quarantine-free, zero
        # tasks) -- neither briefs nor task headings existing is not an
        # error.
        change_row = conn.execute("SELECT 1 FROM changes WHERE name = ?", (change_name,)).fetchone()
        self.assertIsNotNone(change_row)

    def test_neither_briefs_nor_plan_yields_zero_tasks(self):
        change_name = "add-nothingatall"
        archive = self.repo / "openspec" / "changes" / "archive" / f"2026-07-14-{change_name}"
        archive.mkdir(parents=True)
        (archive / "proposal.md").write_text("proposal\n")
        _run_git(self.repo, "add", "-A")
        _run_git(self.repo, "commit", "-q", "-m", "add change with nothing")

        conn = self._conn()
        report = self._run(conn, agent_runner=FakeAgentRunner(), change=change_name)
        self.assertEqual(report.tasks_written, 0)
        self.assertEqual(len(report.quarantined), 0)

    def test_review_subsections_excluded_from_task_briefs(self):
        # fix-round-1 (codex review, Important finding): regression
        # fixture modeled directly on the real
        # docs/superpowers/plans/2026-06-26-add-synth-midi-controller-io.md
        # shape -- "### Task N:" followed later by "### Task N Review"
        # before the next "### Task N+1:". The original (pre-fix) rule
        # ("section runs until the next Task heading of any number")
        # folded the whole review section into the preceding task's
        # brief_text; the fixed rule (close on the next SAME-level
        # heading, and require a colon for a line to count as a task
        # heading at all) must not.
        change_name = "add-planreview"
        self._commit_plan_only_change(
            change_name,
            "# Add Plan Review\n\n"
            "### Task 1: First Task\n\n"
            "Do the first thing.\n\n"
            "### Task 1 Review\n\n"
            "Reviewer verdict: PASS. This text must never appear in any brief.\n\n"
            "### Task 2: Second Task\n\n"
            "Do the second thing.\n",
        )

        conn = self._conn()
        plan = self._plan(conn, change=change_name)
        self.assertIn(f"{change_name}/task-1", plan.new_tasks)
        self.assertIn(f"{change_name}/task-2", plan.new_tasks)
        # Exactly two tasks -- no phantom "task-1-review" (or similar)
        # task from the plan's own "### Task 1 Review" sub-heading. (Note:
        # can't substring-match "review" against the whole
        # "<change>/<task_key>" entry here -- the change name itself,
        # "add-planreview", contains that substring.)
        self.assertEqual(sorted(plan.new_tasks), [f"{change_name}/task-1", f"{change_name}/task-2"])

        self._run(conn, agent_runner=FakeAgentRunner(), change=change_name)
        rows = {
            r["task_key"]: r["brief_text"]
            for r in conn.execute(
                "SELECT t.task_key, t.brief_text FROM tasks t "
                "JOIN changes c ON c.change_id = t.change_id WHERE c.name = ?",
                (change_name,),
            ).fetchall()
        }
        self.assertEqual(set(rows), {"task-1", "task-2"})  # exactly two tasks, no phantom
        self.assertNotIn("Reviewer verdict", rows["task-1"])
        self.assertNotIn("Task 1 Review", rows["task-1"])
        self.assertIn("Do the first thing.", rows["task-1"])
        self.assertIn("Do the second thing.", rows["task-2"])


class TestNumericVersionOrdering(unittest.TestCase):
    """Review finding 4: version strings are compared numerically (design.md:
    "the row with the numerically greatest version"), never lexicographically
    -- "10" < "2" as strings but 10 > 2 as versions."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.db_path = Path(self._tmp.name) / "t.sqlite"

    def test_stale_versions_sorted_numerically_not_lexicographically(self):
        conn = db.connect(self.db_path)
        db.upsert(conn, "changes", {"name": "x", "ingested_at": "t0"}, ["name"])
        change_id = conn.execute("SELECT change_id FROM changes WHERE name = 'x'").fetchone()[0]
        db.upsert(
            conn, "tasks",
            {"change_id": change_id, "task_key": "task-1", "brief_text": "b"},
            ["change_id", "task_key"],
        )
        task_id = conn.execute("SELECT task_id FROM tasks").fetchone()[0]

        db.upsert(
            conn, "complexity",
            {"task_id": task_id, "rubric_version": "2", "input_sha256": "aaa"},
            ["task_id", "rubric_version"],
        )
        db.upsert(
            conn, "complexity",
            {"task_id": task_id, "rubric_version": "10", "input_sha256": "bbb"},
            ["task_id", "rubric_version"],
        )
        conn.commit()

        # Current version "11": both "2" and "10" are stale -- and must be
        # reported in numeric ascending order, not string order (which would
        # put "10" before "2").
        current_hash, stale = ingest._current_and_stale(
            conn, "complexity", "rubric_version", "task_id", task_id, "11"
        )
        self.assertIsNone(current_hash)
        self.assertEqual(stale, ["2", "10"])

        # Exact-match resolution also must not be fooled by digit count.
        current_hash2, stale2 = ingest._current_and_stale(
            conn, "complexity", "rubric_version", "task_id", task_id, "10"
        )
        self.assertEqual(current_hash2, "bbb")
        self.assertEqual(stale2, ["2"])


class TestTurnPersistence(IngestTestBase):
    """design.md D5 amendment (followup-4): a live `run()` must populate
    ``session_turns`` (and, for phase-labeled round-0 implementers,
    ``turn_phases``) for every session it writes, alongside the existing
    ``sessions`` row -- not just at some later ``backfill-turns`` step. The
    fixture sessions each hand-write exactly one assistant message, so each
    produces exactly one turn (extractors.py: claude turns split per
    assistant API message)."""

    def test_session_turns_populated_at_ingest_time(self):
        conn = self._conn()
        self._run(conn, agent_runner=FakeAgentRunner())

        impl_turns = conn.execute(
            "SELECT turn_idx, started_at, ended_at, output_tokens FROM session_turns "
            "WHERE session_id = ? ORDER BY turn_idx",
            ("claude:impl-1",),
        ).fetchall()
        self.assertEqual(len(impl_turns), 1)
        self.assertEqual(impl_turns[0]["turn_idx"], 1)
        self.assertEqual(impl_turns[0]["started_at"], "2026-07-10T00:05:00Z")
        self.assertEqual(impl_turns[0]["ended_at"], "2026-07-10T00:05:00Z")
        self.assertEqual(impl_turns[0]["output_tokens"], 50)

        review_turns = conn.execute(
            "SELECT turn_idx, started_at, output_tokens FROM session_turns "
            "WHERE session_id = ? ORDER BY turn_idx",
            ("claude:review-1",),
        ).fetchall()
        self.assertEqual(len(review_turns), 1)
        self.assertEqual(review_turns[0]["started_at"], "2026-07-10T01:10:00Z")

    def test_verdict_boundaries_json_persisted_on_sessions(self):
        # Neither fixture session's message matches the SPEC:/QUALITY:
        # verdict regex, so both sessions must persist an empty (not NULL)
        # boundaries list -- the documented no-verdict-data fallback.
        conn = self._conn()
        self._run(conn, agent_runner=FakeAgentRunner())
        for session_id in ("claude:impl-1", "claude:review-1"):
            vb_json = conn.execute(
                "SELECT verdict_boundaries_json FROM sessions WHERE session_id = ?", (session_id,)
            ).fetchone()[0]
            self.assertEqual(json.loads(vb_json), [])

    def test_turn_phases_matches_phase_tokens_aggregation(self):
        # Integrity invariant (design.md D2/D5 amendment): turn_phases,
        # aggregated by (phase, taxonomy_version) with session_turns'
        # output_tokens, must equal phase_tokens exactly for any session
        # with turn-level data.
        conn = self._conn()
        self._run(conn, agent_runner=FakeAgentRunner())  # cans "green" for turn 1

        phase_tokens_row = conn.execute(
            "SELECT phase, output_tokens, taxonomy_version FROM phase_tokens "
            "WHERE session_id = ?",
            ("claude:impl-1",),
        ).fetchone()
        self.assertIsNotNone(phase_tokens_row)

        turn_phase_rows = conn.execute(
            "SELECT tp.phase, st.output_tokens FROM turn_phases tp "
            "JOIN session_turns st ON st.session_id = tp.session_id AND st.turn_idx = tp.turn_idx "
            "WHERE tp.session_id = ? AND tp.taxonomy_version = ?",
            ("claude:impl-1", phase_tokens_row["taxonomy_version"]),
        ).fetchall()
        aggregated = sum(r["output_tokens"] for r in turn_phase_rows if r["phase"] == phase_tokens_row["phase"])
        self.assertEqual(aggregated, phase_tokens_row["output_tokens"])


def _write_one_turn_claude_transcript(path: Path, session_id: str):
    """A standalone (not through IngestTestBase's git/staging fixtures)
    one-turn claude transcript, for backfill-turns tests that need a real
    transcript file to re-extract but don't need the full ingest join."""
    _write_claude_session(
        path, session_id, "Implement something.", "Done.",
        ts_start="2026-07-01T00:00:00Z", ts_end="2026-07-01T00:05:00Z",
    )


class TestBackfillTurns(unittest.TestCase):
    """``ingest.py backfill-turns`` (design.md D5 amendment, followup-4):
    populates session_turns (and, for round-0 implementers, turn_phases)
    for already-ingested sessions that predate those tables, keyed off
    ``sessions.transcript_path`` directly -- independent of `run()`'s git/
    prompt join machinery, since these sessions were ingested before that
    column/tables existed at all."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.db_path = self.root / "t.sqlite"
        self.conn = db.connect(self.db_path)
        self.no_analysis_dir = self.root / "no-analysis-dir"  # deliberately absent

    def _insert_session(self, session_id, provider, role, transcript_path, review_round=0, n_turns=None):
        db.upsert(
            self.conn, "sessions",
            {
                "session_id": session_id, "task_id": None, "provider": provider,
                "role": role, "model": "claude-sonnet-5",
                "transcript_path": str(transcript_path) if transcript_path else None,
                "input_tokens": 0, "cached_tokens": 0, "output_tokens": 0,
                "review_round": review_round, "n_turns": n_turns,
            },
            ["session_id"],
        )
        self.conn.commit()

    def test_backfills_session_turns_from_existing_transcript(self):
        transcript = self.root / "reviewer.jsonl"
        shutil.copy(os.path.join(FIXTURES, "codex_persistent_reviewer.jsonl"), transcript)
        self._insert_session("codex:reviewer-sess-1", "codex", "reviewer", transcript, review_round=1)

        stats = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir)

        self.assertEqual(stats["session_turns_backfilled"], 1)
        self.assertEqual(stats["session_turns_unrecoverable"], 0)

        turns = self.conn.execute(
            "SELECT turn_idx, output_tokens FROM session_turns WHERE session_id = ? ORDER BY turn_idx",
            ("codex:reviewer-sess-1",),
        ).fetchall()
        self.assertEqual([r["output_tokens"] for r in turns], [200, 150])

        # Both verdict turns' ended_at persisted as review boundaries
        # (fixture has two SPEC:/QUALITY: verdicts -- see
        # test_extractors.TestExtractCodexPersistentReviewer).
        vb = json.loads(
            self.conn.execute(
                "SELECT verdict_boundaries_json FROM sessions WHERE session_id = ?",
                ("codex:reviewer-sess-1",),
            ).fetchone()[0]
        )
        self.assertEqual(vb, ["2026-07-05T00:00:08Z", "2026-07-05T01:00:05Z"])

    def test_missing_transcript_path_is_unrecoverable(self):
        self._insert_session("codex:gone-1", "codex", "implementer", None)
        stats = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir)
        self.assertEqual(stats["session_turns_unrecoverable"], 1)
        self.assertEqual(stats["session_turns_backfilled"], 0)
        count = self.conn.execute(
            "SELECT COUNT(*) FROM session_turns WHERE session_id = ?", ("codex:gone-1",)
        ).fetchone()[0]
        self.assertEqual(count, 0)

    def test_deleted_transcript_file_is_unrecoverable(self):
        transcript = self.root / "deleted.jsonl"
        transcript.write_text('{"not": "used"}\n')
        self._insert_session("codex:deleted-1", "codex", "implementer", transcript)
        transcript.unlink()
        stats = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir)
        self.assertEqual(stats["session_turns_unrecoverable"], 1)
        self.assertEqual(stats["session_turns_backfilled"], 0)

    def test_n_turns_refreshed_from_authoritative_reextraction(self):
        # followup-5 (design.md D5/D2 amendment): sessions.n_turns is
        # refreshed from the freshly re-extracted turn count during
        # backfill -- many rows are stale from the original migration and
        # were never corrected since.
        transcript = self.root / "reviewer.jsonl"
        shutil.copy(os.path.join(FIXTURES, "codex_persistent_reviewer.jsonl"), transcript)
        self._insert_session("codex:reviewer-sess-1", "codex", "reviewer", transcript, n_turns=999)

        ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir)

        n_turns = self.conn.execute(
            "SELECT n_turns FROM sessions WHERE session_id = ?", ("codex:reviewer-sess-1",)
        ).fetchone()[0]
        self.assertEqual(n_turns, 2)  # the fixture's real turn count, not the stale 999

    def test_regenerate_false_leaves_existing_session_turns_untouched(self):
        # Default behavior (regenerate=False) only fills gaps -- a session
        # that already has session_turns rows (even under an older,
        # leakier extractor) is left alone.
        transcript = self.root / "leaky.jsonl"
        shutil.copy(os.path.join(FIXTURES, "codex_leaky.jsonl"), transcript)
        self._insert_session("codex:leaky-1", "codex", "implementer", transcript, review_round=0)
        db.upsert(
            self.conn, "session_turns",
            {
                "session_id": "codex:leaky-1", "turn_idx": 1,
                "started_at": "t", "ended_at": "t", "output_tokens": 1,  # deliberately stale/wrong
            },
            ["session_id", "turn_idx"],
        )
        self.conn.commit()

        stats = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir, regenerate=False)
        self.assertEqual(stats["session_turns_backfilled"], 0)
        self.assertEqual(stats["session_turns_regenerated"], 0)
        row = self.conn.execute(
            "SELECT output_tokens FROM session_turns WHERE session_id = ? AND turn_idx = 1",
            ("codex:leaky-1",),
        ).fetchone()
        self.assertEqual(row["output_tokens"], 1)  # unchanged

    def test_regenerate_true_replaces_existing_session_turns(self):
        # followup-5: regenerate=True widens the candidate set to every
        # session with a transcript, replacing existing (possibly stale,
        # pre-fix) session_turns rows with freshly re-extracted ones.
        transcript = self.root / "leaky.jsonl"
        shutil.copy(os.path.join(FIXTURES, "codex_leaky.jsonl"), transcript)
        self._insert_session("codex:leaky-1", "codex", "implementer", transcript, review_round=0)
        db.upsert(
            self.conn, "session_turns",
            {
                "session_id": "codex:leaky-1", "turn_idx": 1,
                "started_at": "t", "ended_at": "t", "output_tokens": 1,  # stale/wrong
            },
            ["session_id", "turn_idx"],
        )
        self.conn.commit()

        stats = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir, regenerate=True)
        self.assertEqual(stats["session_turns_regenerated"], 1)
        self.assertEqual(stats["session_turns_backfilled"], 0)

        turns = self.conn.execute(
            "SELECT turn_idx, output_tokens FROM session_turns WHERE session_id = ? ORDER BY turn_idx",
            ("codex:leaky-1",),
        ).fetchall()
        # Matches TestExtractCodexLeaky's hand-traced values exactly.
        self.assertEqual([r["output_tokens"] for r in turns], [250, 50, 100])

    def test_regenerate_true_reconciles_previously_leaky_session(self):
        # End-to-end: a session whose session_turns were originally
        # written under the pre-followup-5 (leaky) extractor no longer
        # reconciles with sessions.output_tokens; --regenerate must fix
        # that by re-deriving session_turns under the fixed extractor.
        transcript = self.root / "leaky.jsonl"
        shutil.copy(os.path.join(FIXTURES, "codex_leaky.jsonl"), transcript)
        # sessions.output_tokens is the session-level total the (already
        # correct, untouched-by-this-fix) codex token_count cumulative
        # counter reports -- 400, matching the fixture's final checkpoint.
        self._insert_session("codex:leaky-2", "codex", "implementer", transcript, review_round=0)
        self.conn.execute(
            "UPDATE sessions SET output_tokens = 400 WHERE session_id = ?", ("codex:leaky-2",)
        )
        # Simulate the OLD leaky extraction's under-counted session_turns
        # (only the turns' own last_token_usage deltas, none of the
        # silent-checkpoint mass folded in): 50 + 50 + 40 = 140, not 400.
        for idx, tok in enumerate([50, 50, 40], start=1):
            db.upsert(
                self.conn, "session_turns",
                {"session_id": "codex:leaky-2", "turn_idx": idx, "started_at": "t", "ended_at": "t", "output_tokens": tok},
                ["session_id", "turn_idx"],
            )
        self.conn.commit()
        stale_sum = self.conn.execute(
            "SELECT SUM(output_tokens) FROM session_turns WHERE session_id = ?", ("codex:leaky-2",)
        ).fetchone()[0]
        self.assertNotEqual(stale_sum, 400)

        ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir, regenerate=True)

        fresh_sum = self.conn.execute(
            "SELECT SUM(output_tokens) FROM session_turns WHERE session_id = ?", ("codex:leaky-2",)
        ).fetchone()[0]
        self.assertEqual(fresh_sum, 400)  # reconciles exactly after regeneration

    def test_turn_phases_backfilled_from_analysis_dataset_for_round0_implementer(self):
        transcript = self.root / "impl.jsonl"
        _write_one_turn_claude_transcript(transcript, "old-impl-1")
        self._insert_session("claude:old-impl-1", "claude", "implementer", transcript, review_round=0)
        # Existing session-level phase_tokens row -- backfill reuses its
        # cache-key metadata (taxonomy_version/input_sha256/scored_by) so
        # the backfilled turn_phases row is directly comparable to it.
        db.upsert(
            self.conn, "phase_tokens",
            {
                "session_id": "claude:old-impl-1", "phase": "red", "output_tokens": 50,
                "turns": 1, "taxonomy_version": "1", "input_sha256": "x", "scored_by": "test",
            },
            ["session_id", "phase", "taxonomy_version"],
        )
        self.conn.commit()

        analysis_dir = self.root / "analysis"
        (analysis_dir / "phase_labels").mkdir(parents=True)
        (analysis_dir / "phase_labels" / "claude-old-impl-1.json").write_text(
            json.dumps({"labels": {"1": "red"}})
        )

        stats = ingest.backfill_turns(self.conn, analysis_dir=analysis_dir)
        self.assertEqual(stats["session_turns_backfilled"], 1)
        self.assertEqual(stats["turn_phases_backfilled"], 1)
        self.assertEqual(stats["turn_phases_unavailable"], 0)

        row = self.conn.execute(
            "SELECT phase, taxonomy_version, input_sha256 FROM turn_phases "
            "WHERE session_id = ? AND turn_idx = 1",
            ("claude:old-impl-1",),
        ).fetchone()
        self.assertEqual(row["phase"], "red")
        self.assertEqual(row["taxonomy_version"], "1")
        self.assertEqual(row["input_sha256"], "x")

    def test_turn_phases_unavailable_when_no_label_source_survives(self):
        transcript = self.root / "impl2.jsonl"
        _write_one_turn_claude_transcript(transcript, "old-impl-2")
        self._insert_session("claude:old-impl-2", "claude", "implementer", transcript, review_round=0)
        db.upsert(
            self.conn, "phase_tokens",
            {
                "session_id": "claude:old-impl-2", "phase": "red", "output_tokens": 50,
                "turns": 1, "taxonomy_version": "1", "input_sha256": "x", "scored_by": "test",
            },
            ["session_id", "phase", "taxonomy_version"],
        )
        self.conn.commit()

        stats = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir)
        self.assertEqual(stats["session_turns_backfilled"], 1)
        self.assertEqual(stats["turn_phases_unavailable"], 1)
        self.assertEqual(stats["turn_phases_backfilled"], 0)

    def test_non_round0_session_never_gets_turn_phases_backfill_attempt(self):
        # A fixer/reviewer/round>=1 session has no phase categories to
        # backfill turn_phases for at all -- must not even be attempted.
        transcript = self.root / "fixer.jsonl"
        _write_one_turn_claude_transcript(transcript, "old-fixer-1")
        self._insert_session("claude:old-fixer-1", "claude", "fixer", transcript, review_round=1)
        stats = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir)
        self.assertEqual(stats["session_turns_backfilled"], 1)
        self.assertEqual(stats["turn_phases_backfilled"], 0)
        self.assertEqual(stats["turn_phases_unavailable"], 0)

    def test_idempotent_second_backfill_is_a_no_op(self):
        transcript = self.root / "reviewer2.jsonl"
        shutil.copy(os.path.join(FIXTURES, "codex_persistent_reviewer.jsonl"), transcript)
        self._insert_session("codex:reviewer-sess-2", "codex", "reviewer", transcript)

        stats1 = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir)
        self.assertEqual(stats1["session_turns_backfilled"], 1)

        stats2 = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir)
        self.assertEqual(stats2["session_turns_backfilled"], 0)
        self.assertEqual(stats2["session_turns_unrecoverable"], 0)
        count = self.conn.execute(
            "SELECT COUNT(*) FROM session_turns WHERE session_id = ?", ("codex:reviewer-sess-2",)
        ).fetchone()[0]
        self.assertEqual(count, 2)  # not doubled

    def test_zero_turn_extraction_counted_separately_not_as_backfilled(self):
        # fix-round-1 review, finding 4: a transcript that parses cleanly
        # but yields zero turns (only a user message, no assistant
        # response at all) must not inflate session_turns_backfilled.
        transcript = self.root / "zero_turns.jsonl"
        transcript.write_text(json.dumps({
            "type": "user", "message": {"role": "user", "content": "hello"},
            "sessionId": "zero-turns-1", "timestamp": "2026-07-01T00:00:00Z",
        }) + "\n")
        # A stale n_turns from an earlier (pre-followup-5) migration/ingest
        # -- fix-round-1 review of followup-5, High finding: the original
        # code `continue`d on the zero-turns path BEFORE ever reaching the
        # n_turns UPDATE, so a zero-turn session kept whatever stale value
        # it had instead of being corrected to 0.
        self._insert_session("claude:zero-turns-1", "claude", "implementer", transcript, n_turns=1)

        stats = ingest.backfill_turns(self.conn, analysis_dir=self.no_analysis_dir)
        self.assertEqual(stats["session_turns_zero_turns"], 1)
        self.assertEqual(stats["session_turns_backfilled"], 0)
        count = self.conn.execute(
            "SELECT COUNT(*) FROM session_turns WHERE session_id = ?", ("claude:zero-turns-1",)
        ).fetchone()[0]
        self.assertEqual(count, 0)
        n_turns = self.conn.execute(
            "SELECT n_turns FROM sessions WHERE session_id = ?", ("claude:zero-turns-1",)
        ).fetchone()[0]
        self.assertEqual(n_turns, 0)  # refreshed to the true (zero) count, not left stale

    def test_verification_failure_writes_nothing_and_counts_separately(self):
        # fix-round-1 review, finding 1: turn_phases must be verified
        # against the stored phase_tokens BEFORE anything is written --
        # a mismatch writes zero rows for that session, counted distinctly
        # from "no label source at all".
        transcript = self.root / "impl3.jsonl"
        _write_one_turn_claude_transcript(transcript, "old-impl-3")
        self._insert_session("claude:old-impl-3", "claude", "implementer", transcript, review_round=0)
        # Stored phase_tokens (999) does not match what the label file
        # implies for the actual re-extracted turn (output_tokens=50) --
        # simulates a historical label source that no longer lines up
        # with this transcript.
        db.upsert(
            self.conn, "phase_tokens",
            {
                "session_id": "claude:old-impl-3", "phase": "red", "output_tokens": 999,
                "turns": 1, "taxonomy_version": "1", "input_sha256": "x", "scored_by": "test",
            },
            ["session_id", "phase", "taxonomy_version"],
        )
        self.conn.commit()

        analysis_dir = self.root / "analysis3"
        (analysis_dir / "phase_labels").mkdir(parents=True)
        (analysis_dir / "phase_labels" / "claude-old-impl-3.json").write_text(
            json.dumps({"labels": {"1": "red"}})
        )

        stats = ingest.backfill_turns(self.conn, analysis_dir=analysis_dir)
        self.assertEqual(stats["turn_phases_verification_failed"], 1)
        self.assertEqual(stats["turn_phases_backfilled"], 0)
        count = self.conn.execute(
            "SELECT COUNT(*) FROM turn_phases WHERE session_id = ?", ("claude:old-impl-3",)
        ).fetchone()[0]
        self.assertEqual(count, 0)

    def test_existing_turn_phases_gets_phase_tokens_recomputed_not_deleted(self):
        # followup-5 (design.md D5/D2 amendment): a session that ALREADY
        # has turn_phases rows has its LABELS trusted (the agentic
        # judgment, cache-keyed) -- if the stored phase_tokens weight is
        # stale relative to the CURRENT session_turns (e.g. after an
        # extractors.py delta fix changed per-turn token counts),
        # backfill_turns must RECOMPUTE phase_tokens to match those
        # existing labels, not delete them as if they were wrong. This
        # supersedes the old fix-round-1 self-heal-by-deletion behavior
        # for this exact shape of input -- deletion is now reserved for a
        # genuine LABEL mismatch (see the sibling test below).
        transcript = self.root / "impl4.jsonl"
        _write_one_turn_claude_transcript(transcript, "old-impl-4")
        self._insert_session("claude:old-impl-4", "claude", "implementer", transcript, review_round=0)
        db.upsert(
            self.conn, "phase_tokens",
            {
                "session_id": "claude:old-impl-4", "phase": "red", "output_tokens": 50,
                "turns": 1, "taxonomy_version": "1", "input_sha256": "x", "scored_by": "test",
            },
            ["session_id", "phase", "taxonomy_version"],
        )
        # session_turns already present (as if backfilled by a prior run
        # under an OLDER extractor whose deltas have since been
        # corrected) -- its current output_tokens (999) no longer matches
        # the STALE stored phase_tokens weight (50), even though the
        # LABEL itself (turn 1 -> "red") is correct and unchanged.
        db.upsert(
            self.conn, "session_turns",
            {
                "session_id": "claude:old-impl-4", "turn_idx": 1,
                "started_at": "t", "ended_at": "t", "output_tokens": 999,
            },
            ["session_id", "turn_idx"],
        )
        db.upsert(
            self.conn, "turn_phases",
            {
                "session_id": "claude:old-impl-4", "turn_idx": 1, "phase": "red",
                "taxonomy_version": "1", "input_sha256": "x", "scored_by": "prior-run",
            },
            ["session_id", "turn_idx", "taxonomy_version"],
        )
        self.conn.commit()
        self.assertNotEqual(costs.turn_phases_integrity_violations(self.conn, "claude:old-impl-4"), {})

        analysis_dir = self.root / "analysis4"
        (analysis_dir / "phase_labels").mkdir(parents=True)
        (analysis_dir / "phase_labels" / "claude-old-impl-4.json").write_text(
            json.dumps({"labels": {"1": "red"}})
        )

        stats = ingest.backfill_turns(self.conn, analysis_dir=analysis_dir)
        self.assertEqual(stats["session_turns_backfilled"], 0)  # already had session_turns
        self.assertEqual(stats["phase_tokens_recomputed"], 1)
        self.assertEqual(stats["turn_phases_verification_failed"], 0)
        self.assertEqual(stats["turn_phases_backfilled"], 1)

        row = self.conn.execute(
            "SELECT output_tokens FROM phase_tokens WHERE session_id = ? AND phase = 'red'",
            ("claude:old-impl-4",),
        ).fetchone()
        self.assertEqual(row["output_tokens"], 999)  # recomputed to match session_turns
        self.assertEqual(costs.turn_phases_integrity_violations(self.conn, "claude:old-impl-4"), {})

    def test_genuine_label_mismatch_self_heals_by_deletion(self):
        # A real label discrepancy (not just a stale token weight): the
        # existing turn_phases row says turn 1 is "red", but the
        # authoritative historical label source says it should be
        # "green". Recomputing phase_tokens from the EXISTING (arguably
        # wrong) label first, then finding pass 2's independently-derived
        # candidate disagrees, must still self-heal by removing the
        # session's turn_phases entirely -- not silently keep the
        # mismatched label.
        transcript = self.root / "impl5.jsonl"
        _write_one_turn_claude_transcript(transcript, "old-impl-5")
        self._insert_session("claude:old-impl-5", "claude", "implementer", transcript, review_round=0)
        db.upsert(
            self.conn, "phase_tokens",
            {
                "session_id": "claude:old-impl-5", "phase": "red", "output_tokens": 50,
                "turns": 1, "taxonomy_version": "1", "input_sha256": "x", "scored_by": "test",
            },
            ["session_id", "phase", "taxonomy_version"],
        )
        db.upsert(
            self.conn, "session_turns",
            {
                "session_id": "claude:old-impl-5", "turn_idx": 1,
                "started_at": "t", "ended_at": "t", "output_tokens": 50,
            },
            ["session_id", "turn_idx"],
        )
        db.upsert(
            self.conn, "turn_phases",
            {
                "session_id": "claude:old-impl-5", "turn_idx": 1, "phase": "red",
                "taxonomy_version": "1", "input_sha256": "x", "scored_by": "prior-run",
            },
            ["session_id", "turn_idx", "taxonomy_version"],
        )
        self.conn.commit()

        analysis_dir = self.root / "analysis5"
        (analysis_dir / "phase_labels").mkdir(parents=True)
        # The authoritative historical source disagrees: turn 1 should be
        # "green", not "red".
        (analysis_dir / "phase_labels" / "claude-old-impl-5.json").write_text(
            json.dumps({"labels": {"1": "green"}})
        )

        stats = ingest.backfill_turns(self.conn, analysis_dir=analysis_dir)
        self.assertEqual(stats["turn_phases_verification_failed"], 1)
        self.assertEqual(stats["turn_phases_backfilled"], 0)

        count = self.conn.execute(
            "SELECT COUNT(*) FROM turn_phases WHERE session_id = ?", ("claude:old-impl-5",)
        ).fetchone()[0]
        self.assertEqual(count, 0)  # self-healed: the mismatched label is gone


class TestVerifyTurnPhasesCli(unittest.TestCase):
    """fix-round-1 review, finding 1: ``ingest.py verify-turn-phases`` is
    the standalone check the review asked for -- so the turn_phases/
    phase_tokens invariant can be asserted against a real, committed
    database (e.g. a CI gate), not just implied by other tests passing."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.db_path = Path(self._tmp.name) / "t.sqlite"
        self.conn = db.connect(self.db_path)

    def _make_session_with_turn_data(self, session_id, output_tokens):
        db.upsert(self.conn, "changes", {"name": "c", "ingested_at": "t"}, ["name"])
        change_id = self.conn.execute("SELECT change_id FROM changes WHERE name = 'c'").fetchone()[0]
        db.upsert(
            self.conn, "tasks", {"change_id": change_id, "task_key": "task-1", "brief_text": "b"},
            ["change_id", "task_key"],
        )
        task_id = self.conn.execute("SELECT task_id FROM tasks").fetchone()[0]
        db.upsert(
            self.conn, "sessions",
            {
                "session_id": session_id, "task_id": task_id, "provider": "claude",
                "role": "implementer", "model": "claude-sonnet-5", "review_round": 0,
                "input_tokens": 0, "cached_tokens": 0, "output_tokens": 50,
            },
            ["session_id"],
        )
        db.upsert(
            self.conn, "session_turns",
            {"session_id": session_id, "turn_idx": 1, "started_at": "t", "ended_at": "t", "output_tokens": output_tokens},
            ["session_id", "turn_idx"],
        )
        db.upsert(
            self.conn, "phase_tokens",
            {
                "session_id": session_id, "phase": "red", "output_tokens": 50,
                "turns": 1, "taxonomy_version": "1", "input_sha256": "x",
            },
            ["session_id", "phase", "taxonomy_version"],
        )
        db.upsert(
            self.conn, "turn_phases",
            {
                "session_id": session_id, "turn_idx": 1, "phase": "red",
                "taxonomy_version": "1", "input_sha256": "x",
            },
            ["session_id", "turn_idx", "taxonomy_version"],
        )
        self.conn.commit()

    def test_exit_zero_on_a_clean_database(self):
        self._make_session_with_turn_data("claude:clean-1", output_tokens=50)  # matches stored red=50
        self.conn.close()

        with contextlib.redirect_stdout(io.StringIO()) as buf:
            rc = ingest.main(["verify-turn-phases", "--db", str(self.db_path)])
        self.assertEqual(rc, 0)
        report = json.loads(buf.getvalue())
        self.assertEqual(report["violation_count"], 0)
        self.assertEqual(report["violations"], [])

    def test_exit_one_on_a_database_with_a_violation(self):
        self._make_session_with_turn_data("claude:bad-1", output_tokens=999)  # mismatches stored red=50
        self.conn.close()

        with contextlib.redirect_stdout(io.StringIO()) as buf:
            rc = ingest.main(["verify-turn-phases", "--db", str(self.db_path)])
        self.assertEqual(rc, 1)
        report = json.loads(buf.getvalue())
        self.assertEqual(report["violation_count"], 1)
        self.assertEqual(report["violations"][0]["session_id"], "claude:bad-1")


class TestDryRunCliOutput(unittest.TestCase):
    """followup-6, defect 1: ``ingest.py --dry-run`` printed a RunReport
    shell (all-zero write counts, no plan content) instead of the actual
    WorkPlan the README promises ("prints the work plan"). Runs the real
    CLI entry point (``ingest.main``), not ``run()`` directly, since the
    bug was specifically in ``main()``'s stdout formatting -- `run()`
    itself already returned a WorkPlan-populated report before this fix."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.repo = _make_repo(self.root)
        self.claude_root = _make_sessions(self.root)
        self.db_path = self.root / "t.sqlite"

    def _dry_run_json(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = ingest.main([
                "--dry-run", "--repo", str(self.repo), "--db", str(self.db_path),
                "--change", CHANGE_NAME,
            ])
        self.assertEqual(rc, 0)
        return json.loads(buf.getvalue())

    def test_dry_run_prints_plan_not_a_runreport_shell(self):
        data = self._dry_run_json()
        self.assertTrue(data["dry_run"])
        self.assertIn("plan", data)
        self.assertIsInstance(data["plan"], dict)
        # The RunReport-shaped, always-zero-on-a-dry-run fields must be
        # ABSENT -- the pre-fix bug printed exactly these instead of a plan.
        for key in ("changes_written", "tasks_written", "sessions_written", "total_writes"):
            self.assertNotIn(key, data)

    def test_plan_fields_present_and_populated(self):
        # `ingest.main` has no flag to point session discovery at a fixture
        # directory (it always scans the real ~/.codex/sessions and
        # ~/.claude/projects, same limitation TestDryRunNoDbFile already
        # lives with) -- so this only asserts what's true regardless of
        # whatever real sessions happen to exist on the machine running the
        # test: the task-level gaps (independent of session discovery) and
        # the plan's overall shape. Session-dependent counts (grading,
        # phase-labels, new_sessions) are exercised precisely via `run()`
        # directly in `TestDryRun`, not through this CLI-level test.
        data = self._dry_run_json()
        plan = data["plan"]
        self.assertIn(f"{CHANGE_NAME}/{TASK_KEY}", plan["new_tasks"])
        self.assertEqual(len(plan["missing_complexity"]), 1)
        gap = plan["missing_complexity"][0]
        self.assertIn("entity_key", gap)
        self.assertIn("kind", gap)
        self.assertIn("version", gap)
        self.assertIn("reason", gap)
        for key in ("missing_grades", "missing_phase_labels", "new_sessions", "quarantined", "unlanded"):
            self.assertIn(key, plan)
        self.assertIn("new_sessions_count", plan)
        self.assertIn("quarantined_count", plan)


class TestDryRunNoDbFile(unittest.TestCase):
    """Review finding 5: `--dry-run` against a database path that doesn't
    exist yet must not create/touch it."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.repo = _make_repo(self.root)

    def test_dry_run_on_nonexistent_db_path_creates_no_file(self):
        missing_db = self.root / "does-not-exist" / "t.sqlite"
        self.assertFalse(missing_db.exists())

        # ingest.main prints a JSON run summary to stdout on this path (see
        # ingest.py) -- captured rather than left to leak into the test
        # runner's own output.
        with contextlib.redirect_stdout(io.StringIO()):
            rc = ingest.main(["--dry-run", "--repo", str(self.repo), "--db", str(missing_db)])

        self.assertEqual(rc, 0)
        self.assertFalse(missing_db.exists())
        self.assertFalse(missing_db.parent.exists())  # not even the directory


if __name__ == "__main__":
    unittest.main()
