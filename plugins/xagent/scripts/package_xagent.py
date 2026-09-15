#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = PLUGIN_ROOT.parents[1]
XAGENT_ROOT = REPO_ROOT / "projects" / "xagent"
ASSET_ROOT = PLUGIN_ROOT / "assets" / "xagent"
PACKAGE_DIRECTORIES = (".codex-plugin", "skills", "scripts")
PACKAGE_COMPANION_FILES = (".mcp.json",)
HOOK_SCRIPT_NAME = "controller_stop_hook.py"
CLIENT_SERVICE_FILES = frozenset(
    {
        "await_liveness.d.ts",
        "await_liveness.d.ts.map",
        "await_liveness.js",
        "await_liveness.js.map",
        "client.d.ts",
        "client.d.ts.map",
        "client.js",
        "client.js.map",
        "config.d.ts",
        "config.d.ts.map",
        "config.js",
        "config.js.map",
        "tool_schemas.d.ts",
        "tool_schemas.d.ts.map",
        "tool_schemas.js",
        "tool_schemas.js.map",
    }
)


def packaged_runtime_ignore(directory: str, names: list[str]) -> set[str]:
    ignored: set[str] = set()
    dir_path = Path(directory)
    for name in names:
        if name.startswith("service_main."):
            ignored.add(name)
            continue
        if dir_path.name == "service" and name not in CLIENT_SERVICE_FILES:
            ignored.add(name)
    return ignored


def run(
    args: list[str],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        args,
        cwd=cwd,
        env=merged_env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )


def copy_runtime(asset_root: Path) -> None:
    run(["npm", "run", "build"], cwd=XAGENT_ROOT)
    if asset_root.exists():
        shutil.rmtree(asset_root)
    (asset_root / "dist").mkdir(parents=True)
    shutil.copy2(XAGENT_ROOT / "package.json", asset_root / "package.json")
    shutil.copytree(
        XAGENT_ROOT / "dist" / "src",
        asset_root / "dist" / "src",
        ignore=packaged_runtime_ignore,
    )
    copy_packaged_dependencies(asset_root)


def copy_packaged_dependencies(asset_root: Path) -> None:
    source_modules = XAGENT_ROOT / "node_modules"
    if not source_modules.is_dir():
        raise RuntimeError(
            f"missing xagent node_modules: {source_modules}; "
            "run `npm install` in projects/xagent before packaging"
        )
    destination = asset_root / "node_modules"
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(
        source_modules,
        destination,
        ignore=shutil.ignore_patterns(".cache"),
    )


def build_package(destination: Path) -> None:
    destination = destination.resolve()
    if destination.exists():
        if not destination.is_dir():
            raise RuntimeError(f"package output destination is not a directory: {destination}")
        if any(destination.iterdir()):
            raise RuntimeError(
                f"refusing to use non-empty package output destination: {destination}"
            )
    else:
        destination.mkdir(parents=True)
    for directory in PACKAGE_DIRECTORIES:
        shutil.copytree(
            PLUGIN_ROOT / directory,
            destination / directory,
            copy_function=shutil.copy2,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
    for companion in PACKAGE_COMPANION_FILES:
        source = PLUGIN_ROOT / companion
        if not source.is_file():
            raise RuntimeError(f"missing plugin companion file: {source}")
        shutil.copy2(source, destination / companion)
    copy_runtime(destination / "assets" / "xagent")


def file_snapshot(
    root: Path,
    *,
    exclude_prefixes: tuple[str, ...] = (),
) -> dict[str, tuple[int, str]]:
    snapshot: dict[str, tuple[int, str]] = {}
    if not root.exists():
        return snapshot
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = str(path.relative_to(root))
        if any(
            relative == prefix or relative.startswith(f"{prefix}/")
            for prefix in exclude_prefixes
        ):
            continue
        snapshot[relative] = (
            path.stat().st_mode & 0o777,
            hashlib.sha256(path.read_bytes()).hexdigest(),
        )
    return snapshot


def describe_snapshot_drift(
    expected: dict[str, tuple[int, str]],
    actual: dict[str, tuple[int, str]],
) -> list[str]:
    lines: list[str] = []
    expected_paths = set(expected)
    actual_paths = set(actual)
    for path in sorted(expected_paths - actual_paths):
        lines.append(f"missing: {path}")
    for path in sorted(actual_paths - expected_paths):
        lines.append(f"extra: {path}")
    for path in sorted(expected_paths & actual_paths):
        if expected[path] != actual[path]:
            lines.append(f"changed: {path}")
    return lines


def check_tracked_assets_current(*, validate: bool = False) -> None:
    with tempfile.TemporaryDirectory(prefix="xagent-plugin-check-") as tempdir:
        package_root = Path(tempdir) / "package"
        build_package(package_root)
        if validate:
            validate_package(package_root)
        expected = file_snapshot(
            package_root / "assets" / "xagent",
            exclude_prefixes=("node_modules",),
        )
    actual = file_snapshot(ASSET_ROOT, exclude_prefixes=("node_modules",))
    if expected != actual:
        drift = "\n".join(describe_snapshot_drift(expected, actual))
        raise RuntimeError(
            "tracked xagent plugin assets are stale; run `make xagent-plugin-build` "
            "and commit the regenerated assets"
            + (f"\n{drift}" if drift else "")
        )


def stage_runtime() -> None:
    copy_runtime(ASSET_ROOT)


def validate_package(plugin_root: Path) -> None:
    validate_hook_registration(plugin_root)
    validate_launcher(plugin_root)


def validate_hook_registration(plugin_root: Path) -> None:
    """The controller stop hook ships as a program, never as a registration.

    Codex discovers plugin-root `hooks.json` and `hooks/` components on its
    own. The installer registers these hooks explicitly in global JSON, so a
    discoverable component here would be a silent second registration.
    """
    hook = plugin_root / "scripts" / HOOK_SCRIPT_NAME
    if not hook.is_file():
        raise RuntimeError(f"packaged plugin is missing the controller stop hook: {hook}")
    for discoverable in (plugin_root / "hooks.json", plugin_root / "hooks"):
        if discoverable.exists():
            raise RuntimeError(
                f"packaged plugin must not register hooks by discovery: {discoverable}"
            )
    manifest_path = plugin_root / ".codex-plugin" / "plugin.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if "hooks" in manifest:
        raise RuntimeError(f"{manifest_path} must not declare a hooks field")


def validate_launcher(plugin_root: Path) -> None:
    launcher = plugin_root / "scripts" / "xagent"
    with tempfile.TemporaryDirectory(prefix="xagent-plugin-help-") as tempdir:
        help_result = run([str(launcher), "--help"], cwd=Path(tempdir))
    if "xagent run" not in help_result.stdout:
        raise RuntimeError("packaged launcher help output did not include xagent usage")
    with tempfile.TemporaryDirectory(prefix="xagent-plugin-repo-") as tempdir:
        repo = Path(tempdir)
        log_root = repo.parent / f"{repo.name}-central-log"
        run(
            [str(launcher), "run", "--harness", "codex", "--subagent", "hello"],
            cwd=repo,
            env={"XAGENT_TEST_ADAPTER": "fake", "XAGENT_LOG_ROOT": str(log_root)},
        )
        if not log_root.is_dir():
            raise RuntimeError("packaged launcher did not write logs under the configured log root")
        if (repo / "data" / "xagent").exists():
            raise RuntimeError("packaged launcher wrote logs under the active repository")
    validate_missing_runtime_error(plugin_root, launcher)


def validate_missing_runtime_error(plugin_root: Path, launcher: Path) -> None:
    entrypoint = plugin_root / "assets" / "xagent" / "dist" / "src" / "main.js"
    hidden_entrypoint = entrypoint.with_name("main.js.hidden-for-validation")
    entrypoint.rename(hidden_entrypoint)
    try:
        with tempfile.TemporaryDirectory(prefix="xagent-plugin-missing-runtime-") as tempdir:
            result = subprocess.run(
                [str(launcher), "--help"],
                cwd=Path(tempdir),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        if result.returncode == 0:
            raise RuntimeError("launcher succeeded even though packaged runtime was missing")
        if "xagent packaged runtime is missing" not in result.stderr:
            raise RuntimeError("missing runtime error did not name the missing packaged asset")
    finally:
        hidden_entrypoint.rename(entrypoint)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build and validate the xagent Codex plugin.")
    parser.add_argument(
        "--output",
        type=Path,
        help="Build a complete plugin package at this untracked destination.",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Verify tracked plugin runtime assets match the built xagent package.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.check:
        if args.output is not None:
            raise RuntimeError("--check cannot be combined with --output")
        check_tracked_assets_current(validate=True)
        message = f"tracked xagent runtime assets are current in {ASSET_ROOT}"
        print(message)
        return 0
    elif args.output is None:
        stage_runtime()
        package_root = PLUGIN_ROOT
        message = f"staged xagent runtime in {ASSET_ROOT}"
    else:
        package_root = args.output.expanduser().resolve()
        build_package(package_root)
        message = f"staged xagent plugin package in {package_root}"
    validate_package(package_root)
    print(message)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
