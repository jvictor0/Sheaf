import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import { x_AwaitLivenessPingIntervalMs, } from "./await_liveness.js";
import { XAGENT_DEFAULT_BIND_HOST, XAGENT_DEFAULT_BIND_PORT, } from "./config.js";
export const XAGENT_DEFAULT_SERVICE_BASE_URL = `http://${XAGENT_DEFAULT_BIND_HOST}:${XAGENT_DEFAULT_BIND_PORT}`;
export class XagentServiceUnavailableError extends Error {
    structured;
    constructor(message, details) {
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
    structured;
    constructor(structured) {
        super(structured.message);
        this.name = "XagentServiceToolError";
        this.structured = structured;
    }
}
export function resolveXagentServiceBaseUrl(explicit, env = process.env) {
    const configured = explicit?.trim() || env.XAGENT_SERVICE_URL?.trim();
    if (configured !== undefined && configured.length > 0) {
        return configured.replace(/\/$/, "");
    }
    return XAGENT_DEFAULT_SERVICE_BASE_URL;
}
export function createXagentServiceClient(options = {}) {
    const baseUrl = resolveXagentServiceBaseUrl(options.baseUrl);
    const mcpUrl = new URL("/mcp", `${baseUrl}/`);
    let client;
    let transport;
    let connectPromise;
    let closed = false;
    let lastAwaitToolCallsIssued = 0;
    async function ensureConnected() {
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
            }
            catch (error) {
                await nextTransport.close().catch(() => { });
                await nextClient.close().catch(() => { });
                connectPromise = undefined;
                throw toUnavailableError(error, baseUrl);
            }
            client = nextClient;
            transport = nextTransport;
            return nextClient;
        })();
        return connectPromise;
    }
    async function resetConnection() {
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
    async function callToolOnce(name, args, signal) {
        let connected;
        try {
            connected = await ensureConnected();
        }
        catch (error) {
            throw toUnavailableError(error, baseUrl);
        }
        if (name === "xagent_await") {
            lastAwaitToolCallsIssued += 1;
        }
        let result;
        try {
            result = await connected.callTool({ name, arguments: args }, undefined, mcpToolRequestOptions(name, signal));
        }
        catch (error) {
            throw toUnavailableError(error, baseUrl);
        }
        return unpackToolResult(result);
    }
    async function callTool(name, args, signal) {
        try {
            return await callToolOnce(name, args, signal);
        }
        catch (error) {
            if (!(error instanceof XagentServiceUnavailableError) || closed) {
                throw error;
            }
            // One reconnect covers transient stream drops mid-request.
            //
            await resetConnection();
            return callToolOnce(name, args, signal);
        }
    }
    async function awaitRun(input, signal) {
        lastAwaitToolCallsIssued = 0;
        return callTool("xagent_await", { ...input }, signal);
    }
    return {
        start(input) {
            return callTool("xagent_start_non_sdd", { ...input });
        },
        await(input, signal) {
            return awaitRun(input, signal);
        },
        inspect(input) {
            return callTool("xagent_inspect", { ...input });
        },
        listRuns(input) {
            return callTool("xagent_list", { ...input });
        },
        message(input) {
            return callTool("xagent_message", { ...input });
        },
        interrupt(input) {
            return callTool("xagent_interrupt", { ...input });
        },
        closeRun(input) {
            return callTool("xagent_close", { ...input });
        },
        async close() {
            closed = true;
            await resetConnection();
        },
        get awaitToolCallsIssued() {
            return lastAwaitToolCallsIssued;
        },
    };
}
function unpackToolResult(result) {
    const toolResult = result;
    const body = structuredBody(toolResult);
    if (toolResult.isError === true) {
        throw new XagentServiceToolError({
            error: typeof body.error === "string" ? body.error : "tool_failed",
            message: typeof body.message === "string" ? body.message : "xagent tool failed",
            ...(body.details === undefined ? {} : { details: body.details }),
        });
    }
    return body;
}
function structuredBody(result) {
    if (result.structuredContent !== undefined
        && result.structuredContent !== null
        && typeof result.structuredContent === "object"
        && !Array.isArray(result.structuredContent)) {
        return result.structuredContent;
    }
    if (Array.isArray(result.content)) {
        const textPart = result.content.find((part) => part.type === "text" && typeof part.text === "string");
        if (textPart?.text !== undefined) {
            return JSON.parse(textPart.text);
        }
    }
    throw new XagentServiceToolError({
        error: "tool_failed",
        message: "xagent tool returned no structured content",
    });
}
function toUnavailableError(error, baseUrl) {
    if (error instanceof XagentServiceUnavailableError) {
        return error;
    }
    const message = error instanceof Error ? error.message : String(error);
    return new XagentServiceUnavailableError(`xagent service unavailable at ${baseUrl}: ${message}`, { cause: message });
}
// Progress pings reset this idle timeout. Do not set maxTotalTimeout — it is a
// hard ceiling that progress resets cannot extend, and would reintroduce an
// arbitrary limit the ping path removes. The onprogress handler may be a
// no-op; its presence is what makes the SDK send a progressToken.
//
export const x_McpAwaitRequestTimeoutMs = 2 * x_AwaitLivenessPingIntervalMs;
export function mcpToolRequestOptions(toolName, signal) {
    if (toolName !== "xagent_await") {
        return signal === undefined ? {} : { signal };
    }
    return {
        timeout: x_McpAwaitRequestTimeoutMs,
        resetTimeoutOnProgress: true,
        // Presence alone requests a progressToken; the service pings only when
        // one is present.
        //
        onprogress: (_progress) => { },
        ...(signal === undefined ? {} : { signal }),
    };
}
//# sourceMappingURL=client.js.map