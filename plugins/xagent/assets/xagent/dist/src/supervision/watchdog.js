import { truncateUtf8 } from "../sanitize.js";
const DEFAULT_CADENCE_MS = [10 * 60_000, 20 * 60_000, 40 * 60_000];
const DEFAULT_MINIMUM_INTERVAL_MS = 5 * 60_000;
const DEFAULT_MAXIMUM_CALLS = 8;
const DEFAULT_CONFIDENCE_FLOOR = 0.8;
const DEFAULT_TIMEOUT_MS = 90_000;
const MAX_INPUT_BYTES = 64 * 1024;
const MAX_OUTPUT_BYTES = 2 * 1024;
const MAX_REASON_CODE_LENGTH = 128;
const MAX_EVIDENCE_ITEMS = 8;
// JSON Schema `maxLength` is character-based, so the schema sent to Claude
// Code bounds each evidence item at this many characters. The model can
// therefore produce up to MAX_EVIDENCE_ITEM_LENGTH characters per item.
//
const MAX_EVIDENCE_ITEM_LENGTH = 192;
// The classifier verdict output cap (MAX_OUTPUT_BYTES) is measured in UTF-8
// bytes. A schema-valid item at the character bound can still exceed the
// byte cap when it is non-ASCII (e.g. 192 CJK characters serialize to 576
// bytes, so 8 such items alone exceed the 2 KiB cap and force a healthy
// verdict into `classifier_output_too_large`). To keep the byte cap
// reachable for genuinely oversized output while accepting every
// schema-valid verdict, each evidence item is truncated to this byte limit
// after normalization. 8 items at this byte bound plus the verdict envelope
// fit comfortably under the 2 KiB output cap regardless of encoding.
//
const MAX_EVIDENCE_ITEM_BYTES = 192;
export class WatchdogScheduler {
    #classifier;
    #clock;
    #scheduler;
    #cadenceMs;
    #minimumIntervalMs;
    #maximumCalls;
    #confidenceFloor;
    #outputLimitBytes;
    #timeoutMs;
    #onVerdict;
    #turnStartedAt = 0;
    #nextPeriodicAt = 0;
    #cadenceIndex = 0;
    #lastInvocationAt;
    #callsUsed = 0;
    #inFlight;
    #turnGeneration = 0;
    constructor(options) {
        this.#classifier = options.classifier;
        this.#clock = options.clock ?? (() => new Date());
        this.#scheduler = options.scheduler ?? defaultScheduler;
        validateInvocationBounds(options.policy);
        this.#cadenceMs = validatedCadence(options.policy?.cadenceMs);
        this.#minimumIntervalMs = validatedPositiveInteger(options.policy?.minimumIntervalMs ?? DEFAULT_MINIMUM_INTERVAL_MS, "minimumIntervalMs");
        if (this.#minimumIntervalMs < DEFAULT_MINIMUM_INTERVAL_MS) {
            throw new Error(`minimumIntervalMs must be at least ${DEFAULT_MINIMUM_INTERVAL_MS} milliseconds.`);
        }
        this.#maximumCalls = validatedPositiveInteger(options.policy?.maximumCalls ?? DEFAULT_MAXIMUM_CALLS, "maximumCalls");
        if (this.#maximumCalls > DEFAULT_MAXIMUM_CALLS) {
            throw new Error(`maximumCalls cannot exceed ${DEFAULT_MAXIMUM_CALLS}.`);
        }
        this.#confidenceFloor = validatedConfidence(options.policy?.confidenceFloor ?? DEFAULT_CONFIDENCE_FLOOR);
        this.#outputLimitBytes = options.policy?.outputLimitBytes ?? MAX_OUTPUT_BYTES;
        this.#timeoutMs = options.policy?.timeoutMs ?? DEFAULT_TIMEOUT_MS;
        this.#onVerdict = options.onVerdict ?? (() => { });
        this.resetTurn();
    }
    get callsUsed() {
        return this.#callsUsed;
    }
    get coverageExhausted() {
        return this.#callsUsed >= this.#maximumCalls;
    }
    resetTurn() {
        this.#turnGeneration += 1;
        this.#turnStartedAt = this.#clock().getTime();
        this.#cadenceIndex = 0;
        this.#nextPeriodicAt = this.#turnStartedAt + this.#cadenceMs[0];
    }
    async settle() {
        const pending = this.#inFlight;
        if (pending !== undefined) {
            await pending;
        }
    }
    onActiveEvidence(request) {
        // Backwards-compatible eager overload: tests drive a pre-built request
        // through this seam. The supervisor uses `onActiveEvidenceThunk` so the
        // snapshot is only constructed when an invocation is actually eligible,
        // avoiding tens of `snapshot()` calls per second on the `message.delta`
        // hot path (review I2).
        //
        return this.onActiveEvidenceThunk(() => request);
    }
    onActiveEvidenceThunk(getRequest) {
        if (this.coverageExhausted || this.#inFlight !== undefined) {
            return this.#inFlight ?? Promise.resolve();
        }
        const now = this.#clock().getTime();
        const lastRelevantCheck = this.#lastInvocationAt ?? this.#turnStartedAt;
        if (now - lastRelevantCheck < this.#minimumIntervalMs || now < this.#nextPeriodicAt) {
            return Promise.resolve();
        }
        return this.#invokeWatchdog(getRequest(), now);
    }
    #invokeWatchdog(request, now) {
        this.#callsUsed += 1;
        const callCount = this.#callsUsed;
        this.#lastInvocationAt = now;
        const turnGeneration = this.#turnGeneration;
        this.#nextPeriodicAt = now + this.#cadenceMs[this.#cadenceIndex];
        const controller = new AbortController();
        const pending = (async () => {
            let verdict;
            try {
                const raw = await this.#classifyWithinDeadline(request, controller);
                verdict = withClassifierTelemetry(normalizeWatchdogVerdict(raw, this.#confidenceFloor), raw, this.#outputLimitBytes);
            }
            catch {
                verdict = uncertain("classifier_invocation_failed");
            }
            const currentTurn = turnGeneration === this.#turnGeneration;
            if (currentTurn) {
                this.#cadenceIndex = verdict.verdict === "healthy"
                    ? Math.min(this.#cadenceIndex + 1, this.#cadenceMs.length - 1)
                    : 0;
                this.#nextPeriodicAt = now + this.#cadenceMs[this.#cadenceIndex];
            }
            await this.#onVerdict(request, verdict, callCount, currentTurn);
        })();
        this.#inFlight = pending.finally(() => {
            this.#inFlight = undefined;
        });
        return this.#inFlight;
    }
    #classifyWithinDeadline(request, controller) {
        return new Promise((resolve, reject) => {
            let settled = false;
            const timer = this.#scheduler.setTimeout(() => {
                if (settled) {
                    return;
                }
                settled = true;
                controller.abort();
                resolve(uncertain("classifier_timeout"));
            }, this.#timeoutMs);
            void this.#classifier.classify(request, controller.signal).then((verdict) => {
                if (settled) {
                    return;
                }
                settled = true;
                this.#scheduler.clearTimeout(timer);
                resolve(verdict);
            }, (error) => {
                if (settled) {
                    return;
                }
                settled = true;
                this.#scheduler.clearTimeout(timer);
                reject(error);
            });
        });
    }
}
const defaultScheduler = {
    setTimeout(callback, delayMs) {
        return globalThis.setTimeout(callback, delayMs);
    },
    clearTimeout(handle) {
        globalThis.clearTimeout(handle);
    },
};
export function normalizeWatchdogVerdict(value, confidenceFloor = DEFAULT_CONFIDENCE_FLOOR) {
    if (!isRecord(value) || Object.keys(value).some((key) => ![
        "verdict",
        "confidence",
        "reason_code",
        "evidence",
        "usage",
        "estimated_cost_usd",
        "output_bytes",
    ].includes(key))) {
        return uncertain("invalid_classifier_output");
    }
    if (value.verdict !== "healthy"
        && value.verdict !== "derailed"
        && value.verdict !== "uncertain") {
        return uncertain("invalid_classifier_output");
    }
    if (typeof value.confidence !== "number"
        || !Number.isFinite(value.confidence)
        || value.confidence < 0
        || value.confidence > 1
        || typeof value.reason_code !== "string"
        || value.reason_code.length === 0
        || value.reason_code.length > MAX_REASON_CODE_LENGTH
        || !/^[a-z0-9_]+$/.test(value.reason_code)
        || !Array.isArray(value.evidence)
        || value.evidence.length > MAX_EVIDENCE_ITEMS
        || value.evidence.some((item) => typeof item !== "string" || item.length > MAX_EVIDENCE_ITEM_LENGTH)) {
        return uncertain("invalid_classifier_output");
    }
    const normalized = {
        verdict: value.verdict,
        confidence: value.confidence,
        reason_code: value.reason_code,
        evidence: value.evidence.map((item) => truncateUtf8(item, MAX_EVIDENCE_ITEM_BYTES)),
    };
    if (normalized.verdict === "healthy" && normalized.confidence < confidenceFloor) {
        return {
            ...normalized,
            verdict: "uncertain",
            reason_code: "healthy_below_confidence_floor",
        };
    }
    return normalized;
}
function withClassifierTelemetry(normalized, raw, outputLimitBytes) {
    const usage = normalizedUsage(raw.usage);
    const estimatedCost = finiteNonNegative(raw.estimated_cost_usd);
    const outputBytes = boundedNonNegativeInteger(raw.output_bytes, outputLimitBytes);
    return {
        ...normalized,
        ...(usage === undefined ? {} : { usage }),
        ...(estimatedCost === undefined ? {} : { estimated_cost_usd: estimatedCost }),
        ...(outputBytes === undefined ? {} : { output_bytes: outputBytes }),
    };
}
function normalizedUsage(value) {
    if (!isRecord(value)) {
        return undefined;
    }
    const inputTokens = nonNegativeSafeInteger(value.input_tokens);
    const outputTokens = nonNegativeSafeInteger(value.output_tokens);
    if (inputTokens === undefined && outputTokens === undefined) {
        return undefined;
    }
    return {
        ...(inputTokens === undefined ? {} : { input_tokens: inputTokens }),
        ...(outputTokens === undefined ? {} : { output_tokens: outputTokens }),
    };
}
function finiteNonNegative(value) {
    return typeof value === "number" && Number.isFinite(value) && value >= 0
        ? value
        : undefined;
}
function nonNegativeSafeInteger(value) {
    return typeof value === "number" && Number.isSafeInteger(value) && value >= 0
        ? value
        : undefined;
}
function boundedNonNegativeInteger(value, maximum) {
    const normalized = nonNegativeSafeInteger(value);
    return normalized === undefined ? undefined : Math.min(normalized, maximum + 1);
}
function validatedCadence(value) {
    const cadence = value ?? DEFAULT_CADENCE_MS;
    if (cadence.length === 0) {
        throw new Error("cadenceMs must contain at least one interval.");
    }
    return cadence.map((interval) => validatedPositiveInteger(interval, "cadenceMs"));
}
function validatedPositiveInteger(value, name) {
    if (!Number.isSafeInteger(value) || value <= 0) {
        throw new Error(`${name} must be a positive safe integer.`);
    }
    return value;
}
function validatedConfidence(value) {
    if (!Number.isFinite(value) || value < 0 || value > 1) {
        throw new Error("confidenceFloor must be between 0 and 1.");
    }
    return value;
}
function validateInvocationBounds(policy) {
    if (policy?.inputLimitBytes !== undefined
        && (!Number.isSafeInteger(policy.inputLimitBytes)
            || policy.inputLimitBytes <= 0
            || policy.inputLimitBytes > MAX_INPUT_BYTES)) {
        throw new Error(`inputLimitBytes cannot exceed ${MAX_INPUT_BYTES}.`);
    }
    if (policy?.outputLimitBytes !== undefined
        && (!Number.isSafeInteger(policy.outputLimitBytes)
            || policy.outputLimitBytes <= 0
            || policy.outputLimitBytes > MAX_OUTPUT_BYTES)) {
        throw new Error(`outputLimitBytes cannot exceed ${MAX_OUTPUT_BYTES}.`);
    }
    if (policy?.timeoutMs !== undefined
        && (!Number.isFinite(policy.timeoutMs) || policy.timeoutMs <= 0)) {
        throw new Error("timeoutMs must be a positive finite number.");
    }
    if (policy?.maxBudgetUsd !== undefined
        && (!Number.isFinite(policy.maxBudgetUsd) || policy.maxBudgetUsd <= 0)) {
        throw new Error("maxBudgetUsd must be a positive finite number.");
    }
}
function uncertain(reasonCode) {
    return {
        verdict: "uncertain",
        confidence: 0,
        reason_code: reasonCode,
        evidence: [],
    };
}
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
//# sourceMappingURL=watchdog.js.map