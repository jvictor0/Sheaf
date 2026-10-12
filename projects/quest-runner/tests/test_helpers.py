"""Shared helpers for Quest Runner REST and dashboard tests."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

from quest_runner_service.api import create_app
from quest_runner_service.quest_lock import QuestLock
from quest_runner_service.quest_service import QuestService
from quest_runner_service.worktrees import quest_worktree_base_dir


def git_init(path: Path) -> None:
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


def ensure_project(root: Path, project: str) -> None:
    (root / "projects" / project).mkdir(parents=True)


def cleanup_worktrees(source_root: Path) -> None:
    base = quest_worktree_base_dir(source_root)
    if base.is_dir():
        shutil.rmtree(base, ignore_errors=True)


class TempRepo:
    def __init__(self, source_repo_root: Path) -> None:
        self._tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self._tmpdir.name)
        self.source_repo_root = source_repo_root
        git_init(self.root)

    def cleanup(self) -> None:
        cleanup_worktrees(self.root)
        self._tmpdir.cleanup()


def make_app_client(
    repo_root: Path,
    source_repo_root: Path,
    *,
    lock: QuestLock | None = None,
    started_at: float = 1000.0,
):
    svc = QuestService(lock or QuestLock(), source_repo_root)
    app = create_app(
        quest_service=svc,
        source_repo_root=repo_root,
        started_at=started_at,
    )
    return app.test_client(), svc


def quest_dir_on_checkout(source_root: Path, create_out: dict) -> Path:
    from quest_runner_service.dashboard_data import resolve_quest_dirs

    _checkout, qdir = resolve_quest_dirs(
        source_root,
        create_out["project"],
        create_out["quest_type"],
        create_out["quest_number"],
    )
    return qdir


def commit_v2_quest_step(
    repo: Path,
    quest_dir: Path,
    *,
    global_step: int,
    role: str | None = None,
    allow_empty: bool = True,
) -> str:
    """Create a v2 quest-step commit on repo and return its SHA."""
    from quest_runner_service.quest_types import RecursiveSnapshot, StepCommitMetadata
    from quest_runner_service.state_machine.commit_metadata import (
        render_step_commit_message,
    )
    from quest_runner_service.worktrees import run_git

    qrel = quest_dir.resolve().relative_to(repo.resolve()).as_posix()
    child = None
    if role is not None:
        child = RecursiveSnapshot(
            machine_path=f"{qrel}/slices/0000_test",
            machine_name="slice",
            node_name="SliceImplementingNode",
            state_before="Implementing",
            state_after="Implementing",
            tags={},
            child=None,
            role=role,
            thread_name=f"thread_{role}",
        )
    snap = RecursiveSnapshot(
        machine_path=qrel,
        machine_name="quest",
        node_name="ExecuteActiveSliceNode",
        state_before="ExecuteSlice",
        state_after="ExecuteSlice",
        tags={},
        child=child,
    )
    message = render_step_commit_message(
        StepCommitMetadata(global_step=global_step, snapshot=snap)
    )
    run_git(repo, "add", "-A")
    args = ["commit", "-m", message]
    if allow_empty:
        args.insert(1, "--allow-empty")
    run_git(repo, *args)
    return run_git(repo, "rev-parse", "HEAD").stdout.strip()
