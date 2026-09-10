#!/usr/bin/env python3
"""Build, stage, and register the Sheaf xagent plugin for the current user."""

from __future__ import annotations

import argparse
import copy
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Sequence


PLUGIN_NAME = "xagent"
MANAGED_FILE = ".sheaf-managed"
MANAGED_CONTENT = "sheaf-xagent-plugin\n"

# A1/A2: xagent must be reachable from every harness, not just Codex. Codex
# gets both the skill and the MCP declaration inside the installed plugin
# package; the other harnesses have no plugin, so the skill is written to
# their global skill directory and the MCP endpoint is upserted into their own
# registry.
#
# The marker is deliberately NOT the agents installer's marker: install.py
# treats xagent-subagents as plugin-owned and must neither render nor delete
# these files.
SKILL_ID = "xagent-subagents"
SKILL_MARKER = "<!-- sheaf-xagent-managed: DO NOT EDIT -->"
SKILL_SOURCE_REL = Path("plugins") / "xagent" / "skills" / SKILL_ID / "SKILL.md"
HARNESS_SKILL_DIRS = (
    Path(".claude") / "skills",
    Path(".cursor") / "skills",
    Path(".pi") / "skills",
)

XAGENT_MCP_URL = "http://127.0.0.1:9005/mcp"
XAGENT_MCP_ENTRY = {"type": "http", "url": XAGENT_MCP_URL}
# pi is absent on purpose: it ships without built-in MCP by design, so it has
# no registry to write. On pi the skill plus the packaged CLI is the whole
# story. See plugins/xagent/README.md.
HARNESS_MCP_REGISTRIES = (
    Path(".claude.json"),
    Path(".cursor") / "mcp.json",
)


def write_json_atomic(path: Path, payload: object) -> None:
    """Replace a JSON registry without ever leaving it truncated.

    `~/.claude.json` is Claude Code's entire global state file — project list,
    history metadata, account. A bare write_text that dies between truncate and
    flush destroys it, so stage a sibling temp file, keep one backup of the
    previous contents, and swap with os.replace.
    """
    serialized = json.dumps(payload, indent=2) + "\n"
    staged = path.with_name(f".{path.name}.xagent-stage")
    staged.write_text(serialized, encoding="utf-8")
    if path.exists():
        shutil.copy2(path, path.with_name(f"{path.name}.xagent-backup"))
    os.replace(staged, path)


def render_harness_skill(repo_root: Path) -> str:
    """Skill text with the managed marker placed after the YAML frontmatter.

    Harnesses parse frontmatter from the first line, so the marker cannot lead.
    """
    source = (repo_root / SKILL_SOURCE_REL).read_text(encoding="utf-8")
    if not source.startswith("---\n"):
        raise RuntimeError(f"{SKILL_SOURCE_REL} must start with YAML frontmatter")
    closing = source.index("\n---\n", 3)
    frontmatter = source[: closing + len("\n---\n")]
    body = source[closing + len("\n---\n") :].lstrip("\n")
    return f"{frontmatter}\n{SKILL_MARKER}\n\n{body}"


def install_harness_skill(*, repo_root: Path, home: Path) -> None:
    repo_root = repo_root.expanduser().resolve()
    home = home.expanduser().resolve()
    content = render_harness_skill(repo_root)
    for relative in HARNESS_SKILL_DIRS:
        destination = home / relative / SKILL_ID / "SKILL.md"
        if destination.exists():
            existing = destination.read_text(encoding="utf-8")
            if SKILL_MARKER not in existing:
                raise RuntimeError(
                    f"refusing to overwrite unmanaged {destination}; "
                    "move it aside and re-run"
                )
            if existing == content:
                continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(content, encoding="utf-8")


def register_harness_mcp(*, home: Path) -> None:
    """Upsert the xagent HTTP MCP endpoint into each harness's own registry.

    Only the `xagent` key is touched; every other server the user configured
    survives untouched.
    """
    home = home.expanduser().resolve()
    for relative in HARNESS_MCP_REGISTRIES:
        path = home / relative
        registry: dict[str, object] = {}
        if path.exists():
            raw = path.read_text(encoding="utf-8").strip()
            if raw:
                loaded = json.loads(raw)
                if not isinstance(loaded, dict):
                    raise RuntimeError(f"{path} must contain a JSON object")
                registry = loaded
        servers = registry.get("mcpServers")
        if servers is None:
            servers = {}
        if not isinstance(servers, dict):
            raise RuntimeError(f'{path} "mcpServers" must be a JSON object')
        if servers.get(PLUGIN_NAME) == XAGENT_MCP_ENTRY:
            continue
        servers[PLUGIN_NAME] = dict(XAGENT_MCP_ENTRY)
        registry["mcpServers"] = servers
        path.parent.mkdir(parents=True, exist_ok=True)
        write_json_atomic(path, registry)

# The controller stop hooks are registered only here, in each harness's own
# global JSON. The plugin package deliberately ships no discoverable hooks.json
# or hooks/ component, so this merge is the single registration path.
#
# Group shape follows the payloads captured from both harnesses during hook
# fixture capture: one command entry, no matcher, no timeout. Neither capture
# established matcher semantics for MCP tool names on either harness, and the
# hook program validates the normalized tool name itself, so no matcher is
# claimed here.
HOOK_SCRIPT_NAME = "controller_stop_hook.py"
HOOK_STATE_ROOT_RELATIVE = (
    Path(".agents") / "plugins" / "data" / "xagent" / "controller-stop-hooks"
)
HOOK_EVENTS = {"observe": "PostToolUse", "guard": "Stop"}
CLAUDE_SETTINGS_RELATIVE = Path(".claude") / "settings.json"
CODEX_HOOKS_NAME = "hooks.json"
CODEX_TRUST_NOTICE = (
    "xagent Codex hooks are registered but stay inert until their commands are "
    "trusted: run /hooks in Codex to review and approve them."
)


def hook_state_root(home: Path) -> Path:
    """Where the hook program keeps per-session pending-run state.

    Rendered from the installer's resolved home so the hook never has to infer
    it from its own environment, and kept outside the replaceable plugin
    package so reinstalling does not discard live controller state.
    """
    return (home.expanduser() / HOOK_STATE_ROOT_RELATIVE).resolve()


def hook_commands(*, installed_hook: Path, harness: str, state_root: Path) -> dict[str, str]:
    base = (
        f"python3 {shlex.quote(str(installed_hook))} "
        f"--harness {harness} --state-root {shlex.quote(str(state_root))}"
    )
    return {mode: f"{base} {mode}" for mode in HOOK_EVENTS}


def hook_command_identity(command: object) -> tuple[str, str, str] | None:
    """Return `(script, harness, mode)` for an xagent hook command.

    Ownership is the canonical command signature itself; neither harness JSON
    schema has a field we could claim without inventing one.
    """
    if not isinstance(command, str):
        return None
    try:
        tokens = shlex.split(command)
    except ValueError:
        return None
    if len(tokens) < 2 or tokens[-1] not in HOOK_EVENTS:
        return None
    script = next(
        (token for token in tokens if Path(token).name == HOOK_SCRIPT_NAME),
        None,
    )
    harness = next(
        (
            tokens[index + 1]
            for index, token in enumerate(tokens[:-1])
            if token == "--harness"
        ),
        None,
    )
    if script is None or harness is None:
        return None
    return script, harness, tokens[-1]


def _owns_entry(entry: object, identity: tuple[str, str, str]) -> bool:
    return (
        isinstance(entry, dict)
        and hook_command_identity(entry.get("command")) == identity
    )


def _group_entries(group: object) -> list[object] | None:
    if not isinstance(group, dict):
        return None
    entries = group.get("hooks")
    return entries if isinstance(entries, list) else None


def _merge_event_groups(
    groups: list[object],
    *,
    identity: tuple[str, str, str],
    command: str,
) -> tuple[list[object], bool]:
    """Canonicalize our own command, drop its duplicates, keep everything else.

    Ownership is per command entry, not per group: a harness group is a list of
    commands and only our own entry in it is ours. Our entry is therefore
    lifted into a clean group of its own at the first owned group's index —
    group-level keys such as `matcher` or `timeout` would otherwise apply to
    our command and could silently disable observation — while its unrelated
    siblings stay together immediately beside it, keeping their group's own
    keys and their original order. Codex trust is positional, so nothing else
    moves.
    """
    canonical = {"type": "command", "command": command}
    merged: list[object] = []
    kept_owned = False
    for group in groups:
        entries = _group_entries(group)
        if entries is None or not any(_owns_entry(entry, identity) for entry in entries):
            merged.append(group)
            continue
        if not kept_owned:
            kept_owned = True
            merged.append({"hooks": [dict(canonical)]})
        remainder = [entry for entry in entries if not _owns_entry(entry, identity)]
        if not remainder:
            # The group held nothing but our own command.
            continue
        assert isinstance(group, dict)
        merged.append({**group, "hooks": remainder})
    if not kept_owned:
        merged.append({"hooks": [dict(canonical)]})
    return merged, merged != groups


def merge_xagent_hook_groups(
    payload: dict[str, object],
    *,
    harness: str,
    observe_command: str,
    guard_command: str,
) -> tuple[dict[str, object], bool]:
    """Return the merged configuration and whether it differs from `payload`.

    `load_shared_hook_json()` is where a file's whole shape is checked; the
    checks here are this function's own argument contract, because it is also
    callable on a payload that never came from disk.
    """
    merged = copy.deepcopy(payload)
    # Only a missing key means absence; an explicit null is malformed.
    hooks = merged["hooks"] if "hooks" in merged else {}
    if not isinstance(hooks, dict):
        raise RuntimeError('field "hooks" must be a JSON object')

    changed = False
    for mode, command in (("observe", observe_command), ("guard", guard_command)):
        identity = hook_command_identity(command)
        if identity is None or identity[1:] != (harness, mode):
            raise RuntimeError(f"not a canonical xagent {mode} command: {command}")
        event = HOOK_EVENTS[mode]
        groups = hooks[event] if event in hooks else []
        if not isinstance(groups, list):
            raise RuntimeError(f'field "hooks.{event}" must be a JSON array')
        updated, event_changed = _merge_event_groups(
            groups, identity=identity, command=command
        )
        if event_changed:
            hooks[event] = updated
            changed = True
    merged["hooks"] = hooks
    return merged, changed


def load_shared_hook_json(path: Path) -> dict[str, object]:
    """Read a harness configuration that xagent shares with its owner.

    Only a missing file means absence. An existing file we cannot read as a
    configuration — empty, unparseable, or carrying an incompatible `hooks`
    shape, including an explicit null — is malformed and raises before the
    caller writes, because replacing it would discard whatever the user meant
    to have there.
    """
    if not path.exists():
        return {}
    raw = path.read_text(encoding="utf-8")
    if not raw.strip():
        raise RuntimeError(f"{path} is empty; remove it or restore its JSON object")
    try:
        loaded = json.loads(raw)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{path} must contain valid JSON: {error}") from error
    if not isinstance(loaded, dict):
        raise RuntimeError(f"{path} must contain a JSON object")
    if "hooks" in loaded:
        hooks = loaded["hooks"]
        if not isinstance(hooks, dict):
            raise RuntimeError(f'{path} field "hooks" must be a JSON object')
        for event, groups in hooks.items():
            if not isinstance(groups, list):
                raise RuntimeError(f'{path} field "hooks.{event}" must be a JSON array')
    return loaded


def register_harness_hooks(
    *,
    home: Path,
    codex_home: Path,
    installed_hook: Path,
) -> None:
    """Register the installed hook program with Claude Code and Codex.

    Both files belong to their harness, and the Codex one is also written by
    the agents installer, so only the canonical xagent groups are touched.
    Every target is validated and merged before any of them is written, so a
    malformed second file cannot leave the first one half-migrated.
    """
    home = home.expanduser().resolve()
    codex_home = codex_home.expanduser().resolve()
    installed_hook = installed_hook.expanduser().resolve()
    if not installed_hook.is_file():
        raise RuntimeError(f"missing installed xagent hook program: {installed_hook}")

    state_root = hook_state_root(home)
    planned: list[tuple[str, Path, dict[str, object]]] = []
    for harness, path in (
        ("claude", home / CLAUDE_SETTINGS_RELATIVE),
        ("codex", codex_home / CODEX_HOOKS_NAME),
    ):
        commands = hook_commands(
            installed_hook=installed_hook, harness=harness, state_root=state_root
        )
        payload = load_shared_hook_json(path)
        try:
            merged, changed = merge_xagent_hook_groups(
                payload,
                harness=harness,
                observe_command=commands["observe"],
                guard_command=commands["guard"],
            )
        except RuntimeError as error:
            raise RuntimeError(f"{path}: {error}") from error
        if changed:
            planned.append((harness, path, merged))

    for harness, path, merged in planned:
        path.parent.mkdir(parents=True, exist_ok=True)
        write_json_atomic(path, merged)
        if harness == "codex":
            # Removing a duplicate or changing a command shifts the positional
            # trust records Codex keeps, so re-approval may be required.
            print(CODEX_TRUST_NOTICE)


REPO_ROOT = Path(__file__).resolve().parents[3]
HELPER_NAMES = (
    "create_basic_plugin.py",
    "update_plugin_cachebuster.py",
    "read_marketplace_name.py",
    "validate_plugin.py",
)


def run_command(
    args: list[str],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    try:
        return subprocess.run(
            args,
            cwd=cwd,
            env=merged_env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
    except FileNotFoundError as error:
        raise RuntimeError(f"missing command dependency: {args[0]}") from error
    except subprocess.CalledProcessError as error:
        details = (error.stderr or error.stdout or "").strip()
        message = f"command failed ({error.returncode}): {shlex.join(args)}"
        if details:
            message = f"{message}\n{details}"
        raise RuntimeError(message) from error


def require_helpers(codex_home: Path) -> dict[str, Path]:
    scripts_root = (
        codex_home.expanduser().resolve()
        / "skills"
        / ".system"
        / "plugin-creator"
        / "scripts"
    )
    helpers: dict[str, Path] = {}
    for name in HELPER_NAMES:
        path = scripts_root / name
        if not path.is_file():
            raise FileNotFoundError(f"missing plugin-creator helper: {path}")
        helpers[name] = path
    return helpers


def require_managed_destination(destination: Path) -> None:
    if not destination.exists():
        return
    if not destination.is_dir() or destination.is_symlink():
        raise RuntimeError(
            f"refusing to replace unmanaged xagent plugin destination: {destination}"
        )
    marker = destination / MANAGED_FILE
    try:
        content = marker.read_text(encoding="utf-8")
    except (FileNotFoundError, IsADirectoryError, UnicodeDecodeError) as error:
        raise RuntimeError(
            f"refusing to replace unmanaged xagent plugin destination: {destination}"
        ) from error
    if content != MANAGED_CONTENT:
        raise RuntimeError(
            f"refusing to replace unmanaged xagent plugin destination: {destination}"
        )


def replace_managed_destination(staged: Path, destination: Path) -> None:
    backup = destination.with_name(f".{destination.name}-backup-{uuid.uuid4().hex}")
    had_destination = destination.exists()
    if had_destination:
        os.replace(destination, backup)
    try:
        os.replace(staged, destination)
    except OSError:
        if had_destination:
            os.replace(backup, destination)
        raise
    if had_destination:
        shutil.rmtree(backup)


def require_installed_plugin(plugin_list: str, destination: Path) -> None:
    expected_path = str(destination.resolve())
    matching_rows = []
    for raw_line in plugin_list.splitlines():
        line = raw_line.strip()
        lowered = line.lower()
        first_column = lowered.split(maxsplit=1)[0] if lowered else ""
        if (
            (first_column == PLUGIN_NAME or first_column.startswith(f"{PLUGIN_NAME}@"))
            and "installed" in lowered
            and "enabled" in lowered
            and line.endswith(expected_path)
        ):
            matching_rows.append(line)
    if len(matching_rows) != 1:
        raise RuntimeError(
            "codex plugin list did not report exactly one installed and enabled "
            f"{PLUGIN_NAME} row at {expected_path}"
        )


def marketplace_source_path(*, home: Path, destination: Path) -> str:
    try:
        relative = destination.resolve().relative_to(home.resolve())
    except ValueError:
        return str(destination.resolve())
    return f"./{relative.as_posix()}"


def point_marketplace_entry_to_destination(
    marketplace_path: Path,
    *,
    source_path: str,
) -> None:
    payload = json.loads(marketplace_path.read_text(encoding="utf-8"))
    plugins = payload.get("plugins")
    if not isinstance(plugins, list):
        raise RuntimeError(f"{marketplace_path} field 'plugins' must be an array")
    for entry in plugins:
        if isinstance(entry, dict) and entry.get("name") == PLUGIN_NAME:
            source = entry.setdefault("source", {})
            if not isinstance(source, dict):
                source = {}
                entry["source"] = source
            source["source"] = "local"
            source["path"] = source_path
            marketplace_path.write_text(
                json.dumps(payload, indent=2) + "\n",
                encoding="utf-8",
            )
            return
    raise RuntimeError(f"marketplace entry '{PLUGIN_NAME}' missing in {marketplace_path}")


def install_global(
    *,
    repo_root: Path,
    home: Path,
    codex_home: Path,
    codex: str = "codex",
    cachebuster: str | None = None,
) -> None:
    repo_root = repo_root.expanduser().resolve()
    home = home.expanduser().resolve()
    helpers = require_helpers(codex_home)
    package_script = repo_root / "plugins" / "xagent" / "scripts" / "package_xagent.py"
    if not package_script.is_file():
        raise FileNotFoundError(f"missing xagent package builder: {package_script}")

    marketplace_root = home / ".agents" / "plugins"
    marketplace_path = marketplace_root / "marketplace.json"
    destination = marketplace_root / "plugins" / PLUGIN_NAME
    source_path = marketplace_source_path(home=home, destination=destination)
    codex_env = {
        "HOME": str(home),
        "CODEX_HOME": str(codex_home.expanduser().resolve()),
    }

    with tempfile.TemporaryDirectory(prefix="sheaf-xagent-install-") as tempdir:
        temporary_root = Path(tempdir)
        package_root = temporary_root / "package"
        run_command(
            [sys.executable, str(package_script), "--output", str(package_root)],
            cwd=repo_root,
        )
        run_command(
            [sys.executable, str(helpers["validate_plugin.py"]), str(package_root)],
            cwd=repo_root,
        )

        require_managed_destination(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        staged = Path(
            tempfile.mkdtemp(prefix=f".{PLUGIN_NAME}-stage-", dir=destination.parent)
        )
        try:
            shutil.copytree(
                package_root,
                staged,
                copy_function=shutil.copy2,
                dirs_exist_ok=True,
            )
            (staged / MANAGED_FILE).write_text(MANAGED_CONTENT, encoding="utf-8")
            cachebuster_command = [
                sys.executable,
                str(helpers["update_plugin_cachebuster.py"]),
                str(staged),
            ]
            if cachebuster is not None:
                cachebuster_command.extend(["--cachebuster", cachebuster])
            run_command(cachebuster_command, cwd=repo_root)
            replace_managed_destination(staged, destination)
        finally:
            shutil.rmtree(staged, ignore_errors=True)

        scaffold_parent = temporary_root / "scaffold"
        scaffold_parent.mkdir()
        run_command(
            [
                sys.executable,
                str(helpers["create_basic_plugin.py"]),
                PLUGIN_NAME,
                "--path",
                str(scaffold_parent),
                "--with-marketplace",
                "--marketplace-path",
                str(marketplace_path),
                "--install-policy",
                "AVAILABLE",
                "--auth-policy",
                "ON_INSTALL",
                "--category",
                "Productivity",
                "--force",
            ],
            cwd=repo_root,
        )
        point_marketplace_entry_to_destination(marketplace_path, source_path=source_path)
        marketplace_name = run_command(
            [
                sys.executable,
                str(helpers["read_marketplace_name.py"]),
                "--marketplace-path",
                str(marketplace_path),
            ],
            cwd=repo_root,
        ).stdout.strip()
        if not marketplace_name:
            raise RuntimeError(
                f"read_marketplace_name.py returned an empty marketplace name for {marketplace_path}"
            )
        run_command(
            [codex, "plugin", "add", f"{PLUGIN_NAME}@{marketplace_name}"],
            cwd=repo_root,
            env=codex_env,
        )
        plugin_list = run_command(
            [codex, "plugin", "list"],
            cwd=repo_root,
            env=codex_env,
        ).stdout
        require_installed_plugin(plugin_list, destination)
        # Force Codex to resolve and parse the plugin-provided MCP declaration
        # now, while installation can still fail loudly, rather than leaving a
        # malformed or undiscoverable server for the next controller session.
        run_command(
            [codex, "mcp", "get", PLUGIN_NAME],
            cwd=repo_root,
            env=codex_env,
        )

    # Codex is served by the plugin package above. Every other harness needs
    # the skill and the MCP endpoint written into its own locations, or a
    # controller running there has neither the tools nor the manual.
    install_harness_skill(repo_root=repo_root, home=home)
    register_harness_mcp(home=home)
    # Only now is the hook program committed at its stable installed path, so
    # the commands written into global JSON cannot outlive a failed install.
    register_harness_hooks(
        home=home,
        codex_home=codex_home,
        installed_hook=destination / "scripts" / HOOK_SCRIPT_NAME,
    )


def default_codex_home() -> Path:
    configured = os.environ.get("CODEX_HOME")
    return Path(configured) if configured else Path.home() / ".codex"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    install_parser = subparsers.add_parser("install", help="Install the global xagent plugin")
    install_parser.add_argument("--repo-root", type=Path, default=REPO_ROOT, help=argparse.SUPPRESS)
    install_parser.add_argument("--home", type=Path, default=Path.home())
    install_parser.add_argument("--codex-home", type=Path, default=default_codex_home())
    install_parser.add_argument("--codex", default="codex")
    install_parser.add_argument("--cachebuster", help=argparse.SUPPRESS)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        install_global(
            repo_root=args.repo_root,
            home=args.home,
            codex_home=args.codex_home,
            codex=args.codex,
            cachebuster=args.cachebuster,
        )
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
