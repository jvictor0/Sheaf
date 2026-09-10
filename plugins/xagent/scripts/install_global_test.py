from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import re
import select
import shlex
import shutil
import stat
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from unittest import mock

from plugins.xagent.scripts import install_global, package_xagent
from projects.agents.scripts.install_test import assert_xagent_subagents_sdd_guidance


REPO_ROOT = Path(__file__).resolve().parents[3]
PLUGIN_CREATOR_SCRIPTS = (
    Path.home() / ".codex" / "skills" / ".system" / "plugin-creator" / "scripts"
)
HELPER_NAMES = (
    "create_basic_plugin.py",
    "update_plugin_cachebuster.py",
    "read_marketplace_name.py",
    "validate_plugin.py",
    "identifier_validation.py",
)
FIXED_CACHEBUSTER = "test-20260725"
XAGENT_MCP_URL = "http://127.0.0.1:9005/mcp"
XAGENT_MCP_TIMEOUT_SEC = 7200
XAGENT_MCP_STARTUP_TIMEOUT_SEC = 30
EXPECTED_MCP_TOOL_NAMES = (
    "xagent_await",
    "xagent_close",
    "xagent_inspect",
    "xagent_interrupt",
    "xagent_list",
    "xagent_message",
    "xagent_sdd_followup",
    "xagent_sdd_start",
    "xagent_start_non_sdd",
)


def write_stub_mcp_json(plugin_root: Path) -> None:
    (plugin_root / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "xagent": {
                        "type": "http",
                        "url": XAGENT_MCP_URL,
                        "startup_timeout_sec": XAGENT_MCP_STARTUP_TIMEOUT_SEC,
                        "tool_timeout_sec": XAGENT_MCP_TIMEOUT_SEC,
                        "required": True,
                    }
                }
            }
        )
        + "\n",
        encoding="utf-8",
    )


def write_executable(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def tree_snapshot(root: Path) -> dict[str, tuple[int, str]]:
    snapshot: dict[str, tuple[int, str]] = {}
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        snapshot[str(path.relative_to(root))] = (
            stat.S_IMODE(path.stat().st_mode),
            hashlib.sha256(path.read_bytes()).hexdigest(),
        )
    return snapshot


def create_isolated_sheaf_root(parent: Path) -> Path:
    sheaf_root = parent / "sheaf-root"
    (sheaf_root / "config").mkdir(parents=True)
    (sheaf_root / "structure").mkdir(parents=True)
    (sheaf_root / "structure" / ".gitkeep").write_text("", encoding="utf-8")
    (sheaf_root / "config" / "services.json").write_text(
        json.dumps(
            [
                {
                    "name": "xagent",
                    "host": "127.0.0.1",
                    "port": 0,
                    "command": "make xagent-service-run",
                }
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    return sheaf_root


def create_sdd_capable_sheaf_root(parent: Path) -> tuple[Path, Path, Path]:
    sheaf_root = create_isolated_sheaf_root(parent)
    projects_link = sheaf_root / "projects"
    if not projects_link.exists():
        projects_link.symlink_to(REPO_ROOT / "projects", target_is_directory=True)
    templates_root = parent / "superpowers-templates" / "6.2.0" / "skills"
    implementer_template = templates_root / "subagent-driven-development" / "implementer-prompt.md"
    implementer_template.parent.mkdir(parents=True, exist_ok=True)
    implementer_template.write_text(
        "\n".join(
            [
                "# Implementer",
                "",
                "```",
                "Subagent (general-purpose):",
                '  description: "Do the thing"',
                "  model: [MODEL — REQUIRED: choose per SKILL.md]",
                "  prompt: |",
                "    You are implementing Task N: [task name]",
                "    Read your task brief first: [BRIEF_FILE]",
                "    [Scene-setting: where this fits, dependencies, architectural context]",
                "    Work from: [directory]",
                "    Write your full report to [REPORT_FILE]",
                "```",
                "",
            ]
        ),
        encoding="utf-8",
    )
    work = parent / "work"
    work.mkdir()
    plan = work / "plan.md"
    brief = work / "brief.md"
    report = work / "report.md"
    plan.write_text("# Plan\n\n## Task 1: Smoke\n\nDo it.\n", encoding="utf-8")
    brief.write_text("Packaged SDD brief for smoke.\n", encoding="utf-8")
    report.write_text("mutable artifact must not be stored\n", encoding="utf-8")
    subprocess.run(["git", "init", "-q"], cwd=work, check=True)
    subprocess.run(["git", "config", "user.email", "xagent@test"], cwd=work, check=True)
    subprocess.run(["git", "config", "user.name", "xagent"], cwd=work, check=True)
    (work / "seed.txt").write_text("seed\n", encoding="utf-8")
    subprocess.run(["git", "add", "-A"], cwd=work, check=True)
    subprocess.run(["git", "commit", "-qm", "seed"], cwd=work, check=True)
    return sheaf_root, work, templates_root


def spawn_isolated_xagent_service(
    sheaf_root: Path,
    *,
    extra_env: dict[str, str] | None = None,
) -> subprocess.Popen[str]:
    service_main = REPO_ROOT / "projects" / "xagent" / "dist" / "src" / "service_main.js"
    if not service_main.is_file():
        raise unittest.SkipTest(
            f"{service_main} is missing; build projects/xagent before running MCP probe"
        )
    env = os.environ.copy()
    if extra_env is not None:
        env.update(extra_env)
    return subprocess.Popen(
        ["node", str(service_main)],
        cwd=sheaf_root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )


def wait_for_service_port(
    process: subprocess.Popen[str],
    *,
    timeout_sec: float = 10.0,
) -> int:
    pattern = re.compile(r"xagent service listening on 127\.0\.0\.1:(\d+)")
    deadline = time.monotonic() + timeout_sec
    captured: list[str] = []
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stderr = process.stderr.read() if process.stderr is not None else ""
            raise RuntimeError(f"xagent service exited before announcing a port: {stderr}")
        if process.stderr is None:
            raise RuntimeError("xagent service stderr pipe is unavailable")
        line = process.stderr.readline()
        if line:
            captured.append(line)
            match = pattern.search(line)
            if match:
                return int(match.group(1))
        time.sleep(0.02)
    stderr = "".join(captured)
    if process.stderr is not None:
        stderr += process.stderr.read()
    raise RuntimeError(f"timed out waiting for xagent service port; stderr={stderr}")


def shutdown_xagent_service(process: subprocess.Popen[str], port: int) -> None:
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/exit",
        method="POST",
        data=b"",
    )
    try:
        with urllib.request.urlopen(request, timeout=5):
            pass
    except urllib.error.URLError:
        process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


@contextlib.contextmanager
def isolated_xagent_service(
    sheaf_root: Path,
    *,
    extra_env: dict[str, str] | None = None,
):
    service_process = spawn_isolated_xagent_service(sheaf_root, extra_env=extra_env)
    port: int | None = None
    try:
        port = wait_for_service_port(service_process)
        yield service_process, port
    finally:
        if service_process.poll() is None:
            if port is not None:
                shutdown_xagent_service(service_process, port)
            else:
                service_process.terminate()
                try:
                    service_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    service_process.kill()
                    service_process.wait(timeout=5)
        if service_process.stderr is not None:
            service_process.stderr.close()


def discover_mcp_tools(url: str) -> list[str]:
    probe = f"""
import {{ Client }} from "@modelcontextprotocol/sdk/client/index.js";
import {{ StreamableHTTPClientTransport }} from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const transport = new StreamableHTTPClientTransport(new URL({url!r}));
const client = new Client({{ name: "xagent-plugin-package-test", version: "0.0.0" }});
await client.connect(transport);
const listed = await client.listTools();
console.log(JSON.stringify(listed.tools.map((tool) => tool.name).sort()));
await client.close();
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", probe],
        cwd=REPO_ROOT / "projects" / "xagent",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return json.loads(result.stdout.strip())


def is_process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def run_packaged_cleanup_probe(service_url: str, log_root: Path, cwd: Path) -> None:
    probe = f"""
import {{ readFile }} from "node:fs/promises";
import {{ join }} from "node:path";
import {{ Client }} from "@modelcontextprotocol/sdk/client/index.js";
import {{ StreamableHTTPClientTransport }} from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const transport = new StreamableHTTPClientTransport(new URL({service_url!r}));
const client = new Client({{ name: "xagent-plugin-cleanup-test", version: "0.0.0" }});
await client.connect(transport);
const started = await client.callTool({{
  name: "xagent_start_non_sdd",
  arguments: {{ cwd: {str(cwd)!r}, prompt: "owned child cleanup", harness: "codex" }},
}});
const body = started.structuredContent ?? started.content?.[0]?.text;
const parsed = typeof body === "string" ? JSON.parse(body) : body;
const runId = parsed.run_id;
const metadata = JSON.parse(
  await readFile(join({str(log_root)!r}, runId, "metadata.json"), "utf8"),
);
const pid = metadata.owned_process?.pid;
if (typeof pid !== "number") {{
  throw new Error("expected owned_process.pid in metadata");
}}
await client.callTool({{ name: "xagent_close", arguments: {{ run_id: runId }} }});
try {{
  process.kill(pid, 0);
  throw new Error(`owned child ${{pid}} still alive after close`);
}} catch (error) {{
  if (error && typeof error === "object" && "code" in error && error.code === "ESRCH") {{
    console.log(JSON.stringify({{ closed: true, pid }}));
  }} else {{
    throw error;
  }}
}}
await client.close();
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", probe],
        cwd=REPO_ROOT / "projects" / "xagent",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        timeout=30,
    )
    payload = json.loads(result.stdout.strip())
    if payload.get("closed") is not True:
        raise RuntimeError(f"cleanup probe did not close owned child: {payload!r}")


def run_packaged_await_probe(service_url: str, cwd: Path) -> None:
    # I2: exercise xagent_await over the plugin MCP path. The fake adapter
    # (XAGENT_TEST_ADAPTER=fake + XAGENT_TEST_DELAY_MS) emits a delta, a tool
    # started/completed pair, then a final assistant message and turn
    # completion after the configured delay. The await must stay pending
    # through the intermediate progress and return once with the complete
    # final report. This is the only packaged coverage of the primary Codex
    # MCP await transport path.
    #
    probe = f"""
import {{ Client }} from "@modelcontextprotocol/sdk/client/index.js";
import {{ StreamableHTTPClientTransport }} from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const transport = new StreamableHTTPClientTransport(new URL({service_url!r}));
const client = new Client({{ name: "xagent-plugin-await-test", version: "0.0.0" }});
await client.connect(transport);

const started = await client.callTool({{
  name: "xagent_start_non_sdd",
  arguments: {{ cwd: {str(cwd)!r}, prompt: "packaged await smoke", harness: "codex" }},
}});
const startBody = started.structuredContent ?? started.content?.[0]?.text;
const startParsed = typeof startBody === "string" ? JSON.parse(startBody) : startBody;
const runId = startParsed.run_id;
if (typeof runId !== "string" || runId.length === 0) {{
  throw new Error(`xagent_start_non_sdd did not return a run_id: ${{JSON.stringify(startParsed)}}`);
}}

const inspected = await client.callTool({{
  name: "xagent_inspect",
  arguments: {{ run_id: runId }},
}});
const inspectBody = inspected.structuredContent ?? inspected.content?.[0]?.text;
const inspectParsed = typeof inspectBody === "string" ? JSON.parse(inspectBody) : inspectBody;
const cursor = inspectParsed.sequence;
if (typeof cursor !== "number" || cursor < 0) {{
  throw new Error(`xagent_inspect did not return a sequence: ${{JSON.stringify(inspectParsed)}}`);
}}

const awaited = await client.callTool({{
  name: "xagent_await",
  arguments: {{ run_id: runId, after_sequence: cursor, deadline_seconds: 15 }},
}});
const awaitBody = awaited.structuredContent ?? awaited.content?.[0]?.text;
const awaitParsed = typeof awaitBody === "string" ? JSON.parse(awaitBody) : awaitBody;

if (awaitParsed.event !== "turn.completed") {{
  throw new Error(`xagent_await did not settle on turn.completed: ${{JSON.stringify(awaitParsed)}}`);
}}
const reportText = awaitParsed.report?.text;
if (reportText !== "complete final assistant message") {{
  throw new Error(`xagent_await delivered an unexpected final report: ${{JSON.stringify(reportText)}}`);
}}
if (typeof awaitParsed.sequence !== "number" || awaitParsed.sequence <= cursor) {{
  throw new Error(`xagent_await returned a non-advancing sequence: ${{JSON.stringify(awaitParsed)}}`);
}}

await client.close();
console.log(JSON.stringify({{ event: awaitParsed.event, run_id: runId, sequence: awaitParsed.sequence }}));
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", probe],
        cwd=REPO_ROOT / "projects" / "xagent",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        timeout=60,
    )
    payload = json.loads(result.stdout.strip())
    if payload.get("event") != "turn.completed":
        raise RuntimeError(f"await probe did not deliver turn.completed: {payload!r}")


def run_packaged_sdd_probe(
    service_url: str,
    log_root: Path,
    cwd: Path,
    *,
    plan_path: Path,
    brief_path: Path,
    report_path: Path,
) -> None:
    probe = f"""
import {{ join }} from "node:path";
import Database from "better-sqlite3";
import {{ Client }} from "@modelcontextprotocol/sdk/client/index.js";
import {{ StreamableHTTPClientTransport }} from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const transport = new StreamableHTTPClientTransport(new URL({service_url!r}));
const client = new Client({{ name: "xagent-plugin-sdd-test", version: "0.0.0" }});
await client.connect(transport);

const started = await client.callTool({{
  name: "xagent_sdd_start",
  arguments: {{
    role: "implementer",
    cwd: {str(cwd)!r},
    plan: {str(plan_path)!r},
    model: "fake-model",
    harness: "codex",
    effort: "high",
    task: 1,
    name: "packaged-sdd-smoke",
    brief: {str(brief_path)!r},
    report_out: {str(report_path)!r},
  }},
}});
const startBody = started.structuredContent ?? started.content?.[0]?.text;
const startParsed = typeof startBody === "string" ? JSON.parse(startBody) : startBody;
const runId = startParsed.run_id;
if (typeof runId !== "string" || runId.length === 0) {{
  throw new Error(`xagent_sdd_start did not return a run_id: ${{JSON.stringify(startParsed)}}`);
}}
if (typeof startParsed.sequence !== "number") {{
  throw new Error(`xagent_sdd_start did not return a sequence: ${{JSON.stringify(startParsed)}}`);
}}

const database = new Database(join({str(log_root)!r}, "sdd.sqlite"), {{ readonly: true }});
const row = database.prepare(
  "SELECT role, task FROM sdd_agents WHERE agent_id = ?",
).get(runId);
database.close();
if (row?.role !== "implementer" || row?.task !== 1) {{
  throw new Error(`expected immutable sdd_agents row: ${{JSON.stringify(row)}}`);
}}

const expectedReport = "packaged sanitized SDD report";
const awaited = await client.callTool({{
  name: "xagent_await",
  arguments: {{
    run_id: runId,
    after_sequence: startParsed.sequence,
  }},
}});
const awaitBody = awaited.structuredContent ?? awaited.content?.[0]?.text;
const awaitParsed = typeof awaitBody === "string" ? JSON.parse(awaitBody) : awaitBody;
if (awaitParsed.event !== "turn.completed") {{
  throw new Error(`xagent_await did not settle on turn.completed: ${{JSON.stringify(awaitParsed)}}`);
}}
if (awaitParsed.report?.text !== expectedReport) {{
  throw new Error(`unexpected SDD report: ${{JSON.stringify(awaitParsed.report)}}`);
}}

await client.callTool({{ name: "xagent_close", arguments: {{ run_id: runId }} }});
await client.close();
console.log(JSON.stringify({{ event: awaitParsed.event, run_id: runId }}));
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", probe],
        cwd=REPO_ROOT / "projects" / "xagent",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        timeout=60,
    )
    payload = json.loads(result.stdout.strip())
    if payload.get("event") != "turn.completed":
        raise RuntimeError(f"sdd probe did not deliver turn.completed: {payload!r}")


class PackageXagentOutputTests(unittest.TestCase):
    def test_built_package_excludes_python_bytecode_and_cache_directories(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xagent-package-output-test-") as tempdir:
            root = Path(tempdir)
            plugin_root = root / "plugin"
            destination = root / "package"
            for directory in (".codex-plugin", "skills", "scripts"):
                (plugin_root / directory).mkdir(parents=True)
            write_stub_mcp_json(plugin_root)
            launcher = plugin_root / "scripts" / "xagent"
            write_executable(launcher, "#!/bin/sh\n")
            (plugin_root / "scripts" / "package_xagent.py").write_text(
                "# package builder\n",
                encoding="utf-8",
            )
            (plugin_root / "scripts" / "package_xagent.pyc").write_bytes(b"bytecode")
            cache_file = plugin_root / "scripts" / "__pycache__" / "package_xagent.cpython.pyc"
            cache_file.parent.mkdir()
            cache_file.write_bytes(b"cached bytecode")

            def copy_runtime(asset_root: Path) -> None:
                asset_root.mkdir(parents=True)
                (asset_root / "package.json").write_text(
                    '{"name":"xagent"}\n',
                    encoding="utf-8",
                )

            with (
                mock.patch.object(package_xagent, "PLUGIN_ROOT", plugin_root),
                mock.patch.object(package_xagent, "copy_runtime", copy_runtime),
            ):
                package_xagent.build_package(destination)

            self.assertTrue((destination / "scripts" / "xagent").is_file())
            self.assertTrue(os.access(destination / "scripts" / "xagent", os.X_OK))
            self.assertEqual([], list(destination.rglob("__pycache__")))
            self.assertEqual([], list(destination.rglob("*.pyc")))

    def test_non_empty_output_is_rejected_without_deleting_contents(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xagent-package-output-test-") as tempdir:
            destination = Path(tempdir) / "existing-output"
            destination.mkdir()
            sentinel = destination / "keep.txt"
            sentinel.write_text("user data\n", encoding="utf-8")

            with (
                mock.patch.object(package_xagent, "copy_runtime"),
                self.assertRaisesRegex(RuntimeError, "non-empty"),
            ):
                package_xagent.build_package(destination)

            self.assertEqual("user data\n", sentinel.read_text(encoding="utf-8"))
            self.assertEqual(["keep.txt"], [path.name for path in destination.iterdir()])

    def test_check_tracked_assets_accepts_current_assets(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xagent-package-check-test-") as tempdir:
            root = Path(tempdir)
            plugin_root = root / "plugin"
            asset_root = plugin_root / "assets" / "xagent"
            for directory in (".codex-plugin", "skills", "scripts"):
                (plugin_root / directory).mkdir(parents=True)
            write_stub_mcp_json(plugin_root)
            asset_root.mkdir(parents=True)
            (asset_root / "package.json").write_text('{"name":"xagent"}\n', encoding="utf-8")

            def copy_runtime(destination: Path) -> None:
                destination.mkdir(parents=True)
                (destination / "package.json").write_text('{"name":"xagent"}\n', encoding="utf-8")

            with (
                mock.patch.object(package_xagent, "PLUGIN_ROOT", plugin_root),
                mock.patch.object(package_xagent, "ASSET_ROOT", asset_root),
                mock.patch.object(package_xagent, "copy_runtime", copy_runtime),
            ):
                package_xagent.check_tracked_assets_current()

    def test_check_tracked_assets_reports_stale_assets_without_rewriting(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xagent-package-check-test-") as tempdir:
            root = Path(tempdir)
            plugin_root = root / "plugin"
            asset_root = plugin_root / "assets" / "xagent"
            for directory in (".codex-plugin", "skills", "scripts"):
                (plugin_root / directory).mkdir(parents=True)
            write_stub_mcp_json(plugin_root)
            asset_root.mkdir(parents=True)
            tracked = asset_root / "package.json"
            tracked.write_text('{"name":"stale"}\n', encoding="utf-8")

            def copy_runtime(destination: Path) -> None:
                destination.mkdir(parents=True)
                (destination / "package.json").write_text('{"name":"current"}\n', encoding="utf-8")

            with (
                mock.patch.object(package_xagent, "PLUGIN_ROOT", plugin_root),
                mock.patch.object(package_xagent, "ASSET_ROOT", asset_root),
                mock.patch.object(package_xagent, "copy_runtime", copy_runtime),
                self.assertRaisesRegex(RuntimeError, "tracked xagent plugin assets are stale"),
            ):
                package_xagent.check_tracked_assets_current()

            self.assertEqual('{"name":"stale"}\n', tracked.read_text(encoding="utf-8"))

    def test_check_main_validates_temporary_package_before_cleanup(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xagent-package-check-test-") as tempdir:
            root = Path(tempdir)
            asset_root = root / "plugin" / "assets" / "xagent"
            asset_root.mkdir(parents=True)
            (asset_root / "package.json").write_text('{"name":"xagent"}\n', encoding="utf-8")
            validated_roots: list[Path] = []

            def build_package(destination: Path) -> None:
                (destination / "assets" / "xagent").mkdir(parents=True)
                (destination / "assets" / "xagent" / "package.json").write_text(
                    '{"name":"xagent"}\n',
                    encoding="utf-8",
                )
                (destination / "scripts").mkdir()
                (destination / "scripts" / "xagent").write_text("#!/bin/sh\n", encoding="utf-8")

            def validate_package(plugin_root: Path) -> None:
                if not plugin_root.exists():
                    raise RuntimeError(f"validated after cleanup: {plugin_root}")
                validated_roots.append(plugin_root)

            with (
                mock.patch.object(package_xagent, "parse_args", return_value=mock.Mock(check=True, output=None)),
                mock.patch.object(package_xagent, "ASSET_ROOT", asset_root),
                mock.patch.object(package_xagent, "build_package", build_package),
                mock.patch.object(package_xagent, "validate_package", validate_package),
            ):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(0, package_xagent.main())

            self.assertEqual(1, len(validated_roots))

    def test_check_main_uses_shared_drift_guard(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xagent-package-check-test-") as tempdir:
            asset_root = Path(tempdir) / "plugin" / "assets" / "xagent"
            asset_root.mkdir(parents=True)
            (asset_root / "package.json").write_text('{"name":"xagent"}\n', encoding="utf-8")
            calls: list[bool] = []

            def build_package(destination: Path) -> None:
                (destination / "assets" / "xagent").mkdir(parents=True)
                (destination / "assets" / "xagent" / "package.json").write_text(
                    '{"name":"xagent"}\n',
                    encoding="utf-8",
                )

            def check_tracked_assets_current(*, validate: bool = False) -> None:
                calls.append(validate)

            with (
                mock.patch.object(
                    package_xagent,
                    "parse_args",
                    return_value=mock.Mock(check=True, output=None),
                ),
                mock.patch.object(package_xagent, "ASSET_ROOT", asset_root),
                mock.patch.object(package_xagent, "build_package", build_package),
                mock.patch.object(package_xagent, "validate_package"),
                mock.patch.object(
                    package_xagent,
                    "check_tracked_assets_current",
                    check_tracked_assets_current,
                ),
            ):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(0, package_xagent.main())

            self.assertEqual([True], calls)

    def test_package_validation_requires_the_hook_program_without_discovery(self) -> None:
        # xhook-8: the hook program must ship under scripts/, and nothing in
        # the package may register it through Codex component discovery.
        with tempfile.TemporaryDirectory(prefix="xagent-package-hooks-test-") as tempdir:
            package_root = Path(tempdir) / "package"
            manifest_path = package_root / ".codex-plugin" / "plugin.json"
            manifest_path.parent.mkdir(parents=True)
            manifest_path.write_text('{"name": "xagent"}\n', encoding="utf-8")
            hook = package_root / "scripts" / "controller_stop_hook.py"
            hook.parent.mkdir(parents=True)
            hook.write_text("#!/usr/bin/env python3\n", encoding="utf-8")

            package_xagent.validate_hook_registration(package_root)

            hook.rename(hook.with_name("controller_stop_hook.py.moved"))
            with self.assertRaisesRegex(RuntimeError, "controller_stop_hook.py"):
                package_xagent.validate_hook_registration(package_root)
            hook.with_name("controller_stop_hook.py.moved").rename(hook)

            for discoverable in (package_root / "hooks.json", package_root / "hooks"):
                if discoverable.name.endswith(".json"):
                    discoverable.write_text("{}\n", encoding="utf-8")
                else:
                    discoverable.mkdir()
                with self.assertRaisesRegex(RuntimeError, "discovery"):
                    package_xagent.validate_hook_registration(package_root)
                if discoverable.is_dir():
                    discoverable.rmdir()
                else:
                    discoverable.unlink()

            manifest_path.write_text(
                json.dumps({"name": "xagent", "hooks": "./hooks.json"}) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "hooks"):
                package_xagent.validate_hook_registration(package_root)

    def test_packaged_plugin_declares_http_mcp_without_local_supervisor(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xagent-package-mcp-test-") as tempdir:
            destination = Path(tempdir) / "package"
            package_xagent.build_package(destination)

            manifest = json.loads(
                (destination / ".codex-plugin" / "plugin.json").read_text(encoding="utf-8")
            )
            self.assertEqual("./.mcp.json", manifest.get("mcpServers"))

            mcp_path = destination / ".mcp.json"
            self.assertTrue(mcp_path.is_file(), ".mcp.json must be packaged")
            mcp = json.loads(mcp_path.read_text(encoding="utf-8"))
            xagent = mcp["mcpServers"]["xagent"]
            self.assertEqual("http", xagent["type"])
            self.assertEqual(XAGENT_MCP_URL, xagent["url"])
            self.assertEqual(
                XAGENT_MCP_STARTUP_TIMEOUT_SEC,
                xagent["startup_timeout_sec"],
            )
            self.assertEqual(XAGENT_MCP_TIMEOUT_SEC, xagent["tool_timeout_sec"])
            self.assertIs(xagent["required"], True)
            self.assertNotIn("command", xagent)

            runtime_root = destination / "assets" / "xagent" / "dist" / "src"
            self.assertFalse((runtime_root / "service_main.js").exists())
            service_root = runtime_root / "service"
            self.assertTrue((service_root / "client.js").is_file())
            self.assertTrue((service_root / "config.js").is_file())
            self.assertTrue((service_root / "tool_schemas.js").is_file())
            self.assertFalse((service_root / "mcp.js").exists())
            self.assertFalse((service_root / "run_manager.js").exists())
            self.assertFalse((service_root / "server.js").exists())

            sheaf_root = create_isolated_sheaf_root(Path(tempdir))
            with isolated_xagent_service(sheaf_root) as (_service_process, port):
                tool_names = discover_mcp_tools(f"http://127.0.0.1:{port}/mcp")
                self.assertEqual(list(EXPECTED_MCP_TOOL_NAMES), tool_names)

    def test_packaged_quiet_cli_supervise_stays_silent_until_completion(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xagent-package-quiet-test-") as tempdir:
            destination = Path(tempdir) / "package"
            package_xagent.build_package(destination)

            sheaf_root = create_isolated_sheaf_root(Path(tempdir))
            log_root = sheaf_root / "data" / "xagent"
            cwd = Path(tempdir) / "work"
            cwd.mkdir()
            with isolated_xagent_service(
                sheaf_root,
                extra_env={
                    "XAGENT_TEST_ADAPTER": "fake",
                    "XAGENT_TEST_DELAY_MS": "1500",
                },
            ) as (_service_process, port):
                service_url = f"http://127.0.0.1:{port}"
                launcher = destination / "scripts" / "xagent"
                env = os.environ.copy()
                env["XAGENT_SERVICE_URL"] = service_url
                env["XAGENT_LOG_ROOT"] = str(log_root)
                proc = subprocess.Popen(
                    [
                        str(launcher),
                        "supervise",
                        "--harness",
                        "codex",
                        "--cwd",
                        str(cwd),
                        "--deadline-seconds",
                        "30",
                        "packaged quiet smoke",
                    ],
                    cwd=cwd,
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                try:
                    time.sleep(0.5)
                    self.assertIsNone(proc.poll(), "supervise finished before quiet-progress check")
                    if proc.stdout is not None:
                        ready, _, _ = select.select([proc.stdout], [], [], 0)
                        if ready:
                            partial = proc.stdout.read()
                            if partial:
                                self.fail(f"quiet supervise emitted stdout during progress: {partial!r}")
                    completed = proc.wait(timeout=30)
                    stdout = proc.stdout.read() if proc.stdout is not None else ""
                    stderr = proc.stderr.read() if proc.stderr is not None else ""
                    self.assertEqual(completed, 0, stderr)
                    lines = [line for line in stdout.strip().splitlines() if line.strip()]
                    self.assertEqual(1, len(lines))
                    body = json.loads(lines[0])
                    self.assertEqual("turn.completed", body.get("event"))
                    self.assertEqual(
                        "complete final assistant message",
                        body.get("report", {}).get("text"),
                    )
                finally:
                    if proc.poll() is None:
                        proc.kill()
                        proc.wait(timeout=5)

    def test_packaged_cleanup_smoke_closes_owned_child(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xagent-package-cleanup-test-") as tempdir:
            destination = Path(tempdir) / "package"
            package_xagent.build_package(destination)

            sheaf_root = create_isolated_sheaf_root(Path(tempdir))
            log_root = sheaf_root / "data" / "xagent"
            cwd = Path(tempdir) / "work"
            cwd.mkdir()
            with isolated_xagent_service(
                sheaf_root,
                extra_env={
                    "XAGENT_TEST_ADAPTER": "fake",
                    "XAGENT_TEST_OWNED_CHILD": "1",
                },
            ) as (_service_process, port):
                run_packaged_cleanup_probe(
                    f"http://127.0.0.1:{port}/mcp",
                    log_root,
                    cwd,
                )

    def test_packaged_mcp_await_smoke_delivers_final_report(self) -> None:
        # xa-20 / I2: exercise xagent_await over the plugin MCP path. The
        # fake adapter emits a delta and a tool started/completed pair before
        # the final assistant message and turn completion, so the await must
        # stay pending through that progress and return once with the
        # complete final report. This is the only packaged coverage of the
        # primary Codex MCP await transport path; the blocking-wait/final
        # report smoke previously ran through the quiet CLI only.
        #
        with tempfile.TemporaryDirectory(prefix="xagent-package-await-test-") as tempdir:
            destination = Path(tempdir) / "package"
            package_xagent.build_package(destination)

            sheaf_root = create_isolated_sheaf_root(Path(tempdir))
            cwd = Path(tempdir) / "work"
            cwd.mkdir()
            with isolated_xagent_service(
                sheaf_root,
                extra_env={
                    "XAGENT_TEST_ADAPTER": "fake",
                    "XAGENT_TEST_DELAY_MS": "800",
                },
            ) as (_service_process, port):
                run_packaged_await_probe(
                    f"http://127.0.0.1:{port}/mcp",
                    cwd,
                )

    def test_packaged_mcp_sdd_smoke_delivers_report_via_generic_await(self) -> None:
        # Exercise xagent_sdd_start plus generic xagent_await over the plugin
        # MCP path. The v2 ledger stores identity only; the report lives in
        # turn.completed and is delivered by xagent_await with run_id.
        #
        with tempfile.TemporaryDirectory(prefix="xagent-package-sdd-test-") as tempdir:
            destination = Path(tempdir) / "package"
            package_xagent.build_package(destination)

            sheaf_root, work, templates_root = create_sdd_capable_sheaf_root(Path(tempdir))
            log_root = sheaf_root / "data" / "xagent"
            plan_path = work / "plan.md"
            brief_path = work / "brief.md"
            report_path = work / "report.md"
            with isolated_xagent_service(
                sheaf_root,
                extra_env={
                    "XAGENT_TEST_ADAPTER": "fake",
                    "XAGENT_TEST_DELAY_MS": "800",
                    "XAGENT_TEST_SDD_REPORTS": "packaged sanitized SDD report",
                    "SUPERPOWERS_TEMPLATES_ROOT": str(templates_root),
                },
            ) as (_service_process, port):
                run_packaged_sdd_probe(
                    f"http://127.0.0.1:{port}/mcp",
                    log_root,
                    work,
                    plan_path=plan_path,
                    brief_path=brief_path,
                    report_path=report_path,
                )


class GlobalPluginInstallTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="xagent-global-install-test-")
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.codex_home = self.root / "codex-home"
        self.repo_root = self.root / "repo"
        self.bin_root = self.root / "bin"
        self.codex_log = self.root / "codex.log"
        self.destination = self.home / ".agents" / "plugins" / "plugins" / "xagent"
        self.marketplace_path = self.home / ".agents" / "plugins" / "marketplace.json"

        plugin_root = self.repo_root / "plugins" / "xagent"
        for directory in (".codex-plugin", "skills", "scripts"):
            shutil.copytree(REPO_ROOT / "plugins" / "xagent" / directory, plugin_root / directory)
        shutil.copy2(REPO_ROOT / "plugins" / "xagent" / ".mcp.json", plugin_root / ".mcp.json")

        xagent_root = self.repo_root / "projects" / "xagent"
        real_xagent = REPO_ROOT / "projects" / "xagent"
        xagent_root.mkdir(parents=True)
        shutil.copy2(real_xagent / "package.json", xagent_root / "package.json")
        if (real_xagent / "package-lock.json").is_file():
            shutil.copy2(real_xagent / "package-lock.json", xagent_root / "package-lock.json")
        shutil.copytree(
            real_xagent / "dist" / "src",
            xagent_root / "dist" / "src",
            dirs_exist_ok=True,
        )
        if (real_xagent / "node_modules").is_dir():
            node_modules_link = xagent_root / "node_modules"
            if not node_modules_link.exists():
                node_modules_link.symlink_to(
                    real_xagent / "node_modules",
                    target_is_directory=True,
                )

        helper_root = (
            self.codex_home / "skills" / ".system" / "plugin-creator" / "scripts"
        )
        helper_root.mkdir(parents=True)
        for helper_name in HELPER_NAMES:
            shutil.copy2(PLUGIN_CREATOR_SCRIPTS / helper_name, helper_root / helper_name)
        self.helper_root = helper_root

        write_executable(
            self.bin_root / "npm",
            """#!/usr/bin/env python3
raise SystemExit(0)
""",
        )
        write_executable(
            self.bin_root / "node",
            """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

args = sys.argv[1:]
if args and args[0] == "-p":
    print("20")
elif args and args[0] == "--version":
    print("v20.0.0")
elif len(args) >= 2 and args[1] == "--help":
    print("xagent run --harness HARNESS --subagent PROMPT")
elif len(args) >= 2 and args[1] == "run":
    Path(os.environ["XAGENT_LOG_ROOT"]).mkdir(parents=True, exist_ok=True)
else:
    raise SystemExit(f"unexpected fake node arguments: {args!r}")
""",
        )
        write_executable(
            self.bin_root / "codex",
            """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

args = sys.argv[1:]
log_path = Path(os.environ["FAKE_CODEX_LOG"])
with log_path.open("a", encoding="utf-8") as handle:
    handle.write(" ".join(args) + "\\n")
if os.environ.get("FAKE_CODEX_FAIL") == "add" and args[:2] == ["plugin", "add"]:
    print("fake codex add failure", file=sys.stderr)
    raise SystemExit(23)
if os.environ.get("FAKE_CODEX_FAIL") == "mcp" and args == ["mcp", "get", "xagent"]:
    print("fake codex MCP resolution failure", file=sys.stderr)
    raise SystemExit(24)
if args == ["plugin", "list"]:
    print("NAME    VERSION    STATUS               PATH")
    print(
        "xagent  0.1.0     installed, enabled   "
        + str(Path(os.environ["FAKE_CODEX_PLUGIN_PATH"]).resolve())
    )
elif args == ["mcp", "get", "xagent"]:
    print("xagent MCP configured")
""",
        )
        self.environment = mock.patch.dict(
            os.environ,
            {
                "PATH": f"{self.bin_root}{os.pathsep}{os.environ.get('PATH', '')}",
                "FAKE_CODEX_LOG": str(self.codex_log),
                "FAKE_CODEX_PLUGIN_PATH": str(self.destination),
            },
        )
        self.environment.start()

    def tearDown(self) -> None:
        self.environment.stop()
        self.temporary.cleanup()

    def install(self) -> str:
        return self.install_with_codex(self.bin_root / "codex")

    def install_with_codex(self, codex: Path) -> str:
        # Installation prints the Codex trust notice; capturing it keeps the
        # test output pristine and lets tests assert on the notice.
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            install_global.install_global(
                repo_root=self.repo_root,
                home=self.home,
                codex_home=self.codex_home,
                codex=str(codex),
                cachebuster=FIXED_CACHEBUSTER,
            )
        return stdout.getvalue()

    def test_new_marketplace_defaults_to_personal(self) -> None:
        self.install()

        marketplace = json.loads(self.marketplace_path.read_text(encoding="utf-8"))
        self.assertEqual("personal", marketplace["name"])
        self.assertEqual(
            [
                {
                    "name": "xagent",
                    "source": {
                        "source": "local",
                        "path": "./.agents/plugins/plugins/xagent",
                    },
                    "policy": {
                        "installation": "AVAILABLE",
                        "authentication": "ON_INSTALL",
                    },
                    "category": "Productivity",
                }
            ],
            marketplace["plugins"],
        )
        self.assertEqual(
            ["plugin add xagent@personal", "plugin list", "mcp get xagent"],
            self.codex_log.read_text(encoding="utf-8").splitlines(),
        )

    def test_existing_marketplace_name_interface_and_unrelated_entries_survive(self) -> None:
        existing = {
            "name": "joyo_local",
            "interface": {
                "displayName": "Joyo's Plugins",
                "description": "Personal development plugins",
            },
            "plugins": [
                {
                    "name": "other",
                    "source": {"source": "local", "path": "./plugins/other"},
                    "policy": {
                        "installation": "NOT_AVAILABLE",
                        "authentication": "ON_USE",
                    },
                    "category": "Other",
                }
            ],
        }
        self.marketplace_path.parent.mkdir(parents=True)
        self.marketplace_path.write_text(json.dumps(existing) + "\n", encoding="utf-8")

        self.install()

        marketplace = json.loads(self.marketplace_path.read_text(encoding="utf-8"))
        self.assertEqual(existing["name"], marketplace["name"])
        self.assertEqual(existing["interface"], marketplace["interface"])
        self.assertEqual(existing["plugins"][0], marketplace["plugins"][0])
        self.assertEqual(["other", "xagent"], [entry["name"] for entry in marketplace["plugins"]])
        self.assertEqual(
            "./.agents/plugins/plugins/xagent",
            marketplace["plugins"][1]["source"]["path"],
        )
        self.assertIn(
            "plugin add xagent@joyo_local",
            self.codex_log.read_text(encoding="utf-8").splitlines(),
        )

    def test_second_install_updates_one_xagent_entry(self) -> None:
        self.install()
        self.install()

        marketplace = json.loads(self.marketplace_path.read_text(encoding="utf-8"))
        xagent_entries = [
            entry for entry in marketplace["plugins"] if entry.get("name") == "xagent"
        ]
        self.assertEqual(1, len(xagent_entries))
        self.assertEqual(
            "sheaf-xagent-plugin\n",
            (self.destination / ".sheaf-managed").read_text(encoding="utf-8"),
        )
        self.assertEqual(
            2,
            self.codex_log.read_text(encoding="utf-8")
            .splitlines()
            .count("plugin add xagent@personal"),
        )

    def test_unmanaged_destination_is_rejected(self) -> None:
        self.destination.mkdir(parents=True)
        sentinel = self.destination / "keep.txt"
        sentinel.write_text("unmanaged\n", encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "unmanaged"):
            self.install()

        self.assertEqual("unmanaged\n", sentinel.read_text(encoding="utf-8"))
        self.assertFalse(self.marketplace_path.exists())
        self.assertFalse(self.codex_log.exists())

        shutil.rmtree(self.destination)
        self.destination.write_text("unmanaged file\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "unmanaged"):
            self.install()

        self.assertEqual("unmanaged file\n", self.destination.read_text(encoding="utf-8"))

    def test_staged_package_has_marker_cachebuster_and_executable_launcher(self) -> None:
        self.install()

        self.assertEqual(
            "sheaf-xagent-plugin\n",
            (self.destination / ".sheaf-managed").read_text(encoding="utf-8"),
        )
        manifest = json.loads(
            (self.destination / ".codex-plugin" / "plugin.json").read_text(encoding="utf-8")
        )
        self.assertEqual("0.1.0+codex.test-20260725", manifest["version"])
        self.assertTrue(os.access(self.destination / "scripts" / "xagent", os.X_OK))
        self.assertTrue((self.destination / "skills" / "xagent-subagents" / "SKILL.md").is_file())
        packaged_skill = (
            self.destination / "skills" / "xagent-subagents" / "SKILL.md"
        ).read_text(encoding="utf-8")
        assert_xagent_subagents_sdd_guidance(self, packaged_skill)
        self.assertIn("xagent_sdd_start", packaged_skill)
        self.assertIn("xagent_sdd_followup", packaged_skill)
        self.assertNotIn("xagent_sdd_await", packaged_skill)
        self.assertNotIn("xagent_sdd_close", packaged_skill)
        for role in ("implementer", "reviewer", "fixer", "re-reviewer"):
            self.assertIn(role, packaged_skill)
        self.assertIn("report", packaged_skill.lower())
        self.assertIn("deadline_seconds", packaged_skill)
        self.assertIn("no longer", packaged_skill.lower())
        self.assertTrue((self.destination / "assets" / "xagent" / "dist" / "src" / "main.js").is_file())
        with self.assertRaisesRegex(RuntimeError, "installed and enabled"):
            install_global.require_installed_plugin(
                f"xagent  enabled  {self.destination.resolve()}\n",
                self.destination,
            )

    def test_non_default_home_is_used_for_codex_marketplace_resolution(self) -> None:
        write_executable(
            self.bin_root / "codex-home-aware",
            """#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

expected_home = Path(os.environ["EXPECTED_HOME"]).resolve()
actual_home = Path(os.environ["HOME"]).resolve()
if actual_home != expected_home:
    print(f"HOME mismatch: {actual_home} != {expected_home}", file=sys.stderr)
    raise SystemExit(24)
args = sys.argv[1:]
if args == ["plugin", "add", "xagent@personal"]:
    raise SystemExit(0)
if args == ["plugin", "list"]:
    marketplace = json.loads(
        (actual_home / ".agents" / "plugins" / "marketplace.json").read_text(
            encoding="utf-8"
        )
    )
    [entry] = [item for item in marketplace["plugins"] if item["name"] == "xagent"]
    source_path = Path(entry["source"]["path"])
    if not source_path.is_absolute():
        source_path = actual_home / source_path
    print("NAME    VERSION    STATUS               PATH")
    print("xagent  0.1.0     installed, enabled   " + str(source_path.resolve()))
    raise SystemExit(0)
if args == ["mcp", "get", "xagent"]:
    print("xagent MCP configured")
    raise SystemExit(0)
raise SystemExit(f"unexpected fake codex arguments: {args!r}")
""",
        )
        with mock.patch.dict(os.environ, {"EXPECTED_HOME": str(self.home)}):
            self.install_with_codex(self.bin_root / "codex-home-aware")

        marketplace = json.loads(self.marketplace_path.read_text(encoding="utf-8"))
        [entry] = [item for item in marketplace["plugins"] if item["name"] == "xagent"]
        self.assertEqual(
            f"./{self.destination.relative_to(self.home).as_posix()}",
            entry["source"]["path"],
        )

    def test_tracked_plugin_tree_is_unchanged(self) -> None:
        plugin_root = self.repo_root / "plugins" / "xagent"
        before = tree_snapshot(plugin_root)

        self.install()

        self.assertEqual(before, tree_snapshot(plugin_root))
        self.assertFalse((plugin_root / "assets").exists())

    def test_hooks_are_registered_for_claude_and_codex(self) -> None:
        # xhook-1: a completed global install must leave both harnesses running
        # the hook program that the install just committed.
        self.install()

        installed_hook = self.destination / "scripts" / "controller_stop_hook.py"
        self.assertTrue(installed_hook.is_file())
        state_root = (
            self.home / ".agents" / "plugins" / "data" / "xagent" / "controller-stop-hooks"
        )
        for harness, path in (
            ("claude", self.home / ".claude" / "settings.json"),
            ("codex", self.codex_home / "hooks.json"),
        ):
            payload = json.loads(path.read_text(encoding="utf-8"))
            observe = payload["hooks"]["PostToolUse"][-1]["hooks"][0]["command"]
            guard = payload["hooks"]["Stop"][-1]["hooks"][0]["command"]
            for command, mode in ((observe, "observe"), (guard, "guard")):
                self.assertIn(str(installed_hook.resolve()), command)
                self.assertIn(f"--harness {harness}", command)
                self.assertIn(f"--state-root {state_root.resolve()}", command)
                self.assertTrue(command.endswith(f" {mode}"), command)

    def test_installed_package_contains_script_but_no_discoverable_plugin_hooks(self) -> None:
        # xhook-8: global JSON is the only registration path, so the package
        # must ship the program without any component Codex would discover.
        self.install()

        self.assertTrue((self.destination / "scripts" / "controller_stop_hook.py").is_file())
        self.assertFalse((self.destination / "hooks.json").exists())
        self.assertFalse((self.destination / "hooks").exists())
        manifest = json.loads(
            (self.destination / ".codex-plugin" / "plugin.json").read_text(encoding="utf-8")
        )
        self.assertNotIn("hooks", manifest)

    def test_missing_helper_and_failed_codex_command_are_reported(self) -> None:
        (self.helper_root / "validate_plugin.py").unlink()
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = install_global.main(
                [
                    "install",
                    "--repo-root",
                    str(self.repo_root),
                    "--home",
                    str(self.home),
                    "--codex-home",
                    str(self.codex_home),
                    "--codex",
                    str(self.bin_root / "codex"),
                    "--cachebuster",
                    FIXED_CACHEBUSTER,
                ]
            )
        self.assertNotEqual(0, result)
        self.assertIn("validate_plugin.py", stderr.getvalue())

        shutil.copy2(
            PLUGIN_CREATOR_SCRIPTS / "validate_plugin.py",
            self.helper_root / "validate_plugin.py",
        )
        os.environ["FAKE_CODEX_FAIL"] = "add"
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = install_global.main(
                [
                    "install",
                    "--repo-root",
                    str(self.repo_root),
                    "--home",
                    str(self.home),
                    "--codex-home",
                    str(self.codex_home),
                    "--codex",
                    str(self.bin_root / "codex"),
                    "--cachebuster",
                    FIXED_CACHEBUSTER,
                ]
            )
        self.assertNotEqual(0, result)
        self.assertIn("plugin add xagent@personal", stderr.getvalue())

        os.environ["FAKE_CODEX_FAIL"] = "mcp"
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = install_global.main(
                [
                    "install",
                    "--repo-root",
                    str(self.repo_root),
                    "--home",
                    str(self.home),
                    "--codex-home",
                    str(self.codex_home),
                    "--codex",
                    str(self.bin_root / "codex"),
                    "--cachebuster",
                    FIXED_CACHEBUSTER,
                ]
            )
        self.assertNotEqual(0, result)
        self.assertIn("mcp get xagent", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()


class LauncherPortabilityTests(unittest.TestCase):
    def test_launcher_has_no_hardcoded_user_path(self) -> None:
        launcher = (REPO_ROOT / "plugins/xagent/scripts/xagent").read_text(encoding="utf-8")
        self.assertNotIn("/Users/", launcher)
        self.assertIn("SHEAF_XAGENT_LOG_ROOT", launcher)
        self.assertIn(".xagent/data", launcher)

    def test_launcher_discovers_the_sheaf_root_from_the_working_directory(self) -> None:
        with tempfile.TemporaryDirectory() as raw_base:
            base = Path(raw_base).resolve()
            sheaf_root = base / "some-checkout"
            (sheaf_root / "config").mkdir(parents=True)
            (sheaf_root / "structure").mkdir()
            (sheaf_root / "config" / "services.json").write_text("[]\n", encoding="utf-8")
            nested = sheaf_root / "projects" / "deep"
            nested.mkdir(parents=True)

            launcher = (REPO_ROOT / "plugins/xagent/scripts/xagent").read_text(encoding="utf-8")
            start = launcher.index("find_sheaf_root()")
            end = launcher.index("\n", launcher.index("export XAGENT_LOG_ROOT="))
            slice_path = base / "log_root_slice.sh"
            slice_path.write_text(launcher[start:end] + "\n", encoding="utf-8")

            probe = base / "probe.sh"
            probe.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                f'. "{slice_path}"\n'
                'printf "%s\\n" "${XAGENT_LOG_ROOT}"\n',
                encoding="utf-8",
            )
            probe.chmod(0o755)

            found = subprocess.run(
                ["bash", str(probe)],
                cwd=str(nested),
                capture_output=True,
                text=True,
                check=True,
                env={**os.environ, "XAGENT_LOG_ROOT": "", "SHEAF_XAGENT_LOG_ROOT": ""},
            ).stdout.strip()
            self.assertEqual(found, str(sheaf_root / "data" / "xagent"))

            outside = subprocess.run(
                ["bash", str(probe)],
                cwd=str(base),
                capture_output=True,
                text=True,
                check=True,
                env={**os.environ, "XAGENT_LOG_ROOT": "", "SHEAF_XAGENT_LOG_ROOT": ""},
            ).stdout.strip()
            self.assertEqual(outside, str(Path.home() / ".xagent" / "data"))

    def test_launcher_resolves_the_main_checkout_from_inside_a_worktree(self) -> None:
        # A Sheaf worktree carries config/services.json and structure/ too, so a
        # naive upward walk would resolve the worktree's own data/ while the
        # service writes to the main checkout's. The launcher must agree with
        # the service or `xagent list` reports an empty log root.
        launcher = (REPO_ROOT / "plugins/xagent/scripts/xagent").read_text(encoding="utf-8")
        start = launcher.index("find_sheaf_root()")
        end = launcher.index("\n", launcher.index("export XAGENT_LOG_ROOT="))
        with tempfile.TemporaryDirectory() as raw_base:
            base = Path(raw_base).resolve()
            slice_path = base / "log_root_slice.sh"
            slice_path.write_text(launcher[start:end] + "\n", encoding="utf-8")

            main = base / "main-checkout"
            (main / "config").mkdir(parents=True)
            (main / "structure").mkdir()
            (main / "config" / "services.json").write_text("[]\n", encoding="utf-8")
            for command in (
                ["git", "init", "-q", "-b", "main"],
                ["git", "config", "user.email", "t@example.com"],
                ["git", "config", "user.name", "t"],
                ["git", "add", "-A"],
                ["git", "commit", "-qm", "seed"],
            ):
                subprocess.run(command, cwd=str(main), check=True, capture_output=True)

            tree = base / "wt"
            subprocess.run(
                ["git", "worktree", "add", "-q", "-b", "feature", str(tree)],
                cwd=str(main),
                check=True,
                capture_output=True,
            )

            probe = base / "probe.sh"
            probe.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                f'. "{slice_path}"\n'
                'printf "%s\\n" "${XAGENT_LOG_ROOT}"\n',
                encoding="utf-8",
            )
            probe.chmod(0o755)

            resolved = subprocess.run(
                ["bash", str(probe)],
                cwd=str(tree),
                capture_output=True,
                text=True,
                check=True,
                env={**os.environ, "XAGENT_LOG_ROOT": "", "SHEAF_XAGENT_LOG_ROOT": ""},
            ).stdout.strip()
            self.assertEqual(resolved, str(main / "data" / "xagent"))


class AllHarnessDistributionTests(unittest.TestCase):
    """A1/A2: every harness must reach xagent, not just Codex."""

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.home = Path(self.temporary.name) / "home"
        self.home.mkdir(parents=True)
        self.codex_home = Path(self.temporary.name) / "codex-home"
        self.claude_settings = self.home / ".claude" / "settings.json"
        self.codex_hooks = self.codex_home / "hooks.json"
        self.installed_hook = (
            self.home
            / ".agents"
            / "plugins"
            / "plugins"
            / "xagent"
            / "scripts"
            / "controller_stop_hook.py"
        )
        self.installed_hook.parent.mkdir(parents=True)
        self.installed_hook.write_text("#!/usr/bin/env python3\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def register_hooks(self) -> str:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            install_global.register_harness_hooks(
                home=self.home,
                codex_home=self.codex_home,
                installed_hook=self.installed_hook,
            )
        return stdout.getvalue()

    def expected_command(self, harness: str, mode: str) -> str:
        state_root = (
            self.home.resolve()
            / ".agents"
            / "plugins"
            / "data"
            / "xagent"
            / "controller-stop-hooks"
        )
        return (
            f"python3 {shlex.quote(str(self.installed_hook.resolve()))} "
            f"--harness {harness} --state-root {shlex.quote(str(state_root))} {mode}"
        )

    def canonical_group(self, harness: str, mode: str) -> dict[str, object]:
        return {
            "hooks": [
                {"type": "command", "command": self.expected_command(harness, mode)}
            ]
        }

    def write_json(self, path: Path, payload: object) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    def test_skill_is_installed_for_claude_cursor_and_pi(self) -> None:
        install_global.install_harness_skill(repo_root=REPO_ROOT, home=self.home)

        source = (
            REPO_ROOT / "plugins/xagent/skills/xagent-subagents/SKILL.md"
        ).read_text(encoding="utf-8")
        for relative in install_global.HARNESS_SKILL_DIRS:
            path = self.home / relative / "xagent-subagents" / "SKILL.md"
            self.assertTrue(path.is_file(), f"missing skill for {relative}")
            content = path.read_text(encoding="utf-8")
            self.assertTrue(content.startswith("---\n"), "frontmatter must stay first")
            self.assertIn(install_global.SKILL_MARKER, content)
            self.assertIn("## Superpowers SDD", content)
            self.assertIn(source.split("---\n", 2)[2].strip()[:60], content)

    def test_skill_install_is_idempotent(self) -> None:
        install_global.install_harness_skill(repo_root=REPO_ROOT, home=self.home)
        first = (
            self.home / "cursor_probe" if False else
            self.home / install_global.HARNESS_SKILL_DIRS[0] / "xagent-subagents" / "SKILL.md"
        ).read_text(encoding="utf-8")
        install_global.install_harness_skill(repo_root=REPO_ROOT, home=self.home)
        second = (
            self.home / install_global.HARNESS_SKILL_DIRS[0] / "xagent-subagents" / "SKILL.md"
        ).read_text(encoding="utf-8")
        self.assertEqual(first, second)

    def test_skill_install_refuses_to_clobber_an_unmanaged_file(self) -> None:
        path = (
            self.home
            / install_global.HARNESS_SKILL_DIRS[0]
            / "xagent-subagents"
            / "SKILL.md"
        )
        path.parent.mkdir(parents=True)
        path.write_text("hand written, not ours\n", encoding="utf-8")

        with self.assertRaises(RuntimeError) as caught:
            install_global.install_harness_skill(repo_root=REPO_ROOT, home=self.home)

        self.assertIn(str(path), str(caught.exception))
        self.assertEqual("hand written, not ours\n", path.read_text(encoding="utf-8"))

    def test_mcp_is_registered_for_claude_and_cursor(self) -> None:
        claude_path = self.home / ".claude.json"
        claude_path.write_text(
            json.dumps({"mcpServers": {"other": {"type": "http", "url": "http://x"}}}),
            encoding="utf-8",
        )

        install_global.register_harness_mcp(home=self.home)

        claude = json.loads(claude_path.read_text(encoding="utf-8"))
        self.assertEqual(
            {"type": "http", "url": XAGENT_MCP_URL},
            claude["mcpServers"]["xagent"],
        )
        self.assertIn("other", claude["mcpServers"])

        cursor = json.loads((self.home / ".cursor" / "mcp.json").read_text(encoding="utf-8"))
        self.assertEqual({"type": "http", "url": XAGENT_MCP_URL}, cursor["mcpServers"]["xagent"])

    def test_mcp_registration_is_idempotent(self) -> None:
        install_global.register_harness_mcp(home=self.home)
        first = (self.home / ".claude.json").read_text(encoding="utf-8")
        install_global.register_harness_mcp(home=self.home)
        self.assertEqual(first, (self.home / ".claude.json").read_text(encoding="utf-8"))

    def test_pi_gets_the_skill_but_no_mcp_registration(self) -> None:
        # pi ships without built-in MCP by design, so there is no registry to
        # write. The skill and the packaged CLI are how a pi controller reaches
        # xagent; asserting the absence keeps that a decision, not a gap.
        install_global.install_harness_skill(repo_root=REPO_ROOT, home=self.home)
        install_global.register_harness_mcp(home=self.home)

        self.assertTrue(
            (self.home / ".pi" / "skills" / "xagent-subagents" / "SKILL.md").is_file()
        )
        self.assertFalse((self.home / ".pi" / "mcp.json").exists())
        settings = self.home / ".pi" / "agent" / "settings.json"
        if settings.exists():
            self.assertNotIn("mcpServers", settings.read_text(encoding="utf-8"))

    def test_mcp_registration_never_leaves_a_truncated_registry(self) -> None:
        # ~/.claude.json is Claude Code's whole global state file.
        claude_path = self.home / ".claude.json"
        original = {"mcpServers": {}, "projects": {"/a": {"history": [1, 2, 3]}}}
        claude_path.write_text(json.dumps(original), encoding="utf-8")

        install_global.register_harness_mcp(home=self.home)

        merged = json.loads(claude_path.read_text(encoding="utf-8"))
        self.assertEqual(original["projects"], merged["projects"])
        self.assertIn("xagent", merged["mcpServers"])

        backup = self.home / ".claude.json.xagent-backup"
        self.assertTrue(backup.is_file(), "the previous contents must be recoverable")
        self.assertEqual(original, json.loads(backup.read_text(encoding="utf-8")))
        self.assertFalse((self.home / ".claude.json.xagent-stage").exists())

    def test_hook_registration_is_idempotent(self) -> None:
        # xhook-6: repeated installation must leave exactly one canonical
        # observer group and one canonical stop group per harness.
        self.register_hooks()

        self.assertEqual(
            {
                "hooks": {
                    "PostToolUse": [self.canonical_group("claude", "observe")],
                    "Stop": [self.canonical_group("claude", "guard")],
                }
            },
            json.loads(self.claude_settings.read_text(encoding="utf-8")),
        )
        self.assertEqual(
            {
                "hooks": {
                    "PostToolUse": [self.canonical_group("codex", "observe")],
                    "Stop": [self.canonical_group("codex", "guard")],
                }
            },
            json.loads(self.codex_hooks.read_text(encoding="utf-8")),
        )

        first = self.claude_settings.read_text(encoding="utf-8")
        self.assertEqual("", self.register_hooks())
        self.assertEqual(first, self.claude_settings.read_text(encoding="utf-8"))
        for path in (self.claude_settings, self.codex_hooks):
            payload = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(1, len(payload["hooks"]["PostToolUse"]))
            self.assertEqual(1, len(payload["hooks"]["Stop"]))

    def test_hook_registration_preserves_unrelated_values_and_group_order(self) -> None:
        unrelated_session_start = {
            "matcher": "^compact$",
            "hooks": [{"type": "command", "command": "python3 /opt/other/compact.py"}],
        }
        unrelated_post_tool_use = {
            "hooks": [{"type": "command", "command": "python3 /opt/other/audit.py"}]
        }
        self.write_json(
            self.codex_hooks,
            {
                "hooks": {
                    "SessionStart": [unrelated_session_start],
                    "PostToolUse": [unrelated_post_tool_use],
                },
                "experimental": {"keep": [1, 2, 3]},
            },
        )
        self.write_json(
            self.claude_settings,
            {
                "enabledPlugins": {"xagent@personal": True},
                "model": "opus",
                "hooks": {"Stop": [unrelated_post_tool_use]},
            },
        )

        self.register_hooks()

        codex = json.loads(self.codex_hooks.read_text(encoding="utf-8"))
        self.assertEqual({"keep": [1, 2, 3]}, codex["experimental"])
        self.assertEqual([unrelated_session_start], codex["hooks"]["SessionStart"])
        self.assertEqual(
            [unrelated_post_tool_use, self.canonical_group("codex", "observe")],
            codex["hooks"]["PostToolUse"],
        )
        self.assertEqual([self.canonical_group("codex", "guard")], codex["hooks"]["Stop"])

        claude = json.loads(self.claude_settings.read_text(encoding="utf-8"))
        self.assertEqual({"xagent@personal": True}, claude["enabledPlugins"])
        self.assertEqual("opus", claude["model"])
        self.assertEqual(
            [unrelated_post_tool_use, self.canonical_group("claude", "guard")],
            claude["hooks"]["Stop"],
        )

    def test_hook_registration_updates_owned_groups_in_place(self) -> None:
        # Codex trust is positional, so a stale xagent command must be
        # rewritten where it already sits rather than appended after it.
        stale = {
            "hooks": [
                {
                    "type": "command",
                    "command": (
                        f"python3 {self.installed_hook.resolve()} --harness codex "
                        "--state-root /stale/state observe"
                    ),
                }
            ]
        }
        trailing = {"hooks": [{"type": "command", "command": "python3 /opt/other/audit.py"}]}
        self.write_json(self.codex_hooks, {"hooks": {"PostToolUse": [stale, trailing]}})

        self.register_hooks()

        codex = json.loads(self.codex_hooks.read_text(encoding="utf-8"))
        self.assertEqual(
            [self.canonical_group("codex", "observe"), trailing],
            codex["hooks"]["PostToolUse"],
        )

    def test_hook_registration_preserves_unrelated_entries_in_mixed_groups(self) -> None:
        # A harness group is a list of commands, and only our own entry in it
        # is ours. Updating or de-duplicating must never take a neighbouring
        # command down with it.
        canonical_entry = {"type": "command", "command": self.expected_command("codex", "observe")}
        stale_entry = {
            "type": "command",
            "command": (
                f"python3 {self.installed_hook.resolve()} --harness codex "
                "--state-root /stale/state observe"
            ),
            "timeout": 9,
        }
        before = {"type": "command", "command": "python3 /opt/other/before.py"}
        after = {"type": "command", "command": "python3 /opt/other/after.py"}
        neighbour = {"type": "command", "command": "python3 /opt/other/neighbour.py"}
        unrelated_group = {"hooks": [{"type": "command", "command": "python3 /opt/other/audit.py"}]}
        self.write_json(
            self.codex_hooks,
            {
                "hooks": {
                    "PostToolUse": [
                        {
                            "matcher": "Bash",
                            "timeout": 5,
                            "hooks": [before, stale_entry, after],
                        },
                        unrelated_group,
                        {"hooks": [neighbour, dict(canonical_entry)]},
                        {"hooks": [dict(canonical_entry)]},
                    ]
                }
            },
        )

        self.register_hooks()

        codex = json.loads(self.codex_hooks.read_text(encoding="utf-8"))
        self.assertEqual(
            [
                # The canonical entry takes the first owned group's index in a
                # group of its own: a host matcher or timeout would apply to
                # our command and could silently disable observation. The
                # stale entry's own timeout is normalized away with it.
                {"hooks": [canonical_entry]},
                # Its unrelated siblings keep their group keys and their order,
                # immediately beside it.
                {"matcher": "Bash", "timeout": 5, "hooks": [before, after]},
                unrelated_group,
                # A later duplicate loses only its own entry.
                {"hooks": [neighbour]},
                # A group that held nothing but a duplicate goes away.
            ],
            codex["hooks"]["PostToolUse"],
        )

        # The split shape must itself be stable: a reinstall that reshuffled
        # groups would invalidate Codex's positional trust records.
        settled = self.codex_hooks.read_text(encoding="utf-8")
        self.assertEqual("", self.register_hooks())
        self.assertEqual(settled, self.codex_hooks.read_text(encoding="utf-8"))

    def test_hook_registration_removes_only_owned_duplicates(self) -> None:
        duplicate = self.canonical_group("codex", "observe")
        other_harness = {
            "hooks": [
                {
                    "type": "command",
                    "command": self.expected_command("claude", "observe"),
                }
            ]
        }
        unrelated = {"hooks": [{"type": "command", "command": "python3 /opt/other/audit.py"}]}
        self.write_json(
            self.codex_hooks,
            {
                "hooks": {
                    "PostToolUse": [
                        duplicate,
                        unrelated,
                        other_harness,
                        self.canonical_group("codex", "observe"),
                    ]
                }
            },
        )

        self.register_hooks()

        codex = json.loads(self.codex_hooks.read_text(encoding="utf-8"))
        self.assertEqual(
            [self.canonical_group("codex", "observe"), unrelated, other_harness],
            codex["hooks"]["PostToolUse"],
        )

    def test_hook_registration_rejects_malformed_json_and_hooks_shapes(self) -> None:
        # Only a missing file or a missing "hooks" key means absence. An
        # existing file we cannot read as a configuration is malformed, and
        # replacing it would destroy whatever the user meant to have there.
        for payload_text in (
            "{not json",
            "",
            "  \n\t\n",
            json.dumps(["not", "an", "object"]),
            json.dumps({"hooks": None}),
            json.dumps({"hooks": ["not an object"]}),
            json.dumps({"hooks": {"Stop": None}}),
            json.dumps({"hooks": {"Stop": {"not": "an array"}}}),
        ):
            with self.subTest(payload=payload_text):
                self.claude_settings.parent.mkdir(parents=True, exist_ok=True)
                self.claude_settings.write_text(payload_text, encoding="utf-8")
                backup = self.claude_settings.with_name("settings.json.xagent-backup")
                backup.unlink(missing_ok=True)
                self.codex_hooks.unlink(missing_ok=True)

                with self.assertRaises(RuntimeError) as caught:
                    self.register_hooks()

                self.assertIn(str(self.claude_settings), str(caught.exception))
                self.assertEqual(
                    payload_text, self.claude_settings.read_text(encoding="utf-8")
                )
                self.assertFalse(backup.exists(), "a rejected file must not be replaced")
                self.assertFalse(
                    self.codex_hooks.exists(),
                    "no target may be written when another target is malformed",
                )

    def test_hook_registration_writes_atomic_backup(self) -> None:
        original = {"model": "opus", "hooks": {"Stop": []}}
        self.write_json(self.claude_settings, original)

        self.register_hooks()

        backup = self.claude_settings.with_name("settings.json.xagent-backup")
        self.assertTrue(backup.is_file(), "the previous contents must be recoverable")
        self.assertEqual(original, json.loads(backup.read_text(encoding="utf-8")))
        self.assertFalse(self.claude_settings.with_name(".settings.json.xagent-stage").exists())

    def test_codex_effective_change_prints_trust_notice(self) -> None:
        first = self.register_hooks()
        self.assertIn("/hooks", first)
        self.assertIn("codex", first.lower())

        self.assertEqual("", self.register_hooks())

        # A Claude-only change must not claim that Codex needs re-approval.
        self.claude_settings.unlink()
        self.assertEqual("", self.register_hooks())
        self.assertTrue(self.claude_settings.is_file())

    def test_legacy_agents_marker_is_tolerated(self) -> None:
        # The agents installer still writes a whole-file marker; xagent must
        # preserve it until that installer migrates to group ownership.
        agents_group = {
            "matcher": "^compact$",
            "hooks": [
                {
                    "type": "command",
                    "command": "python3 /codex/hooks/sheaf/session_start_after_compact.py",
                    "statusMessage": "Loading post-compaction reminder",
                    "timeout": 5,
                }
            ],
        }
        marker = "sheaf-agents-managed: DO NOT EDIT; source=projects/agents/global/codex/hooks/hooks.json"
        self.write_json(
            self.codex_hooks,
            {"hooks": {"SessionStart": [agents_group]}, "_sheaf_agents_managed": marker},
        )

        self.register_hooks()

        codex = json.loads(self.codex_hooks.read_text(encoding="utf-8"))
        self.assertEqual(marker, codex["_sheaf_agents_managed"])
        self.assertEqual([agents_group], codex["hooks"]["SessionStart"])
        self.assertEqual(
            [self.canonical_group("codex", "observe")], codex["hooks"]["PostToolUse"]
        )

    def test_hook_registration_requires_the_installed_program(self) -> None:
        self.installed_hook.unlink()

        with self.assertRaisesRegex(RuntimeError, "controller_stop_hook.py"):
            self.register_hooks()

        self.assertFalse(self.claude_settings.exists())
        self.assertFalse(self.codex_hooks.exists())
