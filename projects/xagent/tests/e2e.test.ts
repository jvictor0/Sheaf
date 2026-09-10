import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtemp, readFile, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";

import { parseArgs } from "../src/cli.js";
import type { OutputEvent } from "../src/events.js";
import { startMcpService, structuredToolBody } from "./support/mcp_service.js";

// The v1 full-lifecycle MCP SDD test was deleted in Task 4: report binding and
// the sdd await/close facade are gone, so Followup cannot clear open turns and
// closed_at / report_text assertions no longer hold. Task 9 rebuilds e2e
// coverage against the v2 model.
//

const x_MutableArtifactText = "DISK_ARTIFACT_MUST_NOT_BECOME_REPORT_TEXT";

test("public parser rejects fake harness", () => {
  assert.throws(
    () => parseArgs(["run", "--harness", "fake", "--subagent"]),
    /Unsupported harness: fake/,
  );
});

test("executable fake adapter preserves same-session follow-up behavior", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-e2e-"));
  const result = await runXagent(
    ["run", "--harness", "codex", "--subagent", "first"],
    [
      JSON.stringify({ type: "user.message", text: "second" }),
      JSON.stringify({ type: "control.exit" }),
    ].join("\n") + "\n",
    repoRoot,
    { XAGENT_TEST_ADAPTER: "fake" },
  );

  assert.equal(result.code, 0, result.stderr);
  const events = parseJsonl(result.stdout);
  assert.deepEqual(
    events.filter((event) => event.type === "message.completed").map((event) => event.text),
    ["fake response to first", "fake response to second"],
  );
  assert.deepEqual(
    events.filter((event) => event.type === "turn.completed").map((event) => event.provider_thread_id),
    ["fake-thread-1", "fake-thread-1"],
  );
  assert.equal(events.filter((event) => event.type === "session.ready").length, 3);
});

test("executable subagent mode suppresses stdout detail while logs preserve it", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-e2e-"));
  const result = await runXagent(
    ["run", "--harness", "codex", "--subagent"],
    `${JSON.stringify({ type: "user.message", text: "use tool" })}\n${JSON.stringify({ type: "control.exit" })}\n`,
    repoRoot,
    { XAGENT_TEST_ADAPTER: "fake" },
  );

  assert.equal(result.code, 0, result.stderr);
  const events = parseJsonl(result.stdout);
  assert.equal(events.some((event) => event.type === "raw.provider"), false);
  assert.equal(events.some((event) => event.type === "tool.completed" && "output" in event), false);

  const runId = events[0]?.run_id;
  assert.equal(typeof runId, "string");
  const logs = await runXagent(["logs", runId], "", repoRoot);
  assert.equal(logs.code, 0, logs.stderr);
  const logEvents = parseJsonl(logs.stdout);
  assert.equal(logEvents.some((event) => event.type === "raw.provider"), true);
  assert.equal(logEvents.some((event) => event.type === "tool.completed" && "output" in event), true);
});

test("executable full mode emits raw provider and normalized tool events", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-e2e-"));
  const result = await runXagent(
    ["run", "--harness", "codex", "--full"],
    `${JSON.stringify({ type: "user.message", text: "use tool" })}\n${JSON.stringify({ type: "control.exit" })}\n`,
    repoRoot,
    { XAGENT_TEST_ADAPTER: "fake" },
  );

  assert.equal(result.code, 0, result.stderr);
  const events = parseJsonl(result.stdout);
  assert.equal(events.some((event) => event.type === "raw.provider"), true);
  assert.equal(events.some((event) => event.type === "tool.started"), true);
  assert.equal(events.some((event) => event.type === "tool.completed"), true);
  assert.equal(events.some((event) => event.type === "message.completed"), true);
  assert.equal(events.some((event) => event.type === "turn.completed"), true);
});

test("executable local list and logs inspect legacy persisted runs without a server", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-e2e-"));
  const result = await runXagent(
    ["run", "--harness", "codex", "--subagent"],
    `${JSON.stringify({ type: "control.exit" })}\n`,
    repoRoot,
    { XAGENT_TEST_ADAPTER: "fake" },
  );
  assert.equal(result.code, 0, result.stderr);
  const runId = parseJsonl(result.stdout)[0]?.run_id;
  assert.equal(typeof runId, "string");

  const list = await runXagent(["list", "--local"], "", repoRoot);
  assert.equal(list.code, 0, list.stderr);
  const runs = JSON.parse(list.stdout) as Array<{ run_id: string }>;
  assert.equal(runs.some((run) => run.run_id === runId), true);

  const logs = await runXagent(["logs", runId], "", repoRoot);
  assert.equal(logs.code, 0, logs.stderr);
  assert.equal(parseJsonl(logs.stdout)[0]?.run_id, runId);
});

async function runXagent(
  args: string[],
  stdin: string,
  cwd: string,
  extraEnv: Record<string, string> = {},
): Promise<{ code: number | null; stdout: string; stderr: string }> {
  const child = spawn(process.execPath, [path.join(process.cwd(), "dist", "src", "main.js"), ...args], {
    cwd,
    env: { ...process.env, ...extraEnv },
  });
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (chunk: string) => {
    stdout += chunk;
  });
  child.stderr.on("data", (chunk: string) => {
    stderr += chunk;
  });
  child.stdin.end(stdin);
  const code = await new Promise<number | null>((resolve, reject) => {
    child.once("error", reject);
    child.once("close", resolve);
  });
  return { code, stdout, stderr };
}

function parseJsonl(text: string): OutputEvent[] {
  return text
    .trim()
    .split("\n")
    .filter(Boolean)
    .map((line) => JSON.parse(line) as OutputEvent);
}

test("two agents, four submissions, two immutable rows, reports only in the log", async () => {
  const service = await startMcpService();
  try {
    // Concurrent SDD sessions: start the fixer while the implementer is still
    // live, each with its own adapter and distinct agent id.
    //
    const implementer = await service.startSddImplementer({ task: 4 });
    const fixer = await service.startSddFixer({ task: 4 });
    assert.notEqual(implementer.run_id, fixer.run_id);

    // Same path startSddImplementer wrote; overwrite before await so a
    // regression that re-reads the report file would surface the disk text.
    //
    const implementerReportPath = service.artifact("implementer-task-4-report.md");
    await writeFile(implementerReportPath, x_MutableArtifactText, "utf8");

    const implementerFirst = await service.awaitTurn(
      implementer.run_id,
      implementer.sequence,
    );
    assert.equal(implementerFirst.event, "turn.completed");
    // FakeHarnessAdapter reports `fake response to ${submitted text}`.
    //
    const implementerStartReport = "fake response to Rendered implementer prompt.\n";
    assert.equal(
      (implementerFirst.report as { text: string }).text,
      implementerStartReport,
    );
    assert.equal(
      await readFile(implementerReportPath, "utf8"),
      x_MutableArtifactText,
      "the report file must remain the distinctive disk artifact",
    );

    await service.awaitTurn(fixer.run_id, fixer.sequence);

    const fix = await service.sddFollowup({
      kind: "fix",
      run_id: implementer.run_id,
      round: 1,
      findings: service.artifact("task-4-findings.md"),
      findings_text: "Finding 1: the gate is missing.",
      tests: ["npm test"],
      report_out: implementerReportPath,
    });
    const implementerFix = await service.awaitTurn(implementer.run_id, fix.sequence);
    assert.equal(implementerFix.event, "turn.completed");
    assert.match(
      (implementerFix.report as { text: string }).text,
      /^fake response to /,
    );
    assert.notEqual(
      (implementerFix.report as { text: string }).text,
      x_MutableArtifactText,
    );

    // xsvc-5: generic await/message/close serve SDD-owned runs identically.
    //
    const nudgeText = "controller nudge after the fix turn";
    const nudged = await service.message(implementer.run_id, nudgeText);
    const implementerNudge = await service.awaitTurn(
      implementer.run_id,
      nudged.sequence,
    );
    assert.equal(implementerNudge.event, "turn.completed");
    assert.equal(
      (implementerNudge.report as { text: string }).text,
      `fake response to ${nudgeText}`,
    );

    await service.closeRun(implementer.run_id);
    const afterClose = await service.sddFollowupResult({
      kind: "fix",
      run_id: implementer.run_id,
      round: 2,
      findings: service.artifact("task-4-findings-after-close.md"),
      findings_text: "must not land on a closed agent",
      tests: ["npm test"],
      report_out: implementerReportPath,
    });
    assert.equal(afterClose.isError, true);
    assert.equal(structuredToolBody(afterClose).error, "sdd_agent_not_live");

    const rows = service.ledger().ListAll();
    assert.equal(rows.length, 2);
    assert.deepEqual(rows.map((row) => row.role), ["implementer", "fixer"]);
    assert.deepEqual(rows.map((row) => row.task), [4, 4]);
    assert.equal(rows[0]!.plan_path, rows[1]!.plan_path);

    const implementerEvents = await service.normalizedEvents(implementer.run_id);
    const fixerEvents = await service.normalizedEvents(fixer.run_id);
    assert.equal(
      implementerEvents.filter((event) => event.type === "turn.submitted").length,
      3,
    );
    assert.equal(
      fixerEvents.filter((event) => event.type === "turn.submitted").length,
      1,
    );
    assert.equal(
      implementerEvents.filter((event) => event.type === "turn.completed").length,
      3,
    );
    for (const event of implementerEvents.filter((e) => e.type === "turn.completed")) {
      const reportText = (event.payload as { report: { text: string } }).report.text;
      assert.match(reportText, /^fake response to /);
      assert.notEqual(reportText, x_MutableArtifactText);
    }
  } finally {
    await service.close();
  }
});
