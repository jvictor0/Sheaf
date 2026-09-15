import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtemp } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";

import { parseArgs } from "../src/cli.js";
import type { OutputEvent } from "../src/events.js";

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

test("executable list and logs inspect persisted runs without a server", async () => {
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

  const list = await runXagent(["list"], "", repoRoot);
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

