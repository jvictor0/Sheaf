import { z } from "zod";

import { harnessNames, outputModes, thinkingLevels } from "../events.js";

export const x_DefaultAwaitDeadlineSeconds = 7000;
export const x_MaxAwaitDeadlineSeconds = 7000;

export type StructuredToolError = {
  readonly error: string;
  readonly message: string;
  readonly details?: unknown;
};

export class ToolValidationError extends Error {
  readonly structured: StructuredToolError;

  constructor(structured: StructuredToolError) {
    super(structured.message);
    this.name = "ToolValidationError";
    this.structured = structured;
  }
}

export const CwdSchema = z.string().min(1).describe("Absolute path to an existing working directory");

const x_EmbeddedRunIdPattern = /xrun_[0-9]{17}_[0-9a-f]{8}/;

// Controller bookkeeping is not worker-actionable. A dispatched worker has no
// xagent access, so "keep session xrun_… open for re-review" reads as an
// instruction it must obey and cannot. Fail the dispatch rather than shipping
// the confusion into the prompt.
//
// Quoting a run id is legitimate when the subject matter IS xagent — a findings
// list citing a log line, a note naming the sibling run whose work is in the
// tree. Backticks mark it as a quotation rather than an instruction, so text
// inside them is exempt. Bare run ids in prose stay rejected.
//
function StripQuotedSpans(value: string): string {
  return value.replace(/`[^`]*`/g, "");
}

function WorkerFacingText(label: string) {
  return z.string().min(1).superRefine((value, ctx) => {
    const match = x_EmbeddedRunIdPattern.exec(StripQuotedSpans(value));
    if (match !== null) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message:
          `${label} must not contain a controller run id (found ${match[0]}); `
          + "a dispatched worker cannot act on xagent session ids. "
          + "If you are quoting it as subject matter, wrap it in backticks.",
      });
    }
  });
}

const WatchdogPolicySchema = z
  .object({
    inputLimitBytes: z.number().int().positive().optional(),
    outputLimitBytes: z.number().int().positive().optional(),
    cadenceMs: z.array(z.number().int().positive()).optional(),
    minimumIntervalMs: z.number().int().positive().optional(),
    maximumCalls: z.number().int().positive().optional(),
    confidenceFloor: z.number().min(0).max(1).optional(),
    timeoutMs: z.number().int().positive().optional(),
    maxBudgetUsd: z.number().positive().optional(),
  })
  .strict();

export const SupervisionPolicySchema = z
  .object({
    silenceTimeoutMs: z.number().int().positive(),
    hardDeadlineMs: z.number().int().positive().optional(),
    watchdog: WatchdogPolicySchema.default({}),
  })
  .strict();

export const XagentStartInputSchema = z
  .object({
    cwd: CwdSchema,
    prompt: WorkerFacingText("prompt"),
    harness: z.enum(harnessNames),
    mode: z.enum(outputModes).default("subagent"),
    model: z.string().min(1).optional(),
    thinking_level: z.enum(thinkingLevels).optional(),
    permission_mode: z.string().min(1).optional(),
    provider_thread_id: z.string().min(1).optional(),
    policy: SupervisionPolicySchema.optional(),
  })
  .strict();

// Agent-facing discovery (xsvc-5 / xsvc-15): no deadline. Choosing a timeout
// whose only correct value depends on transport behaviour the agent cannot
// observe is not the agent's job. Internal callers and tests still pass
// deadline_seconds through XagentAwaitInputSchema.
//
// `.passthrough()` is load-bearing. The MCP SDK validates call arguments
// against the registered (advertised) schema before the handler runs and
// would otherwise strip `deadline_seconds`. Passthrough keeps unknown keys
// for the handler's real parse, while tools/list still advertises only the
// shape properties below; discovery is a subset of the accepted input.
//
export const XagentAwaitAdvertisedSchema = z.object({
  run_id: z.string().min(1).describe("The supervised run to await."),
  after_sequence: z.number().int().min(0)
    .describe("Return the next durable wake after this cursor."),
}).passthrough();

export const XagentAwaitInputSchema = z
  .object({
    run_id: z.string().min(1),
    after_sequence: z.number().int().min(0),
    deadline_seconds: z
      .number()
      .int()
      .positive()
      .max(x_MaxAwaitDeadlineSeconds)
      .default(x_DefaultAwaitDeadlineSeconds),
  })
  .strict();

export const XagentInspectInputSchema = z
  .object({
    run_id: z.string().min(1),
  })
  .strict();

export const XagentListInputSchema = z
  .object({
    live_only: z.boolean().default(false),
    limit: z.number().int().positive().max(200).default(50),
  })
  .strict();

export const XagentMessageInputSchema = z
  .object({
    run_id: z.string().min(1),
    // Reaches a worker verbatim, so it is guarded like the initial prompt.
    //
    text: WorkerFacingText("text"),
  })
  .strict();

export const XagentInterruptInputSchema = z
  .object({
    run_id: z.string().min(1),
  })
  .strict();

export const XagentCloseInputSchema = z
  .object({
    run_id: z.string().min(1),
  })
  .strict();

export type XagentStartInput = z.infer<typeof XagentStartInputSchema>;
export type XagentAwaitInput = z.infer<typeof XagentAwaitInputSchema>;
export type XagentInspectInput = z.infer<typeof XagentInspectInputSchema>;
export type XagentListInput = z.infer<typeof XagentListInputSchema>;
export type XagentMessageInput = z.infer<typeof XagentMessageInputSchema>;
export type XagentInterruptInput = z.infer<typeof XagentInterruptInputSchema>;
export type XagentCloseInput = z.infer<typeof XagentCloseInputSchema>;

export function parseToolInput<S extends z.ZodTypeAny>(
  schema: S,
  raw: unknown,
): z.output<S> {
  const parsed = schema.safeParse(raw);
  if (parsed.success) {
    return parsed.data as z.output<S>;
  }
  throw new ToolValidationError({
    error: "invalid_tool_input",
    message: parsed.error.issues.map((issue) => issue.message).join("; "),
    details: parsed.error.flatten(),
  });
}

export function structuredErrorFromUnknown(error: unknown): StructuredToolError {
  if (error instanceof ToolValidationError) {
    return error.structured;
  }
  if (
    error !== null
    && typeof error === "object"
    && "error" in error
    && typeof (error as { error: unknown }).error === "string"
    && "message" in error
    && typeof (error as { message: unknown }).message === "string"
  ) {
    const structured = error as StructuredToolError;
    return {
      error: structured.error,
      message: structured.message,
      ...(structured.details === undefined ? {} : { details: structured.details }),
    };
  }
  return {
    error: "tool_failed",
    message: error instanceof Error ? error.message : String(error),
  };
}
