#!/usr/bin/env python3
"""Sync vendored OpenSpec under projects/agents/vendor."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from vendor_manifest import loads as load_vendor_manifest


REQUIRED_PIN_FIELDS = ("url", "revision", "version", "retrieved_at")

OPENSPEC_NPM_PACKAGE = "@fission-ai/openspec"
OPENSPEC_URL = "https://github.com/Fission-AI/OpenSpec.git"

TOOLS = ("openspec",)


class VendorDirtyError(RuntimeError):
    """Raised when sync would clobber a locally modified vendor tree."""


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def vendor_tool_dir(repo_root: Path, tool: str) -> Path:
    return repo_root / "projects" / "agents" / "vendor" / tool


def parse_vendor_toml(path: Path) -> dict[str, str]:
    raw = load_vendor_manifest(path.read_text(encoding="utf-8"), source=path)
    missing = [field for field in REQUIRED_PIN_FIELDS if field not in raw]
    if missing:
        raise ValueError(
            f"{path}: missing required pin fields: {', '.join(missing)}"
        )
    pin: dict[str, str] = {}
    for field in REQUIRED_PIN_FIELDS:
        value = raw[field]
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"{path}: {field} must be a non-empty string")
        pin[field] = value
    return pin


def write_vendor_toml(
    path: Path,
    pin: dict[str, str],
    *,
    tools: list[str] | None = None,
    workflows: list[str] | None = None,
) -> None:
    missing = [field for field in REQUIRED_PIN_FIELDS if field not in pin]
    if missing:
        raise ValueError(f"missing required pin fields: {', '.join(missing)}")
    lines = [
        f"{field} = {json.dumps(pin[field])}" for field in REQUIRED_PIN_FIELDS
    ]
    if tools is not None:
        lines.append(f"tools = {json.dumps(tools)}")
    if workflows is not None:
        lines.append(f"workflows = {json.dumps(workflows)}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_openspec_generation_extras(path: Path) -> tuple[list[str], list[str]]:
    if not path.is_file():
        raise ValueError(f"missing OpenSpec VENDOR.toml: {path}")
    raw = load_vendor_manifest(path.read_text(encoding="utf-8"), source=path)
    tools = raw.get("tools")
    workflows = raw.get("workflows")
    if not isinstance(tools, list) or not tools:
        raise ValueError(f"{path}: tools must be a non-empty list of strings")
    if not isinstance(workflows, list) or not workflows:
        raise ValueError(f"{path}: workflows must be a non-empty list of strings")
    tools_list: list[str] = []
    for item in tools:
        if not isinstance(item, str) or not item.strip():
            raise ValueError(f"{path}: tools entries must be non-empty strings")
        tools_list.append(item.strip())
    workflows_list: list[str] = []
    for item in workflows:
        if not isinstance(item, str) or not item.strip():
            raise ValueError(f"{path}: workflows entries must be non-empty strings")
        workflows_list.append(item.strip())
    return tools_list, workflows_list


def vendor_tree_is_dirty(repo_root: Path, tool: str) -> bool:
    vendor_dir = vendor_tool_dir(repo_root, tool)
    if not vendor_dir.exists():
        return False
    rel = vendor_dir.relative_to(repo_root).as_posix()
    completed = subprocess.run(
        ["git", "-C", str(repo_root), "status", "--porcelain", "--", rel],
        check=True,
        capture_output=True,
        text=True,
    )
    return bool(completed.stdout.strip())


def utc_now_iso() -> str:
    return (
        dt.datetime.now(dt.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )


def run_checked(command: list[str], *, cwd: Path | None = None) -> str:
    completed = subprocess.run(
        command,
        cwd=None if cwd is None else str(cwd),
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout


def resolve_openspec_pin(ref: str) -> dict[str, str]:
    version = run_checked(
        ["npm", "view", f"{OPENSPEC_NPM_PACKAGE}@{ref}", "version"]
    ).strip()
    if not version:
        raise RuntimeError(f"could not resolve npm version for openspec ref {ref!r}")

    revision = ""
    try:
        revision = run_checked(
            ["npm", "view", f"{OPENSPEC_NPM_PACKAGE}@{version}", "gitHead"]
        ).strip()
    except subprocess.CalledProcessError:
        revision = ""
    if revision in ("", "undefined", "null"):
        revision = ""

    if not revision:
        remote = run_checked(["git", "ls-remote", "--tags", OPENSPEC_URL])
        peeled = ""
        lightweight = ""
        tag_ref = f"refs/tags/v{version}"
        for line in remote.splitlines():
            sha, _, refname = line.partition("\t")
            if refname == f"{tag_ref}^{{}}":
                peeled = sha
            elif refname == tag_ref:
                lightweight = sha
        revision = peeled or lightweight

    if not revision:
        raise RuntimeError(
            f"could not resolve git revision for openspec version {version}"
        )

    return {
        "url": OPENSPEC_URL,
        "revision": revision,
        "version": version,
        "retrieved_at": utc_now_iso(),
    }


def replace_directory(destination: Path, source: Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination)


def package_json_version(path: Path) -> str:
    data = json.loads(path.read_text(encoding="utf-8"))
    version = str(data.get("version", "")).strip()
    if not version:
        raise RuntimeError(f"missing version in {path}")
    return version


def openspec_lockfile_matches_package(
    staged_package: Path,
    lockfile: Path,
) -> bool:
    """True when lockfile root version equals staged package.json version."""
    if not lockfile.is_file():
        return False
    package_json = staged_package / "package.json"
    if not package_json.is_file():
        return False
    lock_data = json.loads(lockfile.read_text(encoding="utf-8"))
    lock_version = str(lock_data.get("version", "")).strip()
    return lock_version == package_json_version(package_json)


def install_openspec_production_deps(
    staged_package: Path,
    *,
    existing_lockfile: Path | None,
) -> None:
    # npm pack omits package-lock.json. First sync (or a pin bump whose version
    # no longer matches the committed lockfile) uses npm install to generate a
    # fresh lockfile committed under vendor/. Re-sync of the same pin copies that
    # lockfile into staging and runs npm ci so the lockfile determines node_modules.
    if (
        existing_lockfile is not None
        and openspec_lockfile_matches_package(staged_package, existing_lockfile)
    ):
        shutil.copy2(existing_lockfile, staged_package / "package-lock.json")
        run_checked(["npm", "ci", "--omit=dev"], cwd=staged_package)
        return
    run_checked(["npm", "install", "--omit=dev"], cwd=staged_package)


def sync_openspec(repo_root: Path, ref: str) -> dict[str, str]:
    pin = resolve_openspec_pin(ref)
    vendor_dir = vendor_tool_dir(repo_root, "openspec")
    package_dir = vendor_dir / "package"
    existing_lockfile = package_dir / "package-lock.json"

    with tempfile.TemporaryDirectory(prefix="sheaf-openspec-vendor-") as tempdir:
        staging_root = Path(tempdir)
        run_checked(
            ["npm", "pack", f"{OPENSPEC_NPM_PACKAGE}@{pin['version']}"],
            cwd=staging_root,
        )
        tarballs = sorted(staging_root.glob("*.tgz"))
        if len(tarballs) != 1:
            raise RuntimeError(
                f"expected one npm pack tarball, found {len(tarballs)} in {staging_root}"
            )
        extract_dir = staging_root / "extract"
        extract_dir.mkdir()
        run_checked(
            ["tar", "-xzf", str(tarballs[0]), "-C", str(extract_dir)]
        )
        staged_package = extract_dir / "package"
        if not staged_package.is_dir():
            raise RuntimeError(f"npm pack extract missing package/: {extract_dir}")
        install_openspec_production_deps(
            staged_package,
            existing_lockfile=existing_lockfile if existing_lockfile.is_file() else None,
        )
        replace_directory(package_dir, staged_package)

    pin_path = vendor_dir / "VENDOR.toml"
    tools, workflows = read_openspec_generation_extras(pin_path)
    write_vendor_toml(pin_path, pin, tools=tools, workflows=workflows)
    return pin


def sync_tool(
    *,
    repo_root: Path,
    tool: str,
    ref: str,
    force: bool = False,
) -> dict[str, str]:
    if tool not in TOOLS:
        raise ValueError(f"unsupported tool {tool!r}; expected one of {TOOLS}")
    if not force and vendor_tree_is_dirty(repo_root, tool):
        raise VendorDirtyError(
            f"vendor tree for {tool} has local modifications; "
            "re-run with --force to clobber"
        )
    return sync_openspec(repo_root, ref)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Sync vendored OpenSpec."
    )
    parser.add_argument(
        "--tool",
        required=True,
        choices=TOOLS,
        help="Which vendored tool to sync",
    )
    parser.add_argument(
        "--ref",
        required=True,
        help="Upstream pin: npm version/dist-tag for openspec",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Replace vendor tree even when it has local modifications",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=None,
        help="Repository root (defaults to the Sheaf checkout containing this script)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    repo_root = (args.repo_root or repo_root_from_script()).resolve()
    try:
        pin = sync_tool(
            repo_root=repo_root,
            tool=args.tool,
            ref=args.ref,
            force=args.force,
        )
    except VendorDirtyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(
        f"synced {args.tool} version={pin['version']} "
        f"revision={pin['revision']} retrieved_at={pin['retrieved_at']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
