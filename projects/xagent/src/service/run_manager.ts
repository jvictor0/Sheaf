import { readFile, realpath, stat } from "node:fs/promises";
import path from "node:path";

import { generateRunId, createRunRecord, appendNormalizedEvent, appendRawProviderEvent, appendWatchdogTelemetry, listRuns, updateRunSupervision, openRunRecord, type RunRecord } from "../logs.js";
import { Supervisor } from "../supervision/supervisor.js";
import type {
  AwaitResult,
  SupervisionEvent,
  SupervisionPersistenceState,
  SupervisionPhase,
  SupervisionPolicy,
  SupervisionScheduler,
  SupervisorInspection,
  WatchdogClassifier,
} from "../supervision/types.js";
import { createAdapter } from "../adapters/index.js";
import type { HarnessAdapter } from "../adapters/types.js";
import type { HarnessName, OutputMode, ThinkingLevel } from "../events.js";
import {
  x_DefaultAwaitDeadlineSeconds,
  type StructuredToolError,
  type XagentAwaitInput,
  type XagentCloseInput,
  type XagentInspectInput,
  type XagentListInput,
  type XagentInterruptInput,
  type XagentMessageInput,
  type XagentStartInput,
} from "./tool_schemas.js";
import { ToolValidationError } from "./tool_schemas.js";

const terminalPersistedPhases = new Set<SupervisionPhase>([
  "completed",
  "failed",
  "cancelled",
  "abandoned",
]);

export type XagentRunManagerOptions = {
  readonly repoRoot: string;
  readonly logRoot: string;
  readonly adapterFactory?: (harness: HarnessName) => HarnessAdapter;
  readonly clock?: () => Date;
  readonly scheduler?: SupervisionScheduler;
  readonly policy?: SupervisionPolicy;
  readonly watchdogClassifier?: WatchdogClassifier;
};

export type CreateRunOptions = {
  readonly runId?: string;
  readonly harness: HarnessName;
  readonly mode: OutputMode;
  readonly cwd: string;
  readonly model?: string;
  readonly thinkingLevel?: ThinkingLevel;
  readonly permissionMode?: string;
  readonly providerThreadId?: string;
  readonly policy?: SupervisionPolicy;
};

export type StartRunResult = {
  readonly run_id: string;
  readonly sequence: number;
  readonly phase: string;
};

export type InspectRunResult = {
  readonly run_id: string;
  readonly phase: string;
  readonly sequence: number;
  readonly provider_thread_id?: string;
  readonly callback_failure?: {
    source: "health_callback" | "watchdog_callback";
    message: string;
  };
};

export type XagentListRow = {
  readonly run_id: string;
  readonly harness: string;
  readonly model?: string;
  readonly phase: string;
  readonly sequence: number;
  readonly exit_status: string;
  readonly live: boolean;
  readonly supervised: boolean;
  readonly created_at: string;
  readonly updated_at: string;
};

export type ListRunsResult = {
  readonly runs: readonly XagentListRow[];
};

export type AwaitRunResult = {
  readonly schema_version: 1;
  readonly event: string;
  readonly run_id: string;
  readonly sequence: number;
  readonly phase: string;
  readonly elapsed_ms: number;
  readonly reason?: string;
  readonly report?: { readonly text: string };
  readonly usage?: { readonly input_tokens?: number; readonly output_tokens?: number };
  readonly payload?: unknown;
};

export type MessageRunResult = {
  readonly run_id: string;
  readonly phase: string;
  readonly sequence: number;
};

export type InterruptRunResult = {
  readonly run_id: string;
  readonly phase: string;
  readonly sequence: number;
};

export type CloseRunResult = {
  readonly run_id: string;
  readonly closed: true;
};

type OwnedRun = {
  readonly supervisor: Supervisor;
  readonly record: RunRecord;
};

export class XagentRunManager {
  readonly #repoRoot: string;
  readonly #logRoot: string;
  readonly #adapterFactory: (harness: HarnessName) => HarnessAdapter;
  readonly #clock: () => Date;
  readonly #scheduler: SupervisionScheduler | undefined;
  readonly #defaultPolicy: SupervisionPolicy;
  readonly #watchdogClassifier: WatchdogClassifier | undefined;
  readonly #runs = new Map<string, OwnedRun>();
  #closed = false;

  constructor(options: XagentRunManagerOptions) {
    this.#repoRoot = options.repoRoot;
    this.#logRoot = options.logRoot;
    this.#adapterFactory = options.adapterFactory ?? ((harness) => createAdapter(harness));
    this.#clock = options.clock ?? (() => new Date());
    this.#scheduler = options.scheduler;
    this.#defaultPolicy = options.policy ?? {
      silenceTimeoutMs: 300_000,
      watchdog: {},
    };
    this.#watchdogClassifier = options.watchdogClassifier;
  }

  allocateRunId(): string {
    return generateRunId(this.#clock());
  }

  async create(options: CreateRunOptions): Promise<{ readonly runId: string }> {
    if (this.#closed) {
      throw new Error("XagentRunManager is closed.");
    }
    const runId = options.runId ?? this.allocateRunId();
    if (this.#runs.has(runId)) {
      throw new Error(`xagent run id already in use: ${runId}`);
    }
    const record = await createRunRecord({
      repoRoot: this.#repoRoot,
      logRoot: this.#logRoot,
      runId,
      harness: options.harness,
      mode: options.mode,
      model: options.model,
      thinkingLevel: options.thinkingLevel,
      clock: this.#clock,
      // Stamp service-owned runs so startup reconciliation can distinguish
      // them from in-flight legacy `xagent run` records that share the log
      // root. Reconciliation only enumerates supervised records; a legacy
      // interactive run stays untouched across service restarts (review I2).
      //
      supervised: true,
    });
    const adapter = this.#adapterFactory(options.harness);
    const policy = options.policy ?? this.#defaultPolicy;
    const supervisor = new Supervisor({
      runId,
      adapter,
      startOptions: {
        cwd: options.cwd,
        ...(options.model === undefined ? {} : { model: options.model }),
        ...(options.thinkingLevel === undefined ? {} : { thinkingLevel: options.thinkingLevel }),
        ...(options.permissionMode === undefined ? {} : { permissionMode: options.permissionMode }),
        ...(options.providerThreadId === undefined
          ? {}
          : { providerThreadId: options.providerThreadId }),
      },
      policy,
      clock: this.#clock,
      ...(this.#scheduler === undefined ? {} : { scheduler: this.#scheduler }),
      ...(this.#watchdogClassifier === undefined ? {} : { watchdogClassifier: this.#watchdogClassifier }),
      eventSink: async (event) => {
        await appendNormalizedEvent(record, event);
      },
      metadataSink: async (state) => {
        await updateRunSupervision(record, toRunSupervisionUpdate(state));
      },
      // Wire the sanitized watchdog telemetry sink so per-verdict records
      // (request hash, input/output bytes, verdict, confidence, reason
      // code, call count, truncation, attention sequence, usage, estimated
      // cost) are persisted to watchdog.jsonl for every supervised run —
      // service, MCP, and quiet CLI alike. Without this wire the file stays
      // empty in production because the supervisor falls back to a no-op
      // sink.
      //
      watchdogTelemetrySink: async (telemetry) => {
        await appendWatchdogTelemetry(record, telemetry);
      },
      // Without this wire, raw-provider.jsonl stayed empty for every supervised
      // run even though the run record advertises it, so a failed run left
      // nothing to post-mortem. The supervisor already sanitized the payload.
      //
      providerTranscriptSink: async (raw) => {
        await appendRawProviderEvent(record, raw);
      },
    });
    this.#runs.set(runId, { supervisor, record });
    return { runId };
  }

  start(runId: string): Promise<void> {
    return this.#require(runId).supervisor.start();
  }

  submit(runId: string, text: string): Promise<void> {
    return this.#require(runId).supervisor.submit(text);
  }

  awaitEvent(
    runId: string,
    afterSequence: number,
    deadlineMs: number,
    signal?: AbortSignal,
  ): Promise<AwaitResult> {
    return this.#require(runId).supervisor.awaitEvent(afterSequence, deadlineMs, signal);
  }

  inspect(runId: string): SupervisorInspection | undefined {
    return this.#runs.get(runId)?.supervisor.inspect();
  }

  // mcp.ts asks this instead of reaching into supervisor fields. Unknown or
  // already-evicted runs are not vouched for.
  //
  isRunVouching(runId: string): boolean {
    const run = this.#runs.get(runId);
    if (run === undefined) {
      return false;
    }
    return run.supervisor.isVouching();
  }

  interrupt(runId: string): Promise<void> {
    return this.#require(runId).supervisor.interrupt();
  }

  close(runId: string): Promise<void> {
    const run = this.#runs.get(runId);
    if (run === undefined) {
      return Promise.resolve();
    }
    return run.supervisor.close().finally(() => {
      this.#runs.delete(runId);
    });
  }

  has(runId: string): boolean {
    return this.#runs.has(runId);
  }

  listRunIds(): string[] {
    return [...this.#runs.keys()];
  }

  get size(): number {
    return this.#runs.size;
  }

  async closeAll(): Promise<void> {
    if (this.#closed) {
      return;
    }
    this.#closed = true;
    const runs = [...this.#runs.values()];
    this.#runs.clear();
    await Promise.allSettled(runs.map((run) => run.supervisor.close()));
  }

  async startRun(input: XagentStartInput): Promise<StartRunResult> {
    const cwd = await canonicalizeWorkingDirectory(input.cwd);
    const { runId } = await this.create({
      harness: input.harness,
      mode: input.mode,
      cwd,
      ...(input.model === undefined ? {} : { model: input.model }),
      ...(input.thinking_level === undefined ? {} : { thinkingLevel: input.thinking_level }),
      ...(input.permission_mode === undefined ? {} : { permissionMode: input.permission_mode }),
      ...(input.provider_thread_id === undefined
        ? {}
        : { providerThreadId: input.provider_thread_id }),
      ...(input.policy === undefined ? {} : { policy: input.policy }),
    });
    try {
      await this.start(runId);
      // Snapshot the cursor from the ready state before the turn starts so a
      // controller awaiting from the returned sequence always observes the
      // turn's running, attention, and completion events even if the turn
      // finishes before the first poll.
      const startCursor = this.inspect(runId);
      if (startCursor === undefined) {
        throw new Error(`Run disappeared after start: ${runId}`);
      }
      const startSequence = startCursor.sequence;
      const submitPromise = this.submit(runId, input.prompt);
      // The supervisor surfaces submit failures through durable failed events
      // (or via inspect when the sink itself is broken); swallow the
      // unhandled-rejection warning without losing visibility of the failure.
      void submitPromise.catch(() => {});
      await waitForTurnRunning(this, runId, submitPromise);
      const phase = this.inspect(runId)?.phase ?? "running";
      return {
        run_id: runId,
        sequence: startSequence,
        phase,
      };
    } catch (error) {
      await this.close(runId).catch(() => {});
      throw error;
    }
  }

  async awaitRun(input: XagentAwaitInput, signal?: AbortSignal): Promise<AwaitRunResult> {
    const live = this.#runs.get(input.run_id);
    if (live === undefined) {
      const persisted = await this.#awaitPersistedRun(input, signal);
      if (persisted === undefined) {
        throw unknownRunError(input.run_id);
      }
      return persisted;
    }
    const deadlineSeconds = input.deadline_seconds ?? x_DefaultAwaitDeadlineSeconds;
    const startedAtMs = this.#clock().getTime();
    const result = await this.awaitEvent(
      input.run_id,
      input.after_sequence,
      deadlineSeconds * 1000,
      signal,
    );
    const elapsedMs = Math.max(0, this.#clock().getTime() - startedAtMs);
    return shapeAwaitEnvelope(result, elapsedMs);
  }

  publishAttention(runId: string, reason: string, payload?: unknown): Promise<SupervisionEvent> {
    return this.#require(runId).supervisor.publishAttention(reason, payload);
  }

  // Recovery, not polling. When a start response is lost — a dropped tool
  // result, a crashed client — this is the only supported way to find the
  // orphaned run id over MCP; without it the run leaks until someone reads
  // the log root by hand.
  //
  async listOwnedRuns(input: XagentListInput): Promise<ListRunsResult> {
    const persisted = await listRuns(this.#logRoot);
    const runs = persisted
      .filter((metadata) => metadata.supervised === true)
      .map((metadata) => ({
        run_id: metadata.run_id,
        harness: metadata.harness,
        ...(metadata.model === undefined ? {} : { model: metadata.model }),
        phase: metadata.supervision.phase,
        sequence: metadata.supervision.sequence,
        exit_status: metadata.exit_status,
        live: this.#runs.has(metadata.run_id),
        supervised: true,
        created_at: metadata.created_at,
        updated_at: metadata.updated_at,
      }))
      .filter((row) => !input.live_only || row.live)
      .sort((left, right) => right.created_at.localeCompare(left.created_at))
      .slice(0, input.limit);
    return { runs };
  }

  async inspectRun(input: XagentInspectInput): Promise<InspectRunResult> {
    const live = this.inspect(input.run_id);
    if (live !== undefined) {
      return {
        run_id: live.run_id,
        phase: live.phase,
        sequence: live.sequence,
        ...(live.provider_thread_id === undefined
          ? {}
          : { provider_thread_id: live.provider_thread_id }),
        ...(live.callback_failure === undefined
          ? {}
          : { callback_failure: live.callback_failure }),
      };
    }
    // Fall back to persisted metadata so abandoned/terminal runs from a prior
    // service incarnation remain distinguishable from never-existed runs.
    const persisted = await this.#inspectPersistedRun(input.run_id);
    if (persisted === undefined) {
      throw unknownRunError(input.run_id);
    }
    return persisted;
  }

  async #inspectPersistedRun(runId: string): Promise<InspectRunResult | undefined> {
    let record: RunRecord;
    try {
      record = await openRunRecord(this.#logRoot, runId);
    } catch {
      return undefined;
    }
    return {
      run_id: record.run_id,
      phase: record.supervision.phase,
      sequence: record.supervision.sequence,
      ...(record.supervision.provider_thread_id === undefined
        ? {}
        : { provider_thread_id: record.supervision.provider_thread_id }),
    };
  }

  async #awaitPersistedRun(
    input: XagentAwaitInput,
    signal?: AbortSignal,
  ): Promise<AwaitRunResult | undefined> {
    if (signal?.aborted === true) {
      throw abortError();
    }
    let record: RunRecord;
    try {
      record = await openRunRecord(this.#logRoot, input.run_id);
    } catch {
      return undefined;
    }
    if (!terminalPersistedPhases.has(record.supervision.phase)) {
      // Active phases without a live supervisor cannot be awaited; the
      // controller must treat the run as unknown until restart reconciliation
      // (or a live owner) makes a durable wake available.
      return undefined;
    }
    const startedAtMs = this.#clock().getTime();
    const wake = await findPersistedAwaitWake(record, input.after_sequence);
    const elapsedMs = Math.max(0, this.#clock().getTime() - startedAtMs);
    if (wake !== undefined) {
      return shapeAwaitEnvelope(wake, elapsedMs);
    }
    // The run is terminal and no durable wake exists after the cursor. The
    // requested deadline was never reached; surface a distinguishable reason so
    // the quiet client stops promptly instead of looping on a synthetic
    // await_deadline that returned in ~0 ms.
    //
    return shapeAwaitEnvelope({
      schema_version: 1,
      type: "supervision.deadline",
      run_id: record.run_id,
      sequence: record.supervision.sequence,
      timestamp: this.#clock().toISOString(),
      phase: record.supervision.phase,
      reason: "run_terminal",
    }, elapsedMs);
  }

  async messageRun(input: XagentMessageInput): Promise<MessageRunResult> {
    // Snapshot the cursor before the new turn starts so a controller awaiting
    // from the returned sequence observes the turn's events even when it
    // completes before the first poll.
    const before = this.inspect(input.run_id);
    if (before === undefined) {
      throw unknownRunError(input.run_id);
    }
    const startSequence = before.sequence;
    const submitPromise = this.submit(input.run_id, input.text);
    void submitPromise.catch(() => {});
    await waitForTurnRunning(this, input.run_id, submitPromise);
    const phase = this.inspect(input.run_id)?.phase ?? before.phase;
    return {
      run_id: input.run_id,
      phase,
      sequence: startSequence,
    };
  }

  async interruptRun(input: XagentInterruptInput): Promise<InterruptRunResult> {
    await this.interrupt(input.run_id);
    const inspection = this.inspect(input.run_id);
    if (inspection === undefined) {
      throw unknownRunError(input.run_id);
    }
    return {
      run_id: inspection.run_id,
      phase: inspection.phase,
      sequence: inspection.sequence,
    };
  }

  async closeRun(input: XagentCloseInput): Promise<CloseRunResult> {
    await this.close(input.run_id);
    return {
      run_id: input.run_id,
      closed: true,
    };
  }

  #require(runId: string): OwnedRun {
    const run = this.#runs.get(runId);
    if (run === undefined) {
      throw unknownRunError(runId);
    }
    return run;
  }
}

export async function canonicalizeWorkingDirectory(cwd: string): Promise<string> {
  if (!path.isAbsolute(cwd)) {
    throw invalidWorkingDirectoryError(cwd, "working directory must be an absolute path");
  }
  let info;
  try {
    info = await stat(cwd);
  } catch {
    throw invalidWorkingDirectoryError(cwd, "working directory does not exist");
  }
  if (!info.isDirectory()) {
    throw invalidWorkingDirectoryError(cwd, "working directory path is not a directory");
  }
  return realpath(cwd);
}

function invalidWorkingDirectoryError(cwd: string, message: string): ToolValidationError {
  const structured: StructuredToolError = {
    error: "invalid_working_directory",
    message,
    details: { cwd },
  };
  return new ToolValidationError(structured);
}

function unknownRunError(runId: string): ToolValidationError {
  return new ToolValidationError({
    error: "unknown_run",
    message: `Unknown xagent run: ${runId}`,
    details: { run_id: runId },
  });
}

function toRunSupervisionUpdate(state: SupervisionPersistenceState) {
  return {
    phase: state.phase,
    sequence: state.sequence,
    provider_thread_id: state.provider_thread_id,
    last_transport_progress_at: state.last_transport_progress_at,
    last_semantic_progress_at: state.last_semantic_progress_at,
    owned_process: state.owned_process,
    watchdog: state.watchdog,
  };
}

function shapeAwaitEnvelope(result: AwaitResult, elapsedMs: number): AwaitRunResult {
  const base = {
    schema_version: 1 as const,
    event: result.type,
    run_id: result.run_id,
    sequence: result.sequence,
    phase: result.phase,
    elapsed_ms: elapsedMs,
  };
  if (result.type === "turn.completed") {
    const payload = ("payload" in result ? result.payload : undefined) as
      | {
          readonly report?: { readonly text?: string };
          readonly usage?: { readonly input_tokens?: number; readonly output_tokens?: number };
        }
      | undefined;
    const report = payload?.report;
    if (report === undefined || report.text === undefined) {
      return {
        ...base,
        reason: "missing_final_report",
      };
    }
    const usage = payload?.usage;
    return {
      ...base,
      report: { text: report.text },
      ...(usage === undefined ? {} : { usage }),
    };
  }
  if (result.type === "supervision.deadline") {
    return {
      ...base,
      reason: result.reason,
    };
  }
  const reason = result.reason;
  const payload = "payload" in result ? result.payload : undefined;
  return {
    ...base,
    ...(reason === undefined ? {} : { reason }),
    ...(payload === undefined ? {} : { payload }),
  };
}

async function findPersistedAwaitWake(
  record: RunRecord,
  afterSequence: number,
): Promise<SupervisionEvent | undefined> {
  let raw: string;
  try {
    raw = await readFile(record.normalizedLogPath, "utf8");
  } catch (error) {
    if (isNodeError(error) && error.code === "ENOENT") {
      return undefined;
    }
    throw error;
  }
  for (const line of raw.split("\n")) {
    if (line.trim().length === 0) {
      continue;
    }
    let parsed: unknown;
    try {
      parsed = JSON.parse(line);
    } catch {
      continue;
    }
    if (!isPersistedAwaitWake(parsed) || parsed.sequence <= afterSequence) {
      continue;
    }
    return parsed;
  }
  return undefined;
}

export function isPersistedAwaitWake(value: unknown): value is SupervisionEvent {
  if (!isRecord(value)) {
    return false;
  }
  if (
    value.schema_version !== 1
    || typeof value.run_id !== "string"
    || !Number.isSafeInteger(value.sequence)
    || typeof value.timestamp !== "string"
    || typeof value.phase !== "string"
    || typeof value.reason !== "string"
  ) {
    return false;
  }
  // Whitelist, not a blacklist: only these three shapes may wake a persisted
  // await. `turn.submitted` is deliberately absent — it is a record of what was
  // sent, never a deliverable outcome.
  if (value.type === "supervision.attention" || value.type === "turn.completed") {
    return true;
  }
  return value.type === "supervision.state"
    && terminalPersistedPhases.has(value.phase as SupervisionPhase);
}

function abortError(): Error {
  return new DOMException("The operation was aborted.", "AbortError");
}

function isNodeError(error: unknown): error is NodeJS.ErrnoException {
  return error instanceof Error && "code" in error;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export async function waitForTurnRunning(
  manager: {
    inspect(runId: string): { readonly phase: string } | undefined;
  },
  runId: string,
  submitPromise: Promise<void>,
): Promise<void> {
  let settleError: unknown;
  const settled = submitPromise.then(
    () => "done" as const,
    (error: unknown) => {
      settleError = error;
      return "error" as const;
    },
  );

  for (let attempt = 0; attempt < 200; attempt += 1) {
    const outcome = await Promise.race([
      settled,
      new Promise<"poll">((resolve) => {
        setTimeout(() => resolve("poll"), 5);
      }),
    ]);
    const inspection = manager.inspect(runId);
    if (inspection === undefined) {
      throw new Error(`Run disappeared after start: ${runId}`);
    }
    if (outcome === "error") {
      throw settleError;
    }
    if (outcome === "done" || inspection.phase === "running") {
      return;
    }
  }
}
