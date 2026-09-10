import { spawn as nodeSpawn } from "node:child_process";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

import { truncateUtf8 } from "../sanitize.js";
import type {
  WatchdogClassifier,
  WatchdogRequest,
  WatchdogUsage,
  WatchdogVerdict,
} from "./types.js";
import { normalizeWatchdogVerdict } from "./watchdog.js";

const DEFAULT_INPUT_LIMIT_BYTES = 64 * 1024;
const DEFAULT_OUTPUT_LIMIT_BYTES = 2 * 1024;
// The stdout byte-level cap used to truncate runaway Claude Code output.
// This is deliberately larger than the verdict output bound: Claude Code's
// `--output-format json` envelope wraps the structured verdict in
// `structured_output` and duplicates it as a string in `result`, plus
// `session_id`, `usage`, `modelUsage`, `total_cost_usd`, `duration_ms`, and
// `permission_denials`. The 2 KiB verdict bound is applied to the extracted
// structured output below; this stdout cap only guards against a
// runaway process filling the pipe.
//
export const DEFAULT_STDOUT_LIMIT_BYTES = 16 * 1024;
const DEFAULT_TIMEOUT_MS = 90_000;
const HAIKU_INPUT_USD_PER_MILLION_TOKENS = 1;
const HAIKU_OUTPUT_USD_PER_MILLION_TOKENS = 5;
const CLAUDE_CODE_OVERHEAD_RESERVE_USD = 0.02;

const WATCHDOG_SCHEMA = {
  type: "object",
  additionalProperties: false,
  properties: {
    verdict: {
      type: "string",
      enum: ["healthy", "derailed", "uncertain"],
    },
    confidence: {
      type: "number",
      minimum: 0,
      maximum: 1,
    },
    reason_code: {
      type: "string",
      minLength: 1,
      maxLength: 128,
      pattern: "^[a-z0-9_]+$",
    },
    evidence: {
      type: "array",
      maxItems: 8,
      items: {
        type: "string",
        maxLength: 192,
      },
    },
  },
  required: ["verdict", "confidence", "reason_code", "evidence"],
} as const;

export const WATCHDOG_SCHEMA_JSON = JSON.stringify(WATCHDOG_SCHEMA);

const WATCHDOG_SYSTEM_PROMPT = [
  "Classify only the semantic health of the active worker from bounded provider JSON.",
  "Use the harness field to interpret provider-specific records.",
  "Repeated tools, retries, failures, empty deltas, and unfamiliar transport records are ambiguous and are not by themselves evidence of derailment.",
  "Failing tests, temporary build failures, and briefly inconsistent files are normal while an implementation is in progress and are not by themselves evidence of derailment.",
  "Use derailed only for sustained evidence that the worker abandoned or materially contradicted the assigned task.",
  "Return healthy, derailed, or uncertain using the required schema.",
  "Evidence entries must be short factual observations already supported by the input.",
  "Do not provide commands, remediation, controller actions, or instructions to the worker.",
  "Use uncertain whenever the bounded evidence cannot establish semantic health.",
].join(" ");

export type WatchdogSpawnRequest = {
  readonly command: string;
  readonly args: readonly string[];
  readonly cwd: string;
  readonly input: string;
  readonly timeoutMs: number;
  readonly outputLimitBytes: number;
  readonly signal: AbortSignal;
};

export type WatchdogSpawnResult = {
  readonly exitCode: number | null;
  readonly stdout: string;
  readonly stderr: string;
  readonly timedOut?: boolean;
  readonly outputTooLarge?: boolean;
  readonly budgetExceeded?: boolean;
  readonly aborted?: boolean;
};

export type WatchdogSpawn = (
  request: WatchdogSpawnRequest,
) => Promise<WatchdogSpawnResult>;

export type ClaudeWatchdogClassifierOptions = {
  readonly spawn?: WatchdogSpawn;
  readonly inputLimitBytes?: number;
  readonly outputLimitBytes?: number;
  readonly timeoutMs?: number;
  readonly maxBudgetUsd?: number;
  readonly confidenceFloor?: number;
};

export class ClaudeWatchdogClassifier implements WatchdogClassifier {
  readonly #spawn: WatchdogSpawn;
  readonly #inputLimitBytes: number;
  readonly #outputLimitBytes: number;
  readonly #timeoutMs: number;
  readonly #maxBudgetUsd: number;
  readonly #confidenceFloor: number;

  constructor(options: ClaudeWatchdogClassifierOptions = {}) {
    this.#spawn = options.spawn ?? spawnWatchdogProcess;
    this.#inputLimitBytes = boundedPositiveInteger(
      options.inputLimitBytes ?? DEFAULT_INPUT_LIMIT_BYTES,
      DEFAULT_INPUT_LIMIT_BYTES,
      "inputLimitBytes",
    );
    this.#outputLimitBytes = boundedPositiveInteger(
      options.outputLimitBytes ?? DEFAULT_OUTPUT_LIMIT_BYTES,
      DEFAULT_OUTPUT_LIMIT_BYTES,
      "outputLimitBytes",
    );
    this.#timeoutMs = positiveNumber(options.timeoutMs ?? DEFAULT_TIMEOUT_MS, "timeoutMs");
    const minimumBudget = minimumWatchdogBudgetUsd(
      this.#inputLimitBytes,
      this.#outputLimitBytes,
    );
    this.#maxBudgetUsd = positiveNumber(
      options.maxBudgetUsd ?? minimumBudget,
      "maxBudgetUsd",
    );
    if (this.#maxBudgetUsd < minimumBudget) {
      throw new Error(`maxBudgetUsd must be at least ${minimumBudget}.`);
    }
    this.#confidenceFloor = confidence(options.confidenceFloor ?? 0.8);
  }

  async classify(
    request: WatchdogRequest,
    signal: AbortSignal,
  ): Promise<WatchdogVerdict> {
    const input = JSON.stringify(request);
    if (Buffer.byteLength(input, "utf8") > this.#inputLimitBytes) {
      return uncertain("classifier_input_too_large");
    }

    const cwd = await mkdtemp(path.join(tmpdir(), "xagent-watchdog-"));
    try {
      let result: WatchdogSpawnResult;
      try {
        result = await this.#spawn({
          command: "claude",
          args: [
            "--print",
            "--model",
            "haiku",
            "--safe-mode",
            "--tools",
            "",
            "--strict-mcp-config",
            "--no-session-persistence",
            "--output-format",
            "json",
            "--json-schema",
            WATCHDOG_SCHEMA_JSON,
            "--mcp-config",
            '{"mcpServers":{}}',
            "--max-budget-usd",
            String(this.#maxBudgetUsd),
            "--system-prompt",
            WATCHDOG_SYSTEM_PROMPT,
          ],
          cwd,
          input,
          timeoutMs: this.#timeoutMs,
          outputLimitBytes: DEFAULT_STDOUT_LIMIT_BYTES,
          signal,
        });
      } catch {
        return uncertain(
          "classifier_spawn_failed",
          ["Claude Code could not be started."],
        );
      }

      const stdoutBytes = Buffer.byteLength(result.stdout, "utf8");
      if (result.aborted === true) {
        return withOutput(uncertain("classifier_aborted"), stdoutBytes);
      }
      if (result.timedOut === true) {
        return withOutput(uncertain("classifier_timeout"), stdoutBytes);
      }
      if (result.outputTooLarge === true) {
        return withOutput(uncertain("classifier_output_too_large"), stdoutBytes);
      }
      if (result.budgetExceeded === true) {
        return withOutput(uncertain("classifier_budget_exceeded"), stdoutBytes);
      }
      let envelope: unknown;
      try {
        envelope = JSON.parse(result.stdout);
      } catch {
        if (result.exitCode !== 0) {
          return withOutput(uncertain(
            "classifier_nonzero_exit",
            [`Claude Code exited with status ${String(result.exitCode)}.`],
          ), stdoutBytes);
        }
        return withOutput(uncertain("invalid_classifier_output"), stdoutBytes);
      }
      if (isRecord(envelope) && envelope.subtype === "error_max_budget_usd") {
        return withOutput(uncertain("classifier_budget_exceeded"), stdoutBytes);
      }
      if (result.exitCode !== 0) {
        return withOutput(uncertain(
          "classifier_nonzero_exit",
          [`Claude Code exited with status ${String(result.exitCode)}.`],
        ), stdoutBytes);
      }
      if (!isRecord(envelope)) {
        return withOutput(uncertain("invalid_classifier_output"), stdoutBytes);
      }

      const candidate = structuredOutput(envelope);
      // The verdict output bound is applied to the normalized structured
      // output (the schema-valid classifier verdict with evidence truncated
      // to the per-item UTF-8 byte bound), not the raw Claude Code stdout
      // envelope. The envelope's `result` field duplicates the structured
      // output as a string and adds transport metadata (`session_id`,
      // `usage`, `duration_ms`, ...) that the classifier did not author;
      // counting those against the verdict bound would force every
      // schema-valid maximal verdict into `classifier_output_too_large`
      // and wake the controller once per checkpoint. See OpenSpec xas-6.
      //
      const verdict = normalizeWatchdogVerdict(candidate, this.#confidenceFloor);
      const verdictBytes = Buffer.byteLength(
        JSON.stringify(verdict),
        "utf8",
      );
      if (verdictBytes > this.#outputLimitBytes) {
        return withOutput(uncertain("classifier_output_too_large"), stdoutBytes);
      }
      const usage = watchdogUsage(envelope.usage);
      const estimatedCost = finiteNonNegative(envelope.total_cost_usd);
      return {
        ...verdict,
        output_bytes: verdictBytes,
        ...(usage === undefined ? {} : { usage }),
        ...(estimatedCost === undefined
          ? {}
          : { estimated_cost_usd: estimatedCost }),
      };
    } finally {
      await rm(cwd, { recursive: true, force: true });
    }
  }
}

export function minimumWatchdogBudgetUsd(
  inputLimitBytes: number,
  outputLimitBytes: number,
): number {
  const inputBytes = nonNegativeSafeInteger(inputLimitBytes, "inputLimitBytes");
  const outputBytes = nonNegativeSafeInteger(outputLimitBytes, "outputLimitBytes");
  // One token per bounded UTF-8 byte is deliberately conservative for the
  // request/response payload. The fixed reserve covers Claude Code's safe-mode
  // system/schema prompt outside that byte envelope.
  const envelopeCost =
    inputBytes * HAIKU_INPUT_USD_PER_MILLION_TOKENS / 1_000_000
    + outputBytes * HAIKU_OUTPUT_USD_PER_MILLION_TOKENS / 1_000_000;
  return roundUpCents(envelopeCost + CLAUDE_CODE_OVERHEAD_RESERVE_USD);
}

export function maximumWatchdogRunExposureUsd(
  maxBudgetUsd: number,
  maximumCalls: number,
): number {
  const budget = positiveNumber(maxBudgetUsd, "maxBudgetUsd");
  const calls = nonNegativeSafeInteger(maximumCalls, "maximumCalls");
  return Math.round(budget * calls * 100) / 100;
}

export const spawnWatchdogProcess: WatchdogSpawn = async (
  request,
): Promise<WatchdogSpawnResult> => new Promise((resolve, reject) => {
  const child = nodeSpawn(request.command, [...request.args], {
    cwd: request.cwd,
    stdio: ["pipe", "pipe", "pipe"],
  });
  let stdout = "";
  let stderr = "";
  let outputTooLarge = false;
  let timedOut = false;
  let aborted = false;
  let settled = false;

  const finish = (result: WatchdogSpawnResult): void => {
    if (settled) {
      return;
    }
    settled = true;
    clearTimeout(timer);
    request.signal.removeEventListener("abort", onAbort);
    resolve(result);
  };
  const terminate = (): void => {
    if (!child.killed) {
      child.kill("SIGKILL");
    }
  };
  const timer = setTimeout(() => {
    timedOut = true;
    terminate();
  }, request.timeoutMs);
  const onAbort = (): void => {
    aborted = true;
    terminate();
  };
  request.signal.addEventListener("abort", onAbort, { once: true });
  if (request.signal.aborted) {
    onAbort();
  }

  child.stdout.setEncoding("utf8");
  child.stdout.on("data", (chunk: string) => {
    if (outputTooLarge) {
      return;
    }
    const combined = `${stdout}${chunk}`;
    if (Buffer.byteLength(combined, "utf8") > request.outputLimitBytes) {
      outputTooLarge = true;
      stdout = truncateUtf8(combined, request.outputLimitBytes + 1);
      terminate();
      return;
    }
    stdout = combined;
  });
  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk: string) => {
    stderr = Buffer.from(`${stderr}${chunk}`, "utf8")
      .subarray(0, request.outputLimitBytes)
      .toString("utf8");
  });
  child.once("error", (error) => {
    if (settled) {
      return;
    }
    settled = true;
    clearTimeout(timer);
    request.signal.removeEventListener("abort", onAbort);
    reject(error);
  });
  child.once("close", (exitCode) => {
    finish({
      exitCode,
      stdout,
      stderr,
      timedOut,
      outputTooLarge,
      aborted,
    });
  });
  // The child can be SIGKILLed while a large stdin write is still buffered
  // (timeout, abort, or outputTooLarge all call terminate() mid-write). When
  // that happens Node emits `error` on the Socket — not on the ChildProcess
  // — so the `child.once("error", …)` handler above does not cover it. An
  // unhandled stream error would crash the entire xagent service (the
  // supervisor runs in-process with every other run), so swallow stdin
  // errors here; the spawn result already reflects the kill via
  // `timedOut`/`aborted`/`outputTooLarge`.
  //
  child.stdin.on("error", () => {
    // Intentionally empty: see comment above.
    //
  });
  child.stdin.end(request.input);
});

function structuredOutput(envelope: Record<string, unknown>): unknown {
  if (envelope.structured_output !== undefined) {
    return envelope.structured_output;
  }
  if (typeof envelope.result === "string") {
    try {
      return JSON.parse(envelope.result);
    } catch {
      return undefined;
    }
  }
  return envelope;
}

function watchdogUsage(value: unknown): WatchdogUsage | undefined {
  if (!isRecord(value)) {
    return undefined;
  }
  const inputTokens = finiteNonNegative(value.input_tokens);
  const outputTokens = finiteNonNegative(value.output_tokens);
  if (inputTokens === undefined && outputTokens === undefined) {
    return undefined;
  }
  return {
    ...(inputTokens === undefined ? {} : { input_tokens: inputTokens }),
    ...(outputTokens === undefined ? {} : { output_tokens: outputTokens }),
  };
}

function finiteNonNegative(value: unknown): number | undefined {
  return typeof value === "number" && Number.isFinite(value) && value >= 0
    ? value
    : undefined;
}

function withOutput(verdict: WatchdogVerdict, outputBytes: number): WatchdogVerdict {
  return { ...verdict, output_bytes: outputBytes };
}

function uncertain(reasonCode: string, evidence: readonly string[] = []): WatchdogVerdict {
  return {
    verdict: "uncertain",
    confidence: 0,
    reason_code: reasonCode,
    evidence,
  };
}

function boundedPositiveInteger(value: number, maximum: number, name: string): number {
  if (!Number.isSafeInteger(value) || value <= 0 || value > maximum) {
    throw new Error(`${name} must be a positive integer no greater than ${maximum}.`);
  }
  return value;
}

function positiveNumber(value: number, name: string): number {
  if (!Number.isFinite(value) || value <= 0) {
    throw new Error(`${name} must be a positive finite number.`);
  }
  return value;
}

function nonNegativeSafeInteger(value: number, name: string): number {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error(`${name} must be a non-negative safe integer.`);
  }
  return value;
}

function roundUpCents(value: number): number {
  return Math.ceil((value - Number.EPSILON) * 100) / 100;
}

function confidence(value: number): number {
  if (!Number.isFinite(value) || value < 0 || value > 1) {
    throw new Error("confidenceFloor must be between 0 and 1.");
  }
  return value;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
