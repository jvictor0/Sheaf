from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
MODULE_PATH = SCRIPT_DIR / "install.py"
OPENSPEC_VENDOR_TOML = (
    REPO_ROOT / "projects" / "agents" / "vendor" / "openspec" / "VENDOR.toml"
)

# Resolve install.py's sibling imports when another suite imports this module.
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

spec = importlib.util.spec_from_file_location("agents_install", MODULE_PATH)
assert spec is not None
install = importlib.util.module_from_spec(spec)
sys.modules["agents_install"] = install
assert spec.loader is not None
spec.loader.exec_module(install)


def openspec_vendor_version() -> str:
    return install.openspec_pin_field(
        install.read_openspec_vendor_pin(REPO_ROOT), "version"
    )


def hook_outputs(outputs: list[object], codex_home: Path) -> list[object]:
    codex_home = codex_home.resolve()
    wanted = {
        codex_home / "hooks" / "sheaf" / "session_start_after_compact.py",
    }
    return [output for output in outputs if output.path in wanted]


def copied_repo_fixture(tempdir: str) -> Path:
    repo_root = Path(tempdir) / "repo"
    agents_dest = repo_root / "projects" / "agents"
    shutil.copytree(
        REPO_ROOT / "projects" / "agents",
        agents_dest,
        ignore=shutil.ignore_patterns("__pycache__"),
    )
    return repo_root


def run_main(*args: str) -> int:
    with mock.patch.object(sys, "argv", ["install.py", *args]):
        return install.main()


class CodexHookOutputTests(unittest.TestCase):
    def shared_output(self, codex_home: Path) -> install.CodexHookSharedOutput:
        return install.build_codex_hook_shared_output(
            REPO_ROOT,
            home=codex_home.parent / "home",
            codex_home=codex_home,
        )

    def foreign_group(self, command: str = "python3 /opt/other/compact.py") -> dict[str, object]:
        return {
            "matcher": "^compact$",
            "hooks": [{"type": "command", "command": command}],
        }

    def external_group(self, mode: str) -> dict[str, object]:
        return {
            "hooks": [
                {
                    "type": "command",
                    "command": (
                        "python3 /opt/example/observe_hook.py "
                        f"--state-root /tmp/example {mode}"
                    ),
                }
            ]
        }

    def write_json(self, path: Path, payload: object) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    def read_json(self, path: Path) -> dict[str, object]:
        payload = json.loads(path.read_text(encoding="utf-8"))
        self.assertIsInstance(payload, dict)
        return payload

    def test_install_appends_agents_group_and_preserves_unrelated_order(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            before = self.foreign_group("python3 /opt/other/before.py")
            after = self.foreign_group("python3 /opt/other/after.py")
            self.write_json(
                output.config_path,
                {
                    "model": "gpt-5",
                    "hooks": {"SessionStart": [before, after]},
                },
            )
            outputs = install.build_global_outputs(
                REPO_ROOT,
                home=codex_home.parent / "home",
                codex_home=codex_home,
            )

            self.assertNotIn(output.config_path, [generic.path for generic in outputs])
            self.assertEqual(0, install.install_codex_hook_shared(output))

            merged = self.read_json(output.config_path)
            self.assertEqual("gpt-5", merged["model"])
            self.assertEqual(
                [before, after, output.desired_group],
                merged["hooks"]["SessionStart"],
            )
            self.assertTrue(output.script_output.path.is_file())

    def test_install_updates_canonical_agents_group_in_place(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            before = self.foreign_group("python3 /opt/other/before.py")
            after = self.foreign_group("python3 /opt/other/after.py")
            stale_owned = {
                "matcher": "^compact$",
                "hooks": [
                    {
                        "type": "command",
                        "command": output.desired_group["hooks"][0]["command"],
                        "statusMessage": "stale message",
                        "timeout": 99,
                    }
                ],
            }
            self.write_json(
                output.config_path,
                {"hooks": {"SessionStart": [before, stale_owned, after]}},
            )

            self.assertEqual(0, install.install_codex_hook_shared(output))

            merged = self.read_json(output.config_path)
            self.assertEqual(
                [before, output.desired_group, after],
                merged["hooks"]["SessionStart"],
            )

    def owned_entry(self, output: install.CodexHookSharedOutput) -> dict[str, object]:
        return copy.deepcopy(output.desired_group["hooks"][0])

    def mixed_group(self, output: install.CodexHookSharedOutput) -> dict[str, object]:
        # A user's own compact hook sharing the group that holds ours. Only the
        # single entry carrying our command is ours; the group is not.
        sibling = {"type": "command", "command": "python3 /opt/other/sibling.py"}
        stale_owned = {**self.owned_entry(output), "statusMessage": "stale"}
        return {
            "matcher": "^compact$",
            "timeout": 42,
            "hooks": [sibling, stale_owned],
        }

    def test_install_lifts_owned_entry_out_of_mixed_compact_group(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            before = self.foreign_group("python3 /opt/other/before.py")
            mixed = self.mixed_group(output)
            duplicate = {"matcher": "^compact$", "hooks": [self.owned_entry(output)]}
            unmatched = {"matcher": "^startup$", "hooks": [self.owned_entry(output)]}
            self.write_json(
                output.config_path,
                {"hooks": {"SessionStart": [before, mixed, duplicate, unmatched]}},
            )

            self.assertEqual(0, install.install_codex_hook_shared(output))

            merged = self.read_json(output.config_path)
            self.assertEqual(
                [
                    before,
                    output.desired_group,
                    {
                        "matcher": "^compact$",
                        "timeout": 42,
                        "hooks": [mixed["hooks"][0]],
                    },
                    unmatched,
                ],
                merged["hooks"]["SessionStart"],
            )
            self.assertEqual(0, install.check_codex_hook_shared(output))

    def test_check_rejects_an_owned_entry_beside_the_canonical_group(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            self.assertEqual(0, install.install_codex_hook_shared(output))
            config = self.read_json(output.config_path)
            config["hooks"]["SessionStart"] = [
                output.desired_group,
                self.mixed_group(output),
            ]
            self.write_json(output.config_path, config)

            self.assertEqual(1, install.check_codex_hook_shared(output))

    def test_clean_removes_owned_entries_from_mixed_compact_groups(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            before = self.foreign_group("python3 /opt/other/before.py")
            mixed = self.mixed_group(output)
            self.write_json(
                output.config_path,
                {
                    "hooks": {
                        "SessionStart": [before, mixed, output.desired_group],
                    }
                },
            )
            output.script_output.path.parent.mkdir(parents=True, exist_ok=True)
            output.script_output.path.write_text(
                output.script_output.content, encoding="utf-8"
            )

            self.assertEqual(0, install.clean_codex_hook_shared(output))

            cleaned = self.read_json(output.config_path)
            self.assertEqual(
                [
                    before,
                    {
                        "matcher": "^compact$",
                        "timeout": 42,
                        "hooks": [mixed["hooks"][0]],
                    },
                ],
                cleaned["hooks"]["SessionStart"],
            )
            self.assertFalse(output.script_output.path.exists())

    def test_install_migrates_legacy_whole_file_marker(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            legacy = {
                **output.desired_group,
                "hooks": [
                    {
                        **output.desired_group["hooks"][0],
                        "statusMessage": "legacy",
                    }
                ],
            }
            external = self.external_group("observe")
            self.write_json(
                output.config_path,
                {
                    "_sheaf_agents_managed": (
                        "sheaf-agents-managed: DO NOT EDIT; "
                        "source=projects/agents/global/codex/hooks/hooks.json"
                    ),
                    "hooks": {"SessionStart": [legacy], "PostToolUse": [external]},
                },
            )

            self.assertEqual(0, install.install_codex_hook_shared(output))

            merged = self.read_json(output.config_path)
            self.assertNotIn("_sheaf_agents_managed", merged)
            self.assertEqual([output.desired_group], merged["hooks"]["SessionStart"])
            self.assertEqual([external], merged["hooks"]["PostToolUse"])

    def test_install_preserves_canonical_external_groups(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            observe = self.external_group("observe")
            guard = self.external_group("guard")
            self.write_json(
                output.config_path,
                {"hooks": {"PostToolUse": [observe], "Stop": [guard]}},
            )

            self.assertEqual(0, install.install_codex_hook_shared(output))

            merged = self.read_json(output.config_path)
            self.assertEqual([observe], merged["hooks"]["PostToolUse"])
            self.assertEqual([guard], merged["hooks"]["Stop"])
            self.assertEqual([output.desired_group], merged["hooks"]["SessionStart"])

    def test_check_compares_only_script_and_agents_group(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            foreign = self.foreign_group()
            self.assertEqual(0, install.install_codex_hook_shared(output))
            config = self.read_json(output.config_path)
            config["hooks"]["PostToolUse"] = [self.external_group("observe")]
            self.write_json(output.config_path, config)

            self.assertEqual(0, install.check_codex_hook_shared(output))

            config["hooks"]["SessionStart"] = [foreign, output.desired_group]
            self.write_json(output.config_path, config)
            self.assertEqual(0, install.check_codex_hook_shared(output))

            broken = dict(output.desired_group)
            broken["matcher"] = ".*"
            config["hooks"]["SessionStart"] = [foreign, broken]
            self.write_json(output.config_path, config)
            self.assertEqual(1, install.check_codex_hook_shared(output))

            self.write_json(
                output.config_path,
                {"hooks": {"SessionStart": [output.desired_group]}},
            )
            output.script_output.path.write_text("stale\n", encoding="utf-8")
            self.assertEqual(1, install.check_codex_hook_shared(output))

    def test_clean_removes_only_agents_group_and_script(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            before = self.foreign_group("python3 /opt/other/before.py")
            after = self.foreign_group("python3 /opt/other/after.py")
            external = self.external_group("observe")
            self.write_json(
                output.config_path,
                {
                    "hooks": {
                        "SessionStart": [before, output.desired_group, after],
                        "PostToolUse": [external],
                    }
                },
            )
            output.script_output.path.parent.mkdir(parents=True, exist_ok=True)
            output.script_output.path.write_text(output.script_output.content, encoding="utf-8")

            self.assertEqual(0, install.clean_codex_hook_shared(output))

            cleaned = self.read_json(output.config_path)
            self.assertFalse(output.script_output.path.exists())
            self.assertEqual([before, after], cleaned["hooks"]["SessionStart"])
            self.assertEqual([external], cleaned["hooks"]["PostToolUse"])

    def test_clean_deletes_hooks_json_only_when_no_foreign_content_remains(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            self.write_json(
                output.config_path,
                {"hooks": {"SessionStart": [output.desired_group]}},
            )
            output.script_output.path.parent.mkdir(parents=True, exist_ok=True)
            output.script_output.path.write_text(output.script_output.content, encoding="utf-8")

            self.assertEqual(0, install.clean_codex_hook_shared(output))

            self.assertFalse(output.config_path.exists())
            self.assertFalse(output.script_output.path.exists())

    def test_malformed_shared_json_fails_install_check_and_clean_even_with_force(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            output.config_path.parent.mkdir(parents=True, exist_ok=True)
            malformed = '{"hooks": {"SessionStart": null}}\n'
            output.config_path.write_text(malformed, encoding="utf-8")
            args = (
                "--scope",
                "global",
                "--repo-root",
                str(REPO_ROOT),
                "--home",
                str(home),
                "--codex-home",
                str(codex_home),
                "--force",
            )

            for mode in ("install", "check", "clean"):
                with self.subTest(mode=mode):
                    with mock.patch.object(install, "install_openspec_cli", return_value=0):
                        with mock.patch.object(install, "check_openspec_cli", return_value=0):
                            with mock.patch.object(install, "clean_openspec_cli", return_value=0):
                                with contextlib.redirect_stderr(io.StringIO()):
                                    self.assertEqual(1, run_main(mode, *args))
                    self.assertEqual(malformed, output.config_path.read_text(encoding="utf-8"))

    def test_top_level_null_hooks_fails_install_check_and_clean_with_force(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)
            output.config_path.parent.mkdir(parents=True, exist_ok=True)
            malformed = '{\n  "hooks": null\n}\n'
            args = (
                "--scope",
                "global",
                "--repo-root",
                str(REPO_ROOT),
                "--home",
                str(home),
                "--codex-home",
                str(codex_home),
                "--force",
            )

            for mode in ("install", "check", "clean"):
                with self.subTest(mode=mode):
                    output.config_path.write_text(malformed, encoding="utf-8")
                    with mock.patch.object(install, "install_openspec_cli", return_value=0):
                        with mock.patch.object(install, "check_openspec_cli", return_value=0):
                            with mock.patch.object(install, "clean_openspec_cli", return_value=0):
                                with contextlib.redirect_stderr(io.StringIO()):
                                    self.assertEqual(1, run_main(mode, *args))
                    self.assertEqual(
                        malformed,
                        output.config_path.read_text(encoding="utf-8"),
                    )

    def test_shared_payload_has_foreign_content_does_not_mutate_payload(self) -> None:
        payload: dict[str, object] = {"hooks": {"SessionStart": []}}
        original = copy.deepcopy(payload)

        self.assertFalse(install.shared_payload_has_foreign_content(payload))

        self.assertEqual(original, payload)

    def test_install_and_clean_effective_changes_print_trust_notice(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            codex_home = (Path(tempdir) / "codex-home").resolve()
            output = self.shared_output(codex_home)

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(0, install.install_codex_hook_shared(output))
            self.assertIn(install.CODEX_HOOK_TRUST_NOTICE, stdout.getvalue())

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(0, install.install_codex_hook_shared(output))
            self.assertNotIn(install.CODEX_HOOK_TRUST_NOTICE, stdout.getvalue())

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(0, install.check_codex_hook_shared(output))
            self.assertNotIn(install.CODEX_HOOK_TRUST_NOTICE, stdout.getvalue())

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(0, install.clean_codex_hook_shared(output))
            self.assertIn(install.CODEX_HOOK_TRUST_NOTICE, stdout.getvalue())


class SkillScopeOutputTests(unittest.TestCase):

    def test_repo_outputs_include_only_sheaf_skills(self) -> None:
        outputs = install.build_repo_outputs(REPO_ROOT)
        paths = {output.path.relative_to(REPO_ROOT) for output in outputs}
        _global_source, global_skills, sheaf_skills = install.read_sources(REPO_ROOT)

        self.assertIn(Path("AGENTS.md"), paths)
        self.assertIn(Path("CLAUDE.md"), paths)
        for skill in sheaf_skills:
            for target in skill.targets:
                self.assertIn(
                    install.REPO_TARGET_DIRS[target] / skill.skill_id / "SKILL.md",
                    paths,
                )
        for skill in global_skills:
            for target_dir in install.REPO_TARGET_DIRS.values():
                self.assertNotIn(target_dir / skill.skill_id / "SKILL.md", paths)

    def test_repo_outputs_honor_synthetic_codex_only_sheaf_skill(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = copied_repo_fixture(tempdir)
            skill_root = repo_root / "projects" / "agents" / "sheaf" / "skills" / "codex-only"
            skill_root.mkdir(parents=True)
            (skill_root / "skill.yaml").write_text(
                "\n".join(
                    [
                        "id: codex-only",
                        "name: codex-only",
                        "description: Synthetic Codex-only Sheaf skill.",
                        "targets:",
                        "  - codex",
                        "",
                    ]
                ),
                encoding="utf-8",
            )
            (skill_root / "SKILL.md").write_text("# Codex Only\n", encoding="utf-8")

            outputs = install.build_repo_outputs(repo_root)
            paths = {output.path.relative_to(repo_root) for output in outputs}

            self.assertIn(Path(".codex/skills/codex-only/SKILL.md"), paths)
            self.assertNotIn(Path(".claude/skills/codex-only/SKILL.md"), paths)
            self.assertNotIn(Path(".cursor/skills/codex-only/SKILL.md"), paths)
            self.assertNotIn(Path(".pi/skills/codex-only/SKILL.md"), paths)

    def test_global_outputs_include_all_current_global_skills(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            outputs = install.build_global_outputs(
                REPO_ROOT,
                home=home,
                codex_home=codex_home,
            )
            paths = {output.path for output in outputs}
            _global_source, global_skills, _sheaf_skills = install.read_sources(REPO_ROOT)

            for skill in global_skills:
                if "claude" in skill.targets:
                    self.assertIn(
                        home / ".claude" / "skills" / skill.skill_id / "SKILL.md",
                        paths,
                    )
                if "cursor" in skill.targets:
                    self.assertIn(
                        home / ".cursor" / "skills" / skill.skill_id / "SKILL.md",
                        paths,
                    )
                if "pi" in skill.targets:
                    self.assertIn(
                        home / ".pi" / "skills" / skill.skill_id / "SKILL.md",
                        paths,
                    )
                if "codex" in skill.targets:
                    self.assertIn(
                        home / ".agents" / "skills" / skill.skill_id / "SKILL.md",
                        paths,
                    )
                    self.assertIn(
                        codex_home / "skills" / skill.skill_id / "SKILL.md",
                        paths,
                    )


class ObsoleteRepoOutputTests(unittest.TestCase):
    def test_repo_obsolete_outputs_cover_global_ids_and_retired_skills(self) -> None:
        outputs = install.build_obsolete_repo_outputs(REPO_ROOT)
        paths = {output.path.relative_to(REPO_ROOT) for output in outputs}
        _global_source, global_skills, _sheaf_skills = install.read_sources(REPO_ROOT)
        obsolete_ids = {skill.skill_id for skill in global_skills}
        obsolete_ids.update({"xagent-subagents", "openspec-superpowers-workflow"})

        self.assertEqual(
            {
                target_dir / skill_id / "SKILL.md"
                for skill_id in obsolete_ids
                for target_dir in install.REPO_TARGET_DIRS.values()
            },
            paths,
        )

    def test_repo_check_reports_managed_obsolete_output(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = copied_repo_fixture(tempdir)
            managed_obsolete = repo_root / ".claude" / "skills" / "about-me" / "SKILL.md"
            self.assertEqual(
                0,
                run_main("install", "--scope", "repo", "--repo-root", str(repo_root)),
            )
            managed_obsolete.parent.mkdir(parents=True)
            managed_obsolete.write_text(
                "<!-- sheaf-agents-managed: DO NOT EDIT; source=old -->\n",
                encoding="utf-8",
            )

            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = run_main(
                    "check",
                    "--scope",
                    "repo",
                    "--repo-root",
                    str(repo_root),
                )

            self.assertEqual(1, result)
            self.assertIn(
                f"obsolete managed {managed_obsolete.resolve()}",
                stderr.getvalue(),
            )
            self.assertTrue(managed_obsolete.exists())

    def test_repo_clean_removes_managed_and_preserves_unmanaged_obsolete_output(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = copied_repo_fixture(tempdir)
            managed_obsolete = repo_root / ".claude" / "skills" / "about-me" / "SKILL.md"
            unmanaged_obsolete = repo_root / ".cursor" / "skills" / "about-me" / "SKILL.md"
            managed_obsolete.parent.mkdir(parents=True)
            unmanaged_obsolete.parent.mkdir(parents=True)
            managed_obsolete.write_text(
                "<!-- sheaf-agents-managed: DO NOT EDIT; source=old -->\n",
                encoding="utf-8",
            )
            unmanaged_obsolete.write_text("personal skill\n", encoding="utf-8")

            self.assertEqual(
                0,
                run_main("clean", "--scope", "repo", "--repo-root", str(repo_root)),
            )
            self.assertFalse(managed_obsolete.exists())
            self.assertTrue(unmanaged_obsolete.exists())

    def test_repo_install_prunes_managed_and_preserves_unmanaged_obsolete_output(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = copied_repo_fixture(tempdir)
            managed_obsolete = repo_root / ".claude" / "skills" / "about-me" / "SKILL.md"
            unmanaged_obsolete = repo_root / ".cursor" / "skills" / "about-me" / "SKILL.md"
            managed_obsolete.parent.mkdir(parents=True)
            unmanaged_obsolete.parent.mkdir(parents=True)
            managed_obsolete.write_text(
                "<!-- sheaf-agents-managed: DO NOT EDIT; source=old -->\n",
                encoding="utf-8",
            )
            unmanaged_obsolete.write_text("personal skill\n", encoding="utf-8")

            self.assertEqual(
                0,
                run_main("install", "--scope", "repo", "--repo-root", str(repo_root)),
            )
            self.assertFalse(managed_obsolete.exists())
            self.assertTrue(unmanaged_obsolete.exists())

    def test_global_install_prunes_plugin_owned_managed_and_preserves_unmanaged_output(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = copied_repo_fixture(tempdir)
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            managed_obsolete = (
                home / ".agents" / "skills" / "xagent-subagents" / "SKILL.md"
            )
            unmanaged_obsolete = (
                codex_home / "skills" / "xagent-subagents" / "SKILL.md"
            )
            managed_obsolete.parent.mkdir(parents=True)
            unmanaged_obsolete.parent.mkdir(parents=True)
            managed_obsolete.write_text(
                "<!-- sheaf-agents-managed: DO NOT EDIT; source=old -->\n",
                encoding="utf-8",
            )
            unmanaged_obsolete.write_text("personal plugin skill\n", encoding="utf-8")

            self.assertEqual(
                0,
                run_main(
                    "install",
                    "--scope",
                    "global",
                    "--repo-root",
                    str(repo_root),
                    "--home",
                    str(home),
                    "--codex-home",
                    str(codex_home),
                ),
            )

            self.assertFalse(managed_obsolete.exists())
            self.assertEqual(
                "personal plugin skill\n",
                unmanaged_obsolete.read_text(encoding="utf-8"),
            )

    def test_global_check_and_clean_handle_plugin_owned_obsolete_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = copied_repo_fixture(tempdir)
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            common_args = (
                "--scope",
                "global",
                "--repo-root",
                str(repo_root),
                "--home",
                str(home),
                "--codex-home",
                str(codex_home),
            )
            self.assertEqual(
                0,
                run_main("install", *common_args),
            )
            managed_obsolete = (
                home / ".agents" / "skills" / "xagent-subagents" / "SKILL.md"
            )
            unmanaged_obsolete = (
                codex_home / "skills" / "xagent-subagents" / "SKILL.md"
            )
            managed_obsolete.parent.mkdir(parents=True)
            unmanaged_obsolete.parent.mkdir(parents=True)
            managed_obsolete.write_text(
                "<!-- sheaf-agents-managed: DO NOT EDIT; source=old -->\n",
                encoding="utf-8",
            )
            unmanaged_obsolete.write_text("personal plugin skill\n", encoding="utf-8")

            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = run_main("check", *common_args)
            self.assertEqual(1, result)
            self.assertIn(f"obsolete managed {managed_obsolete}", stderr.getvalue())
            self.assertTrue(managed_obsolete.exists())
            self.assertTrue(unmanaged_obsolete.exists())

            self.assertEqual(0, run_main("clean", *common_args))
            self.assertFalse(managed_obsolete.exists())
            self.assertTrue(unmanaged_obsolete.exists())


class OpenSpecCliInstallTests(unittest.TestCase):
    def test_global_install_creates_shim_matching_vendor_version(self) -> None:
        if not install.node_supports_openspec(install.probe_node_version()):
            self.skipTest("requires Node >= 20.19")

        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            common_args = (
                "--scope",
                "global",
                "--repo-root",
                str(REPO_ROOT),
                "--home",
                str(home),
                "--codex-home",
                str(codex_home),
            )

            self.assertEqual(0, run_main("install", *common_args))

            shim = install.openspec_shim_path(home)
            package_root = install.openspec_package_path(home)
            self.assertTrue(shim.is_file())
            self.assertTrue((package_root / "bin" / "openspec.js").is_file())
            self.assertTrue(
                (package_root / install.OPENSPEC_MANAGED_MARKER_NAME).is_file()
            )

            completed = subprocess.run(
                [str(shim), "--version"],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertEqual(openspec_vendor_version(), completed.stdout.strip())

            self.assertEqual(0, run_main("check", *common_args))

    def test_install_preserves_managed_tree_when_staging_copy_fails(self) -> None:
        if not install.node_supports_openspec(install.probe_node_version()):
            self.skipTest("requires Node >= 20.19")

        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            common_args = (
                "--scope",
                "global",
                "--repo-root",
                str(REPO_ROOT),
                "--home",
                str(home),
                "--codex-home",
                str(codex_home),
            )
            self.assertEqual(0, run_main("install", *common_args))

            package_root = install.openspec_package_path(home)
            marker_path = package_root / install.OPENSPEC_MANAGED_MARKER_NAME
            marker_before = marker_path.read_text(encoding="utf-8")
            entry = package_root / "bin" / "openspec.js"
            self.assertTrue(entry.is_file())

            with mock.patch.object(
                install.shutil,
                "copytree",
                side_effect=OSError("disk full"),
            ):
                with contextlib.redirect_stderr(io.StringIO()):
                    with contextlib.redirect_stdout(io.StringIO()):
                        result = run_main("install", *common_args)

            self.assertEqual(1, result)
            self.assertTrue(package_root.is_dir())
            self.assertTrue(marker_path.is_file())
            self.assertEqual(marker_before, marker_path.read_text(encoding="utf-8"))
            self.assertTrue(entry.is_file())
            self.assertEqual(0, run_main("check", *common_args))

    def test_global_install_skips_cli_with_warning_when_node_too_old(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            common_args = (
                "--scope",
                "global",
                "--repo-root",
                str(REPO_ROOT),
                "--home",
                str(home),
                "--codex-home",
                str(codex_home),
            )
            real_run = subprocess.run
            recorded: list[list[str]] = []

            def tracking_run(*args: object, **kwargs: object) -> object:
                if args and isinstance(args[0], (list, tuple)):
                    recorded.append([str(part) for part in args[0]])
                return real_run(*args, **kwargs)

            stderr = io.StringIO()
            with mock.patch.object(
                install,
                "probe_node_version",
                return_value=(18, 20, 0),
            ):
                with mock.patch.object(subprocess, "run", side_effect=tracking_run):
                    with contextlib.redirect_stderr(stderr):
                        result = run_main("install", *common_args)

            self.assertEqual(0, result)
            self.assertIn("OpenSpec CLI", stderr.getvalue())
            self.assertIn("Node", stderr.getvalue())
            self.assertFalse(install.openspec_shim_path(home).exists())
            self.assertFalse(install.openspec_package_path(home).exists())
            self.assertTrue((home / ".claude" / "CLAUDE.md").is_file())
            self.assertTrue((codex_home / "hooks.json").is_file())
            self.assertFalse(
                any(Path(part).name == "npm" for command in recorded for part in command)
            )
            self.assertFalse(
                any(
                    "npm" in Path(part).name and "install" in command
                    for command in recorded
                    for part in command
                )
            )

    def test_global_install_skips_cli_with_warning_when_node_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            common_args = (
                "--scope",
                "global",
                "--repo-root",
                str(REPO_ROOT),
                "--home",
                str(home),
                "--codex-home",
                str(codex_home),
            )
            real_run = subprocess.run
            recorded: list[list[str]] = []

            def tracking_run(*args: object, **kwargs: object) -> object:
                if args and isinstance(args[0], (list, tuple)):
                    recorded.append([str(part) for part in args[0]])
                return real_run(*args, **kwargs)

            stderr = io.StringIO()
            with mock.patch.object(install, "probe_node_version", return_value=None):
                with mock.patch.object(subprocess, "run", side_effect=tracking_run):
                    with contextlib.redirect_stderr(stderr):
                        result = run_main("install", *common_args)

            self.assertEqual(0, result)
            self.assertIn("OpenSpec CLI", stderr.getvalue())
            self.assertFalse(install.openspec_shim_path(home).exists())
            self.assertTrue((home / ".cursor" / "AGENTS.md").is_file())
            self.assertFalse(
                any(Path(part).name == "npm" for command in recorded for part in command)
            )
            self.assertFalse(
                any("install" in command and any(
                    Path(part).name == "npm" for part in command
                ) for command in recorded)
            )

    def test_global_check_reports_missing_openspec_cli(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            common_args = (
                "--scope",
                "global",
                "--repo-root",
                str(REPO_ROOT),
                "--home",
                str(home),
                "--codex-home",
                str(codex_home),
            )
            with mock.patch.object(install, "probe_node_version", return_value=None):
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(0, run_main("install", *common_args))

            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = run_main("check", *common_args)

            self.assertEqual(1, result)
            self.assertIn("openspec", stderr.getvalue().lower())

    def test_check_openspec_cli_reports_stale_version(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            pin = install.read_openspec_vendor_pin(REPO_ROOT)
            expected = install.openspec_pin_field(pin, "version")
            package_root = install.openspec_package_path(home)
            package_root.mkdir(parents=True)
            (package_root / install.OPENSPEC_MANAGED_MARKER_NAME).write_text(
                install.openspec_managed_marker_content(pin),
                encoding="utf-8",
            )
            shim = install.openspec_shim_path(home)
            shim.parent.mkdir(parents=True)
            shim.write_text(
                "#!/bin/sh\n"
                f"# {install.MANAGED_MARKER}; "
                f"source={install.OPENSPEC_PACKAGE_REL.as_posix()}\n"
                "echo 0.0.0-stale\n",
                encoding="utf-8",
            )
            shim.chmod(shim.stat().st_mode | 0o111)

            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                status = install.check_openspec_cli(REPO_ROOT, home=home)

            self.assertEqual(1, status)
            self.assertIn("stale openspec CLI", stderr.getvalue())
            self.assertIn(expected, stderr.getvalue())
            self.assertIn("0.0.0-stale", stderr.getvalue())

    def test_global_clean_removes_managed_openspec_prefix_only(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            home = (Path(tempdir) / "home").resolve()
            codex_home = (Path(tempdir) / "codex-home").resolve()
            common_args = (
                "--scope",
                "global",
                "--repo-root",
                str(REPO_ROOT),
                "--home",
                str(home),
                "--codex-home",
                str(codex_home),
            )
            self.assertEqual(0, run_main("install", *common_args))

            sheaf_bin = install.sheaf_prefix(home) / "bin"
            unmanaged = sheaf_bin / "keep-me"
            unmanaged.write_text("stay\n", encoding="utf-8")
            foreign = install.sheaf_prefix(home) / "vendor" / "other" / "file.txt"
            foreign.parent.mkdir(parents=True)
            foreign.write_text("foreign\n", encoding="utf-8")

            self.assertEqual(0, run_main("clean", *common_args))

            self.assertFalse(install.openspec_shim_path(home).exists())
            self.assertFalse(install.openspec_package_path(home).exists())
            self.assertTrue(unmanaged.is_file())
            self.assertEqual("stay\n", unmanaged.read_text(encoding="utf-8"))
            self.assertTrue(foreign.is_file())


class OpenSpecHarnessInstallTests(unittest.TestCase):
    def test_vendor_pin_includes_tools_and_workflows(self) -> None:
        pin = install.read_openspec_vendor_pin(REPO_ROOT)
        self.assertEqual(
            ["claude", "cursor", "pi", "codex"],
            pin["tools"],
        )
        self.assertEqual(
            [
                "propose",
                "apply-change",
                "archive-change",
                "explore",
                "sync-specs",
            ],
            pin["workflows"],
        )

    def test_build_openspec_harness_outputs_marks_skills_and_commands(self) -> None:
        if not install.node_supports_openspec(install.probe_node_version()):
            self.skipTest("requires Node >= 20.19")

        outputs = install.build_openspec_harness_outputs(REPO_ROOT)
        by_rel = {
            output.path.relative_to(REPO_ROOT).as_posix(): output for output in outputs
        }

        skill_ids = [
            "openspec-propose",
            "openspec-apply-change",
            "openspec-archive-change",
            "openspec-explore",
            "openspec-sync-specs",
        ]
        for harness in ("claude", "cursor", "pi", "codex"):
            for skill_id in skill_ids:
                rel = f".{harness}/skills/{skill_id}/SKILL.md"
                self.assertIn(rel, by_rel)
                content = by_rel[rel].content
                self.assertTrue(
                    content.startswith("---\n"),
                    f"{rel} must start with YAML frontmatter",
                )
                self.assertIn(f"name: {skill_id}", content)
                self.assertIn("description:", content)
                lines = content.splitlines()
                close_idx = next(
                    i for i in range(1, len(lines)) if lines[i].strip() == "---"
                )
                after_frontmatter = lines[close_idx + 1 : close_idx + 5]
                self.assertTrue(
                    any(
                        line.startswith("<!--") and install.MANAGED_MARKER in line
                        for line in after_frontmatter
                    ),
                    f"{rel}: managed marker must appear after frontmatter",
                )
                self.assertIn("projects/agents/vendor/openspec/package", content)
                self.assertFalse(content.lstrip().startswith("<!--"))

        for name in ("propose", "apply", "archive", "explore", "sync"):
            claude_rel = f".claude/commands/opsx/{name}.md"
            cursor_rel = f".cursor/commands/opsx-{name}.md"
            pi_rel = f".pi/prompts/opsx-{name}.md"
            for rel in (claude_rel, cursor_rel, pi_rel):
                self.assertIn(rel, by_rel)
                content = by_rel[rel].content
                self.assertTrue(
                    content.startswith("---\n"),
                    f"{rel} must start with YAML frontmatter",
                )
                self.assertIn("description:", content)
                self.assertIn(install.MANAGED_MARKER, content)
                self.assertFalse(content.lstrip().startswith("<!--"))

        self.assertFalse(any("AGENTS.md" in rel for rel in by_rel))
        self.assertFalse(any("CLAUDE.md" in rel for rel in by_rel))
        self.assertFalse(any(rel.startswith(".codex/commands") for rel in by_rel))
        self.assertFalse(any("/opsx" in rel and rel.startswith(".codex/") for rel in by_rel))

    def test_openspec_harness_generation_invokes_vendored_openspec_js(self) -> None:
        if not install.node_supports_openspec(install.probe_node_version()):
            self.skipTest("requires Node >= 20.19")

        recorded: list[list[str]] = []
        real_run = subprocess.run

        def tracking_run(*args: object, **kwargs: object) -> object:
            if args and isinstance(args[0], (list, tuple)):
                command = [str(part) for part in args[0]]
                recorded.append(command)
                if (
                    len(command) >= 2
                    and command[0] == "node"
                    and command[1].endswith("openspec.js")
                ):
                    project_root = Path(command[-1])
                    for harness in ("claude", "cursor", "pi", "codex"):
                        for skill_id in (
                            "openspec-propose",
                            "openspec-apply-change",
                            "openspec-archive-change",
                            "openspec-explore",
                            "openspec-sync-specs",
                        ):
                            path = (
                                project_root
                                / f".{harness}"
                                / "skills"
                                / skill_id
                                / "SKILL.md"
                            )
                            path.parent.mkdir(parents=True, exist_ok=True)
                            path.write_text(
                                f"---\nname: {skill_id}\ndescription: stub\n---\n",
                                encoding="utf-8",
                            )
                    for name in ("propose", "apply", "archive", "explore", "sync"):
                        for rel in (
                            f".claude/commands/opsx/{name}.md",
                            f".cursor/commands/opsx-{name}.md",
                            f".pi/prompts/opsx-{name}.md",
                        ):
                            path = project_root / rel
                            path.parent.mkdir(parents=True, exist_ok=True)
                            path.write_text(
                                f"---\nname: opsx-{name}\ndescription: stub {name}\n---\n\n# {name}\n",
                                encoding="utf-8",
                            )
                    return subprocess.CompletedProcess(command, 0, "", "")
            return real_run(*args, **kwargs)

        with mock.patch.object(subprocess, "run", side_effect=tracking_run):
            outputs = install.build_openspec_harness_outputs(REPO_ROOT)

        self.assertTrue(outputs)
        entry = str(
            (REPO_ROOT / "projects/agents/vendor/openspec/package/bin/openspec.js").resolve()
        )
        matching = [command for command in recorded if entry in command]
        self.assertTrue(matching)
        for command in matching:
            self.assertEqual("node", command[0])
            self.assertNotIn("openspec", Path(command[0]).name)
            self.assertTrue(any(part == "init" for part in command))
            joined = " ".join(command)
            self.assertIn("--tools", joined)
            self.assertNotIn(str(install.openspec_shim_path(Path.home())), joined)

    def test_repo_install_leaves_root_agents_and_claude_without_openspec_blocks(
        self,
    ) -> None:
        if not install.node_supports_openspec(install.probe_node_version()):
            self.skipTest("requires Node >= 20.19")

        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = copied_repo_fixture(tempdir)
            agents_before = (
                REPO_ROOT / "projects" / "agents" / "global" / "AGENTS.md"
            ).read_text(encoding="utf-8")
            # Seed root files as the installer would, then reinstall
            self.assertEqual(
                0,
                run_main("install", "--scope", "repo", "--repo-root", str(repo_root)),
            )
            agents = (repo_root / "AGENTS.md").read_text(encoding="utf-8")
            claude = (repo_root / "CLAUDE.md").read_text(encoding="utf-8")
            self.assertIn(install.MANAGED_MARKER, agents)
            self.assertIn("projects/agents/global/AGENTS.md", agents)
            self.assertNotIn("OPENSPEC:START", agents)
            self.assertNotIn("OPENSPEC:START", claude)
            self.assertEqual(agents, claude)
            self.assertIn(agents_before.strip(), agents)

            propose = (
                repo_root / ".claude" / "skills" / "openspec-propose" / "SKILL.md"
            )
            self.assertTrue(propose.is_file())
            self.assertIn(install.MANAGED_MARKER, propose.read_text(encoding="utf-8"))
            self.assertFalse((repo_root / ".codex" / "commands").exists())

    def test_repo_install_and_check_hard_fail_when_node_unavailable(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = copied_repo_fixture(tempdir)
            stderr = io.StringIO()
            with mock.patch.object(install, "probe_node_version", return_value=None):
                with contextlib.redirect_stderr(stderr):
                    result = run_main(
                        "install",
                        "--scope",
                        "repo",
                        "--repo-root",
                        str(repo_root),
                    )
            self.assertEqual(1, result)
            self.assertIn("Node", stderr.getvalue())

            stderr_check = io.StringIO()
            with mock.patch.object(install, "probe_node_version", return_value=None):
                with contextlib.redirect_stderr(stderr_check):
                    check_result = run_main(
                        "check",
                        "--scope",
                        "repo",
                        "--repo-root",
                        str(repo_root),
                    )
            self.assertEqual(1, check_result)
            self.assertIn("Node", stderr_check.getvalue())

    def test_repo_install_check_clean_openspec_harness_round_trip(self) -> None:
        if not install.node_supports_openspec(install.probe_node_version()):
            self.skipTest("requires Node >= 20.19")

        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = copied_repo_fixture(tempdir)
            common = ("--scope", "repo", "--repo-root", str(repo_root))

            unmanaged = (
                repo_root / ".claude" / "skills" / "openspec-propose" / "SKILL.md"
            )
            unmanaged.parent.mkdir(parents=True)
            unmanaged.write_text("unmanaged personal copy\n", encoding="utf-8")
            foreign = repo_root / ".claude" / "skills" / "keep-me" / "SKILL.md"
            foreign.parent.mkdir(parents=True)
            foreign.write_text("leave me\n", encoding="utf-8")

            with contextlib.redirect_stderr(io.StringIO()):
                conflict = run_main("install", *common)
            self.assertEqual(1, conflict)

            self.assertEqual(0, run_main("install", *common, "--force"))
            self.assertEqual(0, run_main("check", *common))

            propose = unmanaged.read_text(encoding="utf-8")
            self.assertIn(install.MANAGED_MARKER, propose)
            self.assertNotEqual("unmanaged personal copy\n", propose)

            # Drift detection
            unmanaged.write_text(
                propose + "\n# drifted\n",
                encoding="utf-8",
            )
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(1, run_main("check", *common))
            self.assertIn("stale", stderr.getvalue())

            self.assertEqual(0, run_main("install", *common))
            self.assertEqual(0, run_main("clean", *common))
            self.assertFalse(unmanaged.exists())
            self.assertTrue(foreign.is_file())
            self.assertEqual("leave me\n", foreign.read_text(encoding="utf-8"))


class RetiredPluginSkillTests(unittest.TestCase):
    def test_retired_plugin_marked_skill_is_reported_and_removed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_home:
            home = Path(raw_home)
            stale = home / ".claude" / "skills" / "xagent-subagents" / "SKILL.md"
            stale.parent.mkdir(parents=True)
            stale.write_text(f"<!-- {install.RETIRED_XAGENT_MARKER} -->\nbody\n")
            outputs = install.build_obsolete_global_outputs(home=home, codex_home=None)
            self.assertEqual(1, install.check_obsolete_outputs(outputs))
            self.assertEqual(0, install.clean_outputs(outputs))
            self.assertFalse(stale.exists())
            self.assertEqual(0, install.check_obsolete_outputs(outputs))

    def test_unmanaged_personal_copy_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as raw_home:
            home = Path(raw_home)
            personal = home / ".claude" / "skills" / "xagent-subagents" / "SKILL.md"
            personal.parent.mkdir(parents=True)
            personal.write_text("my personal skill\n")
            outputs = install.build_obsolete_global_outputs(home=home, codex_home=None)
            self.assertEqual(0, install.clean_outputs(outputs))
            self.assertEqual("my personal skill\n", personal.read_text())


if __name__ == "__main__":
    unittest.main()
