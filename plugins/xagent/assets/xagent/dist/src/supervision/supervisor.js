import { createHash } from "node:crypto";
import { sanitizeValue } from "../sanitize.js";
import { ClaudeWatchdogClassifier } from "./claude_watchdog.js";
import { ProviderJsonEvidenceWindow, validateProviderJsonEvidencePolicy, } from "./evidence.js";
import { SequencedEventQueue } from "./event_queue.js";
import { DeterministicHealthMonitor, } from "./health.js";
import { WatchdogScheduler } from "./watchdog.js";
const terminalPhases = new Set([
    "completed",
    "failed",
    "cancelled",
    "abandoned",
]);
export class Supervisor {
    #runId;
    #adapter;
    #startOptions;
    #policy;
    #clock;
    #metadataSink;
    #events;
    #health;
    #watchdogScheduler;
    #watchdogTelemetrySink;
    #providerTranscriptSink;
    #watchdog = {
        invocation_count: 0,
        controller_wake_count: 0,
        deterministic_alert_count: 0,
        evidence_truncation_count: 0,
    };
    #initialPersistence;
    #lifecycleTail = Promise.resolve();
    #phase = "starting";
    #providerThreadId;
    #session;
    #sessionClosePromise;
    #closePromise;
    #interruptedTurnId;
    #inputSequence = 0;
    #startRequested = false;
    #lastTransportProgressAt;
    #lastSemanticProgressAt;
    #evidence;
    #watchdogSuppressedByExposedWait = false;
    #watchdogSuppressedBySilence = false;
    #watchdogSuppressedByHardDeadline = false;
    #watchdogSuppressedByMechanicalTerminal = false;
    #healthCallbackFailure;
    #watchdogCallbackFailure;
    #callbackFailureSurfaced = false;
    constructor(options) {
        this.#runId = options.runId;
        this.#adapter = options.adapter;
        this.#startOptions = options.startOptions;
        this.#policy = options.policy;
        validateProviderJsonEvidencePolicy({
            ...(this.#policy.watchdog.inputLimitBytes === undefined
                ? {}
                : { maxInputBytes: this.#policy.watchdog.inputLimitBytes }),
        });
        this.#clock = options.clock ?? (() => new Date());
        this.#metadataSink = options.metadataSink ?? (async () => { });
        this.#watchdogTelemetrySink = options.watchdogTelemetrySink ?? (async () => { });
        this.#providerTranscriptSink = options.providerTranscriptSink ?? (async () => { });
        const timestamp = this.#clock().toISOString();
        this.#lastTransportProgressAt = timestamp;
        this.#lastSemanticProgressAt = timestamp;
        this.#events = new SequencedEventQueue(this.#runId, options.eventSink, this.#clock, options.scheduler);
        this.#health = new DeterministicHealthMonitor({
            silenceTimeoutMs: this.#policy.silenceTimeoutMs,
            ...(this.#policy.hardDeadlineMs === undefined
                ? {}
                : { hardDeadlineMs: this.#policy.hardDeadlineMs }),
            clock: this.#clock,
            ...(options.scheduler === undefined ? {} : { scheduler: options.scheduler }),
            onClassification: (classification) => {
                void this.#applyHealthClassification(classification).catch((error) => {
                    void this.#surfaceCallbackFailure(asError(error));
                });
            },
        });
        const classifier = options.watchdogClassifier ?? new ClaudeWatchdogClassifier({
            ...(this.#policy.watchdog.inputLimitBytes === undefined
                ? {}
                : { inputLimitBytes: this.#policy.watchdog.inputLimitBytes }),
            ...(this.#policy.watchdog.outputLimitBytes === undefined
                ? {}
                : { outputLimitBytes: this.#policy.watchdog.outputLimitBytes }),
            ...(this.#policy.watchdog.timeoutMs === undefined
                ? {}
                : { timeoutMs: this.#policy.watchdog.timeoutMs }),
            ...(this.#policy.watchdog.maxBudgetUsd === undefined
                ? {}
                : { maxBudgetUsd: this.#policy.watchdog.maxBudgetUsd }),
            ...(this.#policy.watchdog.confidenceFloor === undefined
                ? {}
                : { confidenceFloor: this.#policy.watchdog.confidenceFloor }),
        });
        this.#watchdogScheduler = new WatchdogScheduler({
            classifier,
            policy: this.#policy.watchdog,
            clock: this.#clock,
            ...(options.scheduler === undefined ? {} : { scheduler: options.scheduler }),
            onVerdict: (request, verdict, callCount, currentTurn) => this.#recordWatchdogVerdict(request, verdict, callCount, currentTurn),
        });
        this.#initialPersistence = this.#publishState("starting", "supervisor_created", false);
    }
    inspect() {
        const callbackFailure = this.#healthCallbackFailure ?? this.#watchdogCallbackFailure;
        return {
            run_id: this.#runId,
            phase: this.#phase,
            sequence: Math.max(1, this.#events.sequence),
            provider_thread_id: this.#providerThreadId,
            ...(callbackFailure === undefined
                ? {}
                : {
                    callback_failure: {
                        source: this.#healthCallbackFailure !== undefined
                            ? "health_callback"
                            : "watchdog_callback",
                        message: callbackFailure.message,
                    },
                }),
        };
    }
    // Vouching is derived from supervisor liveness, never a bare interval: the
    // phase must still be live and transport progress must fall inside the
    // silence bound. An await liveness ping that ignores this vouches for
    // nothing and hides death.
    //
    isVouching() {
        if (terminalPhases.has(this.#phase)) {
            return false;
        }
        const lastProgressMs = Date.parse(this.#lastTransportProgressAt);
        if (!Number.isFinite(lastProgressMs)) {
            return false;
        }
        return this.#clock().getTime() - lastProgressMs < this.#policy.silenceTimeoutMs;
    }
    evidenceSnapshot() {
        return this.#evidence?.snapshot();
    }
    start() {
        return this.#withLifecycleMutation(async () => {
            if (terminalPhases.has(this.#phase)) {
                throw invalidPhase("start", this.#phase);
            }
            if (this.#startRequested) {
                throw new Error("Supervisor start has already been requested.");
            }
            if (this.#phase !== "starting") {
                throw invalidPhase("start", this.#phase);
            }
            this.#startRequested = true;
            await this.#initialPersistence;
            try {
                this.#session = await this.#adapter.start(this.#startOptions);
            }
            catch (error) {
                await this.#publishState("failed", errorCode(error), true);
                throw error;
            }
            try {
                await this.#publishState("ready", "session_ready", true);
            }
            catch (error) {
                try {
                    await this.#closeSessionOnce();
                }
                catch {
                    // Preserve the persistence failure that prevented the ready commit.
                }
                throw error;
            }
        });
    }
    async submit(text) {
        // Empty until the in-lock increment assigns the real id. Deliberately not
        // a speculative `turn_${#inputSequence + 1}`: that reads the counter
        // outside the mutation gate, and a plausible-looking but wrong id is a
        // footgun if the rescue condition below is ever widened.
        //
        let turnId = "";
        let turnStarted = false;
        let turn;
        try {
            turn = await this.#withLifecycleMutation(async () => {
                if (this.#phase !== "ready" || this.#session === undefined) {
                    throw invalidPhase("submit", this.#phase);
                }
                this.#inputSequence += 1;
                const inputSequence = this.#inputSequence;
                turnId = `turn_${inputSequence}`;
                await this.#publishState("running", "turn_started", false);
                // Only after turn_started's sink+commit succeeds is phase durable
                // running; a later turn.submitted sink failure must be rescued.
                //
                turnStarted = true;
                await this.#publishSubmitted(turnId, text);
                this.#evidence = new ProviderJsonEvidenceWindow({
                    harness: this.#adapter.harness,
                    originalPrompt: text,
                    clock: this.#clock,
                    ...(this.#policy.watchdog.inputLimitBytes === undefined
                        ? {}
                        : { maxInputBytes: this.#policy.watchdog.inputLimitBytes }),
                });
                this.#watchdogScheduler.resetTurn();
                this.#watchdogSuppressedByExposedWait = false;
                this.#watchdogSuppressedBySilence = false;
                this.#watchdogSuppressedByHardDeadline = false;
                this.#watchdogSuppressedByMechanicalTerminal = false;
                this.#health.recordMechanicalEvent({ type: "provider.started" });
                return { inputSequence, turnId, session: this.#session };
            });
        }
        catch (error) {
            // Rescue outside the lock: rescuePublishFailure re-enters the mutation
            // gate. Skip when turn_started never committed (phase still ready /
            // concurrent invalidPhase) so we preserve the pre-existing retry path.
            //
            if (turnStarted) {
                await this.#rescuePublishFailure(error, turnId);
            }
            throw error;
        }
        try {
            let lastAssistantText;
            let completed;
            let terminalFailure;
            try {
                const providerEvents = turn.session.submit({
                    text,
                    turnId: turn.turnId,
                    inputSequence: turn.inputSequence,
                });
                try {
                    await this.#persistActiveProcessIdentity(turn.session);
                }
                catch (error) {
                    await closeUnconsumedProviderTurn(providerEvents, turn.session);
                    throw error;
                }
                for await (const event of providerEvents) {
                    if (terminalPhases.has(this.#phase)) {
                        return;
                    }
                    this.#recordProgress(event);
                    // ProcessJsonlSession yields exactly one `raw.provider` event per
                    // provider line, so that is the whole transcript; the `rawProvider`
                    // side-channel on other events is a duplicate of the same object.
                    //
                    // The transcript is a diagnostic artifact: a failure to persist it
                    // must never fail a healthy turn. Awaiting inside this loop with the
                    // error escaping would surface a full disk as `transport.lost` and
                    // kill a run that was making progress.
                    //
                    if (event.type === "raw.provider") {
                        try {
                            await this.#providerTranscriptSink(sanitizeValue(event.payload, this.#startOptions.cwd));
                        }
                        catch {
                            // Diagnostics only — drop the line rather than the run.
                        }
                    }
                    const mechanical = mechanicalEventClassification(this.#health, event);
                    this.#recordMechanicalWatchdogSuppression(mechanical);
                    if (mechanical?.kind === "attention") {
                        await this.#applyHealthClassification(mechanical);
                    }
                    if (mechanical?.kind === "failure" && event.type !== "turn.failed") {
                        terminalFailure = {
                            reason: mechanical.reason,
                            payload: sanitizeValue(mechanical.payload, this.#startOptions.cwd),
                        };
                        break;
                    }
                    if (event.type === "message.completed" && event.role === "assistant") {
                        lastAssistantText = event.text;
                    }
                    if (event.type === "turn.failed") {
                        terminalFailure = {
                            reason: event.code,
                            // Provider error text is untrusted content: it can echo an API
                            // key or an absolute worktree path. Every other terminal-failure
                            // path here sanitizes; this one must too. `details` carries the
                            // adapter's structured cause (terminal_reason, subtype).
                            //
                            payload: sanitizeValue({
                                message: event.message,
                                turn_id: turn.turnId,
                                ...(event.details === undefined ? {} : { details: event.details }),
                            }, this.#startOptions.cwd),
                        };
                        break;
                    }
                    if (event.type === "turn.completed") {
                        completed = event;
                    }
                }
            }
            catch (error) {
                if (terminalPhases.has(this.#phase)) {
                    return;
                }
                if (errorCode(error) === "harness_process_interrupted"
                    && this.#interruptedTurnId === turn.turnId) {
                    await this.#withLifecycleMutation(async () => {
                        if (terminalPhases.has(this.#phase)) {
                            return;
                        }
                        await this.#publishState("ready", "turn_interrupted", true, {
                            turn_id: turn.turnId,
                        });
                        this.#interruptedTurnId = undefined;
                    });
                    return;
                }
                const transportFailure = this.#health.recordMechanicalEvent({
                    type: "transport.lost",
                    message: error instanceof Error ? error.message : String(error),
                });
                await this.#withLifecycleMutation(async () => {
                    if (terminalPhases.has(this.#phase)) {
                        return;
                    }
                    await this.#publishState("failed", transportFailure?.reason ?? errorCode(error), true, sanitizeValue({
                        message: error instanceof Error ? error.message : String(error),
                        turn_id: turn.turnId,
                    }, this.#startOptions.cwd));
                });
                return;
            }
            if (terminalFailure !== undefined) {
                await this.#withLifecycleMutation(async () => {
                    if (terminalPhases.has(this.#phase)) {
                        return;
                    }
                    await this.#publishState("failed", terminalFailure.reason, true, terminalFailure.payload);
                });
                return;
            }
            if (terminalPhases.has(this.#phase)) {
                return;
            }
            const reportText = completed?.final_text || lastAssistantText;
            if (completed === undefined || reportText === undefined) {
                await this.#withLifecycleMutation(async () => {
                    if (terminalPhases.has(this.#phase)) {
                        return;
                    }
                    this.#health.recordMechanicalEvent({
                        type: "provider.failed",
                        code: "missing_final_report",
                        message: "Provider turn ended without a final report.",
                    });
                    await this.#publishState("failed", "missing_final_report", true, {
                        turn_id: turn.turnId,
                    });
                });
                return;
            }
            const sanitizedReport = sanitizeValue({ text: reportText }, this.#startOptions.cwd);
            try {
                await this.#withLifecycleMutation(async () => {
                    if (terminalPhases.has(this.#phase)) {
                        return;
                    }
                    await this.#publishEvent({
                        type: "turn.completed",
                        phase: "ready",
                        reason: "turn_completed",
                        payload: {
                            report: sanitizedReport,
                            turn_id: turn.turnId,
                            provider_thread_id: completed.provider_thread_id ?? turn.session.providerThreadId,
                            ...(completed.usage === undefined ? {} : { usage: completed.usage }),
                        },
                    }, true);
                });
            }
            catch (error) {
                // A mid-run persistence failure must not leave the run in a non-terminal
                // phase. Attempt to publish a terminal failed state so awaiters see a
                // durable failure instead of waiting silently. If the failed-state
                // publish also fails (sink still broken), the original error is still
                // re-thrown so the caller observes the persistence failure.
                await this.#rescuePublishFailure(error, turn.turnId);
                throw error;
            }
        }
        finally {
            await this.#settleWatchdog();
        }
    }
    async awaitEvent(afterSequence, deadlineMs, signal) {
        // A live run in a terminal phase (failed/cancelled/completed/abandoned)
        // retains its in-memory supervisor until `close()` removes it from the
        // run manager. Without this short-circuit, a controller that already
        // consumed the terminal event and re-awaits from that cursor would block
        // in the event queue for the full deadline — the same asymmetry the
        // persisted path fixed in r6. Mirror that fix: when the phase is terminal
        // and no deliverable event exists after the cursor, return a
        // distinguishable `run_terminal` deadline so the quiet client stops
        // promptly instead of looping on a synthetic `await_deadline`.
        //
        if (terminalPhases.has(this.#phase)) {
            const peeked = await this.#events.peekDeliverable(afterSequence);
            if (peeked !== undefined) {
                return peeked;
            }
            return {
                schema_version: 1,
                type: "supervision.deadline",
                run_id: this.#runId,
                sequence: afterSequence,
                timestamp: this.#clock().toISOString(),
                phase: this.#phase,
                reason: "run_terminal",
            };
        }
        const event = await this.#events.awaitEvent(afterSequence, deadlineMs, signal);
        if (event !== undefined) {
            return event;
        }
        return {
            schema_version: 1,
            type: "supervision.deadline",
            run_id: this.#runId,
            sequence: afterSequence,
            timestamp: this.#clock().toISOString(),
            phase: this.#phase,
            reason: "await_deadline",
        };
    }
    publishAttention(reason, payload) {
        return this.#withLifecycleMutation(async () => {
            if (terminalPhases.has(this.#phase)) {
                throw invalidPhase("publish attention", this.#phase);
            }
            return this.#publishEvent({
                type: "supervision.attention",
                phase: this.#phase,
                reason,
                ...(payload === undefined ? {} : { payload }),
            }, true);
        });
    }
    interrupt() {
        return this.#withLifecycleMutation(async () => {
            if (this.#phase !== "running" || this.#session === undefined) {
                throw invalidPhase("interrupt", this.#phase);
            }
            if (this.#session.interrupt === undefined) {
                throw new Error("Harness session does not support interrupt.");
            }
            this.#interruptedTurnId = `turn_${this.#inputSequence}`;
            try {
                await this.#session.interrupt();
            }
            catch (error) {
                this.#interruptedTurnId = undefined;
                throw error;
            }
        });
    }
    close() {
        if (this.#closePromise === undefined) {
            const closeSession = this.#withLifecycleMutation(async () => {
                await this.#initialPersistence;
                if (terminalPhases.has(this.#phase)) {
                    await this.#closeSessionOnce();
                    return;
                }
                const classification = this.#health.recordMechanicalEvent({ type: "cancelled" });
                if (classification?.kind === "cancellation") {
                    this.#watchdogSuppressedByMechanicalTerminal = true;
                }
                const closingPhase = this.#phase;
                await this.#closeSessionOnce();
                if (classification?.kind === "cancellation" && closingPhase === "running") {
                    await this.#publishState("cancelled", classification.reason, true);
                    return;
                }
                await this.#publishState("completed", "session_closed", true);
            });
            this.#closePromise = closeSession.then(() => this.#settleWatchdog());
        }
        return this.#closePromise;
    }
    async #publishState(phase, reason, deliverable, payload) {
        await this.#publishEvent({
            type: "supervision.state",
            phase,
            reason,
            ...(payload === undefined ? {} : { payload }),
        }, deliverable);
    }
    // The full submitted text — rendered prompt plus any appended controller
    // note, or a raw xagent_message — recorded before the provider adapter is
    // handed the text. Published non-deliverable so it never completes a live
    // await. If this append fails, submit() rejects and the text is never sent:
    // text the log cannot prove was sent is not sent.
    //
    async #publishSubmitted(turnId, text) {
        await this.#publishEvent({
            type: "turn.submitted",
            phase: "running",
            reason: "turn_submitted",
            payload: sanitizeValue({ text, turn_id: turnId }, this.#startOptions.cwd),
        }, false);
    }
    async #publishEvent(body, deliverable, options = {}) {
        this.#assertTransition(body.phase);
        const baseWatchdog = options.watchdog ?? this.#watchdog;
        const nextWatchdog = {
            ...baseWatchdog,
            controller_wake_count: baseWatchdog.controller_wake_count + (deliverable ? 1 : 0),
            deterministic_alert_count: baseWatchdog.deterministic_alert_count
                + (body.type === "supervision.attention"
                    && options.semanticAttention !== true
                    ? 1
                    : 0),
        };
        return this.#events.publish(body, deliverable, async (event) => {
            const providerThreadId = this.#session?.providerThreadId;
            await this.#metadataSink(this.#persistenceState(body.phase, event.sequence, providerThreadId, nextWatchdog));
            await options.commit?.(event);
            this.#phase = body.phase;
            this.#providerThreadId = providerThreadId;
            Object.assign(this.#watchdog, nextWatchdog);
        });
    }
    #assertTransition(next) {
        if (terminalPhases.has(this.#phase) && next !== this.#phase) {
            throw new Error(`Cannot transition terminal supervision phase ${this.#phase} to ${next}.`);
        }
        const allowed = {
            starting: ["starting", "ready", "failed", "completed"],
            ready: ["ready", "running", "completed", "failed"],
            running: ["running", "ready", "completed", "failed", "cancelled"],
            completed: ["completed"],
            failed: ["failed"],
            cancelled: ["cancelled"],
            abandoned: ["abandoned"],
        };
        if (!allowed[this.#phase].includes(next)) {
            throw new Error(`Invalid supervision phase transition ${this.#phase} -> ${next}.`);
        }
    }
    #recordProgress(event) {
        const activeSemanticEvidence = isActiveSemanticEvidence(event);
        this.#health.recordProviderActivity(activeSemanticEvidence ? "semantic" : "transport");
        if (event.type === "raw.provider") {
            this.#watchdogSuppressedBySilence = false;
        }
        if (activeSemanticEvidence) {
            this.#watchdogSuppressedByExposedWait = false;
        }
        this.#lastTransportProgressAt = this.#health.lastTransportActivityAt;
        this.#lastSemanticProgressAt = this.#health.lastSemanticActivityAt;
        if (event.type === "raw.provider"
            && this.#evidence !== undefined
            && !this.#isWatchdogMechanicallySuppressed()) {
            this.#evidence.record(event.payload);
            // Pass a thunk so the evidence snapshot is only constructed when the
            // scheduler is actually eligible to invoke the classifier. Raw provider
            // lines can be frequent, so the thunk defers `snapshot()` until after the
            // cheap scheduler eligibility checks.
            //
            void this.#watchdogScheduler
                .onActiveEvidenceThunk(() => this.#evidence.snapshot())
                .catch((error) => {
                this.#watchdogCallbackFailure = asError(error);
            });
        }
        else if (event.type === "raw.provider") {
            this.#evidence?.record(event.payload);
        }
    }
    #isWatchdogMechanicallySuppressed() {
        return this.#watchdogSuppressedByExposedWait
            || this.#watchdogSuppressedBySilence
            || this.#watchdogSuppressedByHardDeadline
            || this.#watchdogSuppressedByMechanicalTerminal;
    }
    #recordMechanicalWatchdogSuppression(classification) {
        if (classification === undefined) {
            return;
        }
        if (classification.kind === "completion"
            || classification.kind === "failure"
            || classification.kind === "cancellation") {
            this.#watchdogSuppressedByMechanicalTerminal = true;
            return;
        }
        if (classification.reason === "silence_timeout") {
            this.#watchdogSuppressedBySilence = true;
        }
    }
    #recordWatchdogVerdict(request, verdict, callCount, currentTurn) {
        return this.#withLifecycleMutation(async () => {
            const nextWatchdog = aggregateWatchdog(this.#watchdog, request, verdict, this.#watchdogScheduler.coverageExhausted);
            const telemetryBase = {
                schema_version: 1,
                timestamp: this.#clock().toISOString(),
                request_hash: createHash("sha256")
                    .update(JSON.stringify(request), "utf8")
                    .digest("hex"),
                input_bytes: request.input_bytes,
                output_bytes: verdict.output_bytes ?? 0,
                verdict: verdict.verdict,
                confidence: verdict.confidence,
                reason_code: verdict.reason_code,
                call_count: callCount,
                truncated: request.truncated,
                elapsed_ms: request.elapsed_ms,
                ...(verdict.usage === undefined ? {} : { usage: verdict.usage }),
                ...(verdict.estimated_cost_usd === undefined
                    ? {}
                    : { estimated_cost_usd: verdict.estimated_cost_usd }),
            };
            if (terminalPhases.has(this.#phase)
                || !currentTurn
                || verdict.verdict === "healthy") {
                await this.#metadataSink(this.#persistenceState(this.#phase, this.#events.sequence, this.#session?.providerThreadId, nextWatchdog));
                await this.#watchdogTelemetrySink(telemetryBase);
                Object.assign(this.#watchdog, nextWatchdog);
                return;
            }
            await this.#publishEvent({
                type: "supervision.attention",
                phase: this.#phase,
                reason: `watchdog_${verdict.verdict}`,
                payload: sanitizeValue({
                    advisory: true,
                    worker_continues: true,
                    message: "The watchdog noticed signals that may merit review. No action was taken and the worker is still running. Please check when convenient.",
                    verdict: verdict.verdict,
                    confidence: verdict.confidence,
                    reason_code: verdict.reason_code,
                    evidence: verdict.evidence,
                }, this.#startOptions.cwd),
            }, true, {
                watchdog: nextWatchdog,
                semanticAttention: true,
                commit: async (event) => {
                    await this.#watchdogTelemetrySink({
                        ...telemetryBase,
                        attention_sequence: event.sequence,
                    });
                },
            });
        });
    }
    #applyHealthClassification(classification) {
        if (classification.kind !== "attention") {
            return Promise.resolve();
        }
        if (classification.reason === "input_required"
            || classification.reason === "permission_required") {
            this.#watchdogSuppressedByExposedWait = true;
        }
        if (classification.reason === "silence_timeout") {
            this.#watchdogSuppressedBySilence = true;
        }
        if (classification.reason === "hard_deadline") {
            this.#watchdogSuppressedByHardDeadline = true;
        }
        return this.publishAttention(classification.reason, sanitizeValue(classification.payload, this.#startOptions.cwd)).then(() => { });
    }
    #persistenceState(phase, sequence, providerThreadId, watchdog) {
        return {
            run_id: this.#runId,
            phase,
            sequence,
            provider_thread_id: providerThreadId,
            last_transport_progress_at: this.#lastTransportProgressAt,
            last_semantic_progress_at: this.#lastSemanticProgressAt,
            owned_process: this.#session?.processIdentity,
            watchdog: { ...watchdog },
        };
    }
    #persistActiveProcessIdentity(session) {
        if (session.processIdentity === undefined) {
            return Promise.resolve();
        }
        return this.#withLifecycleMutation(async () => {
            if (this.#phase !== "running" || this.#session !== session) {
                return;
            }
            await this.#metadataSink(this.#persistenceState(this.#phase, this.#events.sequence, session.providerThreadId, this.#watchdog));
        });
    }
    #withLifecycleMutation(operation) {
        const result = this.#lifecycleTail.then(operation);
        this.#lifecycleTail = result.then(() => { }, () => { });
        return result;
    }
    async #settleWatchdog() {
        try {
            await this.#watchdogScheduler.settle();
        }
        catch (error) {
            // Watchdog settle failures (e.g. telemetry sink errors) are secondary:
            // expose them via inspect without transitioning the run to failed, so
            // a successful submit is not rejected by a telemetry-only failure.
            this.#watchdogCallbackFailure = asError(error);
        }
    }
    async #surfaceCallbackFailure(error) {
        // Surface a health-callback failure (timer-driven silence/hard-deadline
        // attention publish) so the controller is not left waiting silently for
        // an attention that will never arrive.
        if (this.#healthCallbackFailure !== undefined) {
            return;
        }
        this.#healthCallbackFailure = error;
        if (this.#callbackFailureSurfaced) {
            return;
        }
        this.#callbackFailureSurfaced = true;
        try {
            await this.#withLifecycleMutation(async () => {
                if (terminalPhases.has(this.#phase)) {
                    return;
                }
                await this.#publishState("failed", "health_callback_failed", true, {
                    message: error.message,
                    source: "health_callback",
                });
            });
        }
        catch {
            // The terminal publish itself failed; the failure remains visible via
            // inspect() so a controller polling is not left waiting silently.
        }
    }
    async #rescuePublishFailure(error, turnId) {
        // Best-effort transition to a terminal failed state when a mid-run
        // persistence failure leaves the phase non-terminal. The original error is
        // re-thrown by the caller regardless of whether this rescue succeeds.
        try {
            await this.#withLifecycleMutation(async () => {
                if (terminalPhases.has(this.#phase)) {
                    return;
                }
                await this.#publishState("failed", "event_persistence_failed", true, {
                    message: error instanceof Error ? error.message : String(error),
                    turn_id: turnId,
                });
            });
        }
        catch {
            // The failed-state publish also failed; nothing more we can do here. The
            // caller re-throws the original error so the submit rejection is visible.
        }
    }
    #closeSessionOnce() {
        if (this.#session === undefined) {
            return Promise.resolve();
        }
        if (this.#sessionClosePromise === undefined) {
            this.#sessionClosePromise = this.#session.close();
        }
        return this.#sessionClosePromise;
    }
}
function invalidPhase(operation, phase) {
    return new Error(`Cannot ${operation} while supervision phase is ${phase}.`);
}
async function closeUnconsumedProviderTurn(events, session) {
    const iterator = events[Symbol.asyncIterator]();
    const cleanup = [];
    if (session.processIdentity !== undefined && session.interrupt !== undefined) {
        cleanup.push(session.interrupt());
    }
    if (iterator.return !== undefined) {
        cleanup.push(iterator.return());
    }
    await Promise.allSettled(cleanup);
}
function errorCode(error) {
    if (typeof error === "object"
        && error !== null
        && "code" in error
        && typeof error.code === "string") {
        return error.code;
    }
    return "supervisor_failed";
}
function mechanicalEventClassification(monitor, event) {
    if (event.type === "turn.completed") {
        return monitor.recordMechanicalEvent({ type: "provider.completed" });
    }
    if (event.type === "turn.failed") {
        return monitor.recordMechanicalEvent({
            type: "provider.failed",
            code: event.code,
            message: event.message,
        });
    }
    if (event.type !== "status" && event.type !== "error") {
        return undefined;
    }
    if (event.code === "input_required") {
        return monitor.recordMechanicalEvent({
            type: "input.required",
            prompt: event.message,
        });
    }
    if (event.code === "permission_required") {
        return monitor.recordMechanicalEvent({
            type: "permission.required",
            permission: permissionFromDetails(event.details),
        });
    }
    if (event.code === "transport_lost" || event.code === "transport_loss") {
        return monitor.recordMechanicalEvent({
            type: "transport.lost",
            message: event.message,
        });
    }
    if (event.code === "process_exit") {
        const exitStatus = processExitFromDetails(event.details);
        return monitor.recordMechanicalEvent({
            type: "process.exited",
            exitCode: exitStatus.exitCode,
            signal: exitStatus.signal,
            // Without this the controller's only diagnosis for a dead provider was
            // `exit_code: 1`. The message is sanitized downstream with the rest of
            // the failure payload.
            //
            ...(event.message === undefined ? {} : { message: event.message }),
        });
    }
    return undefined;
}
function isActiveSemanticEvidence(event) {
    return event.type === "message.delta"
        || event.type === "message.completed"
        || event.type === "tool.started"
        || event.type === "tool.completed";
}
function aggregateWatchdog(current, request, verdict, coverageExhausted) {
    const inputTokens = sumOptional(current.input_tokens, verdict.usage?.input_tokens);
    const outputTokens = sumOptional(current.output_tokens, verdict.usage?.output_tokens);
    const estimatedCost = sumOptional(current.estimated_cost_usd, verdict.estimated_cost_usd);
    return {
        ...current,
        invocation_count: current.invocation_count + 1,
        evidence_truncation_count: current.evidence_truncation_count
            + (request.truncated ? 1 : 0),
        last_verdict: verdict.verdict,
        ...(coverageExhausted ? { coverage_exhausted: true } : {}),
        ...(inputTokens === undefined ? {} : { input_tokens: inputTokens }),
        ...(outputTokens === undefined ? {} : { output_tokens: outputTokens }),
        ...(estimatedCost === undefined ? {} : { estimated_cost_usd: estimatedCost }),
    };
}
function sumOptional(left, right) {
    return left === undefined && right === undefined ? undefined : (left ?? 0) + (right ?? 0);
}
function permissionFromDetails(details) {
    if (typeof details === "object"
        && details !== null
        && "permission" in details
        && typeof details.permission === "string") {
        return details.permission;
    }
    return undefined;
}
function processExitFromDetails(details) {
    if (typeof details !== "object" || details === null) {
        return { exitCode: null, signal: null };
    }
    const exitCode = "exit_code" in details
        && (typeof details.exit_code === "number" || details.exit_code === null)
        ? details.exit_code
        : null;
    const signal = "signal" in details
        && (typeof details.signal === "string" || details.signal === null)
        ? details.signal
        : null;
    return { exitCode, signal };
}
function asError(error) {
    return error instanceof Error ? error : new Error(String(error));
}
//# sourceMappingURL=supervisor.js.map