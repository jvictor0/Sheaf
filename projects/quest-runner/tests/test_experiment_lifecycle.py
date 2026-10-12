"""End-to-end experiment create, run, dashboard, and land integration test."""

from __future__ import annotations

import shutil
import subprocess
import unittest
from pathlib import Path

from quest_runner_service import quest_fs
from quest_runner_service.experiments import (
    experiment_branch_name,
    experiment_id,
    experiment_worktree_path,
    experiments_root,
    read_experiment_meta,
)
from quest_runner_service.quest_types import (
    QuestFileState,
    SliceFileState,
)
from quest_runner_service.worktrees import branch_exists, remove_partial_worktree, run_git

from .test_experiments import _add_bare_remote, _default_workflow_dir
from .test_helpers import (
    TempRepo,
    commit_v2_quest_step,
    ensure_project,
    make_app_client,
)


def _git_commit_all(repo: Path, message: str) -> str:
    run_git(repo, "add", "-A")
    run_git(repo, "commit", "-m", message)
    return run_git(repo, "rev-parse", "HEAD").stdout.strip()


class ExperimentLifecycleIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[1]
        self.temp = TempRepo(self.repo_root)
        self.addCleanup(self.temp.cleanup)
        bare = _add_bare_remote(self.temp.root)
        self.addCleanup(lambda: shutil.rmtree(bare, ignore_errors=True))

    def _prepare_completed_quest_with_v2_steps(
        self,
    ) -> tuple[object, object, dict, Path]:
        ensure_project(self.temp.root, "example")
        client, svc = make_app_client(self.temp.root, self.repo_root)
        out = svc.create_quest(
            str(self.temp.root), "example", "main", "Lifecycle Quest"
        )
        source_qdir = quest_fs.find_quest_dir(
            self.temp.root, "example", "main", out["quest_number"]
        )
        assert source_qdir is not None

        for step in range(1, 6):
            role = "implementer" if step == 5 else None
            commit_v2_quest_step(
                self.temp.root,
                source_qdir,
                global_step=step,
                role=role,
            )
        (source_qdir / "logs").mkdir(exist_ok=True)
        (source_qdir / "logs" / "step_0005_implementer.jsonl").write_text(
            "{}\n", encoding="utf-8"
        )

        quest_fs.write_quest_file_state(
            source_qdir,
            QuestFileState(
                state="Completed",
                current_slice=None,
                updated_at="2026-06-08T00:00:00Z",
                global_step=5,
            ),
        )
        _git_commit_all(self.temp.root, "quest completed")
        remove_partial_worktree(self.temp.root, quest_fs.read_quest_meta(source_qdir))
        return client, svc, out, source_qdir

    def _prepare_experiment_slice_for_stop(self, wt_qdir: Path, wt_path: Path) -> None:
        sl = wt_qdir / "slices" / "0000_done"
        sl.mkdir(parents=True)
        (sl / "implementation_accepted.md").write_text("# Accepted\n", encoding="utf-8")
        (sl / "polishing_issues.md").write_text("# Issues\n", encoding="utf-8")
        quest_fs.write_slice_file_state(
            sl,
            SliceFileState(
                state="PolishingReview",
                updated_at="2026-01-01T00:00:00Z",
            ),
        )
        quest_fs.write_quest_file_state(
            wt_qdir,
            QuestFileState(
                state="ExecuteSlice",
                current_slice=0,
                updated_at="2026-01-01T00:00:00Z",
                active_slice="0000_done",
                global_step=5,
            ),
        )
        _git_commit_all(wt_path, "slice ready to complete")

    def test_full_experiment_lifecycle(self) -> None:
        client, svc, out, source_qdir = self._prepare_completed_quest_with_v2_steps()

        create = svc.create_experiment(
            repo_path=str(self.temp.root),
            project="example",
            quest_type="main",
            quest_number=out["quest_number"],
            start_step=5,
            stop_node="slice_completed",
            notes="lifecycle integration experiment",
            workflow_path=str(_default_workflow_dir(self.repo_root)),
            public_base_url="http://test.local/",
        )
        exp_id = experiment_id("example", "main", out["quest_number"], 0)
        self.assertEqual(create["experiment_id"], exp_id)
        self.assertEqual(create["experiment_number"], 0)
        self.assertEqual(
            create["branch_name"],
            experiment_branch_name("example", "main", out["quest_number"], 0),
        )

        exp_dir = experiments_root(source_qdir) / "0000"
        meta = read_experiment_meta(exp_dir)
        self.assertEqual(meta.status, "open")
        self.assertEqual(meta.start_step.global_step, 5)
        step_parent = run_git(
            self.temp.root, "rev-parse", f"{meta.start_step.step_commit}^"
        ).stdout.strip()
        self.assertEqual(meta.start_step.base_commit, step_parent)

        wt_path = Path(create["worktree_path"])
        self.assertTrue(wt_path.is_dir())
        head = run_git(wt_path, "rev-parse", "HEAD").stdout.strip()
        self.assertEqual(head, meta.start_step.base_commit)

        wt_qdir = quest_fs.find_quest_dir(
            wt_path, "example", "main", out["quest_number"]
        )
        assert wt_qdir is not None
        self.assertTrue((wt_qdir / "workflow" / "workflow.yaml").is_file())
        self.assertFalse((source_qdir / "state_execution_config.yaml").is_file())
        self.assertTrue((source_qdir / "workflow" / "workflow.yaml").is_file())

        self._prepare_experiment_slice_for_stop(wt_qdir, wt_path)
        (wt_qdir / "logs").mkdir(exist_ok=True)
        (wt_qdir / "logs" / "step_0006_implementer.jsonl").write_text(
            '{"event":"test"}\n', encoding="utf-8"
        )
        (wt_qdir / "physicalplan_issues.md").write_text("# Issues\n", encoding="utf-8")

        advance = client.post(
            "/advance_quest",
            json={
                "project": "example",
                "quest_type": "main",
                "quest_number": out["quest_number"],
                "experiment_id": exp_id,
            },
        )
        self.assertEqual(advance.status_code, 200)
        advance_body = advance.get_json()
        self.assertEqual(advance_body["status"], "experiment_complete")
        self.assertEqual(advance_body["workflow_state"], "ExperimentComplete")

        wt_state = quest_fs.read_quest_file_state(wt_qdir)
        self.assertEqual(wt_state.state, "ExperimentComplete")
        source_meta = read_experiment_meta(exp_dir)
        self.assertEqual(source_meta.status, "experiment_complete")
        self.assertIsNotNone(source_meta.completed_at)

        snap = client.get(
            "/api/dashboard/project_snapshot",
            query_string={"project": "example"},
        ).get_json()
        self.assertEqual(len(snap["experiments"]), 1)
        exp_row = snap["experiments"][0]
        self.assertEqual(exp_row["experiment_id"], exp_id)
        self.assertTrue(exp_row["can_land"])
        self.assertEqual(exp_row["state"], "ExperimentComplete")

        overview = client.get(
            "/api/dashboard/quest_overview",
            query_string={
                "project": "example",
                "quest_type": "main",
                "quest_number": str(out["quest_number"]),
                "experiment_id": exp_id,
            },
        ).get_json()
        self.assertEqual(overview["checkout_kind"], "experiment")
        self.assertTrue(overview["experiment"]["can_land"])

        land = svc.land_experiment(
            str(self.temp.root),
            "example",
            "main",
            out["quest_number"],
            exp_id,
            public_base_url="http://test.local/",
        )
        self.assertEqual(land["status"], "landed")
        self.assertGreaterEqual(land["logs_copied"], 1)
        self.assertGreaterEqual(land["issues_copied"], 1)

        landed = read_experiment_meta(exp_dir)
        self.assertEqual(landed.status, "landed")
        self.assertIsNotNone(landed.landed_at)
        self.assertEqual(landed.remote_branch, meta.branch_name)
        archived_log = exp_dir / "logs" / "step_0006_implementer.jsonl"
        self.assertTrue(archived_log.is_file())
        self.assertEqual(archived_log.read_text(), '{"event":"test"}\n')
        relative_log = archived_log.relative_to(self.temp.root).as_posix()
        self.assertEqual(run_git(self.temp.root, "ls-files", "--", relative_log).stdout, "")
        self.assertEqual(run_git(self.temp.root, "check-ignore", "--", relative_log).returncode, 0)
        self.assertTrue(
            (exp_dir / "issues" / "quest" / "physicalplan_issues.md").is_file()
        )
        self.assertFalse(wt_path.exists())
        self.assertFalse(branch_exists(self.temp.root, meta.branch_name))

        bare = self.temp.root.parent / f"{self.temp.root.name}_remote.git"
        show = subprocess.run(
            [
                "git",
                "-C",
                str(bare),
                "show-ref",
                "--verify",
                f"refs/heads/{meta.branch_name}",
            ],
            capture_output=True,
        )
        self.assertEqual(show.returncode, 0)

        log = subprocess.run(
            ["git", "-C", str(self.temp.root), "log", "-1", "--format=%s"],
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertEqual(
            log.stdout.strip(),
            f"experiment-land: example/main/{out['quest_number']:04d}/0000",
        )

        archived_ov = client.get(
            "/api/dashboard/quest_overview",
            query_string={
                "project": "example",
                "quest_type": "main",
                "quest_number": str(out["quest_number"]),
            },
        ).get_json()
        self.assertEqual(len(archived_ov["archived_experiments"]), 1)
        self.assertEqual(
            archived_ov["archived_experiments"][0]["experiment_id"], exp_id
        )
        post_land_snap = client.get(
            "/api/dashboard/project_snapshot",
            query_string={"project": "example"},
        ).get_json()
        self.assertEqual(post_land_snap.get("experiments", []), [])
