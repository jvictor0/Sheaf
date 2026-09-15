import assert from "node:assert/strict";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";

import { FakeHarnessAdapter } from "../src/adapters/fake.js";
import type { AdapterEvent } from "../src/adapters/types.js";

import {
  XagentAwaitAdvertisedSchema,
  XagentAwaitInputSchema,
  XagentListInputSchema,
} from "../src/service/tool_schemas.js";
import {
  asToolCallResult,
  assertInvalidWorkingDirectory,
  assertToolSucceeded,
  structuredToolBody,
  withMcpService,
} from "./support/mcp_service.js";

function deferred<T = void>(): {
  promise: Promise<T>;
  resolve: (value: T) => void;
} {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((res) => {
    resolve = res;
  });
  return { promise, resolve };
}

const x_GenericToolNames = [
  "xagent_start",
  "xagent_await",
  "xagent_inspect",
  "xagent_message",
  "xagent_interrupt",
  "xagent_close",
  "xagent_list",
] as const;

test("Streamable HTTP MCP initializes and discovers exactly the seven controller tools", async () => {
  await withMcpService(async ({ port, client }) => {
    const listed = await client.listTools();
    const names = listed.tools.map((tool) => tool.name).sort();
    assert.deepEqual(names, [...x_GenericToolNames].sort());
    assert.equal(listed.tools.length, 7);

    const health = await fetch(`http://127.0.0.1:${port}/health`);
    assert.equal(health.status, 200);
  });
});

test("removed SDD tools and the old start name cannot dispatch work", async () => {
  await withMcpService(async ({ client, runManager }) => {
    for (const name of ["xagent_sdd_start", "xagent_sdd_followup", "xagent_start_non_sdd"]) {
      const result = asToolCallResult(await client.callTool({ name, arguments: {} }));
      assert.equal(result.isError, true, `${name} must be unavailable`);
    }
    assert.deepEqual(runManager.listRunIds(), []);
  });
});

test("two reviewers can exchange findings through start, await, message, and close", async () => {
  const cwd = await mkdtemp(path.join(tmpdir(), "xagent-review-rounds-"));
  await withMcpService(async ({ client }) => {
    async function call(name: string, args: Record<string, unknown>) {
      const result = asToolCallResult(await client.callTool({ name, arguments: args }));
      assertToolSucceeded(result);
      return structuredToolBody(result);
    }
    const first = await call("xagent_start", { cwd, harness: "codex", prompt: "Review change A" });
    const second = await call("xagent_start", { cwd, harness: "codex", prompt: "Review change B" });
    assert.notEqual(first.run_id, second.run_id);
    const reports: string[] = [];
    for (const started of [first, second]) {
      const initial = await call("xagent_await", {
        run_id: started.run_id, after_sequence: started.sequence,
      });
      assert.equal(initial.event, "turn.completed");
      reports.push((initial.report as { text: string }).text);
    }
    for (const [index, started] of [first, second].entries()) {
      const text = `Check the other review: ${reports[1 - index]}`;
      const followup = await call("xagent_message", { run_id: started.run_id, text });
      assert.equal(followup.run_id, started.run_id);
      assert.ok(Number(followup.sequence) > Number(started.sequence));
      const checked = await call("xagent_await", {
        run_id: started.run_id, after_sequence: followup.sequence,
      });
      assert.equal(checked.event, "turn.completed");
      assert.equal((checked.report as { text: string }).text, `fake response to ${text}`);
    }
    const listed = await call("xagent_list", {});
    assert.equal((listed.runs as unknown[]).length, 2);
    for (const started of [first, second]) {
      const closed = await call("xagent_close", { run_id: started.run_id });
      assert.equal(closed.closed, true);
    }
  });
});

test("xagent_start rejects relative cwd with invalid_working_directory and creates no run", async () => {
  await withMcpService(async ({ client, runManager }) => {
    const result = await client.callTool({
      name: "xagent_start",
      arguments: {
        cwd: "relative/worktree",
        prompt: "hello",
        harness: "codex",
      },
    });
    assertInvalidWorkingDirectory(result);
    assert.deepEqual(runManager.listRunIds(), []);
  });
});

test("xagent_start rejects missing cwd with invalid_working_directory and creates no run", async () => {
  await withMcpService(async ({ client, runManager }) => {
    const missing = path.join(tmpdir(), `xagent-missing-${Math.random().toString(36).slice(2)}`);
    const result = await client.callTool({
      name: "xagent_start",
      arguments: {
        cwd: missing,
        prompt: "hello",
        harness: "codex",
      },
    });
    assertInvalidWorkingDirectory(result);
    assert.deepEqual(runManager.listRunIds(), []);
  });
});

test("xagent_start rejects non-directory cwd with invalid_working_directory and creates no run", async () => {
  await withMcpService(async ({ client, runManager }) => {
    const dir = await mkdtemp(path.join(tmpdir(), "xagent-mcp-file-"));
    const filePath = path.join(dir, "not-a-directory");
    await writeFile(filePath, "not a directory\n", "utf8");
    const result = await client.callTool({
      name: "xagent_start",
      arguments: {
        cwd: filePath,
        prompt: "hello",
        harness: "codex",
      },
    });
    assertInvalidWorkingDirectory(result);
    assert.deepEqual(runManager.listRunIds(), []);
  });
});

test("xagent_start accepts an absolute existing directory and returns run_id plus cursor", async () => {
  await withMcpService(async ({ client, runManager }) => {
    const cwd = await mkdtemp(path.join(tmpdir(), "xagent-mcp-ok-"));
    const result = asToolCallResult(
      await client.callTool({
        name: "xagent_start",
        arguments: {
          cwd,
          prompt: "hello from mcp",
          harness: "codex",
        },
      }),
    );
    assertToolSucceeded(result);
    const body = structuredToolBody(result);
    assert.equal(typeof body.run_id, "string");
    assert.ok((body.run_id as string).length > 0);
    assert.equal(typeof body.sequence, "number");
    assert.equal(runManager.has(body.run_id as string), true);
    const phase = runManager.inspect(body.run_id as string)?.phase;
    assert.ok(
      phase === "ready" || phase === "running",
      `expected phase ready|running, got ${phase}`,
    );

    const closed = asToolCallResult(
      await client.callTool({
        name: "xagent_close",
        arguments: { run_id: body.run_id },
      }),
    );
    assertToolSucceeded(closed);
  });
});

test("xagent_list returns owned runs newest first and flags the live one", async () => {
  const cwd = await mkdtemp(path.join(tmpdir(), "xagent-mcp-list-"));

  await withMcpService(async ({ client }) => {
    const startedResult = asToolCallResult(
      await client.callTool({
        name: "xagent_start",
        arguments: { cwd, prompt: "hello", harness: "codex" },
      }),
    );
    assertToolSucceeded(startedResult);
    const started = structuredToolBody(startedResult);

    const listedResult = asToolCallResult(
      await client.callTool({ name: "xagent_list", arguments: {} }),
    );
    assertToolSucceeded(listedResult);
    const rows = structuredToolBody(listedResult).runs as Array<Record<string, unknown>>;

    const row = rows.find((entry) => entry.run_id === started.run_id);
    assert.ok(row, "the started run must be listed");
    assert.equal(row.live, true);
    assert.equal(row.supervised, true);
    assert.equal(row.harness, "codex");
    assert.equal(typeof row.phase, "string");
    assert.equal(typeof row.created_at, "string");
  });
});

test("xagent_list defaults its paging arguments and rejects unknown ones", () => {
  const defaults = XagentListInputSchema.parse({});
  assert.equal(defaults.live_only, false);
  assert.equal(defaults.limit, 50);

  assert.equal(XagentListInputSchema.safeParse({ bogus: true }).success, false);
  assert.equal(XagentListInputSchema.safeParse({ limit: 0 }).success, false);
  assert.equal(XagentListInputSchema.safeParse({ limit: 201 }).success, false);
});

test("advertised xagent_await offers run_id and after_sequence only", () => {
  const advertisedKeys = Object.keys(XagentAwaitAdvertisedSchema.shape).sort();
  assert.deepEqual(advertisedKeys, ["after_sequence", "run_id"]);
  assert.equal("deadline_seconds" in XagentAwaitAdvertisedSchema.shape, false);

  for (const [key, field] of Object.entries(XagentAwaitInputSchema.shape)) {
    if (field.isOptional()) continue;
    assert.ok(
      advertisedKeys.includes(key),
      `XagentAwaitAdvertisedSchema is missing required field "${key}"`,
    );
  }

  const fromAdvertised = { run_id: "xrun_1", after_sequence: 0 };
  assert.equal(XagentAwaitAdvertisedSchema.safeParse(fromAdvertised).success, true);
  const parsed = XagentAwaitInputSchema.safeParse(fromAdvertised);
  assert.equal(parsed.success, true, "a call built from the advertised schema alone must parse");
  if (parsed.success) {
    assert.equal(typeof parsed.data.deadline_seconds, "number");
  }

  assert.equal(
    XagentAwaitInputSchema.safeParse({
      run_id: "xrun_1",
      after_sequence: 0,
      deadline_seconds: 30,
    }).success,
    true,
    "internal callers may still pass deadline_seconds",
  );
});

test("xagent_await tools/list omits deadline_seconds", async () => {
  await withMcpService(async ({ client }) => {
    const listed = await client.listTools();
    const tool = listed.tools.find((entry) => entry.name === "xagent_await");
    assert.ok(tool, "xagent_await must be registered");
    const properties = (tool.inputSchema as { properties?: Record<string, unknown> })
      .properties ?? {};
    assert.ok("run_id" in properties);
    assert.ok("after_sequence" in properties);
    assert.equal("deadline_seconds" in properties, false);
  });
});

test("xagent_await still honours an unadvertised deadline_seconds over MCP", async () => {
  const releaseTurn = deferred<void>();
  async function* scriptedTurn(): AsyncIterable<AdapterEvent> {
    yield {
      type: "message.delta",
      message_id: "m_deadline_passthrough",
      role: "assistant",
      delta: "holding past a short deadline",
    };
    await releaseTurn.promise;
    yield {
      type: "message.completed",
      message_id: "m_deadline_passthrough",
      role: "assistant",
      text: "too late",
    };
    yield {
      type: "turn.completed",
      final_text: "too late",
      provider_thread_id: "fake-thread-deadline-pt",
    };
  }

  await withMcpService(async ({ client }) => {
    const cwd = await mkdtemp(path.join(tmpdir(), "xagent-await-deadline-"));
    const started = asToolCallResult(await client.callTool({
      name: "xagent_start",
      arguments: {
        cwd,
        prompt: "deadline must survive advertised parse",
        harness: "codex",
        mode: "subagent",
      },
    }));
    assertToolSucceeded(started);
    const startedBody = structuredToolBody(started);
    const start = Date.now();
    const result = asToolCallResult(await client.callTool({
      name: "xagent_await",
      arguments: {
        run_id: startedBody.run_id,
        after_sequence: startedBody.sequence,
        deadline_seconds: 1,
      },
    }));
    const elapsedMs = Date.now() - start;
    assertToolSucceeded(result);
    const body = structuredToolBody(result);
    assert.equal(body.event, "supervision.deadline");
    assert.equal(body.reason, "await_deadline");
    assert.ok(
      elapsedMs < 5_000,
      `deadline_seconds was stripped (elapsed ${elapsedMs}ms); advertised parse must passthrough`,
    );
    releaseTurn.resolve(undefined);
  }, {
    adapterFactory: () => new FakeHarnessAdapter({ scriptedEvents: [scriptedTurn()] }),
  });
});
