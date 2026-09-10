import assert from "node:assert/strict";
import { readdir } from "node:fs/promises";
import test from "node:test";

import {
  ClaudeWatchdogClassifier,
  WATCHDOG_SCHEMA_JSON,
  maximumWatchdogRunExposureUsd,
  minimumWatchdogBudgetUsd,
  spawnWatchdogProcess,
  type WatchdogSpawn,
  type WatchdogSpawnRequest,
} from "../src/supervision/claude_watchdog.js";
import { ProviderJsonEvidenceWindow } from "../src/supervision/evidence.js";
import type { WatchdogRequest } from "../src/supervision/types.js";
import { withWatchdogInputBytes } from "./support/watchdog_request.js";

test("launches one fresh isolated no-tools Haiku invocation with bounded stdin", async () => {
  const calls: Array<WatchdogSpawnRequest & { cwdEntries: string[] }> = [];
  const spawn: WatchdogSpawn = async (invocation) => {
    calls.push({
      ...invocation,
      cwdEntries: await readdir(invocation.cwd),
    });
    return {
      exitCode: 0,
      stdout: JSON.stringify({
        type: "result",
        subtype: "success",
        structured_output: {
          verdict: "healthy",
          confidence: 0.91,
          reason_code: "steady_progress",
          evidence: ["The worker produced new semantic progress."],
        },
        usage: { input_tokens: 120, output_tokens: 30 },
        total_cost_usd: 0.0005,
      }),
      stderr: "",
    };
  };
  const classifier = new ClaudeWatchdogClassifier({ spawn });
  const result = await classifier.classify(request(), new AbortController().signal);

  assert.equal(result.verdict, "healthy");
  assert.equal(calls.length, 1);
  const call = calls[0];
  assert.ok(call);
  assert.equal(call.command, "claude");
  assert.deepEqual(call.args.slice(0, 13), [
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
  ]);
  assert.equal(call.args[13], '{"mcpServers":{}}');
  const budgetIndex = call.args.indexOf("--max-budget-usd");
  assert.ok(budgetIndex >= 0);
  assert.equal(call.args[budgetIndex + 1], "0.1");
  const systemPromptIndex = call.args.indexOf("--system-prompt");
  assert.ok(systemPromptIndex >= 0);
  const systemPrompt = call.args[systemPromptIndex + 1];
  assert.ok(typeof systemPrompt === "string");
  assert.match(systemPrompt, /bounded provider JSON/i);
  assert.match(systemPrompt, /harness/i);
  assert.match(systemPrompt, /repeated tools.*ambiguous/i);
  assert.match(systemPrompt, /failing tests.*normal/i);
  assert.match(systemPrompt, /sustained evidence.*abandoned.*contradicted/i);
  assert.equal(call.timeoutMs, 90_000);
  assert.doesNotMatch(systemPrompt, /sanitized JSON evidence/i);
  assert.deepEqual(call.cwdEntries, []);
  assert.ok(Buffer.byteLength(call.input, "utf8") <= 64 * 1024);
  assert.equal(call.args.some((arg) => arg.includes(process.cwd())), false);
  assert.equal(call.args.includes("--resume"), false);
  assert.equal(call.args.includes("--continue"), false);
});

test("classify accepts a maximal provider JSON request without reporting input too large", async () => {
  let spawnCalls = 0;
  const spawn: WatchdogSpawn = async () => {
    spawnCalls += 1;
    return {
      exitCode: 0,
      stdout: JSON.stringify({
        type: "result",
        subtype: "success",
        structured_output: {
          verdict: "healthy",
          confidence: 0.91,
          reason_code: "steady_progress",
          evidence: ["The bounded provider JSON request was accepted."],
        },
      }),
      stderr: "",
    };
  };
  const evidence = new ProviderJsonEvidenceWindow({
    harness: "claude_code",
    originalPrompt: `Inspect ${process.cwd()} api_key=prompt-secret ${"🧶".repeat(40_000)}`,
  });
  for (let index = 0; index < 200; index += 1) {
    evidence.record({
      type: "assistant",
      index,
      message: {
        content: [{ type: "text", text: `${index}:${"progress ".repeat(90)}` }],
      },
    });
  }
  const requestValue = evidence.snapshot();
  assert.equal(requestValue.input_bytes, Buffer.byteLength(JSON.stringify(requestValue), "utf8"));
  assert.ok(requestValue.input_bytes <= 64 * 1024);

  const result = await new ClaudeWatchdogClassifier({ spawn })
    .classify(requestValue, new AbortController().signal);

  assert.equal(spawnCalls, 1);
  assert.notEqual(result.reason_code, "classifier_input_too_large");
  assert.equal(result.verdict, "healthy");
});

test("default budget covers the maximum envelope and bounds eight-call exposure", () => {
  assert.equal(minimumWatchdogBudgetUsd(64 * 1024, 2 * 1024), 0.1);
  assert.equal(maximumWatchdogRunExposureUsd(0.1, 8), 0.8);
  assert.throws(() => new ClaudeWatchdogClassifier({
    maxBudgetUsd: 0.09,
  }), /maxBudgetUsd must be at least 0.1/);

  assert.equal(minimumWatchdogBudgetUsd(1024, 512), 0.03);
  assert.doesNotThrow(() => new ClaudeWatchdogClassifier({
    inputLimitBytes: 1024,
    outputLimitBytes: 512,
    maxBudgetUsd: 0.03,
  }));
});

test("normalizes invalid JSON, failed invocation, over-budget, and oversized output", async () => {
  const cases: Array<{
    readonly name: string;
    readonly spawn: WatchdogSpawn;
    readonly reason: string;
  }> = [
    {
      name: "invalid JSON",
      spawn: async () => ({ exitCode: 0, stdout: "{bad", stderr: "" }),
      reason: "invalid_classifier_output",
    },
    {
      name: "failed invocation",
      spawn: async () => {
        throw new Error("spawn failed");
      },
      reason: "classifier_spawn_failed",
    },
    {
      name: "budget exceeded",
      spawn: async () => ({
        exitCode: 1,
        stdout: JSON.stringify({ type: "result", subtype: "error_max_budget_usd" }),
        stderr: "",
      }),
      reason: "classifier_budget_exceeded",
    },
    {
      name: "nonzero exit",
      spawn: async () => ({
        exitCode: 7,
        stdout: JSON.stringify({ type: "result", subtype: "error_during_execution" }),
        stderr: "provider failed",
      }),
      reason: "classifier_nonzero_exit",
    },
    {
      name: "output too large",
      spawn: async () => ({
        exitCode: 0,
        stdout: "x".repeat(2 * 1024 + 1),
        stderr: "",
        outputTooLarge: true,
      }),
      reason: "classifier_output_too_large",
    },
  ];

  for (const item of cases) {
    const result = await new ClaudeWatchdogClassifier({
      spawn: item.spawn,
    }).classify(request(), new AbortController().signal);
    assert.equal(result.verdict, "uncertain", item.name);
    assert.equal(result.reason_code, item.reason, item.name);
  }
});

test("normalizes a timed-out invocation and low-confidence healthy output to uncertain", async () => {
  const timedOut = await new ClaudeWatchdogClassifier({
    spawn: async () => ({
      exitCode: null,
      stdout: "",
      stderr: "",
      timedOut: true,
    }),
  }).classify(request(), new AbortController().signal);
  assert.equal(timedOut.verdict, "uncertain");
  assert.equal(timedOut.reason_code, "classifier_timeout");

  const lowConfidence = await new ClaudeWatchdogClassifier({
    spawn: async () => ({
      exitCode: 0,
      stdout: JSON.stringify({
        structured_output: {
          verdict: "healthy",
          confidence: 0.79,
          reason_code: "steady_progress",
          evidence: [],
        },
      }),
      stderr: "",
    }),
  }).classify(request(), new AbortController().signal);
  assert.equal(lowConfidence.verdict, "uncertain");
  assert.equal(lowConfidence.reason_code, "healthy_below_confidence_floor");
});

test("native spawn stops and retains only a bounded prefix after oversized stdout", async () => {
  const result = await spawnWatchdogProcess({
    command: process.execPath,
    args: ["-e", "process.stdout.write('x'.repeat(64 * 1024))"],
    cwd: process.cwd(),
    input: "",
    timeoutMs: 5_000,
    outputLimitBytes: 64,
    signal: new AbortController().signal,
  });

  assert.equal(result.outputTooLarge, true);
  assert.ok(Buffer.byteLength(result.stdout, "utf8") <= 65);
});

// I1: a schema-valid maximal healthy verdict (8 evidence items at the
// per-item length bound) plus the Claude Code JSON envelope must not be
// rejected as `classifier_output_too_large`. The 2 KiB output bound is
// applied to the extracted structured output, and the evidence bounds are
// sized so a maximal verdict fits within it. Without this, every healthy
// checkpoint would wake the controller once with an `uncertain` attention.
//
test("accepts a maximal schema-valid healthy verdict wrapped in a Claude Code envelope", async () => {
  const maximalStructuredOutput = {
    verdict: "healthy",
    confidence: 0.91,
    reason_code: "steady_progress",
    evidence: Array.from({ length: 8 }, () => "x".repeat(192)),
  };
  const envelope = {
    type: "result",
    subtype: "success",
    structured_output: maximalStructuredOutput,
    // Claude Code duplicates the structured output as a string in `result`,
    // so the full stdout is well above 2 KiB even though the verdict itself
    // fits. The bound must apply to the verdict, not the envelope.
    //
    result: JSON.stringify(maximalStructuredOutput),
    session_id: "session-" + "s".repeat(40),
    usage: { input_tokens: 120, output_tokens: 30 },
    total_cost_usd: 0.0005,
    duration_ms: 12345,
    permission_denials: 0,
  };
  const stdout = JSON.stringify(envelope);
  assert.ok(
    Buffer.byteLength(stdout, "utf8") > 2 * 1024,
    "envelope must exceed the 2 KiB verdict bound to prove the bound is not applied to stdout",
  );

  const spawn: WatchdogSpawn = async () => ({
    exitCode: 0,
    stdout,
    stderr: "",
  });
  const classifier = new ClaudeWatchdogClassifier({ spawn });
  const result = await classifier.classify(request(), new AbortController().signal);

  assert.equal(result.verdict, "healthy");
  assert.equal(result.reason_code, "steady_progress");
  assert.equal(
    (result as { output_bytes?: number }).output_bytes,
    Buffer.byteLength(JSON.stringify(maximalStructuredOutput), "utf8"),
  );
  assert.ok(
    (result as { output_bytes?: number }).output_bytes! <= 2 * 1024,
    "verdict bytes must fit within the 2 KiB output bound",
  );
});

test("rejects a schema-invalid structured verdict even when stdout is under the truncation cap", async () => {
  // A classifier that ignores the schema's evidence character limit returns
  // items longer than MAX_EVIDENCE_ITEM_LENGTH. Normalization rejects this
  // as `invalid_classifier_output` (the per-item character guard fires before
  // the byte bound is applied); the byte cap remains a defensive guard for
  // verdicts that are schema-valid in character count but still oversized
  // after per-item byte truncation.
  //
  const oversizedStructuredOutput = {
    verdict: "healthy",
    confidence: 0.91,
    reason_code: "steady_progress",
    evidence: Array.from({ length: 8 }, () => "x".repeat(512)),
  };
  const envelope = {
    type: "result",
    subtype: "success",
    structured_output: oversizedStructuredOutput,
    result: JSON.stringify(oversizedStructuredOutput),
  };
  const stdout = JSON.stringify(envelope);
  // The envelope is under the 16 KiB stdout truncation cap, so the spawn
  // itself does not flag outputTooLarge; the rejection is enforced at the
  // structured-output layer.
  //
  assert.ok(Buffer.byteLength(stdout, "utf8") <= 16 * 1024);

  const spawn: WatchdogSpawn = async () => ({
    exitCode: 0,
    stdout,
    stderr: "",
  });
  const classifier = new ClaudeWatchdogClassifier({ spawn });
  const result = await classifier.classify(request(), new AbortController().signal);

  assert.equal(result.verdict, "uncertain");
  assert.equal(result.reason_code, "invalid_classifier_output");
});

// I1: a schema-valid maximal healthy verdict whose evidence is non-ASCII
// (CJK, 3 bytes per character) must still be accepted as healthy. The
// per-item character bound (192) is enforced by the JSON Schema sent to
// Claude Code, but 192 CJK characters serialize to 576 bytes per item, so
// 8 such items alone exceed the 2 KiB verdict byte cap. Normalization
// truncates each evidence item to a UTF-8 byte bound so the serialized
// verdict always fits, and a healthy verdict stays healthy rather than
// being normalized to `uncertain` with `classifier_output_too_large` (which
// would wake the controller once per checkpoint for every non-English run).
//
test("accepts a maximal schema-valid healthy verdict with non-ASCII (CJK) evidence", async () => {
  // 192 CJK characters per item — schema-valid (maxLength: 192 characters)
  // but 576 bytes per item before truncation.
  //
  const cjkCharacter = "\u4e2d"; // "中" — 3 bytes in UTF-8
  const maximalStructuredOutput = {
    verdict: "healthy",
    confidence: 0.91,
    reason_code: "steady_progress",
    evidence: Array.from({ length: 8 }, () => cjkCharacter.repeat(192)),
  };
  const envelope = {
    type: "result",
    subtype: "success",
    structured_output: maximalStructuredOutput,
    result: JSON.stringify(maximalStructuredOutput),
    session_id: "session-" + "s".repeat(40),
    usage: { input_tokens: 120, output_tokens: 30 },
    total_cost_usd: 0.0005,
  };
  const stdout = JSON.stringify(envelope);
  // The raw structured output alone (8 × 576 bytes + envelope) exceeds the
  // 2 KiB verdict byte cap before per-item byte truncation is applied.
  //
  assert.ok(
    Buffer.byteLength(JSON.stringify(maximalStructuredOutput), "utf8") > 2 * 1024,
    "raw CJK maximal structured output must exceed the 2 KiB byte cap before truncation",
  );

  const spawn: WatchdogSpawn = async () => ({
    exitCode: 0,
    stdout,
    stderr: "",
  });
  const classifier = new ClaudeWatchdogClassifier({ spawn });
  const result = await classifier.classify(request(), new AbortController().signal);

  assert.equal(result.verdict, "healthy");
  assert.equal(result.reason_code, "steady_progress");
  assert.equal(result.evidence.length, 8);
  // Each evidence item is truncated to the per-item UTF-8 byte bound.
  //
  for (const item of result.evidence) {
    assert.ok(
      Buffer.byteLength(item, "utf8") <= 192,
      `evidence item must be truncated to 192 bytes, found ${Buffer.byteLength(item, "utf8")} bytes`,
    );
  }
  assert.ok(
    (result as { output_bytes?: number }).output_bytes! <= 2 * 1024,
    "normalized verdict bytes must fit within the 2 KiB output bound",
  );
});

// C1 regression: spawnWatchdogProcess deliberately SIGKILLs the child from
// three paths (timeout, abort, outputTooLarge) while a stdin write may
// still be buffered. When the child dies mid-write, Node emits `error` on
// the Socket — not on the ChildProcess — so the `child.once("error", …)`
// handler does not cover it. Without a stdin error listener the unhandled
// stream error crashes the entire xagent service. These tests prove the
// stdin error guard swallows the EPIPE and the spawn still resolves with a
// proper failure/uncertain outcome.
//
test("outputTooLarge SIGKILL during a buffered stdin write does not crash the spawn", async () => {
  // Child writes a large stdout burst (triggering outputTooLarge →
  // SIGKILL) while never reading stdin (keeping the 64 KiB stdin write
  // buffered). This is the exact window the review flagged.
  //
  const result = await spawnWatchdogProcess({
    command: process.execPath,
    args: ["-e", "process.stdout.write('x'.repeat(64 * 1024))"],
    cwd: process.cwd(),
    input: "x".repeat(64 * 1024),
    timeoutMs: 5_000,
    outputLimitBytes: 64,
    signal: new AbortController().signal,
  });

  assert.equal(result.outputTooLarge, true);
  assert.ok(Buffer.byteLength(result.stdout, "utf8") <= 65);
});

test("abort SIGKILL during a buffered stdin write does not crash the spawn", async () => {
  // Child never reads stdin and never exits on its own, so the 64 KiB
  // stdin write stays buffered. Aborting the request SIGKILLs the child
  // mid-write; the spawn must still resolve with `aborted: true` rather
  // than throwing an unhandled EPIPE.
  //
  const abort = new AbortController();
  const resultPromise = spawnWatchdogProcess({
    command: process.execPath,
    args: ["-e", "setInterval(() => {}, 1000)"],
    cwd: process.cwd(),
    input: "x".repeat(64 * 1024),
    timeoutMs: 30_000,
    outputLimitBytes: 4 * 1024,
    signal: abort.signal,
  });
  // Let the spawn start and queue the buffered stdin write before aborting.
  await new Promise<void>((resolve) => setTimeout(resolve, 20));
  abort.abort();

  const result = await resultPromise;
  assert.equal(result.aborted, true);
});

test("timeout SIGKILL during a buffered stdin write does not crash the spawn", async () => {
  // Child never reads stdin and never exits; the tiny timeout fires
  // SIGKILL while the 64 KiB stdin write is still buffered.
  //
  const result = await spawnWatchdogProcess({
    command: process.execPath,
    args: ["-e", "setInterval(() => {}, 1000)"],
    cwd: process.cwd(),
    input: "x".repeat(64 * 1024),
    timeoutMs: 50,
    outputLimitBytes: 4 * 1024,
    signal: new AbortController().signal,
  });

  assert.equal(result.timedOut, true);
});

test("classifier swallows a stdin EPIPE mid-write and returns an uncertain verdict", async () => {
  // End-to-end proof at the classifier layer: a real spawn that is
  // SIGKILLed mid-write must not throw and must normalize to `uncertain`.
  //
  const classifier = new ClaudeWatchdogClassifier({
    spawn: spawnWatchdogProcess,
    timeoutMs: 50,
  });
  const verdict = await classifier.classify(
    {
      ...request(),
      // Force a large input so the stdin write is still buffered when the
      // timeout fires.
      //
    },
    new AbortController().signal,
  );

  assert.equal(verdict.verdict, "uncertain");
  assert.equal(verdict.reason_code, "classifier_timeout");
});

function request(): WatchdogRequest {
  const value = {
    original_prompt: "Implement the bounded watchdog.",
    harness: "claude_code" as const,
    recent_provider_json: [{
      type: "assistant",
      message: {
        content: [{ type: "text", text: "Adding tests." }],
      },
    }],
    elapsed_ms: 600_000,
    truncated: false,
  };
  return withWatchdogInputBytes(value);
}
