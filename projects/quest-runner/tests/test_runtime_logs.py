"""Git staging and replay use the repository's actual runtime-log policy."""

from pathlib import Path
import unittest

from quest_runner_service.chat_event_bus import ChatEventBus
from quest_runner_service.dashboard_chat import ChatStreamSession
from quest_runner_service.dashboard_slice import resolve_agent_log_path
from quest_runner_service.state_machine.adapters import SubprocessGitOps
from quest_runner_service.worktrees import run_git

from .test_dashboard_chat import FakeWebSocket
from .test_helpers import TempRepo


class RuntimeLogPolicyTests(unittest.TestCase):
    def test_step_staging_preserves_source_and_local_replay(self) -> None:
        project_root = Path(__file__).resolve().parents[1]
        temp = TempRepo(project_root)
        self.addCleanup(temp.cleanup)
        repo = temp.root
        quest = "projects/example/quests/main/0000_example"
        fixture = (Path(__file__).parent / "fixtures/quest_logs/codex.jsonl").read_text()
        runtime_paths = [
            "logs/quest-runner/service.log",
            "quests/side/0000_example/logs/step_0001_runner.md",
            "quests/main/0000_example/experiments/0000/logs/step_0001_runner.jsonl",
            f"{quest}/logs/step_0001_implementer.jsonl",
            f"{quest}/logs/commit_metadata_validation_example.md",
            f"{quest}/experiments/0000/logs/step_0001_implementer.jsonl",
        ]
        source_paths = [
            f"{quest}/state.md",
            f"{quest}/experiments/0000/experiment.json",
            f"{quest}/experiments/0000/issues/quest/example_issues.md",
            "projects/example/src/logs/index.py",
            "projects/example/tests/fixtures/quests/main/0000_example/logs/step_0001_run.jsonl",
            "projects/agents/vendor/example/node_modules/pkg/src/logs/index.js",
        ]
        for relative in runtime_paths + source_paths:
            path = repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(fixture if path.suffix == ".jsonl" else "example\n")

        git_ops = SubprocessGitOps()
        git_ops.StageAll(repo)
        staged = set(run_git(repo, "diff", "--cached", "--name-only").stdout.splitlines())
        self.assertEqual(staged, set(source_paths))
        git_ops.Commit(repo, "Example step")
        self.assertTrue(git_ops.IsClean(repo))
        for relative in runtime_paths:
            self.assertTrue((repo / relative).is_file())
            self.assertEqual(run_git(repo, "check-ignore", "--", relative).returncode, 0)

        step, log_path = resolve_agent_log_path(quest_dir=repo / quest, step=1)
        self.assertEqual(step, 1)
        websocket = FakeWebSocket()
        ChatStreamSession(log_path, websocket, ChatEventBus()).run()
        self.assertEqual(websocket.messages[-1], {"type": "caught_up"})
        events = [event for message in websocket.messages if message["type"] == "events" for event in message["events"]]
        self.assertIn("RUN_FINISHED", {event["type"] for event in events})


if __name__ == "__main__":
    unittest.main()
