import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

import { FakeHarnessAdapter } from "../../src/adapters/fake.js";
import type { HarnessAdapter } from "../../src/adapters/types.js";
import { createXagentMcpHandler } from "../../src/service/mcp.js";
import { XagentRunManager } from "../../src/service/run_manager.js";
import {
  createShutdownController,
  createXagentServer,
  type XagentServer,
} from "../../src/service/server.js";
import type { SupervisionPolicy } from "../../src/supervision/types.js";

export type ToolCallResult = {
  readonly isError?: boolean;
  readonly content?: unknown;
  readonly structuredContent?: unknown;
};

export function asToolCallResult(result: unknown): ToolCallResult
{
  return result as ToolCallResult;
}

export function structuredToolBody(result: ToolCallResult): Record<string, unknown>
{
  if (
    result.structuredContent !== undefined
    && result.structuredContent !== null
    && typeof result.structuredContent === "object"
  )
  {
    return result.structuredContent as Record<string, unknown>;
  }
  assert.ok(Array.isArray(result.content));
  const textPart = (result.content as Array<{ type?: string; text?: string }>).find(
    (part) => part.type === "text" && typeof part.text === "string",
  );
  assert.ok(textPart?.text);
  return JSON.parse(textPart.text) as Record<string, unknown>;
}

export function assertToolSucceeded(result: ToolCallResult): void
{
  assert.equal(result.isError ?? false, false);
}

export function assertInvalidWorkingDirectory(result: unknown): void
{
  const toolResult = asToolCallResult(result);
  assert.equal(toolResult.isError, true);
  const body = structuredToolBody(toolResult);
  assert.equal(body.error, "invalid_working_directory");
}

export type McpServiceContext = {
  readonly port: number;
  readonly server: XagentServer;
  readonly runManager: XagentRunManager;
  readonly client: Client;
  readonly logRoot: string;
};

export type WithMcpServiceOptions = {
  readonly repoRoot?: string;
  readonly logRoot?: string;
  readonly policy?: SupervisionPolicy;
  readonly adapterFactory?: () => HarnessAdapter;
  readonly clientName?: string;
  readonly awaitLivenessPingIntervalMs?: number;
};

export type NormalizedLogEvent = {
  readonly type: string;
  readonly payload?: unknown;
  readonly [key: string]: unknown;
};

export type StartedMcpService = {
  startRun(prompt: string): Promise<{ run_id: string; sequence: number }>;
  listRuns(): Promise<Record<string, unknown>>;
  await(
    runId: string,
    afterSequence: number,
    deadlineSeconds: number,
  ): Promise<{ event: string }>;
  awaitTurn(
    runId: string,
    afterSequence: number,
    deadlineSeconds?: number,
  ): Promise<Record<string, unknown>>;
  message(runId: string, text: string): Promise<{ sequence: number }>;
  submit(runId: string, text: string): Promise<void>;
  closeRun(runId: string): Promise<void>;
  artifact(name: string): string;
  normalizedEvents(runId: string): Promise<NormalizedLogEvent[]>;
  killAbruptly(): Promise<void>;
  ensureClient(): Promise<Client>;
  close(): Promise<void>;
  readonly runManager: XagentRunManager;
  readonly logRoot: string;
};

export type StartMcpServiceOptions = {
  readonly repoRoot?: string;
  readonly logRoot?: string;
  readonly policy?: SupervisionPolicy;
  readonly adapterFactory?: () => HarnessAdapter;
  readonly awaitLivenessPingIntervalMs?: number;
};

// Task 9 MCP harness. The HTTP listener and client start lazily on first MCP
// use so mcp_await callers that only need run-manager helpers pay nothing.
//
export async function startMcpService(
  options: StartMcpServiceOptions = {},
): Promise<StartedMcpService>
{
  // Own only what we created: a caller-supplied logRoot belongs to the caller
  // (Task 9's restart tests reopen one across two services), but the cwd is
  // always ours. Mirrors withMcpService's ownsLogRoot handling below.
  //
  const ownsLogRoot = options.logRoot === undefined;
  const logRoot = options.logRoot ?? await mkdtemp(path.join(tmpdir(), "xagent-await-filter-"));
  const cwd = await mkdtemp(path.join(tmpdir(), "xagent-await-cwd-"));
  const artifactsDir = await mkdtemp(path.join(tmpdir(), "xagent-test-artifacts-"));
  const repoRoot = options.repoRoot ?? process.cwd();
  const runManager = new XagentRunManager({
    repoRoot,
    logRoot,
    adapterFactory: options.adapterFactory ?? (() => new FakeHarnessAdapter()),
    policy: options.policy ?? {
      silenceTimeoutMs: 600_000,
      watchdog: {},
    },
  });

  let server: XagentServer | undefined;
  let client: Client | undefined;
  let transport: StreamableHTTPClientTransport | undefined;
  let mcpReady: Promise<void> | undefined;
  let closed = false;

  async function EnsureMcp(): Promise<Client>
  {
    if (client !== undefined)
    {
      return client;
    }
    if (mcpReady === undefined)
    {
      mcpReady = (async () =>
      {
        const shutdownController = createShutdownController({
          closeRuns: async () =>
          {
            await runManager.closeAll();
          },
          closeServer: async () =>
          {
            await server?.close();
          },
        });
        const allowedHosts = new Set<string>([
          "127.0.0.1",
          "localhost",
          "[::1]",
          "127.0.0.1:9005",
          "localhost:9005",
        ]);
        const allowedOrigins = new Set<string>([
          "http://127.0.0.1",
          "http://localhost",
          "http://[::1]",
          "http://127.0.0.1:9005",
          "http://localhost:9005",
        ]);
        const mcpHandler = createXagentMcpHandler({
          runManager,
          getAllowedHosts: () => [...allowedHosts],
          getAllowedOrigins: () => [...allowedOrigins],
          ...(options.awaitLivenessPingIntervalMs === undefined
            ? {}
            : { awaitLivenessPingIntervalMs: options.awaitLivenessPingIntervalMs }),
        });
        server = createXagentServer({
          bindHost: "127.0.0.1",
          bindPort: 0,
          runManager,
          shutdownController,
          mcpHandler,
        });
        const port = await server.listen();
        allowedHosts.add(`127.0.0.1:${port}`);
        allowedHosts.add(`localhost:${port}`);
        allowedOrigins.add(`http://127.0.0.1:${port}`);
        allowedOrigins.add(`http://localhost:${port}`);
        const nextClient = new Client({
          name: "xagent-start-mcp-service",
          version: "0.0.0",
        });
        const nextTransport = new StreamableHTTPClientTransport(
          new URL(`http://127.0.0.1:${port}/mcp`),
        );
        await nextClient.connect(nextTransport);
        client = nextClient;
        transport = nextTransport;
      })();
    }
    await mcpReady;
    assert.ok(client !== undefined, "MCP client failed to start");
    return client;
  }

  function Artifact(name: string): string
  {
    return path.join(artifactsDir, name);
  }

  async function EnsureArtifact(filePath: string, contents: string): Promise<void>
  {
    await writeFile(filePath, contents, "utf8");
  }

  async function CallTool(
    name: string,
    args: Record<string, unknown>,
  ): Promise<ToolCallResult>
  {
    const mcpClient = await EnsureMcp();
    return asToolCallResult(await mcpClient.callTool({ name, arguments: args }));
  }

  async function CallToolOrThrow(
    name: string,
    args: Record<string, unknown>,
  ): Promise<Record<string, unknown>>
  {
    const result = await CallTool(name, args);
    const body = structuredToolBody(result);
    if (result.isError === true)
    {
      const error = new Error(
        typeof body.message === "string"
          ? body.message
          : typeof body.error === "string"
          ? body.error
          : `MCP tool ${name} failed`,
      );
      (error as { body?: Record<string, unknown> }).body = body;
      throw error;
    }
    return body;
  }

  async function Teardown(options: { readonly abrupt: boolean }): Promise<void>
  {
    if (closed)
    {
      return;
    }
    closed = true;
    // A teardown racing an in-flight EnsureMcp() would find every handle
    // undefined, skip all three guards, and leak the listener that finishes
    // binding afterwards. Settle the construction first.
    //
    await mcpReady?.catch(() => {});
    if (client !== undefined)
    {
      await client.close().catch(() => {});
    }
    if (transport !== undefined)
    {
      await transport.close().catch(() => {});
    }
    if (server !== undefined)
    {
      await server.close().catch(() => {});
    }
    await runManager.closeAll().catch(() => {});
    // cwd / artifactsDir are not the durability surface under test.
    //
    await rm(cwd, { recursive: true, force: true });
    await rm(artifactsDir, { recursive: true, force: true });
    if (!options.abrupt && ownsLogRoot)
    {
      await rm(logRoot, { recursive: true, force: true });
    }
  }

  return {
    runManager,
    logRoot,
    ensureClient: EnsureMcp,
    startRun(prompt: string)
    {
      return runManager.startRun({
        cwd,
        prompt,
        harness: "codex",
        mode: "subagent",
      });
    },
    async listRuns()
    {
      return structuredToolBody(await CallTool("xagent_list", {}));
    },
    await(runId: string, afterSequence: number, deadlineSeconds: number)
    {
      return runManager.awaitRun({
        run_id: runId,
        after_sequence: afterSequence,
        deadline_seconds: deadlineSeconds,
      });
    },
    async awaitTurn(runId: string, afterSequence: number, deadlineSeconds = 30)
    {
      return CallToolOrThrow("xagent_await", {
        run_id: runId,
        after_sequence: afterSequence,
        deadline_seconds: deadlineSeconds,
      });
    },
    async message(runId: string, text: string)
    {
      const body = await CallToolOrThrow("xagent_message", {
        run_id: runId,
        text,
      });
      return { sequence: Number(body.sequence) };
    },
    submit(runId: string, text: string)
    {
      return runManager.submit(runId, text);
    },
    async closeRun(runId: string)
    {
      await CallToolOrThrow("xagent_close", { run_id: runId });
    },
    artifact(name: string)
    {
      return Artifact(name);
    },
    async normalizedEvents(runId: string)
    {
      const raw = await readFile(path.join(logRoot, runId, "normalized.jsonl"), "utf8");
      return raw
        .split("\n")
        .filter(Boolean)
        .map((line) => JSON.parse(line) as NormalizedLogEvent);
    },
    async killAbruptly()
    {
      await Teardown({ abrupt: true });
    },
    async close()
    {
      await Teardown({ abrupt: false });
    },
  };
}

export async function withMcpService(
  run: (context: McpServiceContext) => Promise<void>,
  options: WithMcpServiceOptions = {},
): Promise<void>
{
  const repoRoot = options.repoRoot ?? process.cwd();
  const ownsLogRoot = options.logRoot === undefined;
  const logRoot = options.logRoot ?? await mkdtemp(path.join(tmpdir(), "xagent-mcp-"));
  const runManager = new XagentRunManager({
    repoRoot,
    logRoot,
    adapterFactory: options.adapterFactory ?? (() => new FakeHarnessAdapter()),
    policy: options.policy ?? {
      silenceTimeoutMs: 60_000,
      watchdog: {},
    },
  });
  let server: XagentServer | undefined;
  const shutdownController = createShutdownController({
    closeRuns: async () =>
    {
      await runManager.closeAll();
    },
    closeServer: async () =>
    {
      await server?.close();
    },
  });
  const allowedHosts = new Set<string>([
    "127.0.0.1",
    "localhost",
    "[::1]",
    "127.0.0.1:9005",
    "localhost:9005",
  ]);
  const allowedOrigins = new Set<string>([
    "http://127.0.0.1",
    "http://localhost",
    "http://[::1]",
    "http://127.0.0.1:9005",
    "http://localhost:9005",
  ]);
  const mcpHandler = createXagentMcpHandler({
    runManager,
    getAllowedHosts: () => [...allowedHosts],
    getAllowedOrigins: () => [...allowedOrigins],
    ...(options.awaitLivenessPingIntervalMs === undefined
      ? {}
      : { awaitLivenessPingIntervalMs: options.awaitLivenessPingIntervalMs }),
  });
  server = createXagentServer({
    bindHost: "127.0.0.1",
    bindPort: 0,
    runManager,
    shutdownController,
    mcpHandler,
  });
  const port = await server.listen();
  allowedHosts.add(`127.0.0.1:${port}`);
  allowedHosts.add(`localhost:${port}`);
  allowedOrigins.add(`http://127.0.0.1:${port}`);
  allowedOrigins.add(`http://localhost:${port}`);
  const client = new Client({
    name: options.clientName ?? "xagent-mcp-test",
    version: "0.0.0",
  });
  const transport = new StreamableHTTPClientTransport(
    new URL(`http://127.0.0.1:${port}/mcp`),
  );
  try
  {
    await client.connect(transport);
    await run({
      port,
      server,
      runManager,
      client,
      logRoot,
    });
  }
  finally
  {
    await client.close().catch(() => {});
    await transport.close().catch(() => {});
    if (!shutdownController.wasShutdownRequested())
    {
      await server.close();
    }
    await runManager.closeAll();
    if (ownsLogRoot)
    {
      await rm(logRoot, { recursive: true, force: true });
    }
  }
}
