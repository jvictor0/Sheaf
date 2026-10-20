import { expect, test } from "@playwright/test";
import { makeCommandBuffer, NodeKind } from "./fixtures/command-buffer.js";

const launcherApp = {
  globalId: "example/portable-app",
  catalogUrl: "https://publisher.example/catalog.json",
  publisher: { id: "example", name: "Example Audio" },
  appId: "portable-app",
  displayName: "Portable App",
  author: "Ada Example",
  category: "Instrument",
  buildId: "portable-app-build-1",
  browser: {
    abiVersion: 6,
    uiProtocolVersion: 2,
    runtimeConfigVersion: 1,
    entry: "packages/portable-app/portable-app-build-1/app.js",
    entryUrl: "https://publisher.example/packages/portable-app/portable-app-build-1/app.js",
    files: [],
  },
};

test("begins audio resume and sysex MIDI acquisition synchronously before delayed package work", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/public/index.html");
  const result = await page.evaluate(async () => {
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const calls: string[] = [];
    let finishPackage!: () => void;
    const packageWork = new Promise<void>((resolve) => { finishPackage = resolve; });
    const context = {
      async resume() { calls.push("audio:resume"); },
      async close() { calls.push("audio:close"); },
    };
    const access = { inputs: new Map(), outputs: new Map(), onstatechange: null };

    const select = () => {
      const lease = ActivationLease.acquire({
        audioContextFactory: () => { calls.push("audio:construct"); return context; },
        requestMIDIAccess: (options: unknown) => {
          calls.push(`midi:request:${JSON.stringify(options)}`);
          return Promise.resolve(access);
        },
      });
      calls.push("package:begin");
      return packageWork.then(async () => {
        calls.push("package:resolved");
        await lease.consume();
        lease.dispose();
      });
    };

    const pending = select();
    const beforePackageResolution = [...calls];
    finishPackage();
    await pending;
    await new Promise((resolve) => setTimeout(resolve, 0));
    return { beforePackageResolution, calls };
  });

  expect(result.beforePackageResolution).toEqual([
    "audio:construct",
    "audio:resume",
    'midi:request:{"sysex":true}',
    "package:begin",
  ]);
  expect(result.calls).toEqual([
    ...result.beforePackageResolution,
    "package:resolved",
    "audio:close",
  ]);
});

test("consumes once and idempotently closes audio plus every MIDI port", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/public/index.html");
  const result = await page.evaluate(async () => {
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const counters = { resume: 0, contextClose: 0, inputClose: 0, outputClose: 0 };
    const context = {
      async resume() { counters.resume += 1; },
      async close() { counters.contextClose += 1; },
    };
    const input = { close: async () => { counters.inputClose += 1; } };
    const output = { close: async () => { counters.outputClose += 1; } };
    const access = {
      inputs: new Map([["input", input]]),
      outputs: new Map([["output", output]]),
      onstatechange: null,
    };
    const lease = ActivationLease.acquire({
      audioContextFactory: () => context,
      requestMIDIAccess: async () => access,
    });
    const resources = await lease.consume();
    let secondConsume = "";
    try { await lease.consume(); } catch (error) { secondConsume = (error as Error).message; }
    lease.dispose();
    lease.dispose();
    await new Promise((resolve) => setTimeout(resolve, 0));
    return {
      sameContext: resources.audioContext === context,
      sameAccess: resources.midiAccess === access,
      secondConsume,
      counters,
    };
  });

  expect(result.sameContext).toBe(true);
  expect(result.sameAccess).toBe(true);
  expect(result.secondConsume).toMatch(/already.*consumed/i);
  expect(result.counters).toEqual({ resume: 1, contextClose: 1, inputClose: 1, outputClose: 1 });
});

test("cleans partial denial and permits a fresh lease retry without duplicate live resources", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/public/index.html");
  const result = await page.evaluate(async () => {
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const counters = { contexts: 0, resumes: 0, closes: 0, requests: 0, portCloses: 0 };
    const context = () => {
      counters.contexts += 1;
      return {
        async resume() { counters.resumes += 1; },
        async close() { counters.closes += 1; },
      };
    };
    const access = {
      inputs: new Map([["input", { close: async () => { counters.portCloses += 1; } }]]),
      outputs: new Map(),
      onstatechange: null,
    };
    const request = async () => {
      counters.requests += 1;
      if (counters.requests === 1) throw new Error("sysex denied");
      return access;
    };

    const denied = ActivationLease.acquire({ audioContextFactory: context, requestMIDIAccess: request });
    let denial = "";
    try { await denied.consume(); } catch (error) { denial = (error as Error).message; }
    denied.dispose();
    const retry = ActivationLease.acquire({ audioContextFactory: context, requestMIDIAccess: request });
    await retry.consume();
    retry.dispose();
    await new Promise((resolve) => setTimeout(resolve, 0));
    return { denial, counters };
  });

  expect(result.denial).toContain("sysex denied");
  expect(result.counters).toEqual({ contexts: 2, resumes: 2, closes: 2, requests: 2, portCloses: 1 });
});

test("closes MIDI resources that resolve after audio activation fails", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/public/index.html");
  const result = await page.evaluate(async () => {
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const counters = { contextClose: 0, portClose: 0 };
    let resolveMidi!: (access: unknown) => void;
    const midi = new Promise((resolve) => { resolveMidi = resolve; });
    const lease = ActivationLease.acquire({
      audioContextFactory: () => ({
        resume: async () => { throw new Error("audio denied"); },
        close: async () => { counters.contextClose += 1; },
      }),
      requestMIDIAccess: () => midi,
    });
    let failure = "";
    const consumed = lease.consume().catch((error: Error) => { failure = error.message; });
    resolveMidi({
      inputs: new Map([["input", { close: async () => { counters.portClose += 1; } }]]),
      outputs: new Map(),
      onstatechange: null,
    });
    await consumed;
    await new Promise((resolve) => setTimeout(resolve, 0));
    return { failure, counters };
  });

  expect(result.failure).toContain("audio denied");
  expect(result.counters).toEqual({ contextClose: 1, portClose: 1 });
});

test("launcher acquires once before package work and forwards one materialized package plus declared versions", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  const result = await page.evaluate(async (application) => {
    const main = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const events: string[] = [];
    let finishMaterialization!: () => void;
    const materialization = new Promise<void>((resolve) => { finishMaterialization = resolve; });
    const materialized = {
      entryUrl: "blob:entry",
      locateFile: {},
      mainScriptUrlOrBlob: "blob:main",
      dispose() { events.push("package:dispose"); },
    };
    const launcher = await main.installSheafPatchLauncher(document.querySelector("#synth-root"), {
      client: { loadSources: async () => ({ apps: [application], diagnostics: [], duplicateDiagnostics: [] }) },
      activationLeaseFactory: () => {
        events.push("lease:acquire");
        return { consume: async () => { throw new Error("not consumed by stub"); }, dispose() { events.push("lease:dispose"); } };
      },
      materializePackage: async () => {
        events.push("package:begin");
        await materialization;
        events.push("package:ready");
        return materialized;
      },
      installApp: async (_root: HTMLElement, options: any) => {
        events.push("runtime:install");
        (window as any).__installedOptions = options;
        return { stop() {} };
      },
    });
    const button = document.querySelector<HTMLButtonElement>(".synth-launcher__launch")!;
    button.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    button.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    const beforePackageResolution = [...events];
    finishMaterialization();
    await new Promise((resolve) => setTimeout(resolve, 0));
    const options = (window as any).__installedOptions;
    return {
      beforePackageResolution,
      events,
      moduleIsMaterialized: options.module === materialized,
      leasePresent: Boolean(options.activationLease),
      versions: options.runtimeVersions,
      launcherPresent: Boolean(launcher),
    };
  }, launcherApp);

  expect(result.beforePackageResolution).toEqual(["lease:acquire", "package:begin"]);
  expect(result.events).toEqual(["lease:acquire", "package:begin", "package:ready", "runtime:install"]);
  expect(result.moduleIsMaterialized).toBe(true);
  expect(result.leasePresent).toBe(true);
  expect(result.versions).toEqual({ abiVersion: 6, uiProtocolVersion: 2, runtimeConfigVersion: 1 });
  expect(result.launcherPresent).toBe(true);
});

test("package failure disposes the lease once and a retry acquires fresh resources", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  await page.evaluate(async (application) => {
    const main = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const counters = { leases: 0, leaseDisposals: 0, packages: 0, installs: 0 };
    (window as any).__failureCounters = counters;
    await main.installSheafPatchLauncher(document.querySelector("#synth-root"), {
      client: { loadSources: async () => ({ apps: [application], diagnostics: [], duplicateDiagnostics: [] }) },
      activationLeaseFactory: () => {
        counters.leases += 1;
        let disposed = false;
        return {
          async consume() { return {}; },
          dispose() { if (!disposed) { disposed = true; counters.leaseDisposals += 1; } },
        };
      },
      materializePackage: async () => {
        counters.packages += 1;
        if (counters.packages === 1) throw new Error("package unavailable");
        return { entryUrl: "blob:entry", locateFile: {}, mainScriptUrlOrBlob: "blob:main", dispose() {} };
      },
      installApp: async () => { counters.installs += 1; return { stop() {} }; },
    });
  }, launcherApp);

  await page.getByRole("button", { name: /launch portable app/i }).click();
  const row = page.getByRole("listitem").filter({ hasText: "Portable App" });
  await expect(row).toContainText("package unavailable");
  await page.getByRole("button", { name: /retry portable app/i }).click();
  await expect(row.getByRole("button", { name: /launch portable app/i })).toBeDisabled();
  await expect(page.getByRole("button", { name: /back to launcher/i })).toHaveCount(0);
  expect(await page.evaluate(() => (window as any).__failureCounters)).toEqual({
    leases: 2,
    leaseDisposals: 1,
    packages: 2,
    installs: 1,
  });
});

test("runtime initialization failure releases consumed activation and materialized package resources", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  const result = await page.evaluate(async (application) => {
    const main = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const counters = { contextClose: 0, portClose: 0, packageDispose: 0, runtimeClients: 0, runtimeTerminates: 0 };
    let lease: any;
    let packageDisposed = false;
    const materialized = {
      entryUrl: "blob:entry",
      locateFile: {},
      mainScriptUrlOrBlob: "blob:main",
      dispose() { if (!packageDisposed) { packageDisposed = true; counters.packageDispose += 1; } },
    };
    await main.installSheafPatchLauncher(document.querySelector("#synth-root"), {
      client: { loadSources: async () => ({ apps: [application], diagnostics: [], duplicateDiagnostics: [] }) },
      activationLeaseFactory: () => lease = ActivationLease.acquire({
        audioContextFactory: () => ({
          resume: async () => {},
          close: async () => { counters.contextClose += 1; },
        }),
        requestMIDIAccess: async () => ({
          inputs: new Map([["input", { close: async () => { counters.portClose += 1; } }]]),
          outputs: new Map(),
          onstatechange: null,
        }),
      }),
      materializePackage: async () => materialized,
      runtimeClientFactory: () => {
        counters.runtimeClients += 1;
        return {
          async request(command: { type: string }) {
            if (command.type === "create") return { type: "created", handle: 1 };
            if (command.type === "initialize") return { type: "error", error: "runtime initialization failed" };
            return { type: "ok" };
          },
          terminate() { counters.runtimeTerminates += 1; },
        };
      },
    });
    document.querySelector<HTMLButtonElement>(".synth-launcher__launch")!.click();
    await new Promise((resolve) => setTimeout(resolve, 0));
    await new Promise((resolve) => setTimeout(resolve, 0));
    return { counters, error: document.querySelector(".synth-launcher__app-error")?.textContent, leasePresent: Boolean(lease) };
  }, launcherApp);

  expect(result.error).toContain("runtime initialization failed");
  expect(result.leasePresent).toBe(true);
  expect(result.counters).toEqual({
    contextClose: 1,
    portClose: 1,
    packageDispose: 1,
    runtimeClients: 1,
    runtimeTerminates: 1,
  });
});

test("launcher reports an explicit non-retryable browser input limit diagnostic before capture or worklet startup", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  const result = await page.evaluate(async (application) => {
    const main = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const events: string[] = [];
    Object.defineProperty(navigator, "mediaDevices", {
      configurable: true,
      value: {
        getUserMedia() {
          events.push("getUserMedia");
          throw new Error("capture must not be requested");
        },
      },
    });
    await main.installSheafPatchLauncher(document.querySelector("#synth-root"), {
      client: { loadSources: async () => ({ apps: [application], diagnostics: [], duplicateDiagnostics: [] }) },
      activationLeaseFactory: () => ActivationLease.acquire({
        audioContextFactory: () => ({
          sampleRate: 48_000,
          resume: async () => { events.push("context:resume"); },
          close: async () => { events.push("context:close"); },
        }),
        requestMIDIAccess: async () => ({ inputs: new Map(), outputs: new Map(), onstatechange: null }),
      }),
      materializePackage: async () => {
        let packageDisposed = false;
        return {
          entryUrl: "blob:entry",
          locateFile: {},
          mainScriptUrlOrBlob: "blob:main",
          dispose() {
            if (!packageDisposed) {
              packageDisposed = true;
              events.push("package:dispose");
            }
          },
        };
      },
      runtimeClientFactory: () => ({
        async request(command: { type: string }) {
          if (command.type === "load") { events.push("load"); return { type: "ok" }; }
          if (command.type === "create") { events.push("create"); return { type: "created", handle: 1 }; }
          if (command.type === "initialize") { events.push("initialize"); return { type: "ok" }; }
          if (command.type === "audio-config") { events.push("audio-config"); return { type: "audio-config", channels: 2, inputChannels: 33 }; }
          if (command.type === "midi-endpoints") return { type: "midi-actions", actions: [] };
          if (command.type === "drain-midi-output") return { type: "midi-output" };
          return { type: "ok" };
        },
        async startAudioWorklet() { events.push("startAudioWorklet"); return { started: true }; },
        async consumePendingAudioRequest() { events.push("consumePendingAudioRequest"); return { index: -1, control: 0 }; },
        terminate() { events.push("terminate"); },
      }),
    });
    document.querySelector<HTMLButtonElement>(".synth-launcher__launch")!.click();
    for (let turn = 0; turn < 12; turn++) await new Promise((resolve) => setTimeout(resolve, 0));
    return {
      events,
      error: document.querySelector(".synth-launcher__app-error")?.textContent,
      retryInputVisible: document.body.textContent?.includes("Retry Input") ?? false,
    };
  }, launcherApp);

  expect(result.error).toContain("browser-audio-input-channel-limit-exceeded: requested 33, limit 32");
  expect(result.retryInputVisible).toBe(false);
  expect(result.events).toEqual([
    "context:resume",
    "load",
    "create",
    "initialize",
    "audio-config",
    "terminate",
    "context:close",
    "package:dispose",
  ]);
});

test("an input-capable leased app discovers its request after module load, retries on user demand, and releases capture before the runtime", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  const frame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 60], children: ["retry"] },
    {
      id: "retry",
      kind: NodeKind.Button,
      bounds: [0, 0, 200, 60],
      label: "Retry Input",
      action: { name: "audio-input-retry", value: "" },
    },
  ]);
  const result = await page.evaluate(async (bytes) => {
    const main = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const events: string[] = [];
    const tracks: any[] = [];
    let retryPending = false;

    const context = {
      sampleRate: 48_000,
      destination: {},
      audioWorklet: { addModule: async () => {} },
      resume: async () => {},
      close: async () => { events.push("context:close"); },
      createMediaStreamSource() {
        events.push("source:create");
        const source = { channelCount: 2, disconnect() { events.push("source:disconnect"); } };
        return source;
      },
    };
    Object.defineProperty(navigator, "mediaDevices", {
      configurable: true,
      value: {
        // One presentable input device, so a selection index has something to
        // resolve to once the operator retries.
        async enumerateDevices() {
          return [{ deviceId: "mic-1", label: "USB Mic", kind: "audioinput" }];
        },
        async getUserMedia(requested: any) {
          events.push(`getUserMedia:${requested.audio.channelCount.ideal}`);
          const track = {
            onended: null as null | (() => void),
            readyState: "live",
            getSettings: () => ({ channelCount: 2 }),
            stop() { track.readyState = "ended"; events.push("track:stop"); },
          };
          tracks.push(track);
          return { getAudioTracks: () => [track], getTracks: () => [track] };
        },
      },
    });

    const lease = ActivationLease.acquire({
      audioContextFactory: () => context,
      requestMIDIAccess: async () => ({ inputs: new Map(), outputs: new Map(), onstatechange: null }),
    });
    const runtime = {
      async request(command: any) {
        if (command.type === "load") { events.push("load"); return { type: "ok" }; }
        if (command.type === "create") { events.push("create"); return { type: "created", handle: 1 }; }
        if (command.type === "initialize") { events.push("initialize"); return { type: "ok" }; }
        if (command.type === "audio-config") { events.push("audio-config"); return { type: "audio-config", channels: 2, inputChannels: 4 }; }
        if (command.type === "build-ui-frame") return { type: "ui-frame", frame: bytes };
        if (command.type === "dispatch-action") { events.push(`dispatch:${command.name}`); return { type: "ui-frame", frame: bytes }; }
        if (command.type === "midi-endpoints") return { type: "midi-actions", actions: [] };
        if (command.type === "drain-midi-output") return { type: "midi-output" };
        return { type: "ok" };
      },
      async startAudioWorklet(received?: unknown) {
        if (received !== context) throw new Error("leased context was not passed to native startup");
        events.push("startAudioWorklet");
        return { started: true };
      },
      async setAudioInputSource(_source: unknown, physicalChannels: number, statusCode: number) {
        events.push(`setAudioInputSource:${physicalChannels}:${statusCode}`);
        return 91;
      },
      async clearAudioInputSource(statusCode: number) { events.push(`clearAudioInputSource:${statusCode}`); },
      clearAudioInputSourceNow(statusCode: number) { events.push(`clearAudioInputSourceNow:${statusCode}`); },
      // index: -1 nothing pending, -2 release input, otherwise the index of
      // the operator's selection into the device list `submitAudioDevices`
      // most recently enumerated -- here always the single mocked input
      // device. control: 0 for input, the only control this fixture arms.
      async consumePendingAudioRequest() {
        const pending = retryPending;
        retryPending = false;
        return { index: pending ? 0 : -1, control: 0 };
      },
      terminate() { events.push("terminate"); },
    };

    const app = await main.installSynthBrowserApp(document.querySelector("#synth-root"), {
      module: { entryUrl: "blob:entry", locateFile: {}, mainScriptUrlOrBlob: "blob:main" },
      activationLease: lease,
      runtimeClient: runtime,
      frameIntervalMs: 60_000,
    });
    const afterStart = [...events];

    const settle = async () => {
      for (let turn = 0; turn < 8; turn++) await new Promise((resolve) => setTimeout(resolve, 0));
    };
    const retryButton = document.querySelector<HTMLElement>('[data-node-id="retry"]')!;
    retryButton.click();
    await settle();
    const ignoredRetry = [...events];

    retryPending = true;
    retryButton.click();
    await settle();
    const afterRetry = [...events];

    // Unload gives no chance to await, so everything observable has to have
    // happened by the time the dispatched event returns.
    dispatchEvent(new Event("pagehide"));
    const synchronousUnload = [...events];
    dispatchEvent(new Event("pagehide"));
    await app.stop();
    await app.stop();
    return { afterStart, ignoredRetry, afterRetry, synchronousUnload, events, trackCount: tracks.length };
  }, Array.from(new Uint8Array(frame)));

  // Discovery reports the declared channel count, but capture itself is never
  // acquired automatically -- only an operator's device selection arms it, so
  // opening the app raises no permission prompt.
  expect(result.afterStart).toEqual([
    "load",
    "create",
    "initialize",
    "audio-config",
    // `enumerateDevices` neither prompts nor requires permission, so it runs
    // unconditionally on activation; this fixture's context has no
    // `setSinkId`, so that submission also reports output routing as
    // unsupported, before native startup itself.
    "dispatch:runtime.audio.output.routing_unsupported",
    "startAudioWorklet",
  ]);
  // A UI action with no armed selection must not request capture.
  expect(result.ignoredRetry).toEqual([...result.afterStart, "dispatch:audio-input-retry"]);
  expect(result.afterRetry).toEqual([
    ...result.ignoredRetry,
    "dispatch:audio-input-retry",
    "clearAudioInputSource:1",
    "getUserMedia:4",
    "source:create",
    "setAudioInputSource:2:2",
  ]);
  expect(result.trackCount).toBe(1);
  // A dispatched pagehide releases capture synchronously, in the required
  // clear-native -> disconnect -> track-stop order.
  expect(result.synchronousUnload.slice(result.afterRetry.length)).toEqual([
    "clearAudioInputSourceNow:0",
    "source:disconnect",
    "track:stop",
  ]);
  // Teardown clears the native active count before the runtime is destroyed and
  // the leased context is closed, and repeats do nothing.
  expect(result.events.slice(result.afterRetry.length)).toEqual([
    "clearAudioInputSourceNow:0",
    "source:disconnect",
    "track:stop",
    "terminate",
    "context:close",
  ]);
});

test("a still-pending capture permission from an operator selection does not block synchronous unload, and the late stream still tears down", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  const frame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 60], children: ["retry"] },
    {
      id: "retry",
      kind: NodeKind.Button,
      bounds: [0, 0, 200, 60],
      label: "Retry Input",
      action: { name: "audio-input-retry", value: "" },
    },
  ]);
  const result = await page.evaluate(async (bytes) => {
    const main = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const events: string[] = [];
    const tracks: any[] = [];
    let resolveCapture!: (stream: unknown) => void;
    const pendingCapture = new Promise((resolve) => { resolveCapture = resolve; });
    let retryPending = false;
    const settle = async () => {
      for (let turn = 0; turn < 8; turn++) await new Promise((resolve) => setTimeout(resolve, 0));
    };
    const waitFor = async (ready: () => boolean, label: string) => {
      for (let turn = 0; turn < 24; turn++) {
        if (ready()) return;
        await new Promise((resolve) => setTimeout(resolve, 0));
      }
      throw new Error(label);
    };
    const makeStream = () => {
      const track = {
        stops: 0,
        readyState: "live",
        onended: null as null | (() => void),
        getSettings: () => ({ channelCount: 2 }),
        stop() { track.stops += 1; track.readyState = "ended"; events.push("track:stop"); },
      };
      tracks.push(track);
      return { getAudioTracks: () => [track], getTracks: () => [track] };
    };

    const context = {
      sampleRate: 48_000,
      destination: {},
      audioWorklet: { addModule: async () => {} },
      resume: async () => {},
      close: async () => { events.push("context:close"); },
      createMediaStreamSource() {
        events.push("source:create");
        return { channelCount: 2, disconnect() { events.push("source:disconnect"); } };
      },
    };
    Object.defineProperty(navigator, "mediaDevices", {
      configurable: true,
      value: {
        // One presentable input device, so the operator's selection has
        // something to resolve to.
        async enumerateDevices() {
          return [{ deviceId: "mic-1", label: "USB Mic", kind: "audioinput" }];
        },
        getUserMedia(requested: any) {
          events.push(`getUserMedia:${requested.audio.channelCount.ideal}`);
          return pendingCapture;
        },
      },
    });

    const lease = ActivationLease.acquire({
      audioContextFactory: () => context,
      requestMIDIAccess: async () => ({ inputs: new Map(), outputs: new Map(), onstatechange: null }),
    });
    const runtime = {
      async request(command: any) {
        if (command.type === "load") { events.push("load"); return { type: "ok" }; }
        if (command.type === "create") { events.push("create"); return { type: "created", handle: 1 }; }
        if (command.type === "initialize") { events.push("initialize"); return { type: "ok" }; }
        if (command.type === "audio-config") { events.push("audio-config"); return { type: "audio-config", channels: 2, inputChannels: 2 }; }
        if (command.type === "build-ui-frame") return { type: "ui-frame", frame: bytes };
        if (command.type === "dispatch-action") { events.push(`dispatch:${command.name}`); return { type: "ui-frame", frame: bytes }; }
        if (command.type === "midi-endpoints") return { type: "midi-actions", actions: [] };
        if (command.type === "drain-midi-output") return { type: "midi-output" };
        return { type: "ok" };
      },
      async startAudioWorklet(received?: unknown) {
        if (received !== context) throw new Error("leased context was not passed to native startup");
        events.push("startAudioWorklet");
        return { started: true };
      },
      async setAudioInputSource() { events.push("setAudioInputSource"); return 91; },
      async clearAudioInputSource(statusCode: number) { events.push(`clearAudioInputSource:${statusCode}`); },
      clearAudioInputSourceNow(statusCode: number) { events.push(`clearAudioInputSourceNow:${statusCode}`); },
      // index: -1 nothing pending, -2 release input, otherwise the index of
      // the operator's selection into the device list `submitAudioDevices`
      // most recently enumerated -- here always the single mocked input
      // device. control: 0 for input, the only control this fixture arms.
      async consumePendingAudioRequest() {
        const pending = retryPending;
        retryPending = false;
        return { index: pending ? 0 : -1, control: 0 };
      },
      terminate() { events.push("terminate"); },
    };

    const app = await main.installSynthBrowserApp(document.querySelector("#synth-root"), {
      module: { entryUrl: "blob:entry", locateFile: {}, mainScriptUrlOrBlob: "blob:main" },
      activationLease: lease,
      runtimeClient: runtime,
      frameIntervalMs: 60_000,
    });
    const afterStart = [...events];

    retryPending = true;
    document.querySelector<HTMLElement>('[data-node-id="retry"]')!.click();
    await settle();
    const afterSelection = [...events];

    // Unload gives no chance to await, so everything observable has to have
    // happened by the time the dispatched event returns.
    dispatchEvent(new Event("pagehide"));
    const afterPagehide = [...events];

    resolveCapture(makeStream());
    await waitFor(() => events.includes("terminate"), "app did not finish teardown after the late permission settled");
    await settle();
    await app.stop();
    return {
      afterStart,
      afterSelection,
      afterPagehide,
      events,
      trackStops: tracks.map((track) => track.stops),
    };
  }, Array.from(new Uint8Array(frame)));

  // Startup runs to completion with no capture requested yet: the operator
  // has not selected an input device.
  expect(result.afterStart).toEqual([
    "load",
    "create",
    "initialize",
    "audio-config",
    // `enumerateDevices` neither prompts nor requires permission, so it runs
    // unconditionally on activation; this fixture's context has no
    // `setSinkId`, so that submission also reports output routing as
    // unsupported, before native startup itself.
    "dispatch:runtime.audio.output.routing_unsupported",
    "startAudioWorklet",
  ]);
  // The operator's selection requests capture and leaves it pending -- the
  // permission has not resolved by the time the dispatched pagehide fires.
  expect(result.afterSelection.slice(result.afterStart.length)).toEqual([
    "dispatch:audio-input-retry",
    "clearAudioInputSource:1",
    "getUserMedia:2",
  ]);
  // Teardown clears the native input registration synchronously regardless
  // of the still-open permission prompt.
  expect(result.afterPagehide.slice(result.afterSelection.length)).toEqual([
    "clearAudioInputSourceNow:0",
  ]);
  // Once the permission resolves, the stream arrives too late to be wired
  // into the graph, but it is still stopped, and runtime and context
  // teardown follow.
  expect(result.events.slice(result.afterPagehide.length)).toEqual([
    "track:stop",
    "terminate",
    "context:close",
  ]);
  expect(result.events).not.toContain("source:create");
  expect(result.events).not.toContain("setAudioInputSource");
  expect(result.trackStops).toEqual([1]);
});

test("clears a native registration that lands after a dispatched pagehide", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  const frame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 60], children: ["retry"] },
    {
      id: "retry",
      kind: NodeKind.Button,
      bounds: [0, 0, 200, 60],
      label: "Retry Input",
      action: { name: "audio-input-retry", value: "" },
    },
  ]);
  const result = await page.evaluate(async (bytes) => {
    const main = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const settle = async () => {
      for (let turn = 0; turn < 8; turn++) await new Promise((resolve) => setTimeout(resolve, 0));
    };
    const events: string[] = [];
    const tracks: any[] = [];
    const sources: any[] = [];
    // Mirrors the native publication: a registration publishes a handle and a
    // count, and only a clear takes them back down.
    let published = { handle: 0, physicalChannels: 0 };
    let registrations = 0;
    let releaseRegistration!: () => void;
    const deferredRegistration = new Promise<void>((resolve) => { releaseRegistration = resolve; });
    let retryPending = false;

    const context = {
      sampleRate: 48_000,
      destination: {},
      audioWorklet: { addModule: async () => {} },
      resume: async () => {},
      close: async () => { events.push("context:close"); },
      createMediaStreamSource() {
        const source = {
          channelCount: 2,
          disconnects: 0,
          disconnect() { source.disconnects += 1; events.push("source:disconnect"); },
        };
        sources.push(source);
        events.push("source:create");
        return source;
      },
    };
    Object.defineProperty(navigator, "mediaDevices", {
      configurable: true,
      value: {
        // One presentable input device, so a selection index has something to
        // resolve to on both the initial acquisition and the retry.
        async enumerateDevices() {
          return [{ deviceId: "mic-1", label: "USB Mic", kind: "audioinput" }];
        },
        async getUserMedia() {
          events.push("getUserMedia");
          const track = {
            stops: 0,
            readyState: "live",
            onended: null as null | (() => void),
            getSettings: () => ({ channelCount: 2 }),
            stop() { track.stops += 1; track.readyState = "ended"; events.push("track:stop"); },
          };
          tracks.push(track);
          return { getAudioTracks: () => [track], getTracks: () => [track] };
        },
      },
    });

    const lease = ActivationLease.acquire({
      audioContextFactory: () => context,
      requestMIDIAccess: async () => ({ inputs: new Map(), outputs: new Map(), onstatechange: null }),
    });
    const runtime = {
      async request(command: any) {
        if (command.type === "create") return { type: "created", handle: 1 };
        if (command.type === "audio-config") return { type: "audio-config", channels: 2, inputChannels: 2 };
        if (command.type === "build-ui-frame") return { type: "ui-frame", frame: bytes };
        if (command.type === "dispatch-action") return { type: "ui-frame", frame: bytes };
        if (command.type === "midi-endpoints") return { type: "midi-actions", actions: [] };
        if (command.type === "drain-midi-output") return { type: "midi-output" };
        return { type: "ok" };
      },
      async startAudioWorklet() { return { started: true }; },
      async setAudioInputSource(_source: unknown, physicalChannels: number, statusCode: number) {
        registrations += 1;
        const handle = 90 + registrations;
        // The retry's registration is held open across the unload.
        if (registrations === 2) {
          events.push("setAudioInputSource:deferred");
          await deferredRegistration;
        }
        events.push(`setAudioInputSource:${physicalChannels}:${statusCode}`);
        published = { handle, physicalChannels };
        return handle;
      },
      async clearAudioInputSource(statusCode: number) {
        events.push(`clearAudioInputSource:${statusCode}`);
        published = { handle: 0, physicalChannels: 0 };
      },
      clearAudioInputSourceNow(statusCode: number) {
        events.push(`clearAudioInputSourceNow:${statusCode}`);
        published = { handle: 0, physicalChannels: 0 };
      },
      // index: -1 nothing pending, -2 release input, otherwise the index of
      // the operator's selection into the device list `submitAudioDevices`
      // most recently enumerated -- here always the single mocked input
      // device. control: 0 for input, the only control this fixture arms.
      async consumePendingAudioRequest() {
        const pending = retryPending;
        retryPending = false;
        return { index: pending ? 0 : -1, control: 0 };
      },
      terminate() { events.push("terminate"); },
    };

    const app = await main.installSynthBrowserApp(document.querySelector("#synth-root"), {
      module: { entryUrl: "blob:entry", locateFile: {}, mainScriptUrlOrBlob: "blob:main" },
      activationLease: lease,
      runtimeClient: runtime,
      frameIntervalMs: 60_000,
    });

    // Capture is never acquired automatically, so a first selection is needed
    // to establish the registration the retry below will then replace.
    retryPending = true;
    document.querySelector<HTMLElement>('[data-node-id="retry"]')!.click();
    await settle();

    retryPending = true;
    document.querySelector<HTMLElement>('[data-node-id="retry"]')!.click();
    await settle();
    const duringRegistration = { events: [...events], published: { ...published } };

    dispatchEvent(new Event("pagehide"));
    const afterUnload = { events: [...events], published: { ...published } };

    releaseRegistration();
    await app.stop();
    await settle();
    return {
      duringRegistration,
      afterUnload,
      events,
      published,
      trackStops: tracks.map((track: any) => track.stops),
      sourceDisconnects: sources.map((source: any) => source.disconnects),
    };
  }, Array.from(new Uint8Array(frame)));

  // The retry released the established capture and is now parked inside an
  // awaited registration, with nothing published.
  expect(result.duringRegistration.events.at(-1)).toBe("setAudioInputSource:deferred");
  expect(result.duringRegistration.published).toEqual({ handle: 0, physicalChannels: 0 });
  // Unload clears synchronously, but there is nothing published yet to clear.
  expect(result.afterUnload.events.at(-1)).toBe("clearAudioInputSourceNow:0");
  // The registration lands after the unload and publishes a handle and a count;
  // that late claim has to come back down, and its resources with it.
  expect(result.events.slice(result.afterUnload.events.length)).toEqual([
    "setAudioInputSource:2:2",
    "clearAudioInputSource:0",
    "source:disconnect",
    "track:stop",
    "terminate",
    "context:close",
  ]);
  expect(result.published).toEqual({ handle: 0, physicalChannels: 0 });
  expect(result.trackStops).toEqual([1, 1]);
  expect(result.sourceDisconnects).toEqual([1, 1]);
});

test("successful leased app unload is idempotent and releases one context, MIDI request, runtime, node, and package", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  const frame = makeCommandBuffer([{ id: "root", kind: NodeKind.Root, bounds: [0, 0, 20, 20] }]);
  const result = await page.evaluate(async (bytes) => {
    const main = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const counters = {
      contexts: 0, resumes: 0, contextCloses: 0, midiRequests: 0, portCloses: 0,
      runtimeStarts: 0, nativeStarts: 0, runtimeTerminates: 0, ringCommands: 0,
      fallbackNodes: 0, packageDisposals: 0,
    };
    const context = {
      sampleRate: 48_000,
      destination: {},
      audioWorklet: { addModule: async () => {} },
      resume: async () => { counters.resumes += 1; },
      close: async () => { counters.contextCloses += 1; },
    };
    const lease = ActivationLease.acquire({
      audioContextFactory: () => { counters.contexts += 1; return context; },
      requestMIDIAccess: async () => {
        counters.midiRequests += 1;
        return {
          inputs: new Map([["input", {
            id: "input", name: "Input", state: "connected", onmidimessage: null,
            close: async () => { counters.portCloses += 1; },
          }]]),
          outputs: new Map(),
          onstatechange: null,
        };
      },
    });
    const runtime = {
      async request(command: { type: string }) {
        if (command.type === "create") { counters.runtimeStarts += 1; return { type: "created", handle: 1 }; }
        if (command.type === "configure-audio" || command.type === "render-audio") counters.ringCommands += 1;
        if (command.type === "audio-config") return { type: "audio-config", channels: 2 };
        if (command.type === "build-ui-frame") return { type: "ui-frame", frame: bytes };
        if (command.type === "midi-endpoints") return { type: "midi-actions", actions: [] };
        if (command.type === "drain-midi-output") return { type: "midi-output" };
        return { type: "ok" };
      },
      async startAudioWorklet(received?: AudioContext) {
        if (received !== context as unknown as AudioContext) throw new Error("leased context was not passed to native startup");
        counters.nativeStarts += 1;
        return { started: true };
      },
      terminate() { counters.runtimeTerminates += 1; },
    };
    let packageDisposed = false;
    const app = await main.installSynthBrowserApp(document.querySelector("#synth-root"), {
      module: { entryUrl: "blob:entry", locateFile: {}, mainScriptUrlOrBlob: "blob:main" },
      activationLease: lease,
      runtimeClient: runtime,
      frameIntervalMs: 60_000,
      disposeModule: () => {
        if (!packageDisposed) { packageDisposed = true; counters.packageDisposals += 1; }
      },
      audioOptions: {
        audioContextFactory: () => { throw new Error("second context"); },
        audioWorkletNodeFactory: () => { counters.fallbackNodes += 1; throw new Error("JavaScript AudioWorklet fallback"); },
      },
    });
    dispatchEvent(new Event("pagehide"));
    dispatchEvent(new Event("pagehide"));
    app.stop();
    await new Promise((resolve) => setTimeout(resolve, 0));
    return counters;
  }, Array.from(new Uint8Array(frame)));

  expect(result).toEqual({
    contexts: 1,
    resumes: 1,
    contextCloses: 1,
    midiRequests: 1,
    portCloses: 1,
    runtimeStarts: 1,
    nativeStarts: 1,
    runtimeTerminates: 1,
    ringCommands: 0,
    fallbackNodes: 0,
    packageDisposals: 1,
  });
});
