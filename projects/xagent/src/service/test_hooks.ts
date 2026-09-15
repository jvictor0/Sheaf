import { spawn, type ChildProcess } from "node:child_process";

import { FakeHarnessAdapter } from "../adapters/fake.js";
import { exitWithin } from "../adapters/process_jsonl.js";
import type { AdapterEvent, HarnessAdapter, HarnessSession, HarnessStartOptions } from "../adapters/types.js";
import type { HarnessName } from "../events.js";
import { captureOwnedProcessIdentity } from "../supervision/process_identity.js";

export function isTestAdapterEnabled(): boolean {
  return process.env.XAGENT_TEST_ADAPTER === "fake";
}

function ParseReportTexts(): readonly string[]
{
  const raw = process.env.XAGENT_TEST_REPORTS;
  if (raw === undefined || raw.trim().length === 0)
  {
    return ["complete final assistant message"];
  }
  const reports = raw.split("|").map((entry) => entry.trim()).filter((entry) => entry.length > 0);
  if (reports.length === 0)
  {
    return ["complete final assistant message"];
  }
  return reports;
}

function CreateScriptedTurn(
  reportText: string,
  delayMs: number | undefined,
  inputSequence: number,
): AsyncIterable<AdapterEvent>
{
  async function* scriptedTurn(): AsyncIterable<AdapterEvent>
  {
    if (delayMs !== undefined)
    {
      await new Promise<void>((resolve) =>
      {
        setTimeout(resolve, delayMs);
      });
    }
    yield {
      type: "message.delta",
      message_id: `message_test_hook_${inputSequence}`,
      role: "assistant",
      delta: "packaged test progress",
    };
    yield {
      type: "tool.started",
      tool_call_id: `tool_test_hook_${inputSequence}`,
      name: "fake_tool",
      input: {},
    };
    yield {
      type: "tool.completed",
      tool_call_id: `tool_test_hook_${inputSequence}`,
      name: "fake_tool",
      status: "completed",
      output: { ok: true },
    };
    yield {
      type: "message.completed",
      message_id: `message_test_hook_${inputSequence}`,
      role: "assistant",
      text: reportText,
    };
    yield {
      type: "turn.completed",
      final_text: reportText,
      provider_thread_id: "fake-thread-1",
    };
  }
  return scriptedTurn();
}

export function createTestAdapterFactory(): (harness: HarnessName) => HarnessAdapter {
  return () => {
    const delayMs = parsePositiveInteger(process.env.XAGENT_TEST_DELAY_MS);
    const spawnOwnedChild = process.env.XAGENT_TEST_OWNED_CHILD === "1";
    const reportTexts = ParseReportTexts();
    let ownedChild: ChildProcess | undefined;
    let processIdentity = undefined as ReturnType<typeof captureOwnedProcessIdentity>;

    if (spawnOwnedChild) {
      ownedChild = spawn(
        process.execPath,
        ["-e", "setInterval(() => {}, 1_000)"],
        { stdio: "ignore", detached: true },
      );
      if (typeof ownedChild.pid === "number") {
        processIdentity = captureOwnedProcessIdentity(ownedChild.pid);
      }
    }

    const scriptedEvents = reportTexts.map((reportText, index) =>
      CreateScriptedTurn(reportText, delayMs, index + 1),
    );

    const adapter = new FakeHarnessAdapter({
      scriptedEvents,
      includeDeltas: true,
      includeToolEvents: true,
      ...(processIdentity === undefined ? {} : { processIdentity }),
    });

    if (ownedChild === undefined) {
      return adapter;
    }

    return wrapAdapterWithOwnedChildCleanup(adapter, ownedChild);
  };
}

function wrapAdapterWithOwnedChildCleanup(
  adapter: FakeHarnessAdapter,
  ownedChild: ChildProcess,
): HarnessAdapter {
  return {
    harness: adapter.harness,
    capabilities: adapter.capabilities,
    async start(options: HarnessStartOptions): Promise<HarnessSession> {
      const session = await adapter.start(options);
      return wrapSessionWithOwnedChildCleanup(session, ownedChild);
    },
  };
}

export function wrapSessionWithOwnedChildCleanup(
  session: HarnessSession,
  ownedChild: ChildProcess,
): HarnessSession {
  let closed = false;
  return {
    providerThreadId: session.providerThreadId,
    processIdentity: session.processIdentity,
    ...(session.interrupt === undefined ? {} : { interrupt: session.interrupt }),
    submit: (context) => session.submit(context),
    close: async () => {
      if (closed) {
        return;
      }
      closed = true;
      await session.close();
      await terminateOwnedChild(ownedChild);
    },
  };
}

const OWNED_CHILD_TERMINATION_GRACE_MS = 2_000;

// `close()` must not resolve until the owned child is actually gone: callers
// (and the packaged cleanup smoke) inspect the pid the moment the close
// contract completes. Mirrors the bounded SIGTERM/SIGKILL escalation used for
// real provider processes in `process_jsonl.ts`.
//
async function terminateOwnedChild(child: ChildProcess): Promise<void> {
  if (child.pid === undefined) {
    return;
  }
  const watch = watchOwnedChild(child);
  try {
    signalOwnedChild(child, "SIGTERM");
    if (await exitWithin(watch.exited, OWNED_CHILD_TERMINATION_GRACE_MS)) {
      return;
    }

    signalOwnedChild(child, "SIGKILL");
    if (await exitWithin(watch.exited, OWNED_CHILD_TERMINATION_GRACE_MS)) {
      return;
    }

    throw new Error(
      `Owned test child did not exit after SIGTERM and SIGKILL.${watch.killFailureSuffix()}`,
    );
  } finally {
    watch.dispose();
  }
}

function signalOwnedChild(child: ChildProcess, signal: "SIGTERM" | "SIGKILL"): void {
  const pid = child.pid;
  if (pid === undefined) {
    return;
  }
  try {
    if (process.platform === "win32") {
      child.kill(signal);
      return;
    }
    process.kill(-pid, signal);
  } catch (error) {
    if (!isMissingProcess(error)) {
      throw error;
    }
  }
}

type OwnedChildWatch = {
  readonly exited: Promise<void>;
  killFailureSuffix(): string;
  dispose(): void;
};

// Only a real `exit` releases the wait. Node also emits `error` when a kill
// fails — notably on the Windows `child.kill()` path — but a failed kill is
// evidence the child may still be running, so swallowing it here (rather than
// resolving) keeps `close()` honest while preventing an unhandled `error` event
// from taking down the service. A child that never exits still fails loudly
// through the bounded SIGTERM/SIGKILL escalation, which reports the recorded
// kill failure.
//
function watchOwnedChild(child: ChildProcess): OwnedChildWatch {
  let killFailure: string | undefined;
  const onError = (error: Error) => {
    killFailure = error.message;
  };
  child.on("error", onError);

  let onExit: (() => void) | undefined;
  const exited = hasChildExited(child)
    ? Promise.resolve()
    : new Promise<void>((resolve) => {
      onExit = () => {
        resolve();
      };
      child.once("exit", onExit);
      // The child can exit between the check above and listener installation,
      // in which case `exit` has already been emitted and would never reach the
      // listener. Node records the exit status before emitting, so re-checking
      // here closes that race.
      //
      if (hasChildExited(child)) {
        resolve();
      }
    });

  return {
    exited,
    killFailureSuffix: () =>
      killFailure === undefined ? "" : ` Last kill error: ${killFailure}`,
    dispose: () => {
      if (onExit !== undefined) {
        child.off("exit", onExit);
      }
      // The `error` listener stays attached: a kill we already issued can still
      // fail asynchronously, and an unhandled `error` event would crash the
      // process.
      //
    },
  };
}

function hasChildExited(child: ChildProcess): boolean {
  return child.exitCode !== null || child.signalCode !== null;
}

function parsePositiveInteger(raw: string | undefined): number | undefined {
  if (raw === undefined || raw.trim().length === 0) {
    return undefined;
  }
  const parsed = Number(raw);
  if (!Number.isInteger(parsed) || parsed <= 0) {
    throw new Error(`Expected a positive integer test hook value, found ${raw}.`);
  }
  return parsed;
}

function isMissingProcess(error: unknown): boolean {
  return (
    typeof error === "object"
    && error !== null
    && "code" in error
    && (error as { code?: string }).code === "ESRCH"
  );
}
