import type { Progress } from "@modelcontextprotocol/sdk/types.js";
import type { AwaitRunResult, CloseRunResult, InspectRunResult, InterruptRunResult, ListRunsResult, MessageRunResult, StartRunResult } from "./run_manager.js";
import type { StructuredToolError, XagentAwaitInput, XagentCloseInput, XagentInspectInput, XagentInterruptInput, XagentListInput, XagentMessageInput, XagentStartInput } from "./tool_schemas.js";
export declare const XAGENT_DEFAULT_SERVICE_BASE_URL = "http://127.0.0.1:9005";
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
export declare class XagentServiceUnavailableError extends Error {
    readonly structured: StructuredToolError;
    constructor(message: string, details?: unknown);
}
export declare class XagentServiceToolError extends Error {
    readonly structured: StructuredToolError;
    constructor(structured: StructuredToolError);
}
export declare function resolveXagentServiceBaseUrl(explicit?: string, env?: NodeJS.ProcessEnv): string;
export declare function createXagentServiceClient(options?: XagentServiceClientOptions): XagentServiceClient;
export declare const x_McpAwaitRequestTimeoutMs: number;
export type McpToolRequestOptions = {
    readonly timeout?: number;
    readonly maxTotalTimeout?: number;
    readonly resetTimeoutOnProgress?: boolean;
    readonly onprogress?: (progress: Progress) => void;
    readonly signal?: AbortSignal;
};
export declare function mcpToolRequestOptions(toolName: string, signal?: AbortSignal): McpToolRequestOptions;
//# sourceMappingURL=client.d.ts.map