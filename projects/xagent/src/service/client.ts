import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import type { Progress } from "@modelcontextprotocol/sdk/types.js";

import {
  x_AwaitLivenessPingIntervalMs,
} from "./await_liveness.js";
import {
  XAGENT_DEFAULT_BIND_HOST,
  XAGENT_DEFAULT_BIND_PORT,
} from "./config.js";
import type {
  AwaitRunResult,
  CloseRunResult,
  InspectRunResult,
  InterruptRunResult,
  ListRunsResult,
  MessageRunResult,
  StartRunResult,
} from "./run_manager.js";
import type {
  StructuredToolError,
  XagentAwaitInput,
  XagentCloseInput,
  XagentInspectInput,
  XagentInterruptInput,
  XagentListInput,
  XagentMessageInput,
  XagentStartInput,
} from "./tool_schemas.js";

export const XAGENT_DEFAULT_SERVICE_BASE_URL =
  `http://${XAGENT_DEFAULT_BIND_HOST}:${XAGENT_DEFAULT_BIND_PORT}`;

export type XagentServiceClientOptions = {
  readonly baseUrl?: string;
};

export type XagentServiceClient = {
  start(input: XagentStartInput): Promise<StartRunResult>;
  await(input: XagentAwaitInput, signal?: AbortSignal): Promise<AwaitRunResult>;
  inspect(input: XagentInspectInput): Promise<InspectRunResult>;
  listRuns(input: XagentListInput): Promise<ListRunsResult>;
  message(input: XagentMessageInput): Promise<MessageRunResult>;
  interrupt(input: XagentInterruptInput): Promise<InterruptRunResult>;
  closeRun(input: XagentCloseInput): Promise<CloseRunResult>;
  close(): Promise<void>;
  /**
   * Number of `xagent_await` tool calls issued by the last `await(...)` call.
   * Progress pings keep one held request alive; a healthy await must issue
   * exactly one call rather than chunk-and-reissue.
   */
  readonly awaitToolCallsIssued: number;
};

export class XagentServiceUnavailableError extends Error {
  readonly structured: StructuredToolError;

  constructor(message: string, details?: unknown) {
    super(message);
    this.name = "XagentServiceUnavailableError";
    this.structured = {
      error: "xagent_service_unavailable",
      message,
      ...(details === undefined ? {} : { details }),
    };
  }
}

export class XagentServiceToolError extends Error {
  readonly structured: StructuredToolError;

  constructor(structured: StructuredToolError) {
    super(structured.message);
    this.name = "XagentServiceToolError";
    this.structured = structured;
  }
}

export function resolveXagentServiceBaseUrl(
  explicit?: string,
  env: NodeJS.ProcessEnv = process.env,
): string {
  const configured = explicit?.trim() || env.XAGENT_SERVICE_URL?.trim();
  if (configured !== undefined && configured.length > 0) {
    return configured.replace(/\/$/, "");
  }
  return XAGENT_DEFAULT_SERVICE_BASE_URL;
}

export function createXagentServiceClient(
  options: XagentServiceClientOptions = {},
): XagentServiceClient {
  const baseUrl = resolveXagentServiceBaseUrl(options.baseUrl);
  const mcpUrl = new URL("/mcp", `${baseUrl}/`);
  let client: Client | undefined;
  let transport: StreamableHTTPClientTransport | undefined;
  let connectPromise: Promise<Client> | undefined;
  let closed = false;
  let lastAwaitToolCallsIssued = 0;

  async function ensureConnected(): Promise<Client> {
    if (closed) {
      throw new XagentServiceUnavailableError("xagent service client is closed");
    }
    if (client !== undefined) {
      return client;
    }
    if (connectPromise !== undefined) {
      return connectPromise;
    }
    connectPromise = (async () => {
      const nextClient = new Client({ name: "xagent-cli", version: "0.1.0" });
      const nextTransport = new StreamableHTTPClientTransport(mcpUrl);
      try {
        await nextClient.connect(nextTransport);
      } catch (error) {
        await nextTransport.close().catch(() => {});
        await nextClient.close().catch(() => {});
        connectPromise = undefined;
        throw toUnavailableError(error, baseUrl);
      }
      client = nextClient;
      transport = nextTransport;
      return nextClient;
    })();
    return connectPromise;
  }

  async function resetConnection(): Promise<void> {
    const activeClient = client;
    const activeTransport = transport;
    client = undefined;
    transport = undefined;
    connectPromise = undefined;
    await Promise.allSettled([
      activeClient?.close() ?? Promise.resolve(),
      activeTransport?.close() ?? Promise.resolve(),
    ]);
  }

  async function callToolOnce<T>(
    name: string,
    args: Record<string, unknown>,
    signal?: AbortSignal,
  ): Promise<T> {
    let connected: Client;
    try {
      connected = await ensureConnected();
    } catch (error) {
      throw toUnavailableError(error, baseUrl);
    }

    if (name === "xagent_await") {
      lastAwaitToolCallsIssued += 1;
    }

    let result: unknown;
    try {
      result = await connected.callTool(
        { name, arguments: args },
        undefined,
        mcpToolRequestOptions(name, signal),
      );
    } catch (error) {
      throw toUnavailableError(error, baseUrl);
    }

    return unpackToolResult<T>(result);
  }

  async function callTool<T>(
    name: string,
    args: Record<string, unknown>,
    signal?: AbortSignal,
  ): Promise<T> {
    try {
      return await callToolOnce<T>(name, args, signal);
    } catch (error) {
      if (!(error instanceof XagentServiceUnavailableError) || closed) {
        throw error;
      }
      // One reconnect covers transient stream drops mid-request.
      //
      await resetConnection();
      return callToolOnce<T>(name, args, signal);
    }
  }

  async function awaitRun(
    input: XagentAwaitInput,
    signal?: AbortSignal,
  ): Promise<AwaitRunResult> {
    lastAwaitToolCallsIssued = 0;
    return callTool<AwaitRunResult>("xagent_await", { ...input }, signal);
  }

  return {
    start(input) {
      return callTool<StartRunResult>("xagent_start_non_sdd", { ...input });
    },
    await(input, signal) {
      return awaitRun(input, signal);
    },
    inspect(input) {
      return callTool<InspectRunResult>("xagent_inspect", { ...input });
    },
    listRuns(input) {
      return callTool<ListRunsResult>("xagent_list", { ...input });
    },
    message(input) {
      return callTool<MessageRunResult>("xagent_message", { ...input });
    },
    interrupt(input) {
      return callTool<InterruptRunResult>("xagent_interrupt", { ...input });
    },
    closeRun(input) {
      return callTool<CloseRunResult>("xagent_close", { ...input });
    },
    async close(): Promise<void> {
      closed = true;
      await resetConnection();
    },
    get awaitToolCallsIssued(): number {
      return lastAwaitToolCallsIssued;
    },
  };
}

function unpackToolResult<T>(result: unknown): T {
  const toolResult = result as {
    readonly isError?: boolean;
    readonly structuredContent?: unknown;
    readonly content?: unknown;
  };
  const body = structuredBody(toolResult);
  if (toolResult.isError === true) {
    throw new XagentServiceToolError({
      error: typeof body.error === "string" ? body.error : "tool_failed",
      message: typeof body.message === "string" ? body.message : "xagent tool failed",
      ...(body.details === undefined ? {} : { details: body.details }),
    });
  }
  return body as T;
}

function structuredBody(result: {
  readonly structuredContent?: unknown;
  readonly content?: unknown;
}): Record<string, unknown> {
  if (
    result.structuredContent !== undefined
    && result.structuredContent !== null
    && typeof result.structuredContent === "object"
    && !Array.isArray(result.structuredContent)
  ) {
    return result.structuredContent as Record<string, unknown>;
  }
  if (Array.isArray(result.content)) {
    const textPart = (result.content as Array<{ type?: string; text?: string }>).find(
      (part) => part.type === "text" && typeof part.text === "string",
    );
    if (textPart?.text !== undefined) {
      return JSON.parse(textPart.text) as Record<string, unknown>;
    }
  }
  throw new XagentServiceToolError({
    error: "tool_failed",
    message: "xagent tool returned no structured content",
  });
}

function toUnavailableError(error: unknown, baseUrl: string): XagentServiceUnavailableError {
  if (error instanceof XagentServiceUnavailableError) {
    return error;
  }
  const message = error instanceof Error ? error.message : String(error);
  return new XagentServiceUnavailableError(
    `xagent service unavailable at ${baseUrl}: ${message}`,
    { cause: message },
  );
}

// Progress pings reset this idle timeout. Do not set maxTotalTimeout — it is a
// hard ceiling that progress resets cannot extend, and would reintroduce an
// arbitrary limit the ping path removes. The onprogress handler may be a
// no-op; its presence is what makes the SDK send a progressToken.
//
export const x_McpAwaitRequestTimeoutMs = 2 * x_AwaitLivenessPingIntervalMs;

export type McpToolRequestOptions = {
  readonly timeout?: number;
  readonly maxTotalTimeout?: number;
  readonly resetTimeoutOnProgress?: boolean;
  readonly onprogress?: (progress: Progress) => void;
  readonly signal?: AbortSignal;
};

export function mcpToolRequestOptions(
  toolName: string,
  signal?: AbortSignal,
): McpToolRequestOptions {
  if (toolName !== "xagent_await") {
    return signal === undefined ? {} : { signal };
  }
  return {
    timeout: x_McpAwaitRequestTimeoutMs,
    resetTimeoutOnProgress: true,
    // Presence alone requests a progressToken; the service pings only when
    // one is present.
    //
    onprogress: (_progress: Progress) => {},
    ...(signal === undefined ? {} : { signal }),
  };
}
