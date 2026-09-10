import { stat } from "node:fs/promises";
import { Readable, type Writable } from "node:stream";
import path from "node:path";

import {
  harnessNames,
  thinkingLevels,
  type HarnessName,
  type OutputMode,
  type ThinkingLevel,
} from "./events.js";
import { FakeHarnessAdapter } from "./adapters/fake.js";
import { createAdapter } from "./adapters/index.js";
import type { HarnessAdapter } from "./adapters/types.js";
import { getDefaultLogRoot, listRuns as listLocalRuns, readNormalizedLog } from "./logs.js";
import { runSession } from "./runtime.js";
import {
  createXagentServiceClient,
  resolveXagentServiceBaseUrl,
  XagentServiceToolError,
  XagentServiceUnavailableError,
  type XagentServiceClient,
} from "./service/client.js";
import type { SupervisionPolicy, SupervisionPhase } from "./supervision/types.js";
import {
  x_DefaultAwaitDeadlineSeconds,
  x_MaxAwaitDeadlineSeconds,
  type XagentStartInput,
} from "./service/tool_schemas.js";

const nonTerminalSupervisionPhases = new Set<SupervisionPhase>([
  "starting",
  "running",
  "ready",
]);

export type CliCommand =
  | {
      command: "run";
      harness: HarnessName;
      mode: OutputMode;
      model?: string;
      thinkingLevel?: ThinkingLevel;
      permissionMode?: string;
      initialMessage?: string;
    }
  | {
      command: "start";
      harness: HarnessName;
      model?: string;
      thinkingLevel?: ThinkingLevel;
      permissionMode?: string;
      providerThreadId?: string;
      cwd?: string;
      policy?: SupervisionPolicy;
      prompt: string;
    }
  | {
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
    }
  | {
      command: "await";
      runId: string;
      afterSequence: number;
      deadlineSeconds?: number;
    }
  | { command: "inspect"; runId: string }
  | { command: "message"; runId: string; text: string }
  | { command: "interrupt"; runId: string }
  | { command: "close"; runId: string }
  | { command: "list"; local: boolean }
  | { command: "logs"; runId: string }
  | { command: "help"; topic?: "run" | "supervise" };

export type CliResult = {
  readonly exitCode: number;
};

export type CliDependencies = {
  readonly createAdapter?: (harness: HarnessName) => HarnessAdapter;
  readonly serviceBaseUrl?: string;
  readonly createServiceClient?: (options?: { baseUrl?: string }) => XagentServiceClient;
};

export function parseArgs(argv: string[]): CliCommand {
  const [command, ...rest] = argv;

  if (command === undefined || command === "--help" || command === "-h") {
    return { command: "help" };
  }

  if (command === "run") {
    if (rest.length === 1 && (rest[0] === "--help" || rest[0] === "-h")) {
      return { command: "help", topic: "run" };
    }
    return parseRunArgs(rest);
  }

  if (command === "supervise") {
    if (rest.length === 1 && (rest[0] === "--help" || rest[0] === "-h")) {
      return { command: "help", topic: "supervise" };
    }
    return parseSuperviseArgs(rest);
  }

  if (command === "start") {
    if (rest.length === 1 && (rest[0] === "--help" || rest[0] === "-h")) {
      return { command: "help", topic: "supervise" };
    }
    const parsed = parseSuperviseArgs(rest, "start");
    if (parsed.command !== "supervise") {
      throw new Error("xagent start parser returned an invalid command.");
    }
    const { deadlineSeconds: _deadlineSeconds, ...start } = parsed;
    return { ...start, command: "start" };
  }

  if (command === "await") {
    return parseAwaitArgs(rest);
  }

  if (command === "inspect") {
    return parseRunIdOnlyArgs("inspect", rest);
  }

  if (command === "message") {
    return parseMessageArgs(rest);
  }

  if (command === "interrupt") {
    return parseRunIdOnlyArgs("interrupt", rest);
  }

  if (command === "close") {
    return parseRunIdOnlyArgs("close", rest);
  }

  if (command === "list") {
    if (rest.length > 1 || (rest.length === 1 && rest[0] !== "--local")) {
      throw new Error("Usage: xagent list [--local]");
    }
    return { command: "list", local: rest[0] === "--local" };
  }

  if (command === "logs") {
    if (rest.length !== 1 || rest[0] === undefined || rest[0].startsWith("--")) {
      throw new Error("Usage: xagent logs <run_id>");
    }
    return { command: "logs", runId: rest[0] };
  }

  throw new Error(usage());
}

export async function main(
  argv: string[],
  stdin: Readable,
  stdout: Writable,
  stderr: Writable,
  cwd: string,
  dependencies: CliDependencies = {},
): Promise<CliResult> {
  const command = parseArgs(argv);
  const adapterFactory = dependencies.createAdapter ?? createCliAdapter;

  if (command.command === "run") {
    const logRoot = await resolveLogRoot(cwd);
    return runSession({
      harness: command.harness,
      mode: command.mode,
      model: command.model,
      thinkingLevel: command.thinkingLevel,
      permissionMode: command.permissionMode,
      repoRoot: cwd,
      logRoot,
      cwd,
      stdin: withInitialMessage(command.initialMessage, stdin),
      stdout,
      adapter: adapterFactory(command.harness),
    });
  }

  if (command.command === "help") {
    stdout.write(`${usage(command.topic)}\n`);
    return { exitCode: 0 };
  }

  if (command.command === "logs") {
    const logRoot = await resolveLogRoot(cwd);
    stdout.write(await readNormalizedLog(logRoot, command.runId));
    return { exitCode: 0 };
  }

  if (command.command === "list" && command.local) {
    const logRoot = await resolveLogRoot(cwd);
    stdout.write(`${JSON.stringify(await listLocalRuns(logRoot), null, 2)}\n`);
    return { exitCode: 0 };
  }

  return runQuietServiceCommand(command, stdout, stderr, cwd, dependencies);
}

async function runQuietServiceCommand(
  command: Exclude<CliCommand, { command: "run" | "help" | "logs" }>,
  stdout: Writable,
  stderr: Writable,
  cwd: string,
  dependencies: CliDependencies,
): Promise<CliResult> {
  const baseUrl = resolveXagentServiceBaseUrl(dependencies.serviceBaseUrl);
  const createClient = dependencies.createServiceClient ?? createXagentServiceClient;
  const client = createClient({ baseUrl });
  let startedRunId: string | undefined;
  try {
    if (command.command === "supervise" || command.command === "start") {
      const workingDirectory = path.resolve(command.cwd ?? cwd);
      const startInput: XagentStartInput = {
        cwd: workingDirectory,
        prompt: command.prompt,
        harness: command.harness,
        mode: "subagent",
        ...(command.model === undefined ? {} : { model: command.model }),
        ...(command.thinkingLevel === undefined ? {} : { thinking_level: command.thinkingLevel }),
        ...(command.permissionMode === undefined ? {} : { permission_mode: command.permissionMode }),
        ...(command.providerThreadId === undefined
          ? {}
          : { provider_thread_id: command.providerThreadId }),
        ...(command.policy === undefined
          ? {}
          : { policy: command.policy as XagentStartInput["policy"] }),
      };
      const started = await client.start(startInput);
      startedRunId = started.run_id;
      if (command.command === "start") {
        writeCompactJson(stdout, started);
        return { exitCode: 0 };
      }
      const deadlineSeconds = command.deadlineSeconds ?? x_DefaultAwaitDeadlineSeconds;
      const awaited = await awaitControllerEvent(
        client,
        started.run_id,
        0,
        deadlineSeconds,
        stderr,
      );
      writeCompactJson(stdout, awaited);
      return { exitCode: exitCodeForAwait(awaited) };
    }

    if (command.command === "await") {
      const awaited = await awaitControllerEvent(
        client,
        command.runId,
        command.afterSequence,
        command.deadlineSeconds ?? x_DefaultAwaitDeadlineSeconds,
        stderr,
      );
      writeCompactJson(stdout, awaited);
      return { exitCode: exitCodeForAwait(awaited) };
    }

    if (command.command === "inspect") {
      writeCompactJson(stdout, await client.inspect({ run_id: command.runId }));
      return { exitCode: 0 };
    }

    if (command.command === "list") {
      const listed = await client.listRuns({ live_only: false, limit: 50 });
      writeCompactJson(stdout, listed.runs);
      return { exitCode: 0 };
    }

    if (command.command === "message") {
      writeCompactJson(
        stdout,
        await client.message({ run_id: command.runId, text: command.text }),
      );
      return { exitCode: 0 };
    }

    if (command.command === "interrupt") {
      writeCompactJson(stdout, await client.interrupt({ run_id: command.runId }));
      return { exitCode: 0 };
    }

    writeCompactJson(stdout, await client.closeRun({ run_id: command.runId }));
    return { exitCode: 0 };
  } catch (error) {
    if (
      error instanceof XagentServiceUnavailableError
      || error instanceof XagentServiceToolError
    ) {
      writeCompactJson(stdout, withOptionalRunId(error.structured, startedRunId));
      return { exitCode: 1 };
    }
    throw error;
  } finally {
    await client.close().catch(() => {});
  }
}

function withOptionalRunId(
  structured: { readonly error: string; readonly message: string; readonly details?: unknown },
  runId: string | undefined,
): { readonly error: string; readonly message: string; readonly details?: unknown } {
  if (runId === undefined) {
    return structured;
  }
  const details =
    structured.details !== undefined
    && structured.details !== null
    && typeof structured.details === "object"
    && !Array.isArray(structured.details)
      ? { ...(structured.details as Record<string, unknown>), run_id: runId }
      : { run_id: runId };
  return {
    error: structured.error,
    message: structured.message,
    details,
  };
}

function writeCompactJson(stdout: Writable, body: unknown): void {
  stdout.write(`${JSON.stringify(body)}\n`);
}

async function awaitControllerEvent(
  client: XagentServiceClient,
  runId: string,
  afterSequence: number,
  deadlineSeconds: number,
  advisoryOutput: Writable,
): Promise<{ readonly event: string; readonly sequence: number } & Record<string, unknown>> {
  let cursor = afterSequence;
  const deadlineMs = Date.now() + deadlineSeconds * 1000;
  for (;;) {
    const remainingSeconds = Math.max(1, Math.ceil((deadlineMs - Date.now()) / 1000));
    const awaited = await client.await({
      run_id: runId,
      after_sequence: cursor,
      deadline_seconds: Math.min(remainingSeconds, deadlineSeconds),
    });
    if (
      awaited.event === "supervision.state"
      && nonTerminalSupervisionPhases.has(awaited.phase as SupervisionPhase)
    ) {
      cursor = awaited.sequence;
      if (Date.now() >= deadlineMs) {
        return {
          schema_version: 1,
          event: "supervision.deadline",
          run_id: runId,
          sequence: cursor,
          phase: awaited.phase,
          elapsed_ms: deadlineSeconds * 1000,
          reason: "await_deadline",
        };
      }
      continue;
    }
    if (
      awaited.event === "supervision.attention"
      && typeof awaited.reason === "string"
      && awaited.reason.startsWith("watchdog_")
    ) {
      writeCompactJson(advisoryOutput, awaited);
      cursor = awaited.sequence;
      if (Date.now() >= deadlineMs) {
        return {
          schema_version: 1,
          event: "supervision.deadline",
          run_id: runId,
          sequence: cursor,
          phase: awaited.phase,
          elapsed_ms: deadlineSeconds * 1000,
          reason: "await_deadline",
        };
      }
      continue;
    }
    return awaited;
  }
}

function exitCodeForAwait(awaited: {
  readonly event: string;
  readonly reason?: string;
  readonly phase?: string;
}): number {
  if (awaited.event === "turn.completed" || awaited.event === "supervision.attention") {
    return 0;
  }
  if (awaited.event === "supervision.deadline") {
    return 0;
  }
  if (awaited.reason === "missing_final_report") {
    return 1;
  }
  if (awaited.event === "supervision.state") {
    return awaited.phase === "failed"
      || awaited.phase === "abandoned"
      || awaited.phase === "cancelled"
      ? 1
      : 0;
  }
  return 1;
}

async function resolveLogRoot(cwd: string): Promise<string> {
  const configured = process.env.XAGENT_LOG_ROOT?.trim();
  if (configured !== undefined && configured.length > 0) {
    return path.resolve(configured);
  }
  return getDefaultLogRoot(await findRepoRoot(cwd));
}

async function findRepoRoot(cwd: string): Promise<string> {
  let current = path.resolve(cwd);

  while (true) {
    if (await pathExists(path.join(current, ".git"))) {
      return current;
    }

    const parent = path.dirname(current);
    if (parent === current) {
      return path.resolve(cwd);
    }
    current = parent;
  }
}

async function pathExists(candidate: string): Promise<boolean> {
  try {
    await stat(candidate);
    return true;
  } catch (error) {
    if (error instanceof Error && "code" in error && error.code === "ENOENT") {
      return false;
    }
    throw error;
  }
}

function createCliAdapter(harness: HarnessName): HarnessAdapter {
  if (process.env.XAGENT_TEST_ADAPTER === "fake") {
    return new FakeHarnessAdapter({
      includeToolEvents: true,
      includeRawProvider: true,
      includeDeltas: true,
    });
  }
  return createAdapter(harness);
}

export async function runCli(
  argv: string[],
  stdin: Readable,
  stdout: Writable,
  stderr: Writable,
  cwd: string,
  dependencies: CliDependencies = {},
): Promise<CliResult> {
  try {
    return await main(argv, stdin, stdout, stderr, cwd, dependencies);
  } catch (error) {
    stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    return { exitCode: 1 };
  }
}

function parseRunArgs(argv: string[]): CliCommand {
  let harness: HarnessName | undefined;
  let model: string | undefined;
  let thinkingLevel: ThinkingLevel | undefined;
  let permissionMode: string | undefined;
  let mode: OutputMode | undefined;
  const initialMessageParts: string[] = [];
  let positionalOnly = false;

  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    if (flag === undefined) {
      continue;
    }

    if (positionalOnly) {
      initialMessageParts.push(flag);
      continue;
    }

    if (flag === "--") {
      positionalOnly = true;
      continue;
    }

    if (!flag.startsWith("--")) {
      initialMessageParts.push(flag);
      continue;
    }

    if (flag === "--harness") {
      if (harness !== undefined) {
        throw new Error("xagent run requires exactly one --harness value.");
      }
      const value = readFlagValue(argv, index, flag);
      assertHarness(value);
      harness = value;
      index += 1;
      continue;
    }

    if (flag === "--model") {
      if (model !== undefined) {
        throw new Error("xagent run accepts --model at most once.");
      }
      model = readFlagValue(argv, index, flag);
      index += 1;
      continue;
    }

    if (flag === "--permission-mode") {
      if (permissionMode !== undefined) {
        throw new Error("xagent run accepts --permission-mode at most once.");
      }
      permissionMode = readFlagValue(argv, index, flag);
      index += 1;
      continue;
    }

    if (flag === "--thinking-level") {
      if (thinkingLevel !== undefined) {
        throw new Error("xagent run accepts --thinking-level at most once.");
      }
      const value = readFlagValue(argv, index, flag);
      assertThinkingLevel(value);
      thinkingLevel = value;
      index += 1;
      continue;
    }

    if (flag === "--subagent" || flag === "--full") {
      const nextMode: OutputMode = flag === "--subagent" ? "subagent" : "full";
      if (mode !== undefined) {
        throw new Error("xagent run requires exactly one verbosity mode: --subagent or --full.");
      }
      mode = nextMode;
      continue;
    }

    throw new Error(`Unsupported flag for xagent run: ${flag}.`);
  }

  if (harness === undefined) {
    throw new Error("xagent run requires --harness <codex|pi|cursor|claude_code>.");
  }

  if (mode === undefined) {
    throw new Error("xagent run requires exactly one verbosity mode: --subagent or --full.");
  }

  return {
    command: "run",
    harness,
    mode,
    model,
    thinkingLevel,
    ...(permissionMode === undefined ? {} : { permissionMode }),
    initialMessage: initialMessageParts.length > 0 ? initialMessageParts.join(" ") : undefined,
  };
}

function parseSuperviseArgs(
  argv: string[],
  commandName: "supervise" | "start" = "supervise",
): CliCommand {
  let harness: HarnessName | undefined;
  let model: string | undefined;
  let thinkingLevel: ThinkingLevel | undefined;
  let permissionMode: string | undefined;
  let providerThreadId: string | undefined;
  let cwd: string | undefined;
  let policy: SupervisionPolicy | undefined;
  let deadlineSeconds: number | undefined;
  const promptParts: string[] = [];
  let positionalOnly = false;

  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    if (flag === undefined) {
      continue;
    }

    if (positionalOnly) {
      promptParts.push(flag);
      continue;
    }

    if (flag === "--") {
      positionalOnly = true;
      continue;
    }

    if (!flag.startsWith("--")) {
      promptParts.push(flag);
      continue;
    }

    if (flag === "--harness") {
      if (harness !== undefined) {
        throw new Error(`xagent ${commandName} requires exactly one --harness value.`);
      }
      const value = readFlagValue(argv, index, flag);
      assertHarness(value);
      harness = value;
      index += 1;
      continue;
    }

    if (flag === "--model") {
      if (model !== undefined) {
        throw new Error(`xagent ${commandName} accepts --model at most once.`);
      }
      model = readFlagValue(argv, index, flag);
      index += 1;
      continue;
    }

    if (flag === "--permission-mode") {
      if (permissionMode !== undefined) {
        throw new Error(`xagent ${commandName} accepts --permission-mode at most once.`);
      }
      permissionMode = readFlagValue(argv, index, flag);
      index += 1;
      continue;
    }

    if (flag === "--resume") {
      if (providerThreadId !== undefined) {
        throw new Error(`xagent ${commandName} accepts --resume at most once.`);
      }
      providerThreadId = readFlagValue(argv, index, flag);
      index += 1;
      continue;
    }

    if (flag === "--thinking-level") {
      if (thinkingLevel !== undefined) {
        throw new Error(`xagent ${commandName} accepts --thinking-level at most once.`);
      }
      const value = readFlagValue(argv, index, flag);
      assertThinkingLevel(value);
      thinkingLevel = value;
      index += 1;
      continue;
    }

    if (flag === "--cwd") {
      if (cwd !== undefined) {
        throw new Error(`xagent ${commandName} accepts --cwd at most once.`);
      }
      cwd = readFlagValue(argv, index, flag);
      index += 1;
      continue;
    }

    if (flag === "--policy") {
      if (policy !== undefined) {
        throw new Error(`xagent ${commandName} accepts --policy at most once.`);
      }
      policy = parsePolicyJson(readFlagValue(argv, index, flag));
      index += 1;
      continue;
    }

    if (flag === "--deadline-seconds") {
      if (commandName === "start") {
        throw new Error("xagent start does not accept --deadline-seconds; use xagent await to wait.");
      }
      if (deadlineSeconds !== undefined) {
        throw new Error("xagent supervise accepts --deadline-seconds at most once.");
      }
      deadlineSeconds = parseDeadlineSecondsFlag(readFlagValue(argv, index, flag), flag);
      index += 1;
      continue;
    }

    throw new Error(`Unsupported flag for xagent ${commandName}: ${flag}.`);
  }

  if (harness === undefined) {
    throw new Error(`xagent ${commandName} requires --harness <codex|pi|cursor|claude_code>.`);
  }

  if (promptParts.length === 0) {
    throw new Error(`xagent ${commandName} requires an initial prompt.`);
  }

  return {
    command: "supervise",
    harness,
    ...(model === undefined ? {} : { model }),
    ...(thinkingLevel === undefined ? {} : { thinkingLevel }),
    ...(permissionMode === undefined ? {} : { permissionMode }),
    ...(providerThreadId === undefined ? {} : { providerThreadId }),
    ...(cwd === undefined ? {} : { cwd }),
    ...(policy === undefined ? {} : { policy }),
    ...(deadlineSeconds === undefined ? {} : { deadlineSeconds }),
    prompt: promptParts.join(" "),
  };
}

function parseAwaitArgs(argv: string[]): CliCommand {
  if (argv.length === 0 || argv[0] === undefined || argv[0].startsWith("--")) {
    throw new Error("Usage: xagent await <run_id> --after-sequence <n> [--deadline-seconds <n>]");
  }
  const runId = argv[0];
  let afterSequence: number | undefined;
  let deadlineSeconds: number | undefined;

  for (let index = 1; index < argv.length; index += 1) {
    const flag = argv[index];
    if (flag === undefined) {
      continue;
    }
    if (flag === "--after-sequence") {
      if (afterSequence !== undefined) {
        throw new Error("xagent await accepts --after-sequence at most once.");
      }
      afterSequence = parseNonNegativeIntFlag(readFlagValue(argv, index, flag), flag);
      index += 1;
      continue;
    }
    if (flag === "--deadline-seconds") {
      if (deadlineSeconds !== undefined) {
        throw new Error("xagent await accepts --deadline-seconds at most once.");
      }
      deadlineSeconds = parseDeadlineSecondsFlag(readFlagValue(argv, index, flag), flag);
      index += 1;
      continue;
    }
    throw new Error(`Unsupported flag for xagent await: ${flag}.`);
  }

  if (afterSequence === undefined) {
    throw new Error("xagent await requires --after-sequence <n>.");
  }

  return {
    command: "await",
    runId,
    afterSequence,
    ...(deadlineSeconds === undefined ? {} : { deadlineSeconds }),
  };
}

function parseMessageArgs(argv: string[]): CliCommand {
  if (argv.length < 2 || argv[0] === undefined || argv[0].startsWith("--")) {
    throw new Error("Usage: xagent message <run_id> <text>");
  }
  const runId = argv[0];
  const text = argv.slice(1).join(" ").trim();
  if (text.length === 0) {
    throw new Error("Usage: xagent message <run_id> <text>");
  }
  return { command: "message", runId, text };
}

function parseRunIdOnlyArgs(
  command: "inspect" | "interrupt" | "close",
  argv: string[],
): CliCommand {
  if (argv.length !== 1 || argv[0] === undefined || argv[0].startsWith("--")) {
    throw new Error(`Usage: xagent ${command} <run_id>`);
  }
  return { command, runId: argv[0] };
}

function parsePolicyJson(raw: string): SupervisionPolicy {
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    throw new Error("xagent supervise --policy must be valid JSON.");
  }
  if (
    parsed === null
    || typeof parsed !== "object"
    || Array.isArray(parsed)
    || typeof (parsed as { silenceTimeoutMs?: unknown }).silenceTimeoutMs !== "number"
    || !Number.isInteger((parsed as { silenceTimeoutMs: number }).silenceTimeoutMs)
    || (parsed as { silenceTimeoutMs: number }).silenceTimeoutMs <= 0
  ) {
    throw new Error("xagent supervise --policy requires a positive integer silenceTimeoutMs.");
  }
  const record = parsed as {
    silenceTimeoutMs: number;
    hardDeadlineMs?: unknown;
    watchdog?: unknown;
  };
  const policy: SupervisionPolicy = {
    silenceTimeoutMs: record.silenceTimeoutMs,
    watchdog: (record.watchdog !== undefined
      && record.watchdog !== null
      && typeof record.watchdog === "object"
      && !Array.isArray(record.watchdog))
      ? (record.watchdog as SupervisionPolicy["watchdog"])
      : {},
  };
  if (
    typeof record.hardDeadlineMs === "number"
    && Number.isInteger(record.hardDeadlineMs)
    && record.hardDeadlineMs > 0
  ) {
    return { ...policy, hardDeadlineMs: record.hardDeadlineMs };
  }
  return policy;
}

function parsePositiveIntFlag(value: string, flag: string): number {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed <= 0) {
    throw new Error(`Expected a positive integer after ${flag}.`);
  }
  return parsed;
}

function parseDeadlineSecondsFlag(value: string, flag: string): number {
  const parsed = parsePositiveIntFlag(value, flag);
  if (parsed > x_MaxAwaitDeadlineSeconds) {
    throw new Error(`${flag} cannot exceed ${x_MaxAwaitDeadlineSeconds}.`);
  }
  return parsed;
}

function parseNonNegativeIntFlag(value: string, flag: string): number {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < 0) {
    throw new Error(`Expected a non-negative integer after ${flag}.`);
  }
  return parsed;
}

function readFlagValue(argv: string[], index: number, flag: string): string {
  const value = argv[index + 1];
  if (value === undefined || value.startsWith("--")) {
    throw new Error(`Expected a value after ${flag}.`);
  }
  return value;
}

function assertHarness(value: string): asserts value is HarnessName {
  if (!harnessNames.includes(value as HarnessName)) {
    throw new Error(`Unsupported harness: ${value}.`);
  }
}

function assertThinkingLevel(value: string): asserts value is ThinkingLevel {
  if (!thinkingLevels.includes(value as ThinkingLevel)) {
    throw new Error(`Unsupported thinking level: ${value}.`);
  }
}

function withInitialMessage(initialMessage: string | undefined, stdin: Readable): Readable {
  if (initialMessage === undefined) {
    return stdin;
  }

  return Readable.from((async function* () {
    yield `${JSON.stringify({ type: "user.message", text: initialMessage })}\n`;
    for await (const chunk of stdin) {
      yield chunk;
    }
  })());
}

function usage(topic?: "run" | "supervise"): string {
  if (topic === "run") {
    return [
      "Usage:",
      "  xagent run --harness <codex|pi|cursor|claude_code> [--model <model>] [--thinking-level <low|medium|high|xhigh>] (--subagent|--full) [initial message]",
      "",
      "Reads follow-up commands from stdin as JSON Lines:",
      '  {"type":"user.message","text":"follow up"}',
      '  {"type":"control.exit"}',
      "",
      "Examples:",
      "  xagent run --harness codex --subagent \"hello\"",
      "  printf '%s\\n' '{\"type\":\"user.message\",\"text\":\"hello\"}' '{\"type\":\"control.exit\"}' | xagent run --harness codex --subagent",
      "",
      "Modes:",
      "  --subagent  Parent-agent friendly output: final text, turn lifecycle, errors, and debounced deltas.",
      "  --full      Normalized events plus sanitized raw provider events and tool details.",
    ].join("\n");
  }

  if (topic === "supervise") {
    return [
      "Usage:",
      "  xagent start --harness <codex|pi|cursor|claude_code> [--model <model>] [--thinking-level <low|medium|high|xhigh>] [--permission-mode <mode>] [--resume <provider-thread-id>] [--cwd <abs-path>] [--policy <json>] <prompt>",
      "  xagent supervise --harness <codex|pi|cursor|claude_code> [--model <model>] [--thinking-level <low|medium|high|xhigh>] [--permission-mode <mode>] [--resume <provider-thread-id>] [--cwd <abs-path>] [--policy <json>] [--deadline-seconds <n>] <prompt>",
      "  xagent await <run_id> --after-sequence <n> [--deadline-seconds <n>]",
      "  xagent inspect <run_id>",
      "  xagent message <run_id> <text>",
      "  xagent interrupt <run_id>",
      "  xagent close <run_id>",
      "",
      "Quiet service-client fallback for the Conductor-managed xagent service at 127.0.0.1:9005.",
      "Requires a healthy service; never starts an embedded supervisor.",
      "Use start to get the run_id immediately, then await to wait for the result.",
      "Supervise and await keep stdout quiet through healthy progress and advisory watchdog notices.",
      "Advisory watchdog notices go to stderr; stdout emits one compact terminal or actionable result.",
      "Successful completion includes the sanitized final assistant report inline.",
      "Use the returned run_id to reattach with await/inspect/message/interrupt/close.",
    ].join("\n");
  }

  return [
    "Usage:",
    "  xagent run --harness <codex|pi|cursor|claude_code> [--model <model>] [--thinking-level <low|medium|high|xhigh>] (--subagent|--full) [initial message]",
    "  xagent start --harness <codex|pi|cursor|claude_code> [options] <prompt>",
    "  xagent supervise --harness <codex|pi|cursor|claude_code> [options] <prompt>",
    "  xagent await <run_id> --after-sequence <n> [--deadline-seconds <n>]",
    "  xagent inspect <run_id>",
    "  xagent message <run_id> <text>",
    "  xagent interrupt <run_id>",
    "  xagent close <run_id>",
    "  xagent list [--local]",
    "  xagent logs <run_id>",
    "",
    "Run protocol:",
    "  xagent run stays alive. Pass an optional initial message as CLI text, then send follow-ups on stdin as JSON Lines.",
    '  Supported stdin commands: {"type":"user.message","text":"..."} and {"type":"control.exit"}.',
    "",
    "Quiet supervision:",
    "  xagent supervise talks to the Conductor-managed xagent service (127.0.0.1:9005).",
    "  Use start when the controller needs the run_id before waiting.",
    "  Healthy deltas/tools stay off stdout; advisory watchdog notices go to stderr and do not detach.",
    "  One compact stdout JSON carries actionable attention, terminal events, deadlines, or the final report.",
    "",
    "Examples:",
    "  xagent run --harness codex --subagent \"hello\"",
    "  xagent run --harness claude_code --model haiku --subagent \"hello\"",
    "  xagent supervise --harness claude_code --model sonnet \"implement the task\"",
    "  xagent list",
    "  xagent list --local  # legacy xagent run records",
    "  xagent logs <run_id>",
    "",
    "Use `xagent run --help` for run protocol details.",
    "Use `xagent supervise --help` for quiet service-client details.",
  ].join("\n");
}
