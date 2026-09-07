import { expect, test } from "@playwright/test";

test("syncs IDBFS before runtime initialization and flushes patch/config updates", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserPersistence } = await (new Function("return import('/dist/src/persistence.js')")() as Promise<{
      BrowserPersistence: new (filesystem: unknown, identity: unknown, options: unknown, reportStatus?: (status: string) => void) => {
        paths: unknown; start(): Promise<void>; scheduleSync(): void; status(): string; patchPath(path: string): string;
      };
    }>);
    const { BrowserRuntimeWorker } = await (new Function("return import('/dist/src/worker.js')")() as Promise<{
      BrowserRuntimeWorker: new (loadModule: unknown, createPersistence: unknown, emitStatus: unknown) => { handle(command: unknown): Promise<unknown> };
    }>);
    const calls: string[] = [];
    const emitted: unknown[] = [];
    let flushCount = 0;
    let persistence: InstanceType<typeof BrowserPersistence>;
    const filesystem = {
      filesystems: { IDBFS: "idbfs" },
      mkdir(path: string) { calls.push(`mkdir:${path}`); },
      mount(type: unknown, _options: object, path: string) { calls.push(`mount:${type}:${path}`); },
      syncfs(populate: boolean, complete: (error?: Error) => void) {
        calls.push(`sync:${populate}`);
        if (populate) complete();
        else {
          flushCount += 1;
          complete();
        }
      },
    };
    const worker = new BrowserRuntimeWorker(async () => ({
      abiVersion: 6,
      uiProtocolVersion: 2,
      runtimeConfigVersion: 1,
      filesystem,
      create: () => 3,
      audioOutputChannels: () => 2,
      initialize: (_handle: number, identity: { publisherId: string; appId: string; runtimeConfigVersion: number }) => {
        calls.push(`initialize:${identity.publisherId}/${identity.appId}:v${identity.runtimeConfigVersion}`);
        return 0;
      },
      prepare: () => 0,
      process: () => 0,
      messageTick: () => 0,
      buildUiFrame: () => new ArrayBuffer(0),
      dispatchAction: () => 0,
      submitMidiEndpoints: () => 0,
      dequeueMidiAction: () => undefined,
      deliverMidi: () => 0,
      dequeueMidiOutput: () => undefined,
      destroy: () => {},
    }), (_filesystem: unknown, identity: unknown, reportStatus: (status: string) => void) => {
      persistence = new BrowserPersistence(
        filesystem,
        identity,
        { debounceMs: 0 },
        reportStatus,
      );
      return persistence;
    }, (status: unknown) => emitted.push(status));

    await worker.handle({ type: "load", module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" } });
    await worker.handle({ type: "create" });
    const initialized = await worker.handle({
      type: "initialize",
      identity: { publisherId: "sheaf", appId: "miniapp", runtimeConfigVersion: 1 },
    });
    const pending = await worker.handle({ type: "persistence", state: "patch saved" });
    await new Promise((resolve) => setTimeout(resolve, 10));
    const liveness = await worker.handle({ type: "status" });
    const settled = await worker.handle({ type: "persistence-status" });
    return {
      calls,
      emitted,
      initialized,
      pending,
      liveness,
      settled,
      flushCount,
      paths: persistence!.paths,
      patchPath: persistence!.patchPath("presets/bright.json"),
      rejectsEscape: (() => {
        try {
          persistence!.patchPath("../config.json");
          return false;
        } catch {
          return true;
        }
      })(),
    };
  });

  expect(result.initialized).toEqual({ type: "ok" });
  expect(result.calls).toEqual([
    "mkdir:/data", "mount:idbfs:/data", "mkdir:/data/patches", "mkdir:/data/patches/sheaf",
    "mkdir:/data/patches/sheaf/miniapp", "mkdir:/data/logs", "sync:true", "initialize:sheaf/miniapp:v1", "sync:false",
  ]);
  expect(result.emitted).toEqual([
    { type: "page-status", path: "runtime.file.status", status: "persistence pending" },
    { type: "page-status", path: "runtime.file.status", status: "persistence succeeded" },
    { type: "page-status", path: "runtime.file.status", status: "persistence pending" },
    { type: "page-status", path: "runtime.file.status", status: "persistence succeeded" },
  ]);
  expect(result.pending).toEqual({ type: "page-status", path: "runtime.file.status", status: "persistence pending" });
  expect(result.liveness).toEqual({ type: "status", status: "running" });
  expect(result.settled).toEqual({ type: "page-status", path: "runtime.file.status", status: "persistence succeeded" });
  expect(result.flushCount).toBe(1);
  expect(result.paths).toEqual({
    dataRoot: "/data",
    patchesRoot: "/data/patches/sheaf/miniapp",
    logsRoot: "/data/logs",
    configFile: "/data/config.json",
  });
  expect(result.patchPath).toBe("/data/patches/sheaf/miniapp/presets/bright.json");
  expect(result.rejectsEscape).toBe(true);
});

test("flushes runtime-reported persistence changes after actions and ticks", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserPersistence } = await (new Function("return import('/dist/src/persistence.js')")() as Promise<{
      BrowserPersistence: new (filesystem: unknown, identity: unknown, options: unknown, reportStatus?: (status: string) => void) => unknown;
    }>);
    const { BrowserRuntimeWorker } = await (new Function("return import('/dist/src/worker.js')")() as Promise<{
      BrowserRuntimeWorker: new (loadModule: unknown, createPersistence: unknown, emitStatus: unknown) => { handle(command: unknown): Promise<unknown> };
    }>);
    const calls: string[] = [];
    const statuses: unknown[] = [];
    let flushCount = 0;
    let dirty = false;
    const filesystem = {
      filesystems: { IDBFS: "idbfs" },
      mkdir() {},
      mount() {},
      syncfs(populate: boolean, complete: (error?: Error) => void) {
        calls.push(`sync:${populate}`);
        if (!populate) flushCount += 1;
        complete();
      },
    };
    const worker = new BrowserRuntimeWorker(async () => ({
      abiVersion: 6,
      uiProtocolVersion: 2,
      runtimeConfigVersion: 1,
      filesystem,
      create: () => 9,
      audioOutputChannels: () => 2,
      initialize: () => 0,
      prepare: () => 0,
      process: () => 0,
      messageTick: (_handle: number, timestampMicros: number) => { if (timestampMicros === 20) dirty = true; return 0; },
      buildUiFrame: () => new ArrayBuffer(0),
      dispatchAction: (_handle: number, name: string) => { if (name === "runtime.config.save") dirty = true; return 0; },
      hasPersistenceChanges: () => {
        const result = dirty;
        dirty = false;
        return result;
      },
      submitMidiEndpoints: () => 0,
      dequeueMidiAction: () => undefined,
      deliverMidi: () => 0,
      dequeueMidiOutput: () => undefined,
      destroy: () => {},
    }), (filesystem: unknown, identity: unknown, reportStatus: (status: string) => void) =>
      new BrowserPersistence(filesystem, identity, { debounceMs: 0 }, reportStatus),
    (status: unknown) => statuses.push(status));

    await worker.handle({ type: "load", module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" } });
    await worker.handle({ type: "create" });
    await worker.handle({
      type: "initialize",
      identity: { publisherId: "sheaf", appId: "miniapp", runtimeConfigVersion: 1 },
    });
    calls.length = 0;
    await worker.handle({ type: "dispatch-action", name: "runtime.config.save", value: "" });
    await new Promise((resolve) => setTimeout(resolve, 10));
    const afterAction = { calls: [...calls], flushCount, statuses: [...statuses] };
    await worker.handle({ type: "dispatch-action", name: "runtime.file.save", value: "" });
    await new Promise((resolve) => setTimeout(resolve, 10));
    const afterPendingPatchAction = { calls: [...calls], flushCount };
    await worker.handle({ type: "message-tick", timestampMicros: 20 });
    await new Promise((resolve) => setTimeout(resolve, 10));
    return { afterAction, afterPendingPatchAction, calls, flushCount, statuses };
  });

  expect(result.afterAction.calls).toEqual(["sync:false"]);
  expect(result.afterAction.flushCount).toBe(1);
  expect(result.afterPendingPatchAction.flushCount).toBe(1);
  expect(result.calls).toEqual(["sync:false", "sync:false"]);
  expect(result.flushCount).toBe(2);
  expect(result.statuses).toContainEqual({ type: "page-status", path: "runtime.file.status", status: "persistence succeeded" });
});

test("reports a generic browser-host persistence failure", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserPersistence } = await (new Function("return import('/dist/src/persistence.js')")() as Promise<{
      BrowserPersistence: new (filesystem: unknown, identity: unknown, options: unknown, report: (status: string) => void) => {
        start(): Promise<void>; scheduleSync(): void; status(): string;
      };
    }>);
    const statuses: string[] = [];
    const persistence = new BrowserPersistence({
      filesystems: { IDBFS: "idbfs" },
      mkdir() {},
      mount() {},
      syncfs(populate: boolean, complete: (error?: Error) => void) { complete(populate ? undefined : new Error("quota")); },
    }, { publisherId: "sheaf", appId: "miniapp", runtimeConfigVersion: 1 },
    { debounceMs: 0 }, (status: string) => statuses.push(status));
    await persistence.start();
    persistence.scheduleSync();
    await new Promise((resolve) => setTimeout(resolve, 10));
    return { statuses, status: persistence.status() };
  });

  expect(result.statuses).toEqual(["persistence pending", "persistence succeeded", "persistence pending", "persistence failed"]);
  expect(result.status).toBe("persistence failed");
});

test("derives stable app-isolated patch roots from validated catalog identity", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const catalog = await (new Function("return import('/dist/src/catalog.js')")() as Promise<any>);
    const persistence = await (new Function("return import('/dist/src/persistence.js')")() as Promise<any>);
    const app = (publisherId: string, appId: string, buildId: string) => ({
      publisher: { id: publisherId },
      appId,
      buildId,
      browser: { runtimeConfigVersion: 1 },
    });
    const paths = (publisherId: string, appId: string, buildId: string) => persistence.deriveBrowserPersistencePaths(
      catalog.runtimeIdentityForCatalogApp(app(publisherId, appId, buildId)),
    );
    const rejects = (identity: unknown) => {
      try {
        persistence.deriveBrowserPersistencePaths(identity);
        return false;
      } catch {
        return true;
      }
    };
    return {
      firstBuild: paths("sheaf", "miniapp", "build-one"),
      nextBuild: paths("sheaf", "miniapp", "build-two"),
      braid: paths("sheaf", "braid-4", "build-one"),
      otherPublisher: paths("friend", "miniapp", "build-one"),
      rejectsTraversal: rejects({ publisherId: "../sheaf", appId: "miniapp", runtimeConfigVersion: 1 }),
      rejectsExtraPathInput: rejects({ publisherId: "sheaf", appId: "miniapp", runtimeConfigVersion: 1, patchesRoot: "/tmp" }),
    };
  });

  expect(result.firstBuild).toEqual(result.nextBuild);
  expect(result.firstBuild).toEqual({
    dataRoot: "/data",
    patchesRoot: "/data/patches/sheaf/miniapp",
    logsRoot: "/data/logs",
    configFile: "/data/config.json",
  });
  expect(result.otherPublisher.patchesRoot).toBe("/data/patches/friend/miniapp");
  expect(result.braid.patchesRoot).toBe("/data/patches/sheaf/braid-4");
  expect(result.braid.patchesRoot).not.toBe(result.firstBuild.patchesRoot);
  expect(result.otherPublisher.configFile).toBe(result.firstBuild.configFile);
  expect(result.otherPublisher.logsRoot).toBe(result.firstBuild.logsRoot);
  expect(result.rejectsTraversal).toBe(true);
  expect(result.rejectsExtraPathInput).toBe(true);
});

test("rejects incompatible runtime-config identity before filesystem or runtime initialization", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserRuntimeWorker } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const calls: string[] = [];
    const worker = new BrowserRuntimeWorker(async () => ({
      abiVersion: 6,
      uiProtocolVersion: 2,
      runtimeConfigVersion: 1,
      filesystem: {
        filesystems: { IDBFS: "idbfs" },
        mkdir() { calls.push("mkdir"); },
        mount() { calls.push("mount"); },
        syncfs() { calls.push("sync"); },
      },
      create() { calls.push("create"); return 5; },
      initialize() { calls.push("initialize"); return 0; },
      destroy() {},
    }), () => {
      calls.push("persistence-factory");
      throw new Error("must not construct persistence");
    });
    await worker.handle({ type: "load", module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" } });
    await worker.handle({ type: "create" });
    const initialized = await worker.handle({
      type: "initialize",
      identity: { publisherId: "sheaf", appId: "miniapp", runtimeConfigVersion: 2 },
    });
    await worker.handle({ type: "persistence", state: "should-not-write" });
    return { initialized, calls };
  });

  expect(result.initialized).toEqual({
    type: "error",
    error: expect.stringMatching(/runtimeConfigVersion.*unsupported.*1/i),
  });
  expect(result.calls).toEqual(["create"]);
});
