#!/usr/bin/env node

import { loadXagentServiceConfig } from "./service/config.js";
import { createUncaughtExceptionHandler } from "./service/crash_handler.js";
import {
  logReconciliationResults,
  reconciliationWarning,
} from "./service/reconciliation.js";
import { XagentRunManager } from "./service/run_manager.js";
import { createTestAdapterFactory, isTestAdapterEnabled } from "./service/test_hooks.js";
import {
  createShutdownController,
  createXagentServer,
  type XagentServer,
  type XagentShutdownController,
} from "./service/server.js";
import { platformProcessInspector } from "./supervision/process_identity.js";
import { reconcileStaleRuns } from "./supervision/reconcile.js";

async function main(): Promise<void> {
  const config = await loadXagentServiceConfig();

  const runManager = new XagentRunManager({
    repoRoot: config.repoRoot,
    logRoot: config.logRoot,
    policy: {
      silenceTimeoutMs: 300_000,
      watchdog: {},
    },
    ...(isTestAdapterEnabled()
      ? { adapterFactory: createTestAdapterFactory() }
      : {}),
  });

  // Last-resort guard against an unhandled error taking down the whole
  // service before owned provider process groups are cleaned up. The
  // supervisor runs in-process with every active run, so an uncaught
  // exception (e.g. a stray stream `error` event that escaped the per-spawn
  // stdin guards) would otherwise orphan every detached provider group
  // until the next manual restart + reconciliation. Log, attempt an
  // orderly close of owned runs, then exit non-zero.
  //
  process.on("uncaughtException", createUncaughtExceptionHandler({ runManager }));

  let server: XagentServer | undefined;
  const shutdownController: XagentShutdownController = createShutdownController({
    closeRuns: async () => {
      await runManager.closeAll();
    },
    closeServer: async () => {
      await server?.close();
    },
    onShutdownComplete: () => {
      process.exit(0);
    },
  });

  // Trap SIGTERM and SIGINT so Conductor's stop fallback (which sends
  // SIGTERM to the service pid when POST /exit fails or is unresponsive)
  // and a human Ctrl-C both drive the same orderly shutdown as POST /exit:
  // close owned provider sessions and process groups before exiting 0.
  // Without these handlers, the signal would terminate the process
  // directly and orphan every detached provider process group until the
  // next restart + reconciliation. A repeated signal forces a non-zero
  // exit so a wedged orderly shutdown cannot block a human's second
  // Ctrl-C or Conductor's escalation to SIGKILL.
  //
  let signalShutdownStarted = false;
  const triggerSignalShutdown = (): void => {
    if (signalShutdownStarted) {
      process.exit(1);
    }
    signalShutdownStarted = true;
    void shutdownController.requestShutdown();
  };
  process.on("SIGTERM", triggerSignalShutdown);
  process.on("SIGINT", triggerSignalShutdown);

  server = createXagentServer({
    bindHost: config.bindHost,
    bindPort: config.bindPort,
    runManager,
    shutdownController,
    // Hold `/health` (`healthy: false`) and reject `/mcp` with 503 until
    // startup reconciliation resolves. Without this gate, the skill's
    // "verify Conductor reports the xagent service healthy … then
    // `xagent_start`" sequence aims at the listen→reconcile window in
    // which a brand-new run can be enumerated by the in-flight
    // reconciliation scan, marked `abandoned`, and have its (matching)
    // owned provider process group SIGTERMed. Conductor polls `/health`
    // and only proceeds once `healthy: true`, so this gate converts the
    // race into a wait. The liveRunIds skip in reconcileStaleRuns is
    // the backstop for any caller that races the gate.
    //
    ready: false,
  });

  // Bind the listener BEFORE reconciling stale runs. A duplicate
  // `make xagent-service-run` while Conductor's copy is already up would
  // otherwise scan the shared log root, SIGTERM every live worker owned by
  // the running service, mark those runs `abandoned`, and only then fail
  // with EADDRINUSE — silently destroying in-flight user work on a
  // plausible operator action. Binding first makes the second instance
  // exit on EADDRINUSE without touching any run. Reconciliation then runs
  // against the log root only this instance can reach, and its degradation
  // outcome is wired into `/health` via setWarning.
  //
  const port = await server.listen();
  console.error(`xagent service listening on ${config.bindHost}:${port}`);

  // Pass the live manager's run ids so reconciliation skips runs this
  // instance already owns. Even with the `/health` gate, this is the
  // authoritative guard against a run created in the listen→reconcile
  // window being enumerated from the log root and abandoned.
  //
  const reconciliationResults = await reconcileStaleRuns(
    config.logRoot,
    platformProcessInspector,
    new Set(runManager.listRunIds()),
  );
  // Surface reconciliation outcomes to the operator: log each result to
  // stderr for Conductor capture, and derive the `/health` warning from
  // any non-clean outcome so a sandbox-bypassed child that could not be
  // terminated is visible rather than silent. Previously the return value
  // was discarded and `/health` could never report degradation.
  //
  logReconciliationResults(reconciliationResults);
  server.setWarning(reconciliationWarning(reconciliationResults));

  // Reconciliation has finished (or there was nothing to reconcile): flip
  // the ready gate so `/health` reports `healthy: true` and `/mcp` starts
  // accepting controller work.
  //
  server.setReady(true);
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
