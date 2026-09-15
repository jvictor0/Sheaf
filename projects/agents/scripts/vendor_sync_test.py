from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parent
MODULE_PATH = SCRIPT_DIR / "vendor_sync.py"

if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

spec = importlib.util.spec_from_file_location("agents_vendor_sync", MODULE_PATH)
assert spec is not None
vendor_sync = importlib.util.module_from_spec(spec)
sys.modules["agents_vendor_sync"] = vendor_sync
assert spec.loader is not None
spec.loader.exec_module(vendor_sync)


def run_git(repo: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        capture_output=True,
        text=True,
    )


def init_repo(repo_root: Path) -> None:
    run_git(repo_root, "init")
    run_git(repo_root, "config", "user.email", "vendor-sync-test@example.com")
    run_git(repo_root, "config", "user.name", "Vendor Sync Test")


def commit_all(repo_root: Path, message: str) -> None:
    run_git(repo_root, "add", "-A")
    run_git(repo_root, "commit", "-m", message)


class ParseVendorTomlTests(unittest.TestCase):
    def test_entrypoints_import_without_tomllib_or_tomli(self) -> None:
        blocker = """
import sys

class BlockTomlImports:
    def find_spec(self, fullname, path=None, target=None):
        if fullname in {"tomllib", "tomli"}:
            raise ModuleNotFoundError(fullname)
        return None

sys.meta_path.insert(0, BlockTomlImports())
sys.path.insert(0, __SCRIPTS_DIR__)
import install
import vendor_sync
""".replace("__SCRIPTS_DIR__", repr(str(SCRIPT_DIR)))
        completed = subprocess.run(
            [sys.executable, "-I", "-c", blocker],
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, completed.returncode, completed.stderr)

    def test_parse_vendor_toml_requires_required_fields(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            path = Path(tempdir) / "VENDOR.toml"
            path.write_text(
                'url = "https://example.com/repo.git"\n'
                'revision = "abc123"\n'
                'version = "1.0.0"\n'
                'retrieved_at = "2026-07-27T00:00:00Z"\n',
                encoding="utf-8",
            )
            pin = vendor_sync.parse_vendor_toml(path)
            self.assertEqual("https://example.com/repo.git", pin["url"])
            self.assertEqual("abc123", pin["revision"])
            self.assertEqual("1.0.0", pin["version"])
            self.assertEqual("2026-07-27T00:00:00Z", pin["retrieved_at"])

    def test_parse_vendor_toml_rejects_missing_fields(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            path = Path(tempdir) / "VENDOR.toml"
            path.write_text('url = "https://example.com/repo.git"\n', encoding="utf-8")
            with self.assertRaises(ValueError):
                vendor_sync.parse_vendor_toml(path)


class VendorTreeDirtyDetectionTests(unittest.TestCase):
    def test_vendor_tree_is_dirty_false_for_clean_tracked_tree(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = Path(tempdir)
            init_repo(repo_root)
            vendor_dir = repo_root / "projects" / "agents" / "vendor" / "openspec"
            vendor_dir.mkdir(parents=True)
            (vendor_dir / "VENDOR.toml").write_text(
                'url = "https://example.com/openspec.git"\n'
                'revision = "abc"\n'
                'version = "0.0.1"\n'
                'retrieved_at = "2026-01-01T00:00:00Z"\n',
                encoding="utf-8",
            )
            commit_all(repo_root, "vendor pin")
            self.assertFalse(
                vendor_sync.vendor_tree_is_dirty(repo_root, "openspec")
            )

    def test_vendor_tree_is_dirty_true_for_modified_tracked_file(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = Path(tempdir)
            init_repo(repo_root)
            vendor_dir = repo_root / "projects" / "agents" / "vendor" / "openspec"
            vendor_dir.mkdir(parents=True)
            pin_path = vendor_dir / "VENDOR.toml"
            pin_path.write_text(
                'url = "https://example.com/openspec.git"\n'
                'revision = "abc"\n'
                'version = "0.0.1"\n'
                'retrieved_at = "2026-01-01T00:00:00Z"\n',
                encoding="utf-8",
            )
            commit_all(repo_root, "vendor pin")
            pin_path.write_text(
                'url = "https://example.com/openspec.git"\n'
                'revision = "dirty"\n'
                'version = "0.0.1"\n'
                'retrieved_at = "2026-01-01T00:00:00Z"\n',
                encoding="utf-8",
            )
            self.assertTrue(
                vendor_sync.vendor_tree_is_dirty(repo_root, "openspec")
            )

    def test_vendor_tree_is_dirty_true_for_untracked_file(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = Path(tempdir)
            init_repo(repo_root)
            vendor_dir = repo_root / "projects" / "agents" / "vendor" / "openspec"
            vendor_dir.mkdir(parents=True)
            (vendor_dir / "VENDOR.toml").write_text(
                'url = "https://example.com/openspec.git"\n'
                'revision = "abc"\n'
                'version = "0.0.1"\n'
                'retrieved_at = "2026-01-01T00:00:00Z"\n',
                encoding="utf-8",
            )
            commit_all(repo_root, "vendor pin")
            (vendor_dir / "local-mod.txt").write_text("dirty\n", encoding="utf-8")
            self.assertTrue(
                vendor_sync.vendor_tree_is_dirty(repo_root, "openspec")
            )


class SyncRefuseToClobberTests(unittest.TestCase):
    def test_sync_without_force_raises_when_vendor_dir_has_local_modifications(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            repo_root = Path(tempdir)
            init_repo(repo_root)
            vendor_dir = repo_root / "projects" / "agents" / "vendor" / "openspec"
            vendor_dir.mkdir(parents=True)
            (vendor_dir / "VENDOR.toml").write_text(
                'url = "https://example.com/openspec.git"\n'
                'revision = "old"\n'
                'version = "0.0.1"\n'
                'retrieved_at = "2026-01-01T00:00:00Z"\n',
                encoding="utf-8",
            )
            commit_all(repo_root, "vendor pin")
            (vendor_dir / "local-mod.txt").write_text("dirty\n", encoding="utf-8")

            with mock.patch.object(vendor_sync, "sync_openspec") as sync_openspec:
                with self.assertRaises(vendor_sync.VendorDirtyError):
                    vendor_sync.sync_tool(
                        repo_root=repo_root,
                        tool="openspec",
                        ref="1.4.1",
                        force=False,
                    )
                sync_openspec.assert_not_called()


class OpenspecLockfileReuseTests(unittest.TestCase):
    def test_openspec_lockfile_matches_package_when_versions_equal(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            staged = Path(tempdir) / "package"
            staged.mkdir()
            (staged / "package.json").write_text(
                '{"name":"@fission-ai/openspec","version":"1.4.1"}\n',
                encoding="utf-8",
            )
            lockfile = Path(tempdir) / "package-lock.json"
            lockfile.write_text(
                '{"name":"@fission-ai/openspec","version":"1.4.1","lockfileVersion":3}\n',
                encoding="utf-8",
            )
            self.assertTrue(
                vendor_sync.openspec_lockfile_matches_package(staged, lockfile)
            )

    def test_openspec_lockfile_does_not_match_when_versions_differ(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            staged = Path(tempdir) / "package"
            staged.mkdir()
            (staged / "package.json").write_text(
                '{"name":"@fission-ai/openspec","version":"1.5.0"}\n',
                encoding="utf-8",
            )
            lockfile = Path(tempdir) / "package-lock.json"
            lockfile.write_text(
                '{"name":"@fission-ai/openspec","version":"1.4.1","lockfileVersion":3}\n',
                encoding="utf-8",
            )
            self.assertFalse(
                vendor_sync.openspec_lockfile_matches_package(staged, lockfile)
            )

    def test_install_uses_npm_install_when_lockfile_version_mismatches(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            staged = Path(tempdir) / "package"
            staged.mkdir()
            (staged / "package.json").write_text(
                '{"name":"@fission-ai/openspec","version":"1.5.0"}\n',
                encoding="utf-8",
            )
            lockfile = Path(tempdir) / "package-lock.json"
            lockfile.write_text(
                '{"name":"@fission-ai/openspec","version":"1.4.1","lockfileVersion":3}\n',
                encoding="utf-8",
            )
            with mock.patch.object(vendor_sync, "run_checked") as run_checked:
                vendor_sync.install_openspec_production_deps(
                    staged,
                    existing_lockfile=lockfile,
                )
            run_checked.assert_called_once_with(
                ["npm", "install", "--omit=dev"],
                cwd=staged,
            )
            self.assertFalse((staged / "package-lock.json").exists())

    def test_install_uses_npm_ci_when_lockfile_version_matches(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            staged = Path(tempdir) / "package"
            staged.mkdir()
            (staged / "package.json").write_text(
                '{"name":"@fission-ai/openspec","version":"1.4.1"}\n',
                encoding="utf-8",
            )
            lockfile = Path(tempdir) / "package-lock.json"
            lockfile.write_text(
                '{"name":"@fission-ai/openspec","version":"1.4.1","lockfileVersion":3}\n',
                encoding="utf-8",
            )
            with mock.patch.object(vendor_sync, "run_checked") as run_checked:
                vendor_sync.install_openspec_production_deps(
                    staged,
                    existing_lockfile=lockfile,
                )
            run_checked.assert_called_once_with(
                ["npm", "ci", "--omit=dev"],
                cwd=staged,
            )
            self.assertTrue((staged / "package-lock.json").is_file())


class OpenSpecGenerationExtrasTests(unittest.TestCase):
    def test_write_vendor_toml_preserves_tools_and_workflows_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            path = Path(tempdir) / "VENDOR.toml"
            tools = ["claude", "cursor"]
            workflows = ["propose", "explore"]
            vendor_sync.write_vendor_toml(
                path,
                {
                    "url": "https://example.com/openspec.git",
                    "revision": "abc123",
                    "version": "1.4.1",
                    "retrieved_at": "2026-07-27T00:00:00Z",
                },
                tools=tools,
                workflows=workflows,
            )

            read_tools, read_workflows = vendor_sync.read_openspec_generation_extras(path)
            self.assertEqual(tools, read_tools)
            self.assertEqual(workflows, read_workflows)

            pin = vendor_sync.parse_vendor_toml(path)
            vendor_sync.write_vendor_toml(
                path,
                pin,
                tools=read_tools,
                workflows=read_workflows,
            )
            again_tools, again_workflows = vendor_sync.read_openspec_generation_extras(
                path
            )
            self.assertEqual(tools, again_tools)
            self.assertEqual(workflows, again_workflows)
            raw = path.read_text(encoding="utf-8")
            self.assertIn('tools = ["claude", "cursor"]', raw)
            self.assertIn('workflows = ["propose", "explore"]', raw)

    def test_read_openspec_generation_extras_errors_when_missing_or_invalid(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            missing = Path(tempdir) / "missing.toml"
            with self.assertRaises(ValueError):
                vendor_sync.read_openspec_generation_extras(missing)

            path = Path(tempdir) / "VENDOR.toml"
            path.write_text(
                'url = "https://example.com/openspec.git"\n'
                'revision = "abc123"\n'
                'version = "1.4.1"\n'
                'retrieved_at = "2026-07-27T00:00:00Z"\n',
                encoding="utf-8",
            )
            with self.assertRaises(ValueError):
                vendor_sync.read_openspec_generation_extras(path)


if __name__ == "__main__":
    unittest.main()
