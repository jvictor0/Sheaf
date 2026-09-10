import { Readable, type Writable } from "node:stream";
import { type HarnessName, type OutputMode, type ThinkingLevel } from "./events.js";
import type { HarnessAdapter } from "./adapters/types.js";
import { type XagentServiceClient } from "./service/client.js";
import type { SupervisionPolicy } from "./supervision/types.js";
export type CliCommand = {
    command: "run";
    harness: HarnessName;
    mode: OutputMode;
    model?: string;
    thinkingLevel?: ThinkingLevel;
    permissionMode?: string;
    initialMessage?: string;
} | {
    command: "start";
    harness: HarnessName;
    model?: string;
    thinkingLevel?: ThinkingLevel;
    permissionMode?: string;
    providerThreadId?: string;
    cwd?: string;
    policy?: SupervisionPolicy;
    prompt: string;
} | {
    command: "supervise";
    harness: HarnessName;
    model?: string;
    thinkingLevel?: ThinkingLevel;
    permissionMode?: string;
    providerThreadId?: string;
    cwd?: string;
    policy?: SupervisionPolicy;
    deadlineSeconds?: number;
    prompt: string;
} | {
    command: "await";
    runId: string;
    afterSequence: number;
    deadlineSeconds?: number;
} | {
    command: "inspect";
    runId: string;
} | {
    command: "message";
    runId: string;
    text: string;
} | {
    command: "interrupt";
    runId: string;
} | {
    command: "close";
    runId: string;
} | {
    command: "list";
    local: boolean;
} | {
    command: "logs";
    runId: string;
} | {
    command: "help";
    topic?: "run" | "supervise";
};
export type CliResult = {
    readonly exitCode: number;
};
export type CliDependencies = {
    readonly createAdapter?: (harness: HarnessName) => HarnessAdapter;
    readonly serviceBaseUrl?: string;
    readonly createServiceClient?: (options?: {
        baseUrl?: string;
    }) => XagentServiceClient;
};
export declare function parseArgs(argv: string[]): CliCommand;
export declare function main(argv: string[], stdin: Readable, stdout: Writable, stderr: Writable, cwd: string, dependencies?: CliDependencies): Promise<CliResult>;
export declare function runCli(argv: string[], stdin: Readable, stdout: Writable, stderr: Writable, cwd: string, dependencies?: CliDependencies): Promise<CliResult>;
//# sourceMappingURL=cli.d.ts.map