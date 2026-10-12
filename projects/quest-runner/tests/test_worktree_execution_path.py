"""Tests for quest execution via deterministic quest worktrees."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

from quest_runner_service import quest_fs
from quest_runner_service.quest_lock import QuestLock
from quest_runner_service.quest_runner import (
    current_project_rel_for_quest,
    docs_updated_for_quest,
    expand_modify_allow_patterns,
    expand_modify_path_patterns,
    path_is_legal_under_profile,
    run_quest,
)
from quest_runner_service.quest_service import (
    InvalidQuestInput,
    MissingQuestWorktree,
    QuestNotFound,
    QuestService,
    _build_run_lock_key,
)
from quest_runner_service.quest_types import (
    ExecutionProfile,
    HarnessKind,
    QuestMeta,
    QuestFileState,
    RecursiveSnapshot,
    StepCommitMetadata,
)
from quest_runner_service.state_machine.commit_metadata import (
    parse_step_commit_message,
    render_step_commit_message,
    validate_parsed_step_commit,
)
from quest_runner_service.worktrees import quest_worktree_path, remove_partial_worktree


def _git_init(path: Path) -> None:
    subprocess.run(["git", "init"], cwd=str(path), check=True, capture_output=True)
    subprocess.run(
        ["git", "config", "user.email", "test@example.com"],
        cwd=str(path),
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["git", "config", "user.name", "Test"],
        cwd=str(path),
        check=True,
        capture_output=True,
    )
    (path / ".gitignore").write_text(
        (Path(__file__).resolve().parents[3] / ".gitignore").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    (path / ".gitkeep").write_text("", encoding="utf-8")
    subprocess.run(["git", "add", "-A"], cwd=str(path), check=True, capture_output=True)
    subprocess.run(
        ["git", "commit", "-m", "init"],
        cwd=str(path),
        check=True,
        capture_output=True,
    )


def _ensure_project(root: Path, project: str) -> None:
    (root / "projects" / project).mkdir(parents=True)


def _cleanup_worktrees(source_root: Path) -> None:
    base = source_root.resolve().parent / ".quest-worktrees"
    if base.is_dir():
        shutil.rmtree(base, ignore_errors=True)


class PrepareRunTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[1]
        self._active_roots: list[Path] = []

    def tearDown(self) -> None:
        for root in self._active_roots:
            _cleanup_worktrees(root)

    def _track(self, root: Path) -> Path:
        self._active_roots.append(root)
        return root

    def _service(self) -> QuestService:
        return QuestService(QuestLock(), self.repo_root)

    def test_requires_project(self) -> None:
        svc = self._service()
        with tempfile.TemporaryDirectory() as tmp:
            root = self._track(Path(tmp))
            _git_init(root)
            _ensure_project(root, "example")
            with self.assertRaises(TypeError):
                svc.run_quest(str(root), "main", 0)  # type: ignore[misc]

    def test_legacy_top_level_quest_not_found(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = self._track(Path(tmp))
            _git_init(root)
            _ensure_project(root, "example")
            legacy = root / "quests" / "main" / "0000_legacy"
            legacy.mkdir(parents=True)
            quest_fs.write_quest_meta(
                legacy,
                QuestMeta(
                    project="example",
                    quest_type="main",
                    quest_number=0,
                    quest_slug="legacy",
                    quest_name="Legacy",
                    created_at="2026-01-01T00:00:00Z",
                ),
            )
            svc = self._service()
            with self.assertRaises(QuestNotFound):
                svc.run_quest(str(root), "example", "main", 0)

    def test_missing_worktree_refuses_without_runner(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = self._track(Path(tmp))
            _git_init(root)
            _ensure_project(root, "example")
            svc = self._service()
            created = svc.create_quest(
                repo_path=str(root),
                project="example",
                quest_type="main",
                name="No Worktree",
            )
            meta = quest_fs.read_quest_meta(Path(created["quest_dir"]))
            remove_partial_worktree(root, meta)
            with patch("quest_runner_service.quest_runner.run_quest") as mock_run:
                with self.assertRaises(MissingQuestWorktree) as ctx:
                    svc.run_quest(str(root), "example", "main", 0, max_steps=1)
                mock_run.assert_not_called()
                self.assertEqual(ctx.exception.project, "example")
                self.assertEqual(
                    ctx.exception.worktree_path,
                    quest_worktree_path(root, meta),
                )

    @patch("quest_runner_service.quest_runner.run_quest")
    def test_run_uses_worktree_repo_and_quest_dir(self, mock_run: MagicMock) -> None:
        mock_run.return_value = {"status": "pre_planning", "steps_executed": 0}
        with tempfile.TemporaryDirectory() as tmp:
            root = self._track(Path(tmp))
            _git_init(root)
            _ensure_project(root, "example")
            svc = self._service()
            created = svc.create_quest(
                repo_path=str(root),
                project="example",
                quest_type="main",
                name="Run Me",
            )
            meta = quest_fs.read_quest_meta(Path(created["quest_dir"]))
            worktree_path = quest_worktree_path(root, meta)
            worktree_qdir = (
                worktree_path
                / "projects"
                / "example"
                / "quests"
                / "main"
                / "0000_run_me"
            )
            svc.run_quest(str(root), "example", "main", 0, max_steps=1)
            mock_run.assert_called_once()
            kwargs = mock_run.call_args.kwargs
            self.assertEqual(kwargs["repo_path"], worktree_path.resolve())
            self.assertEqual(kwargs["quest_dir"], worktree_qdir.resolve())

    @patch("quest_runner_service.quest_runner.run_quest")
    def test_source_dirty_worktree_clean_still_runs(self, mock_run: MagicMock) -> None:
        mock_run.return_value = {"status": "pre_planning", "steps_executed": 0}
        with tempfile.TemporaryDirectory() as tmp:
            root = self._track(Path(tmp))
            _git_init(root)
            _ensure_project(root, "example")
            svc = self._service()
            svc.create_quest(
                repo_path=str(root),
                project="example",
                quest_type="main",
                name="Dirty Source",
            )
            (root / "source_dirty.txt").write_text("x", encoding="utf-8")
            svc.run_quest(str(root), "example", "main", 0, max_steps=1)
            mock_run.assert_called_once()

    def test_lock_key_includes_worktree_and_quest_identity(self) -> None:
        wt = Path("/data/.quest-worktrees/example_main_0000_x")
        key = _build_run_lock_key(wt, "example", "main", 0)
        self.assertIn(str(wt.resolve()), key)
        self.assertIn("example/main/0000", key)


class ProjectLocalPathRuleTests(unittest.TestCase):
    def test_current_project_placeholder_resolves_from_quest_path(self) -> None:
        meta = QuestMeta(
            project="example",
            quest_type="main",
            quest_number=0,
            quest_slug="x",
            quest_name="X",
            created_at="2026-03-29T00:00:00Z",
        )
        self.assertEqual(
            current_project_rel_for_quest(
                meta, "projects/example/quests/main/0000_x"
            ),
            "projects/example",
        )
        legacy = QuestMeta(
            project="",
            quest_type="main",
            quest_number=0,
            quest_slug="legacy",
            quest_name="Legacy",
            created_at="2026-03-29T00:00:00Z",
        )
        self.assertEqual(
            current_project_rel_for_quest(legacy, "quests/main/0000_legacy"),
            "",
        )

    def test_expand_current_quest_and_slice_project_local(self) -> None:
        prof = ExecutionProfile(
            harness=HarnessKind.Cursor,
            model="m",
            reasoning_effort=None,
            idle_timeout_seconds=1,
            modify_allow=[
                "$currentQuest/human_intervention_request.md",
                "$currentSlice/notes/x.md",
                "$currentProject/docs/**",
            ],
            modify_block=["**"],
            config_version=2,
        )
        qrel = "projects/example/quests/main/0000_x"
        srel = f"{qrel}/slices/0000_sl"
        exp = expand_modify_allow_patterns(
            prof.modify_allow, qrel, srel, "projects/example"
        )
        block_exp = expand_modify_path_patterns(
            prof.modify_block, qrel, srel, "projects/example"
        )
        self.assertEqual(
            exp,
            [
                f"{qrel}/human_intervention_request.md",
                f"{srel}/notes/x.md",
                "projects/example/docs/**",
            ],
        )
        self.assertTrue(
            path_is_legal_under_profile(
                f"{qrel}/human_intervention_request.md", prof, exp, block_exp
            )
        )
        self.assertTrue(
            path_is_legal_under_profile(f"{srel}/notes/x.md", prof, exp, block_exp)
        )
        self.assertTrue(
            path_is_legal_under_profile(
                "projects/example/docs/index.md", prof, exp, block_exp
            )
        )
        self.assertFalse(
            path_is_legal_under_profile("docs/index.md", prof, exp, block_exp)
        )

    def test_current_project_quest_block_is_expanded(self) -> None:
        prof = ExecutionProfile(
            harness=HarnessKind.Cursor,
            model="m",
            reasoning_effort=None,
            idle_timeout_seconds=1,
            modify_allow=[
                "$currentQuest/human_intervention_request.md",
                "$currentSlice/implementation_done.md",
            ],
            modify_block=["$currentProject/quests/**"],
            config_version=2,
        )
        qrel = "projects/example/quests/main/0000_x"
        srel = f"{qrel}/slices/0000_sl"
        exp = expand_modify_allow_patterns(
            prof.modify_allow, qrel, srel, "projects/example"
        )
        block_exp = expand_modify_path_patterns(
            prof.modify_block, qrel, srel, "projects/example"
        )
        self.assertEqual(block_exp, ["projects/example/quests/**"])
        self.assertTrue(
            path_is_legal_under_profile(
                f"{qrel}/human_intervention_request.md", prof, exp, block_exp
            )
        )
        self.assertFalse(
            path_is_legal_under_profile(
                "projects/example/quests/main/0001_y/state.md", prof, exp, block_exp
            )
        )
        self.assertTrue(
            path_is_legal_under_profile(
                "projects/example/src/app.py", prof, exp, block_exp
            )
        )

    def test_documenter_completion_checks_project_docs_only(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            _git_init(repo)
            base = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=str(repo),
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()

            (repo / "docs").mkdir()
            (repo / "docs" / "index.md").write_text("global\n", encoding="utf-8")
            self.assertFalse(
                docs_updated_for_quest(repo, base, "projects/example/docs")
            )

            project_docs = repo / "projects" / "example" / "docs"
            project_docs.mkdir(parents=True)
            (project_docs / "index.md").write_text("project\n", encoding="utf-8")
            self.assertTrue(
                docs_updated_for_quest(repo, base, "projects/example/docs")
            )


class ProjectLocalCommitMetadataTests(unittest.TestCase):
    def test_render_validate_project_local_machine_path(self) -> None:
        rel = "projects/example/quests/main/0004_migratequest_runner"
        snap = RecursiveSnapshot(
            machine_path=rel,
            machine_name="quest",
            node_name="ExecuteActiveSliceNode",
            state_before="ExecuteSlice",
            state_after="ExecuteSlice",
            tags={
                "active_slice": "0003_worktree_execution_path",
                "quest_type": "main",
                "quest_number": "4",
                "quest_slug": "migratequest_runner",
            },
            child=RecursiveSnapshot(
                machine_path=f"{rel}/slices/0003_worktree_execution_path",
                machine_name="slice",
                node_name="SliceImplementingNode",
                state_before="Implementing",
                state_after="Implementing",
                tags={},
                child=None,
                role="implementer",
                thread_name="example_main_0004_migratequest_runner_quest_0004_slice_0003_implementer",
            ),
        )
        meta = StepCommitMetadata(global_step=13, snapshot=snap)
        msg = render_step_commit_message(meta)
        parsed = parse_step_commit_message(msg)
        self.assertIsNotNone(parsed)
        assert parsed is not None
        validate_parsed_step_commit(parsed)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            machine = root / rel
            machine.mkdir(parents=True)
            (machine / "slices" / "0003_worktree_execution_path").mkdir(
                parents=True
            )
            validate_parsed_step_commit(parsed, repo_root=root)


class WorktreeDirtyWorkspaceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[1]
        self._active_roots: list[Path] = []

    def tearDown(self) -> None:
        for root in self._active_roots:
            _cleanup_worktrees(root)

    def _track(self, root: Path) -> Path:
        self._active_roots.append(root)
        return root

    @patch("quest_runner_service.state_machine.workflow_harness_callback.create_harness")
    def test_dirty_worktree_blocks_harness(self, mock_create: MagicMock) -> None:
        class FH:
            kind = HarnessKind.ClaudeCode

            def validate(self) -> None:
                pass

            def create_thread(self, **kwargs: object) -> str:
                return "fake-thread-id"

            def send_message(self, **kwargs: object) -> object:
                raise AssertionError("harness should not be invoked")

        mock_create.return_value = FH()
        with tempfile.TemporaryDirectory() as tmp:
            root = self._track(Path(tmp))
            _git_init(root)
            _ensure_project(root, "example")
            svc = QuestService(QuestLock(), self.repo_root)
            created = svc.create_quest(
                repo_path=str(root),
                project="example",
                quest_type="main",
                name="Dirty WT",
            )
            meta = quest_fs.read_quest_meta(Path(created["quest_dir"]))
            worktree = quest_worktree_path(root, meta)
            wt_qdir = (
                worktree
                / "projects"
                / "example"
                / "quests"
                / "main"
                / "0000_dirty_wt"
            )
            quest_fs.write_quest_file_state(
                wt_qdir,
                QuestFileState(
                    state="PhysicalPlanning",
                    current_slice=None,
                    updated_at="2026-01-01T00:00:00Z",
                ),
            )
            sl = wt_qdir / "slices" / "0000_a"
            pp = sl / "physicalplan"
            pp.mkdir(parents=True, exist_ok=True)
            (pp / "plan.md").write_text("# plan\n", encoding="utf-8")
            (sl / "state.md").write_text(
                "# Slice State\n\nstate: NotStarted\nupdated_at: t\n",
                encoding="utf-8",
            )
            (sl / "state_history.md").write_text(
                "# State Transition History\n", encoding="utf-8"
            )
            (sl / "polishing_issues.md").write_text("# Issues\n", encoding="utf-8")
            subprocess.run(
                ["git", "-C", str(worktree), "add", "-A"],
                check=True,
                capture_output=True,
            )
            subprocess.run(
                ["git", "-C", str(worktree), "commit", "-m", "slice scaffold"],
                check=True,
                capture_output=True,
            )
            (worktree / "fresh_untracked.md").write_text("dirty", encoding="utf-8")
            out = run_quest(
                repo_path=worktree,
                quest_dir=wt_qdir,
                conductor_repo_path=self.repo_root,
                max_steps=1,
            )
            self.assertEqual(out["status"], "dirty_workspace")


if __name__ == "__main__":
    unittest.main()
