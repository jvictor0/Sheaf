import { randomUUID } from "node:crypto";
import type { IncomingMessage, ServerResponse } from "node:http";

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { isInitializeRequest } from "@modelcontextprotocol/sdk/types.js";

import {
  StartAwaitLivenessPings,
  x_AwaitLivenessPingIntervalMs,
} from "./await_liveness.js";
import type { XagentRunManager } from "./run_manager.js";
import {
  parseToolInput,
  structuredErrorFromUnknown,
  XagentAwaitAdvertisedSchema,
  XagentAwaitInputSchema,
  XagentCloseInputSchema,
  XagentInspectInputSchema,
  XagentInterruptInputSchema,
  XagentListInputSchema,
  XagentMessageInputSchema,
  XagentStartInputSchema,
  type StructuredToolError,
} from "./tool_schemas.js";

type SessionEntry = {
  readonly server: McpServer;
  readonly transport: StreamableHTTPServerTransport;
};

export type XagentMcpHandler = {
  handleRequest(request: IncomingMessage, response: ServerResponse): Promise<void>;
  close(): Promise<void>;
};

export type XagentMcpHandlerOptions = {
  readonly runManager: XagentRunManager;
  // DNS rebinding protection: the transport rejects any request whose Host
  // or Origin header is not in the allow list. The lists are provided as
  // getters because the actual listen port is unknown until `listen()`
  // resolves when the service binds to port 0; the handler constructs a
  // fresh transport per session, so by the time a client initializes a
  // session the getters return the populated values.
  //
  readonly getAllowedHosts: () => string[];
  readonly getAllowedOrigins: () => string[];
  // Test-only override for the await liveness ping cadence. Production uses
  // x_AwaitLivenessPingIntervalMs.
  //
  readonly awaitLivenessPingIntervalMs?: number;
};

export function createXagentMcpHandler(options: XagentMcpHandlerOptions): XagentMcpHandler {
  const { runManager, getAllowedHosts, getAllowedOrigins } = options;
  const awaitLivenessPingIntervalMs =
    options.awaitLivenessPingIntervalMs ?? x_AwaitLivenessPingIntervalMs;
  const sessions = new Map<string, SessionEntry>();

  async function handleRequest(
    request: IncomingMessage,
    response: ServerResponse,
  ): Promise<void> {
    const method = request.method ?? "GET";
    if (method !== "POST" && method !== "GET" && method !== "DELETE") {
      sendJsonRpcError(response, 405, -32000, "Method not allowed.");
      return;
    }

    try {
      const sessionIdHeader = request.headers["mcp-session-id"];
      const sessionId = Array.isArray(sessionIdHeader) ? sessionIdHeader[0] : sessionIdHeader;

      if (sessionId !== undefined && sessions.has(sessionId)) {
        const session = sessions.get(sessionId)!;
        await session.transport.handleRequest(request, response);
        return;
      }

      if (method === "GET" || method === "DELETE") {
        sendJsonRpcError(response, 400, -32000, "Invalid or missing MCP session.");
        return;
      }

      const body = await readJsonBody(request);
      if (!isInitializeRequest(body)) {
        sendJsonRpcError(response, 400, -32000, "Bad Request: No valid session ID provided");
        return;
      }

      const server = createConfiguredMcpServer(
        runManager,
        awaitLivenessPingIntervalMs,
      );
      let transport!: StreamableHTTPServerTransport;
      transport = new StreamableHTTPServerTransport({
        sessionIdGenerator: () => randomUUID(),
        // JSON-response mode silently drops request-scoped notifications
        // (webStandardStreamableHttp.js:702) and leaves the pending POST as a
        // silent socket that nothing can keep alive. SSE mode sends headers
        // immediately and lets progress pings reset body/request timers.
        //
        enableJsonResponse: false,
        // The MCP Streamable HTTP specification requires local servers to
        // validate Host and Origin to prevent DNS rebinding, which would
        // otherwise let a browser page reach the loopback endpoint and
        // launch privileged local agent processes. The SDK ships this guard
        // as a constructor option; we enable it and supply the loopback
        // allow lists derived from the actual listen address.
        //
        enableDnsRebindingProtection: true,
        allowedHosts: getAllowedHosts(),
        allowedOrigins: getAllowedOrigins(),
        onsessioninitialized: (initializedSessionId) => {
          sessions.set(initializedSessionId, { server, transport });
        },
      });
      transport.onclose = () => {
        const closedSessionId = transport.sessionId;
        if (closedSessionId !== undefined) {
          sessions.delete(closedSessionId);
        }
      };
      await server.connect(transport);
      await transport.handleRequest(request, response, body);
    } catch (error) {
      if (!response.headersSent) {
        sendJsonRpcError(
          response,
          500,
          -32603,
          error instanceof Error ? error.message : "Internal server error",
        );
      }
    }
  }

  return {
    handleRequest,
    async close(): Promise<void> {
      const entries = [...sessions.values()];
      sessions.clear();
      await Promise.allSettled(
        entries.map(async (entry) => {
          await entry.transport.close().catch(() => {});
          await entry.server.close().catch(() => {});
        }),
      );
    },
  };
}

function createConfiguredMcpServer(
  runManager: XagentRunManager,
  awaitLivenessPingIntervalMs: number,
): McpServer {
  const server = new McpServer({
    name: "xagent",
    version: "0.1.0",
  });

  server.registerTool(
    "xagent_start",
    {
      title: "Start supervised run",
      description:
        "Validate an absolute working directory, start a service-owned supervised run, and submit the initial prompt. "
        + "Use for reviews, implementation, fixes, and other delegated work.",
      inputSchema: XagentStartInputSchema,
    },
    async (args) => {
      return runTool(async () => {
        const input = parseToolInput(XagentStartInputSchema, args);
        return runManager.startRun(input);
      });
    },
  );

  server.registerTool(
    "xagent_await",
    {
      title: "Await supervised event",
      description:
        "Block until a durable completion or attention event after the given cursor, or until the await deadline.",
      inputSchema: XagentAwaitAdvertisedSchema,
    },
    async (args, extra) => {
      return runTool(async () => {
        const input = parseToolInput(XagentAwaitInputSchema, args);
        const progressToken = extra._meta?.progressToken;
        if (progressToken === undefined) {
          return runManager.awaitRun(input, extra.signal);
        }
        const stopPinging = StartAwaitLivenessPings({
          progressToken,
          intervalMs: awaitLivenessPingIntervalMs,
          isVouching: () => runManager.isRunVouching(input.run_id),
          sendNotification: (notification) => extra.sendNotification(notification),
          signal: extra.signal,
        });
        try {
          return await runManager.awaitRun(input, extra.signal);
        } finally {
          stopPinging();
        }
      });
    },
  );

  server.registerTool(
    "xagent_inspect",
    {
      title: "Inspect supervised run",
      description: "Return compact phase and cursor metadata for an owned run.",
      inputSchema: XagentInspectInputSchema,
    },
    async (args) => {
      return runTool(async () => {
        const input = parseToolInput(XagentInspectInputSchema, args);
        return runManager.inspectRun(input);
      });
    },
  );

  server.registerTool(
    "xagent_list",
    {
      title: "List supervised runs",
      description:
        "List service-owned runs, newest first, for recovery — use it when a start response was lost and its run id is unknown. This is not a progress-polling tool.",
      inputSchema: XagentListInputSchema,
    },
    async (args) => {
      return runTool(async () => {
        const input = parseToolInput(XagentListInputSchema, args);
        return runManager.listOwnedRuns(input);
      });
    },
  );

  server.registerTool(
    "xagent_message",
    {
      title: "Message supervised run",
      description: "Submit a follow-up message when the supervised session is ready.",
      inputSchema: XagentMessageInputSchema,
    },
    async (args) => {
      return runTool(async () => {
        const input = parseToolInput(XagentMessageInputSchema, args);
        return runManager.messageRun(input);
      });
    },
  );

  server.registerTool(
    "xagent_interrupt",
    {
      title: "Interrupt supervised run",
      description: "Interrupt the active provider turn without closing the owned session.",
      inputSchema: XagentInterruptInputSchema,
    },
    async (args) => {
      return runTool(async () => {
        const input = parseToolInput(XagentInterruptInputSchema, args);
        return runManager.interruptRun(input);
      });
    },
  );

  server.registerTool(
    "xagent_close",
    {
      title: "Close supervised run",
      description: "Close the supervised session and owned child processes.",
      inputSchema: XagentCloseInputSchema,
    },
    async (args) => {
      return runTool(async () => {
        const input = parseToolInput(XagentCloseInputSchema, args);
        return runManager.closeRun(input);
      });
    },
  );

  return server;
}

async function runTool(
  operation: () => Promise<unknown> | unknown,
): Promise<{
  content: Array<{ type: "text"; text: string }>;
  structuredContent: Record<string, unknown>;
  isError?: boolean;
}> {
  try {
    const value = await operation();
    const structuredContent = asStructuredObject(value);
    return {
      content: [{ type: "text", text: JSON.stringify(structuredContent) }],
      structuredContent,
    };
  } catch (error) {
    return toolErrorResult(structuredErrorFromUnknown(error));
  }
}

function toolErrorResult(structured: StructuredToolError): {
  content: Array<{ type: "text"; text: string }>;
  structuredContent: Record<string, unknown>;
  isError: true;
} {
  const structuredContent = {
    error: structured.error,
    message: structured.message,
    ...(structured.details === undefined ? {} : { details: structured.details }),
  };
  return {
    content: [{ type: "text", text: JSON.stringify(structuredContent) }],
    structuredContent,
    isError: true,
  };
}

function asStructuredObject(value: unknown): Record<string, unknown> {
  if (value !== null && typeof value === "object" && !Array.isArray(value)) {
    return value as Record<string, unknown>;
  }
  return { value };
}

async function readJsonBody(request: IncomingMessage): Promise<unknown> {
  const chunks: Buffer[] = [];
  for await (const chunk of request) {
    chunks.push(typeof chunk === "string" ? Buffer.from(chunk) : chunk);
  }
  if (chunks.length === 0) {
    return undefined;
  }
  const text = Buffer.concat(chunks).toString("utf8");
  if (text.length === 0) {
    return undefined;
  }
  return JSON.parse(text) as unknown;
}

function sendJsonRpcError(
  response: ServerResponse,
  statusCode: number,
  code: number,
  message: string,
): void {
  const payload = JSON.stringify({
    jsonrpc: "2.0",
    error: { code, message },
    id: null,
  });
  response.statusCode = statusCode;
  response.setHeader("Content-Type", "application/json; charset=utf-8");
  response.setHeader("Content-Length", Buffer.byteLength(payload));
  response.end(payload);
}
