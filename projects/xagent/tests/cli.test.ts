import assert from "node:assert/strict";
import { mkdir, mkdtemp, readFile, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { Readable, Writable } from "node:stream";
import test from "node:test";

import { FakeHarnessAdapter } from "../src/adapters/fake.js";
import { PlaceholderHarnessAdapter } from "../src/adapters/placeholder.js";
import { main, parseArgs, runCli } from "../src/cli.js";
import type { OutputEvent } from "../src/events.js";
import { appendNormalizedEvent, createRunRecord, getDefaultLogRoot, listRuns } from "../src/logs.js";
import type { XagentServiceClient } from "../src/service/client.js";

const originalLogRoot = process.env.XAGENT_LOG_ROOT;
delete process.env.XAGENT_LOG_ROOT;
test.after(() => {
  if (originalLogRoot === undefined) {
    delete process.env.XAGENT_LOG_ROOT;
    return;
  }
  process.env.XAGENT_LOG_ROOT = originalLogRoot;
});

test("parses a valid run command", () => {
  assert.deepEqual(parseArgs(["run", "--harness", "codex", "--subagent"]), {
    command: "run",
    harness: "codex",
    mode: "subagent",
    model: undefined,
    thinkingLevel: undefined,
    initialMessage: undefined,
  });
});

test("parses a valid supervise start command with options", () => {
  assert.deepEqual(
    parseArgs([
      "supervise",
      "--harness",
      "claude_code",
      "--model",
      "sonnet",
      "--thinking-level",
      "high",
      "--permission-mode",
      "acceptEdits",
      "--cwd",
      "/tmp/worktree",
      "--policy",
      "{\"silenceTimeoutMs\":60000}",
      "--deadline-seconds",
      "120",
      "implement the task",
    ]),
    {
      command: "supervise",
      harness: "claude_code",
      model: "sonnet",
      thinkingLevel: "high",
      permissionMode: "acceptEdits",
      cwd: "/tmp/worktree",
      policy: { silenceTimeoutMs: 60_000, watchdog: {} },
      deadlineSeconds: 120,
      prompt: "implement the task",
    },
  );
});

test("parses a service start command without an await deadline", () => {
  assert.deepEqual(
    parseArgs(["start", "--harness", "claude_code", "--model", "opus", "ship it"]),
    {
      command: "start",
      harness: "claude_code",
      model: "opus",
      prompt: "ship it",
    },
  );
  assert.throws(
    () => parseArgs(["start", "--harness", "codex", "--deadline-seconds", "60", "work"]),
    /does not accept --deadline-seconds/,
  );
});

test("parses existing-run quiet service operations", () => {
  assert.deepEqual(parseArgs(["await", "xrun_1", "--after-sequence", "4"]), {
    command: "await",
    runId: "xrun_1",
    afterSequence: 4,
  });
  assert.deepEqual(parseArgs(["await", "xrun_1", "--after-sequence", "0", "--deadline-seconds", "30"]), {
    command: "await",
    runId: "xrun_1",
    afterSequence: 0,
    deadlineSeconds: 30,
  });
  assert.deepEqual(parseArgs(["inspect", "xrun_2"]), {
    command: "inspect",
    runId: "xrun_2",
  });
  assert.deepEqual(parseArgs(["message", "xrun_3", "follow", "up", "please"]), {
    command: "message",
    runId: "xrun_3",
    text: "follow up please",
  });
  assert.deepEqual(parseArgs(["interrupt", "xrun_4"]), {
    command: "interrupt",
    runId: "xrun_4",
  });
  assert.deepEqual(parseArgs(["close", "xrun_5"]), {
    command: "close",
    runId: "xrun_5",
  });
});

test("rejects --deadline-seconds above the service maximum before starting a run", () => {
  assert.throws(
    () => parseArgs(["supervise", "--harness", "codex", "--deadline-seconds", "7001", "prompt"]),
    /--deadline-seconds cannot exceed 7000/,
  );
  assert.throws(
    () => parseArgs(["await", "xrun_1", "--after-sequence", "0", "--deadline-seconds", "7001"]),
    /--deadline-seconds cannot exceed 7000/,
  );
});

test("supervise help explains service dependency without changing legacy run help", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-cli-"));
  const stdout = new MemoryWritable();
  const stderr = new MemoryWritable();

  const top = await main(["--help"], Readable.from([]), stdout, stderr, repoRoot);
  assert.deepEqual(top, { exitCode: 0 });
  assert.match(stdout.text, /xagent supervise/);
  assert.match(stdout.text, /Conductor-managed xagent service|xagent service/);
  assert.match(stdout.text, /run_id/);
  assert.match(stdout.text, /quiet|no healthy|routine/i);
  assert.match(stdout.text, /final report|final assistant report/i);
  assert.match(stdout.text, /xagent run/);
  assert.equal(stderr.text, "");

  const runHelpStdout = new MemoryWritable();
  const runHelp = await main(["run", "--help"], Readable.from([]), runHelpStdout, stderr, repoRoot);
  assert.deepEqual(runHelp, { exitCode: 0 });
  assert.match(runHelpStdout.text, /control\.exit/);
  assert.equal(runHelpStdout.text.includes("xagent supervise"), false);
  assert.equal(runHelpStdout.text.includes("quiet"), false);

  const superviseHelpStdout = new MemoryWritable();
  const superviseHelp = await main(
    ["supervise", "--help"],
    Readable.from([]),
    superviseHelpStdout,
    stderr,
    repoRoot,
  );
  assert.deepEqual(superviseHelp, { exitCode: 0 });
  assert.match(superviseHelpStdout.text, /xagent supervise/);
  assert.match(superviseHelpStdout.text, /127\.0\.0\.1:9005|xagent service/);
  assert.match(superviseHelpStdout.text, /run_id/);
  assert.match(superviseHelpStdout.text, /quiet/i);
  assert.match(superviseHelpStdout.text, /final report|final assistant report/i);
});

test("rejects supervise without harness or prompt", () => {
  assert.throws(() => parseArgs(["supervise", "do work"]), /--harness/);
  assert.throws(
    () => parseArgs(["supervise", "--harness", "codex"]),
    /prompt|initial/,
  );
});

test("rejects await without run id or after-sequence", () => {
  assert.throws(() => parseArgs(["await"]), /await/);
  assert.throws(() => parseArgs(["await", "xrun_1"]), /after-sequence/);
});

test("parses and forwards an explicit permission mode", async () => {
  const parsed = parseArgs([
    "run", "--harness", "claude_code", "--permission-mode", "acceptEdits", "--subagent",
  ]);
  assert.equal(parsed.command === "run" ? parsed.permissionMode : undefined, "acceptEdits");
});

test("parses a valid run command with an initial message", () => {
  assert.deepEqual(parseArgs(["run", "--harness", "codex", "--subagent", "hello", "there"]), {
    command: "run",
    harness: "codex",
    mode: "subagent",
    model: undefined,
    thinkingLevel: undefined,
    initialMessage: "hello there",
  });
});

test("parses a valid full run command with options", () => {
  assert.deepEqual(
    parseArgs([
      "run",
      "--harness",
      "claude_code",
      "--model",
      "sonnet",
      "--thinking-level",
      "high",
      "--full",
    ]),
    {
      command: "run",
      harness: "claude_code",
      mode: "full",
      model: "sonnet",
      thinkingLevel: "high",
      initialMessage: undefined,
    },
  );
});

test("help commands exit zero and describe stdin JSONL", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-cli-"));
  const stdout = new MemoryWritable();
  const stderr = new MemoryWritable();

  const result = await main(["--help"], Readable.from([]), stdout, stderr, repoRoot);

  assert.deepEqual(result, { exitCode: 0 });
  assert.match(stdout.text, /xagent run/);
  assert.match(stdout.text, /user\.message/);
  assert.equal(stderr.text, "");

  const runStdout = new MemoryWritable();
  const runResult = await main(["run", "--help"], Readable.from([]), runStdout, stderr, repoRoot);
  assert.deepEqual(runResult, { exitCode: 0 });
  assert.match(runStdout.text, /control\.exit/);
});

test("rejects run without --harness", () => {
  assert.throws(() => parseArgs(["run", "--subagent"]), /--harness/);
});

test("rejects run without an output mode", () => {
  assert.throws(
    () => parseArgs(["run", "--harness", "codex"]),
    /--subagent.*--full|--full.*--subagent/,
  );
});

test("rejects run with both output modes", () => {
  assert.throws(
    () => parseArgs(["run", "--harness", "codex", "--subagent", "--full"]),
    /exactly one/,
  );
});

test("rejects unsupported run flags", () => {
  assert.throws(
    () => parseArgs(["run", "--harness", "codex", "--subagent", "--bogus"]),
    /--bogus/,
  );
});

test("rejects duplicate harness flags", () => {
  assert.throws(
    () => parseArgs(["run", "--harness", "codex", "--harness", "pi", "--subagent"]),
    /exactly one --harness/,
  );
});

test("rejects unsupported harness values", () => {
  assert.throws(
    () => parseArgs(["run", "--harness", "unknown", "--subagent"]),
    /Unsupported harness: unknown/,
  );
});

test("rejects unsupported thinking levels", () => {
  assert.throws(
    () => parseArgs(["run", "--harness", "codex", "--thinking-level", "max", "--subagent"]),
    /Unsupported thinking level: max/,
  );
});

test("rejects missing values after valued flags", () => {
  assert.throws(() => parseArgs(["run", "--harness"]), /--harness/);
  assert.throws(() => parseArgs(["run", "--harness", "codex", "--model", "--subagent"]), /--model/);
  assert.throws(
    () => parseArgs(["run", "--harness", "codex", "--thinking-level", "--subagent"]),
    /--thinking-level/,
  );
});

test("rejects duplicate model flags", () => {
  assert.throws(
    () =>
      parseArgs([
        "run",
        "--harness",
        "codex",
        "--model",
        "one",
        "--model",
        "two",
        "--subagent",
      ]),
    /--model at most once/,
  );
});

test("rejects duplicate thinking-level flags", () => {
  assert.throws(
    () =>
      parseArgs([
        "run",
        "--harness",
        "codex",
        "--thinking-level",
        "low",
        "--thinking-level",
        "high",
        "--subagent",
      ]),
    /--thinking-level at most once/,
  );
});

test("run command reports unavailable placeholder harness as structured JSONL", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-cli-"));
  await mkdir(path.join(repoRoot, ".git"));
  const stdout = new MemoryWritable();
  const stderr = new MemoryWritable();

  const result = await main(
    ["run", "--harness", "codex", "--subagent"],
    Readable.from([JSON.stringify({ type: "control.exit" }), "\n"]),
    stdout,
    stderr,
    repoRoot,
    { createAdapter: (harness) => new PlaceholderHarnessAdapter(harness) },
  );

  assert.equal(stdout.text.includes("runtime is not implemented yet"), false);
  assert.equal(stderr.text, "");
  assert.deepEqual(result, { exitCode: 1 });

  const events = parseJsonl(stdout.text);
  assert.deepEqual(
    events.map((event) => event.type),
    ["error", "session.ended"],
  );
  assert.equal(events[0]?.type === "error" ? events[0].code : undefined, "harness_unavailable");
  assert.deepEqual(events[1]?.type === "session.ended" ? [events[1].reason, events[1].exit_code] : undefined, [
    "start_failed",
    1,
  ]);

  const runs = await listRuns(getDefaultLogRoot(repoRoot));
  assert.equal(runs.length, 1);
  assert.equal(runs[0]?.harness, "codex");
  assert.equal(runs[0]?.mode, "subagent");
  assert.equal(runs[0]?.exit_status, "failed");
});

test("run command submits initial message before stdin follow-ups", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-cli-"));
  const stdout = new MemoryWritable();
  const stderr = new MemoryWritable();
  const adapter = new FakeHarnessAdapter();

  const result = await main(
    ["run", "--harness", "codex", "--subagent", "initial hello"],
    Readable.from([
      JSON.stringify({ type: "user.message", text: "follow up" }),
      "\n",
      JSON.stringify({ type: "control.exit" }),
      "\n",
    ]),
    stdout,
    stderr,
    repoRoot,
    { createAdapter: () => adapter },
  );

  assert.deepEqual(result, { exitCode: 0 });
  assert.deepEqual(adapter.submittedTexts, ["initial hello", "follow up"]);
});

test("default placeholder adapter fails before processing submitted turns", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-cli-"));
  const stdout = new MemoryWritable();
  const stderr = new MemoryWritable();

  const result = await main(
    ["run", "--harness", "cursor", "--subagent"],
    Readable.from([
      JSON.stringify({ type: "user.message", text: "hello" }),
      "\n",
      JSON.stringify({ type: "control.exit" }),
      "\n",
    ]),
    stdout,
    stderr,
    repoRoot,
    { createAdapter: (harness) => new PlaceholderHarnessAdapter(harness) },
  );

  assert.equal(stdout.text.includes("runtime is not implemented yet"), false);
  assert.equal(stderr.text, "");
  assert.deepEqual(result, { exitCode: 1 });

  const events = parseJsonl(stdout.text);
  assert.equal(events.some((event) => event.type === "turn.started"), false);
  assert.equal(
    events.some(
      (event) =>
        event.type === "error" &&
        event.code === "harness_unavailable" &&
        event.message.includes("cursor adapter is not implemented yet"),
    ),
    true,
  );
  assert.equal(
    events.some((event) => event.type === "turn.failed" && event.code === "harness_unavailable"),
    false,
  );
  assert.equal(events.filter((event) => event.type === "session.ready").length, 0);
});

test("top-level runner writes one diagnostic for command failures", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-cli-"));
  const stdout = new MemoryWritable();
  const stderr = new MemoryWritable();

  const result = await runCli(["logs", "../outside"], Readable.from([]), stdout, stderr, repoRoot);

  assert.deepEqual(result, { exitCode: 1 });
  assert.equal(stdout.text, "");
  assert.match(stderr.text, /Invalid run id/);
  assert.equal(stderr.text.trim().split("\n").length, 1);
});

test("list command reads service-owned metadata", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-cli-"));
  const stdout = new MemoryWritable();
  const stderr = new MemoryWritable();
  const serviceClient = listOnlyServiceClient([{
    run_id: "xrun_20260621000000000_00000003",
    harness: "pi",
    phase: "completed",
    sequence: 4,
    exit_status: "completed",
    live: false,
    supervised: true,
    created_at: "2026-06-21T00:00:00.000Z",
    updated_at: "2026-06-21T00:00:01.000Z",
  }]);

  const result = await main(
    ["list"],
    Readable.from([]),
    stdout,
    stderr,
    repoRoot,
    { createServiceClient: () => serviceClient },
  );

  assert.deepEqual(result, { exitCode: 0 });
  assert.equal(stderr.text, "");
  const runs = JSON.parse(stdout.text) as Array<{ run_id: string; harness: string }>;
  assert.equal(runs[0]?.run_id, "xrun_20260621000000000_00000003");
  assert.equal(runs[0]?.harness, "pi");
});

test("logs command reads persisted normalized logs and rejects traversal", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-cli-"));
  await mkdir(path.join(repoRoot, ".git"));
  const runId = "xrun_20260621000000000_00000004";
  const record = await createRunRecord({
    repoRoot,
    runId,
    harness: "codex",
    mode: "subagent",
    clock: () => new Date("2026-06-21T00:00:00.000Z"),
  });
  await appendNormalizedEvent(record, {
    schema_version: 1,
    type: "status",
    run_id: runId,
    sequence: 1,
    timestamp: "2026-06-21T00:00:00.000Z",
    level: "info",
    message: "hello",
  });

  const stdout = new MemoryWritable();
  const stderr = new MemoryWritable();
  const result = await main(["logs", runId], Readable.from([]), stdout, stderr, repoRoot);

  assert.deepEqual(result, { exitCode: 0 });
  assert.equal(JSON.parse(stdout.text).message, "hello");
  assert.equal(stderr.text, "");

  const traversalStdout = new MemoryWritable();
  const traversalStderr = new MemoryWritable();
  await assert.rejects(
    () => main(["logs", "../outside"], Readable.from([]), traversalStdout, traversalStderr, repoRoot),
    /Invalid run id/,
  );
  assert.equal((await readFile(path.join(record.runDir, "normalized.jsonl"), "utf8")).includes("hello"), true);
});

test("default log root resolves to top-level data when launched from package directory", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-cli-"));
  const packageDir = path.join(repoRoot, "projects", "xagent");
  await mkdir(path.join(repoRoot, ".git"), { recursive: true });
  await mkdir(packageDir, { recursive: true });

  const runStdout = new MemoryWritable();
  const runStderr = new MemoryWritable();
  const runResult = await main(
    ["run", "--harness", "codex", "--subagent"],
    Readable.from([JSON.stringify({ type: "control.exit" }), "\n"]),
    runStdout,
    runStderr,
    packageDir,
    { createAdapter: () => new FakeHarnessAdapter() },
  );

  assert.deepEqual(runResult, { exitCode: 0 });
  assert.equal(runStderr.text, "");

  const topLevelRuns = await listRuns(getDefaultLogRoot(repoRoot));
  assert.equal(topLevelRuns.length, 1);
  assert.equal(await exists(path.join(packageDir, "data", "xagent")), false);

  const logsStdout = new MemoryWritable();
  const logsStderr = new MemoryWritable();
  const logsResult = await main(["logs", topLevelRuns[0]?.run_id ?? ""], Readable.from([]), logsStdout, logsStderr, packageDir);

  assert.deepEqual(logsResult, { exitCode: 0 });
  assert.equal(logsStderr.text, "");
  assert.match(logsStdout.text, /session\.started/);
});

function listOnlyServiceClient(
  runs: Awaited<ReturnType<XagentServiceClient["listRuns"]>>["runs"],
): XagentServiceClient {
  return {
    async start() { throw new Error("unused"); },
    async await() { throw new Error("unused"); },
    async inspect() { throw new Error("unused"); },
    async listRuns() { return { runs }; },
    async message() { throw new Error("unused"); },
    async interrupt() { throw new Error("unused"); },
    async closeRun() { throw new Error("unused"); },
    async close() {},
    awaitToolCallsIssued: 0,
  };
}

class MemoryWritable extends Writable {
  text = "";

  override _write(chunk: Buffer | string, _encoding: BufferEncoding, callback: (error?: Error | null) => void): void {
    this.text += chunk.toString();
    callback();
  }
}

async function exists(filePath: string): Promise<boolean> {
  try {
    await stat(filePath);
    return true;
  } catch (error) {
    if (error instanceof Error && "code" in error && error.code === "ENOENT") {
      return false;
    }
    throw error;
  }
}

function parseJsonl(text: string): OutputEvent[] {
  return text
    .trim()
    .split("\n")
    .filter(Boolean)
    .map((line) => JSON.parse(line) as OutputEvent);
}
