import assert from "node:assert/strict";
import { mkdtemp, readFile, readdir } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

import { FakeHarnessAdapter } from "../src/adapters/fake.js";
import type { AdapterEvent } from "../src/adapters/types.js";
import { XagentRunManager } from "../src/service/run_manager.js";
import {
  createShutdownController,
  createXagentServer,
  type XagentServer,
} from "../src/service/server.js";
import type {
  SupervisionPolicy,
  WatchdogClassifier,
  WatchdogRequest,
  WatchdogVerdict,
} from "../src/supervision/types.js";
import { Supervisor } from "../src/supervision/supervisor.js";


const x_FixtureDir = path.join(process.cwd(), "tests", "fixtures", "watchdog");
const x_PackagedMcpPath = path.join(
  process.cwd(),
  "..",
  "..",
  "plugins",
  "xagent",
  ".mcp.json",
);

const testPolicy: SupervisionPolicy = {
  silenceTimeoutMs: 300_000,
  watchdog: {},
};

test("packaged plugin MCP declaration points at the loopback xagent service endpoint", async () => {
  const mcp = JSON.parse(await readFile(x_PackagedMcpPath, "utf8")) as {
    mcpServers: { xagent: { type: string; url: string; tool_timeout_sec: number } };
  };
  const xagent = mcp.mcpServers.xagent;
  assert.equal(xagent.type, "http");
  assert.equal(xagent.url, "http://127.0.0.1:9005/mcp");
  assert.equal(xagent.tool_timeout_sec, 7200);
});

test("recorded watchdog fixtures classify and alert as documented", async () => {
  const fixtureNames = await readdir(x_FixtureDir);
  const fixtures = await Promise.all(
    fixtureNames
      .filter((name) => name.endsWith(".json"))
      .map(async (name) => JSON.parse(await readFile(path.join(x_FixtureDir, name), "utf8")) as Fixture),
  );
  assert.equal(fixtures.length, 7);

  for (const fixture of fixtures) {
    await runFixture(fixture);
  }
});

async function runFixture(fixture: Fixture): Promise<void> {
  assert.equal(
    "expected_suspicion_signals" in fixture,
    false,
    `${fixture.name}: fixtures must not encode expected_suspicion_signals`,
  );
  const clock = new FakeClock();
  const classifier = new ScriptedClassifier(fixture.scripted_verdict);
  const reasons: string[] = [];
  const adapterOptions = {
    scriptedEvents: [replayFixture(fixture, clock)],
    supportsInterrupt: false,
  };
  const adapter = fixture.harness === "claude_code"
    ? new ClaudeFakeHarnessAdapter(adapterOptions)
    : new FakeHarnessAdapter(adapterOptions);
  const supervisor = new Supervisor({
    runId: `xrun_fixture_${fixture.name}`,
    adapter,
    startOptions: { cwd: process.cwd() },
    policy: testPolicy,
    clock: clock.now,
    scheduler: clock,
    watchdogClassifier: classifier,
    eventSink: async (event) => {
      reasons.push(event.reason);
    },
  });
  await supervisor.start();
  await supervisor.submit(fixture.original_prompt);

  if (fixture.expected_classifier_eligible) {
    assert.equal(
      classifier.calls.length,
      1,
      `${fixture.name}: expected one classifier call`,
    );
    const request = classifier.calls[0];
    assert.ok(request);
    assert.equal(typeof request.original_prompt, "string");
    assert.equal(request.harness, fixture.harness ?? "codex");
    assert.equal(Array.isArray(request.recent_provider_json), true);
    assert.equal(typeof request.elapsed_ms, "number");
    assert.equal(typeof request.truncated, "boolean");
    assert.equal(typeof request.input_bytes, "number");
    assert.equal(
      request.input_bytes,
      Buffer.byteLength(JSON.stringify(request), "utf8"),
      `${fixture.name}: input_bytes must equal serialized request size`,
    );
    if (fixture.expected_classifier_contains !== undefined) {
      const serialized = JSON.stringify(request.recent_provider_json);
      for (const needle of fixture.expected_classifier_contains) {
        assert.equal(
          serialized.includes(needle),
          true,
          `${fixture.name}: classifier request missing ${needle}`,
        );
      }
    }
  } else {
    assert.equal(
      classifier.calls.length,
      0,
      `${fixture.name}: expected zero classifier calls`,
    );
  }

  if (fixture.expected_attention) {
    assert.equal(
      reasons.some((reason) =>
        fixture.expected_attention_reason
          ? reason === fixture.expected_attention_reason
          : reason.startsWith("watchdog_") || reason === "silence_timeout",
      ),
      true,
      `${fixture.name}: expected attention reason`,
    );
  }

  if (fixture.expected_terminal_phase) {
    assert.equal(
      supervisor.inspect().phase,
      fixture.expected_terminal_phase,
      `${fixture.name}: terminal phase`,
    );
  }

  if (fixture.expected_terminal_reason) {
    assert.equal(
      reasons.some((reason) => reason === fixture.expected_terminal_reason),
      true,
      `${fixture.name}: terminal reason ${fixture.expected_terminal_reason}`,
    );
  }

  if (fixture.scripted_verdict?.verdict === "healthy") {
    assert.equal(
      reasons.some((reason) => reason.startsWith("watchdog_")),
      false,
      `${fixture.name}: healthy verdict must stay controller-silent`,
    );
  }
}

async function* replayFixture(fixture: Fixture, clock: FakeClock): AsyncIterable<AdapterEvent> {
  const events = fixture.events as AdapterEvent[];
  if (events.length === 0) {
    clock.advance(fixture.elapsed_ms);
    return;
  }
  const stepMs = 60_000;
  const totalSteps = Math.max(1, Math.ceil(fixture.elapsed_ms / stepMs));
  let eventIndex = 0;
  for (let step = 0; step < totalSteps; step += 1) {
    clock.advance(Math.min(stepMs, fixture.elapsed_ms - step * stepMs));
    if (eventIndex < events.length) {
      yield events[eventIndex];
      eventIndex += 1;
    } else if (fixture.expected_classifier_eligible) {
      yield {
        type: "raw.provider",
        harness: fixture.harness ?? "codex",
        payload: { type: "fixture_progress", step },
      };
      yield {
        type: "message.delta",
        message_id: `pad_${step}`,
        role: "assistant",
        delta: "continuing",
      };
    }
  }
  while (eventIndex < events.length) {
    yield events[eventIndex];
    eventIndex += 1;
  }
  if (fixture.process_exit) {
    yield {
      type: "error",
      code: "process_exit",
      message: `child process exited with code ${fixture.process_exit.exit_code}`,
      details: {
        exit_code: fixture.process_exit.exit_code,
        signal: fixture.process_exit.signal,
      },
      recoverable: false,
    };
    return;
  }
  yield {
    type: "turn.completed",
    final_text: "fixture complete",
    provider_thread_id: "fake-thread-1",
  };
}

test("complete fake-provider service lifecycle through packaged MCP declaration", async () => {
  const mcp = JSON.parse(await readFile(x_PackagedMcpPath, "utf8")) as {
    mcpServers: { xagent: { url: string } };
  };
  const packagedUrl = new URL(mcp.mcpServers.xagent.url);
  assert.equal(packagedUrl.pathname, "/mcp");

  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-e2e-repo-"));
  const logRoot = path.join(tmpdir(), `xagent-e2e-logs-${Math.random().toString(36).slice(2)}`);

  const releaseTurn = deferred<void>();
  const afterProgress = deferred<void>();
  async function* scriptedTurn(): AsyncIterable<AdapterEvent> {
    yield { type: "message.delta", message_id: "m1", role: "assistant", delta: "routine progress one" };
    yield { type: "tool.started", tool_call_id: "t1", name: "fake_tool", input: {} };
    yield { type: "tool.completed", tool_call_id: "t1", name: "fake_tool", status: "completed", output: { ok: true } };
    afterProgress.resolve(undefined);
    await releaseTurn.promise;
    yield { type: "message.completed", message_id: "m1", role: "assistant", text: "complete final assistant message" };
    yield { type: "turn.completed", final_text: "complete final assistant message", provider_thread_id: "fake-thread-1" };
  }

  let adapterCreated = 0;
  const runManager = new XagentRunManager({
    repoRoot,
    logRoot,
    adapterFactory: () => {
      adapterCreated += 1;
      return new FakeHarnessAdapter({ scriptedEvents: [scriptedTurn()] });
    },
    policy: testPolicy,
  });

  let server: XagentServer | undefined;
  const shutdownController = createShutdownController({
    closeRuns: async () => { await runManager.closeAll(); },
    closeServer: async () => { await server?.close(); },
  });
  server = createXagentServer({
    bindHost: "127.0.0.1",
    bindPort: 0,
    runManager,
    shutdownController,
  });
  const port = await server.listen();
  const baseUrl = `http://127.0.0.1:${port}`;

  const firstClient = new Client({ name: "xagent-e2e-first", version: "0.0.0" });
  const firstTransport = new StreamableHTTPClientTransport(new URL(`${baseUrl}/mcp`));
  await firstClient.connect(firstTransport);
  try {
    const cwd = await mkdtemp(path.join(tmpdir(), "xagent-e2e-cwd-"));
    const startResult = asBody(await firstClient.callTool({
      name: "xagent_start",
      arguments: { cwd, prompt: "implement the task", harness: "codex" },
    }));
    assert.equal(startResult.isError ?? false, false);
    const runId = startResult.body.run_id as string;
    assert.ok(runId.length > 0);
    assert.equal(runManager.has(runId), true);

    await afterProgress.promise;

    const abort = new AbortController();
    const awaiting = firstClient.callTool(
      { name: "xagent_await", arguments: { run_id: runId, after_sequence: startResult.body.sequence, deadline_seconds: 30 } },
      undefined,
      { signal: abort.signal },
    );
    abort.abort();
    await awaiting.catch(() => {});
    assert.equal(runManager.has(runId), true);

    await firstClient.close();
    await firstTransport.close();

    const secondClient = new Client({ name: "xagent-e2e-second", version: "0.0.0" });
    const secondTransport = new StreamableHTTPClientTransport(new URL(`${baseUrl}/mcp`));
    await secondClient.connect(secondTransport);
    try {
      const inspectResult = asBody(await secondClient.callTool({
        name: "xagent_inspect",
        arguments: { run_id: runId },
      }));
      assert.equal(inspectResult.body.run_id, runId);

      const awaitPromise = secondClient.callTool({
        name: "xagent_await",
        arguments: { run_id: runId, after_sequence: inspectResult.body.sequence, deadline_seconds: 30 },
      });
      releaseTurn.resolve(undefined);
      const awaitResult = asBody(await awaitPromise);
      assert.equal(awaitResult.body.event, "turn.completed");
      assert.equal((awaitResult.body.report as { text: string }).text, "complete final assistant message");
      assert.equal(awaitResult.body.run_id, runId);
      assert.equal("deltas" in awaitResult.body, false);
      assert.equal("tools" in awaitResult.body, false);

      const closeResult = asBody(await secondClient.callTool({
        name: "xagent_close",
        arguments: { run_id: runId },
      }));
      assert.equal(closeResult.body.closed, true);
    } finally {
      await secondClient.close().catch(() => {});
      await secondTransport.close().catch(() => {});
    }

    assert.equal(runManager.has(runId), false);
    assert.equal(runManager.size, 0);
    assert.equal(adapterCreated, 1);
  } finally {
    if (!shutdownController.wasShutdownRequested()) {
      await server.close();
    }
    await runManager.closeAll();
  }
});

test("health endpoint reports healthy and uptime on the ephemeral service", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-e2e-health-repo-"));
  const logRoot = path.join(tmpdir(), `xagent-e2e-health-logs-${Math.random().toString(36).slice(2)}`);
  const runManager = new XagentRunManager({
    repoRoot,
    logRoot,
    adapterFactory: () => new FakeHarnessAdapter(),
    policy: testPolicy,
  });
  let server: XagentServer | undefined;
  const shutdownController = createShutdownController({
    closeRuns: async () => { await runManager.closeAll(); },
    closeServer: async () => { await server?.close(); },
  });
  server = createXagentServer({
    bindHost: "127.0.0.1",
    bindPort: 0,
    runManager,
    shutdownController,
  });
  const port = await server.listen();
  try {
    const response = await fetch(`http://127.0.0.1:${port}/health`);
    assert.equal(response.status, 200);
    const body = await response.json() as { healthy: boolean; uptime: number };
    assert.equal(body.healthy, true);
    assert.ok(typeof body.uptime === "number" && body.uptime >= 0);
  } finally {
    if (!shutdownController.wasShutdownRequested()) {
      await server.close();
    }
    await runManager.closeAll();
  }
});

test("silence attention is delivered to a controller through xagent_await", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-e2e-attention-repo-"));
  const logRoot = path.join(tmpdir(), `xagent-e2e-attention-logs-${Math.random().toString(36).slice(2)}`);
  const shortSilencePolicy: SupervisionPolicy = {
    silenceTimeoutMs: 1_000,
    watchdog: {},
  };
  const releaseTurn = deferred<void>();
  async function* silentTurn(): AsyncIterable<AdapterEvent> {
    yield { type: "message.delta", message_id: "m1", role: "assistant", delta: "starting" };
    await releaseTurn.promise;
    yield { type: "turn.completed", final_text: "done after attention", provider_thread_id: "fake-thread-1" };
  }
  const runManager = new XagentRunManager({
    repoRoot,
    logRoot,
    adapterFactory: () => new FakeHarnessAdapter({ scriptedEvents: [silentTurn()] }),
    policy: shortSilencePolicy,
  });
  let server: XagentServer | undefined;
  const shutdownController = createShutdownController({
    closeRuns: async () => { await runManager.closeAll(); },
    closeServer: async () => { await server?.close(); },
  });
  server = createXagentServer({
    bindHost: "127.0.0.1",
    bindPort: 0,
    runManager,
    shutdownController,
  });
  const port = await server.listen();
  const baseUrl = `http://127.0.0.1:${port}`;
  const client = new Client({ name: "xagent-e2e-attention", version: "0.0.0" });
  const transport = new StreamableHTTPClientTransport(new URL(`${baseUrl}/mcp`));
  await client.connect(transport);
  try {
    const cwd = await mkdtemp(path.join(tmpdir(), "xagent-e2e-attention-cwd-"));
    const startResult = asBody(await client.callTool({
      name: "xagent_start",
      arguments: { cwd, prompt: "work then go silent", harness: "codex" },
    }));
    const runId = startResult.body.run_id as string;
    const cursor = startResult.body.sequence as number;

    const attentionResult = asBody(await client.callTool({
      name: "xagent_await",
      arguments: { run_id: runId, after_sequence: cursor, deadline_seconds: 30 },
    }));
    assert.equal(attentionResult.isError ?? false, false);
    assert.equal(attentionResult.body.event, "supervision.attention");
    assert.equal(attentionResult.body.reason, "silence_timeout");

    releaseTurn.resolve(undefined);
    const completionResult = asBody(await client.callTool({
      name: "xagent_await",
      arguments: { run_id: runId, after_sequence: attentionResult.body.sequence as number, deadline_seconds: 30 },
    }));
    assert.equal(completionResult.body.event, "turn.completed");

    const closeResult = asBody(await client.callTool({
      name: "xagent_close",
      arguments: { run_id: runId },
    }));
    assert.equal(closeResult.body.closed, true);
  } finally {
    await client.close().catch(() => {});
    await transport.close().catch(() => {});
    if (!shutdownController.wasShutdownRequested()) {
      await server.close();
    }
    await runManager.closeAll();
  }
});

test("cursor deduplication: a second await after the completion sequence does not redeliver it", async () => {
  const repoRoot = await mkdtemp(path.join(tmpdir(), "xagent-e2e-dedup-repo-"));
  const logRoot = path.join(tmpdir(), `xagent-e2e-dedup-logs-${Math.random().toString(36).slice(2)}`);
  const releaseTurn = deferred<void>();
  async function* scriptedTurn(): AsyncIterable<AdapterEvent> {
    yield { type: "message.delta", message_id: "m1", role: "assistant", delta: "progress" };
    await releaseTurn.promise;
    yield { type: "message.completed", message_id: "m1", role: "assistant", text: "final report" };
    yield { type: "turn.completed", final_text: "final report", provider_thread_id: "fake-thread-1" };
  }
  const runManager = new XagentRunManager({
    repoRoot,
    logRoot,
    adapterFactory: () => new FakeHarnessAdapter({ scriptedEvents: [scriptedTurn()] }),
    policy: testPolicy,
  });
  let server: XagentServer | undefined;
  const shutdownController = createShutdownController({
    closeRuns: async () => { await runManager.closeAll(); },
    closeServer: async () => { await server?.close(); },
  });
  server = createXagentServer({
    bindHost: "127.0.0.1",
    bindPort: 0,
    runManager,
    shutdownController,
  });
  const port = await server.listen();
  const baseUrl = `http://127.0.0.1:${port}`;
  const client = new Client({ name: "xagent-e2e-dedup", version: "0.0.0" });
  const transport = new StreamableHTTPClientTransport(new URL(`${baseUrl}/mcp`));
  await client.connect(transport);
  try {
    const cwd = await mkdtemp(path.join(tmpdir(), "xagent-e2e-dedup-cwd-"));
    const startResult = asBody(await client.callTool({
      name: "xagent_start",
      arguments: { cwd, prompt: "produce a final report", harness: "codex" },
    }));
    const runId = startResult.body.run_id as string;
    const startCursor = startResult.body.sequence as number;

    releaseTurn.resolve(undefined);

    const firstAwait = asBody(await client.callTool({
      name: "xagent_await",
      arguments: { run_id: runId, after_sequence: startCursor, deadline_seconds: 30 },
    }));
    assert.equal(firstAwait.body.event, "turn.completed");
    const completionSequence = firstAwait.body.sequence as number;

    const secondAwait = asBody(await client.callTool({
      name: "xagent_await",
      arguments: { run_id: runId, after_sequence: completionSequence, deadline_seconds: 2 },
    }));
    assert.ok(
      (secondAwait.body.sequence as number) > completionSequence || secondAwait.body.event === "supervision.deadline",
      "second await after the completion sequence must not redeliver that completion",
    );
    assert.notEqual(secondAwait.body.event, "turn.completed");

    const closeResult = asBody(await client.callTool({
      name: "xagent_close",
      arguments: { run_id: runId },
    }));
    assert.equal(closeResult.body.closed, true);
  } finally {
    await client.close().catch(() => {});
    await transport.close().catch(() => {});
    if (!shutdownController.wasShutdownRequested()) {
      await server.close();
    }
    await runManager.closeAll();
  }
});

type Fixture = {
  readonly name: string;
  readonly original_prompt: string;
  readonly harness?: "codex" | "claude_code";
  readonly elapsed_ms: number;
  readonly events: readonly unknown[];
  readonly process_exit?: { readonly exit_code: number; readonly signal: string | null };
  readonly expected_classifier_eligible: boolean;
  readonly expected_classifier_contains?: readonly string[];
  readonly scripted_verdict?: WatchdogVerdict;
  readonly expected_attention: boolean;
  readonly expected_attention_reason?: string;
  readonly expected_terminal_phase?: string;
  readonly expected_terminal_reason?: string;
};

class ClaudeFakeHarnessAdapter extends FakeHarnessAdapter {
  override readonly harness = "claude_code" as const;
}

class ScriptedClassifier implements WatchdogClassifier {
  readonly calls: WatchdogRequest[] = [];
  constructor(private readonly verdict: WatchdogVerdict | undefined) {}
  async classify(request: WatchdogRequest): Promise<WatchdogVerdict> {
    this.calls.push(request);
    return this.verdict ?? { verdict: "healthy", confidence: 0.95, reason_code: "steady_progress", evidence: [] };
  }
}

class FakeClock {
  #now = Date.parse("2026-07-25T12:00:00.000Z");
  readonly #scheduled: Array<{ callback: () => void; dueAt: number; cancelled: boolean }> = [];
  readonly now = (): Date => new Date(this.#now);
  advance(ms: number): void {
    this.#now += ms;
    let next;
    while ((next = this.#scheduled
      .filter(({ cancelled, dueAt }) => !cancelled && dueAt <= this.#now)
      .sort((l, r) => l.dueAt - r.dueAt)[0]) !== undefined) {
      next.cancelled = true;
      next.callback();
    }
  }
  setTimeout(callback: () => void, delayMs: number): unknown {
    const entry = { callback, dueAt: this.#now + delayMs, cancelled: false };
    this.#scheduled.push(entry);
    return entry;
  }
  clearTimeout(handle: unknown): void {
    (handle as { cancelled: boolean }).cancelled = true;
  }
}

function asBody(result: unknown): { body: Record<string, unknown>; isError?: boolean } {
  const value = result as { isError?: boolean; structuredContent?: unknown; content?: unknown };
  if (value.structuredContent && typeof value.structuredContent === "object") {
    return { body: value.structuredContent as Record<string, unknown>, isError: value.isError };
  }
  const content = value.content as Array<{ type?: string; text?: string }> | undefined;
  const text = content?.find((part) => part.type === "text")?.text;
  assert.ok(text);
  return { body: JSON.parse(text) as Record<string, unknown>, isError: value.isError };
}

function deferred<T>(): { promise: Promise<T>; resolve: (value: T) => void } {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((res) => { resolve = res; });
  return { promise, resolve };
}
