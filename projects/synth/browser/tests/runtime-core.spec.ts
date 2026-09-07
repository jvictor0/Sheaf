import { expect, test, type Page } from "@playwright/test";
import { makeCommandBuffer, NodeKind } from "./fixtures/command-buffer.js";

async function blockProductAutoBoot(page: Page) {
  await page.route("**/dist/src/main.js", (route) => route.fulfill({
    status: 200,
    contentType: "application/javascript",
    body: "",
  }));
}

test("routes a portable action through the runtime worker facade without app HTML", async ({ page }) => {
  await blockProductAutoBoot(page);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const frame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 40], children: ["button"] },
    { id: "button", kind: NodeKind.Button, bounds: [0, 0, 100, 40], label: "Trigger", action: { name: "generic.trigger", value: "pressed" } },
  ]);

  const result = await page.evaluate(async (bytes) => {
    const loadWorker = new Function("return import('/dist/src/worker.js')") as () => Promise<{
      BrowserRuntimeWorker: new (loadModule: unknown) => {
        handle(command: unknown): Promise<unknown>;
        startAudioWorklet(context?: AudioContext): Promise<unknown>;
      };
    }>;
    const { BrowserRuntimeWorker } = await loadWorker();
    const calls: Array<[string, ...unknown[]]> = [];
    let nextHandle = 1;
    let audioStarted = false;
    const worker = new BrowserRuntimeWorker(async () => ({
      abiVersion: 6,
      uiProtocolVersion: 2,
      runtimeConfigVersion: 1,
      create() { calls.push(["create"]); return nextHandle++; },
      audioOutputChannels(handle: number) { calls.push(["audioOutputChannels", handle]); return 2; },
      audioInputChannels(handle: number) { calls.push(["audioInputChannels", handle]); return 0; },
      initialize(handle: number, identity: unknown) { calls.push(["initialize", handle, identity]); return 0; },
      prepare(handle: number, sampleRate: number, blockSize: number) { calls.push(["prepare", handle, sampleRate, blockSize]); return 0; },
      process(handle: number, frames: number, timestampMicros: number) { calls.push(["process", handle, frames, timestampMicros]); return 0; },
      startAudioWorklet(handle: number, context?: AudioContext) { calls.push(["startAudioWorklet", handle, context ? "supplied" : "direct"]); audioStarted = true; return 0; },
      audioWorkletStats() { return { blocks: audioStarted ? 1 : 0, peakMicrounits: 0, deadlineMicrounits: 1 }; },
      messageTick(handle: number, timestampMicros: number) { calls.push(["messageTick", handle, timestampMicros]); return 0; },
      buildUiFrame(handle: number) { calls.push(["buildUiFrame", handle]); return Uint8Array.from(bytes).buffer; },
      dispatchAction(handle: number, name: string, value: string) { calls.push(["dispatchAction", handle, name, value]); return 0; },
      destroy(handle: number) { calls.push(["destroy", handle]); },
    }));

    await worker.handle({ type: "load", module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" } });
    await worker.handle({ type: "create" });
    await worker.handle({
      type: "initialize",
      identity: { publisherId: "example", appId: "portable-app", runtimeConfigVersion: 1 },
    });
    await worker.handle({ type: "prepare", sampleRate: 48000, blockSize: 128 });
    await worker.handle({ type: "process", frames: 128, timestampMicros: 10 });
    await worker.startAudioWorklet();
    await worker.handle({ type: "message-tick", timestampMicros: 11 });
    await worker.handle({ type: "dispatch-action", name: "generic.trigger", value: "pressed" });
    const uiFrame = await worker.handle({ type: "build-ui-frame" });
    await worker.handle({ type: "destroy" });
    const rejectedAfterDestroy = await worker.handle({ type: "build-ui-frame" });
    return { calls, uiFrame, rejectedAfterDestroy, html: document.body.innerHTML };
  }, Array.from(new Uint8Array(frame)));

  expect(result.uiFrame).toEqual({ type: "ui-frame", frame: Array.from(new Uint8Array(frame)) });
  expect(result.calls).toEqual([
    ["create"], ["initialize", 1, { publisherId: "example", appId: "portable-app", runtimeConfigVersion: 1 }],
    ["prepare", 1, 48000, 128], ["process", 1, 128, 10],
    ["audioInputChannels", 1],
    ["startAudioWorklet", 1, "direct"],
    ["messageTick", 1, 11], ["dispatchAction", 1, "generic.trigger", "pressed"],
    ["buildUiFrame", 1], ["buildUiFrame", 1], ["destroy", 1],
  ]);
  expect(result.rejectedAfterDestroy).toEqual({ type: "error", error: "runtime is destroyed" });
  expect(result.html).not.toMatch(/miniapp|fake-browser/i);
});

test("rejects browser input requests above the platform limit before native worklet startup", async ({ page }) => {
  await blockProductAutoBoot(page);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserRuntimeWorker } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const calls: string[] = [];
    let nativeStarted = false;
    const worker = new BrowserRuntimeWorker(async () => ({
      abiVersion: 6,
      uiProtocolVersion: 2,
      runtimeConfigVersion: 1,
      create() { calls.push("create"); return 1; },
      audioOutputChannels() { calls.push("audioOutputChannels"); return 2; },
      audioInputChannels() { calls.push("audioInputChannels"); return 33; },
      initialize() { calls.push("initialize"); return 0; },
      prepare() { calls.push("prepare"); return 0; },
      process() { calls.push("process"); return 0; },
      startAudioWorklet() { nativeStarted = true; calls.push("startAudioWorklet"); return 0; },
      audioWorkletStats() {
        calls.push("audioWorkletStats");
        return { blocks: nativeStarted ? 1 : 0, peakMicrounits: 0, deadlineMicrounits: 1 };
      },
      messageTick() { calls.push("messageTick"); return 0; },
      buildUiFrame() { calls.push("buildUiFrame"); return new ArrayBuffer(0); },
      dispatchAction() { calls.push("dispatchAction"); return 0; },
      destroy() { calls.push("destroy"); },
    }));

    await worker.handle({ type: "load", module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" } });
    await worker.handle({ type: "create" });
    const started = await worker.startAudioWorklet();
    return { started, calls };
  });

  expect(result).toEqual({
    started: {
      type: "error",
      error: "browser-audio-input-channel-limit-exceeded: requested 33, limit 32",
    },
    calls: ["create", "audioInputChannels"],
  });
});

test("normalizes a distinct worker time origin into the document engine epoch", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserRuntimeWorker } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const offsets: number[] = [];
    const worker = new BrowserRuntimeWorker(async () => ({
      abiVersion: 6,
      uiProtocolVersion: 2,
      runtimeConfigVersion: 1,
      create: () => 3,
      setTimestampEpochOffset: (_handle: number, offsetMicros: number) => { offsets.push(offsetMicros); return 0; },
      audioOutputChannels: () => 2,
      audioInputChannels: () => 0,
      initialize: () => 0,
      prepare: () => 0,
      process: () => 0,
      messageTick: () => 0,
      buildUiFrame: () => new ArrayBuffer(0),
      dispatchAction: () => 0,
      submitMidiEndpoints: () => 0,
      dequeueMidiAction: () => undefined,
      deliverMidi: () => 0,
      dequeueMidiOutput: () => undefined,
      midiDiagnostics: () => ({
        droppedImmediateOutputCount: 0,
        droppedScheduledOutputCount: 0,
        lateScheduledOutputCount: 0,
      }),
      destroy: () => {},
    }), undefined, undefined, () => 1_700_000_000_250);
    await worker.handle({
      type: "load",
      module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" },
    });
    const response = await worker.handle({
      type: "create",
      documentTimeOriginMillis: 1_700_000_000_000,
    });
    return { response, offsets };
  });

  expect(result).toEqual({
    response: { type: "created", handle: 3 },
    offsets: [250_000],
  });
});

test("reads Emscripten browser contract versions without creating a runtime", async ({ page }) => {
  await blockProductAutoBoot(page);
  await page.goto("http://127.0.0.1:4173/public/index.html");

  const result = await page.evaluate(async () => {
    const { emscriptenRuntimeFacade } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const calls: string[] = [];
    const facade = emscriptenRuntimeFacade({
      _synth_browser_abi_version() { calls.push("abiVersion"); return 6; },
      _synth_browser_ui_protocol_version() { calls.push("uiProtocolVersion"); return 2; },
      _synth_browser_runtime_config_version() { calls.push("runtimeConfigVersion"); return 1; },
      _synth_browser_create() { calls.push("create"); return 1; },
      emscriptenRegisterAudioObject() { calls.push("registerAudioContext"); return 91; },
      _synth_browser_start_audio_worklet() { calls.push("startAudioWorklet"); return 0; },
      _synth_browser_audio_input_channels() { calls.push("audioInputChannels"); return 4; },
      _synth_browser_set_audio_input_source() { calls.push("setAudioInputSource"); return 0; },
      _synth_browser_clear_audio_input_source() { calls.push("clearAudioInputSource"); return 0; },
      _synth_browser_consume_pending_audio_request() { calls.push("consumePendingAudioRequest"); return -1; },
    } as any);
    return {
      versions: {
        abiVersion: facade.abiVersion,
        uiProtocolVersion: facade.uiProtocolVersion,
        runtimeConfigVersion: facade.runtimeConfigVersion,
      },
      calls,
    };
  });

  expect(result.versions).toEqual({ abiVersion: 6, uiProtocolVersion: 2, runtimeConfigVersion: 1 });
  expect(result.calls).toEqual(["abiVersion", "uiProtocolVersion", "runtimeConfigVersion"]);
});

test("registers a supplied AudioContext module-locally and preserves direct handle zero", async ({ page }) => {
  await blockProductAutoBoot(page);
  await page.goto("http://127.0.0.1:4173/public/index.html");

  const result = await page.evaluate(async () => {
    const { emscriptenRuntimeFacade } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const calls: unknown[][] = [];
    const context = { sampleRate: 48_000 };
    const facade = emscriptenRuntimeFacade({
      _synth_browser_abi_version: () => 6,
      _synth_browser_ui_protocol_version: () => 2,
      _synth_browser_runtime_config_version: () => 1,
      emscriptenRegisterAudioObject(received: unknown) { calls.push(["register", received === context]); return 73; },
      _synth_browser_start_audio_worklet(handle: number, contextHandle: number) {
        calls.push(["start", handle, contextHandle]);
        return 0;
      },
      _synth_browser_audio_input_channels: () => 0,
      _synth_browser_set_audio_input_source: () => 0,
      _synth_browser_clear_audio_input_source: () => 0,
      _synth_browser_consume_pending_audio_request: () => -1,
    } as any);
    facade.startAudioWorklet(41);
    facade.startAudioWorklet(41, context);
    return calls;
  });

  expect(result).toEqual([
    ["start", 41, 0],
    ["register", true],
    ["start", 41, 73],
  ]);
});

test("registers a native AudioNode input source with a positive physical channel count", async ({ page }) => {
  await blockProductAutoBoot(page);
  await page.goto("http://127.0.0.1:4173/public/index.html");

  const result = await page.evaluate(async () => {
    const { emscriptenRuntimeFacade } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const calls: unknown[][] = [];
    const audioNode = { channelCount: 4 };
    // Only the new consume method needs real heap/malloc plumbing: it is the
    // one facade method in this test that writes an out-param through the
    // module's memory rather than returning a plain value.
    const heap = new Uint8Array(64);
    const facade = emscriptenRuntimeFacade({
      HEAPU8: heap,
      _malloc: () => 0,
      _free: () => {},
      _synth_browser_abi_version: () => 6,
      _synth_browser_ui_protocol_version: () => 2,
      _synth_browser_runtime_config_version: () => 1,
      emscriptenRegisterAudioObject(received: unknown) { calls.push(["register", received === audioNode]); return 96; },
      _synth_browser_start_audio_worklet: () => 0,
      _synth_browser_audio_input_channels: () => 4,
      _synth_browser_set_audio_input_source(handle: number, sourceHandle: number, physicalChannels: number, statusCode: number) {
        calls.push(["set", handle, sourceHandle, physicalChannels, statusCode]);
        return 0;
      },
      _synth_browser_clear_audio_input_source(handle: number, statusCode: number) {
        calls.push(["clear", handle, statusCode]);
        return 0;
      },
      _synth_browser_consume_pending_audio_request(handle: number, outControl: number) {
        calls.push(["pendingAudioRequest", handle]);
        // 1 = BrowserAudioDeviceKind::Output, chosen so a passing assertion on
        // the facade's returned control proves it really read this write back
        // rather than coincidentally matching an unwritten zero.
        new DataView(heap.buffer).setUint32(outControl, 1, true);
        return 2;
      },
    } as any);
    const inputChannels = facade.audioInputChannels(41);
    let rejectedZero = "";
    try {
      facade.setAudioInputSource(41, audioNode, 0, 2);
    } catch (error) {
      rejectedZero = (error as Error).message;
    }
    let rejectedTooMany = "";
    try {
      facade.setAudioInputSource(41, audioNode, 33, 2);
    } catch (error) {
      rejectedTooMany = (error as Error).message;
    }
    let rejectedBadStatus = "";
    try {
      facade.setAudioInputSource(41, audioNode, 4, 11);
    } catch (error) {
      rejectedBadStatus = (error as Error).message;
    }
    let rejectedClearStatus = "";
    try {
      facade.clearAudioInputSource(41, 11);
    } catch (error) {
      rejectedClearStatus = (error as Error).message;
    }
    const handleBeforeRegistration = facade.audioInputSourceHandle(audioNode);
    const setResult = facade.setAudioInputSource(41, audioNode, 4, 2);
    const handleAfterRegistration = facade.audioInputSourceHandle(audioNode);
    const repeatSetResult = facade.setAudioInputSource(41, audioNode, 4, 2);
    const handleAfterRepeat = facade.audioInputSourceHandle(audioNode);
    const otherNodeHandle = facade.audioInputSourceHandle({} as AudioNode);
    const clearResult = facade.clearAudioInputSource(41, 6);
    const pendingAudioRequest = facade.consumePendingAudioRequest(41);
    return {
      inputChannels,
      setResult,
      repeatSetResult,
      clearResult,
      pendingAudioRequest,
      handleBeforeRegistration,
      handleAfterRegistration,
      handleAfterRepeat,
      otherNodeHandle,
      rejectedZero,
      rejectedTooMany,
      rejectedBadStatus,
      rejectedClearStatus,
      calls,
    };
  });

  expect(result).toEqual({
    inputChannels: 4,
    setResult: 0,
    repeatSetResult: 0,
    clearResult: 0,
    pendingAudioRequest: { index: 2, control: 1 },
    rejectedZero: expect.stringMatching(/physical.*between 1 and 32/i),
    rejectedTooMany: expect.stringMatching(/physical.*between 1 and 32/i),
    rejectedBadStatus: expect.stringMatching(/status.*between 0 and 10/i),
    rejectedClearStatus: expect.stringMatching(/status.*between 0 and 10/i),
    // The module-local cache is the single owner of the handle: it is minted on
    // first registration, read back unchanged afterwards, and never invented for
    // a node that was never registered.
    handleBeforeRegistration: 0,
    handleAfterRegistration: 96,
    handleAfterRepeat: 96,
    otherNodeHandle: 0,
    calls: [
      ["register", true],
      ["set", 41, 96, 4, 2],
      ["set", 41, 96, 4, 2],
      ["clear", 41, 6],
      ["pendingAudioRequest", 41],
    ],
  });
});

test("rejects modules missing context registration, native startup, or audio input exports", async ({ page }) => {
  await blockProductAutoBoot(page);
  await page.goto("http://127.0.0.1:4173/public/index.html");

  const result = await page.evaluate(async () => {
    const { emscriptenRuntimeFacade } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const failures: string[] = [];
    for (const omitted of ["register", "start", "channels", "set", "clear", "retry"]) {
      try {
        emscriptenRuntimeFacade({
          _synth_browser_abi_version: () => 6,
          _synth_browser_ui_protocol_version: () => 2,
          _synth_browser_runtime_config_version: () => 1,
          emscriptenRegisterAudioObject: omitted === "register" ? undefined : () => 1,
          _synth_browser_start_audio_worklet: omitted === "start" ? undefined : () => 0,
          _synth_browser_audio_input_channels: omitted === "channels" ? undefined : () => 0,
          _synth_browser_set_audio_input_source: omitted === "set" ? undefined : () => 0,
          _synth_browser_clear_audio_input_source: omitted === "clear" ? undefined : () => 0,
          _synth_browser_consume_pending_audio_request: omitted === "retry" ? undefined : () => 0,
        } as any);
      } catch (error) {
        failures.push((error as Error).message);
      }
    }
    return failures;
  });

  expect(result).toEqual([
    expect.stringMatching(/context registration/i),
    expect.stringMatching(/native AudioWorklet startup/i),
    expect.stringMatching(/audio input channels/i),
    expect.stringMatching(/audio input source/i),
    expect.stringMatching(/audio input source clear/i),
    expect.stringMatching(/pending audio request/i),
  ]);
});

test("rejects incompatible modules before creation or persistence setup", async ({ page }) => {
  await blockProductAutoBoot(page);
  await page.goto("http://127.0.0.1:4173/public/index.html");

  const results = await page.evaluate(async () => {
    const { BrowserRuntimeWorker } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const fields = ["abiVersion", "uiProtocolVersion", "runtimeConfigVersion"];
    return await Promise.all(fields.map(async (field) => {
      const calls: string[] = [];
      const supported = { abiVersion: 6, uiProtocolVersion: 2, runtimeConfigVersion: 1 };
      const versions = { ...supported, [field]: supported[field as keyof typeof supported] === 2 ? 1 : 2 };
      const worker = new BrowserRuntimeWorker(
        async () => ({
          ...versions,
          filesystem: {},
          create() { calls.push("create"); return 1; },
        }),
        () => {
          calls.push("persistence");
          throw new Error("persistence must not be created");
        },
      );
      const loaded = await worker.handle({ type: "load", module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" } });
      const created = await worker.handle({ type: "create" });
      return { field, loaded, created, calls };
    }));
  });

  for (const result of results) {
    expect(result.loaded).toEqual({
      type: "error",
      error: expect.stringMatching(new RegExp(`${result.field}.*incompatible|${result.field}.*mismatch`, "i")),
    });
    expect(result.created).toEqual({ type: "error", error: "runtime module is not loaded" });
    expect(result.calls).toEqual([]);
  }
});

test("main bootstrap composes runtime, UI, audio channels, and actions generically", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  const frame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 120, 50], children: ["button"] },
    { id: "button", kind: NodeKind.Button, bounds: [0, 0, 120, 40], label: "Booted", action: { name: "generic.boot", value: "pressed" } },
  ]);

  const result = await page.evaluate(async (bytes) => {
    const { installSynthBrowserApp } = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const calls: Array<[string, ...unknown[]]> = [];
    let audioStarted = false;
    const app = await installSynthBrowserApp(document.querySelector("#synth-root")!, {
      module: { entryUrl: "blob:test-app", locateFile: {}, mainScriptUrlOrBlob: "blob:test-app" },
      frameIntervalMs: 100000,
      runtimeModuleLoader: async () => ({
        abiVersion: 6,
        uiProtocolVersion: 2,
        runtimeConfigVersion: 1,
        filesystem: {
          filesystems: { IDBFS: "idbfs" },
          mkdir() {},
          mount() {},
          syncfs(_populate: boolean, complete: () => void) { complete(); },
        },
        create() { calls.push(["create"]); return 11; },
        audioOutputChannels(handle: number) { calls.push(["audioOutputChannels", handle]); return 1; },
        audioInputChannels() { return 0; },
        initialize(handle: number, identity: unknown) { calls.push(["initialize", handle, identity]); return 0; },
        prepare(handle: number, sampleRate: number, blockSize: number) { calls.push(["prepare", handle, sampleRate, blockSize]); return 0; },
        process() { return 0; },
        startAudioWorklet(handle: number, context?: AudioContext) { calls.push(["startAudioWorklet", handle, context ? "supplied" : "direct"]); audioStarted = true; return 0; },
        audioWorkletStats() { return { blocks: audioStarted ? 1 : 0, peakMicrounits: 125_000, deadlineMicrounits: 1 }; },
        messageTick() { return 0; },
        buildUiFrame(handle: number) { calls.push(["buildUiFrame", handle]); return Uint8Array.from(bytes).buffer; },
        dispatchAction(handle: number, name: string, value: string) { calls.push(["dispatchAction", handle, name, value]); return 0; },
        submitMidiEndpoints() { return 0; },
        dequeueMidiAction() { return undefined; },
        deliverMidi() { return 0; },
        dequeueMidiOutput() { return undefined; },
        destroy(handle: number) { calls.push(["destroy", handle]); },
      }),
      audioOptions: {
        audioContextFactory: () => ({
          sampleRate: 48000,
          destination: {},
          audioWorklet: { addModule: async () => {} },
          resume: async () => {},
        }),
        audioWorkletNodeFactory: () => ({ connect() {}, disconnect() {} }),
      },
    });
    document.querySelector<HTMLElement>('[data-synth-node-id="button"]')!.click();
    await new Promise((resolve) => setTimeout(resolve, 20));
    app.stop();
    await new Promise((resolve) => setTimeout(resolve, 0));
    return { calls, text: document.querySelector('[data-synth-node-id="button"]')?.textContent, status: (document.querySelector("#synth-root") as HTMLElement).dataset.synthStatus };
  }, Array.from(new Uint8Array(frame)));

  expect(result.text).toBe("Booted");
  expect(result.status).toMatch(/audio:online; midi:(online|offline)/);
  expect(result.calls).toContainEqual(["audioOutputChannels", 11]);
  expect(result.calls).toContainEqual(["startAudioWorklet", 11, "direct"]);
  expect(result.calls).not.toContainEqual(["prepare", 11, 48000, 128]);
  expect(result.calls.some((call) => call[0] === "renderAudio")).toBe(false);
  expect(result.calls).toContainEqual(["dispatchAction", 11, "generic.boot", "pressed"]);
  expect(result.calls).toContainEqual(["destroy", 11]);
});

test("direct runtime installation supersedes delayed launcher auto-boot across fresh module evaluation", async ({ page }) => {
  const catalogUrl = "https://publisher.example/delayed-catalog.json";
  const pendingCatalogs: Array<import("@playwright/test").Route> = [];
  await page.route("http://127.0.0.1:4173/catalog-sources.json", (route) => route.fulfill({ json: [catalogUrl] }));
  await page.route(catalogUrl, (route) => { pendingCatalogs.push(route); });
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await expect.poll(() => pendingCatalogs.length).toBe(1);
  const frame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 120, 50], children: ["button"] },
    { id: "button", kind: NodeKind.Button, bounds: [0, 0, 120, 40], label: "Direct owner", action: { name: "generic.direct", value: "pressed" } },
  ]);

  await page.evaluate(async (bytes) => {
    const main = await (new Function("return import('/dist/src/main.js?review-direct-owner')")() as Promise<any>);
    const runtimeClient = {
      async request(command: { type: string }) {
        if (command.type === "audio-config") return { type: "audio-config", channels: 2 };
        if (command.type === "build-ui-frame") return { type: "ui-frame", frame: bytes };
        return { type: "ok" };
      },
      terminate() {},
    };
    (window as any).__reviewDirectApp = await main.installSynthBrowserApp(
      document.querySelector("#synth-root"),
      {
        module: { entryUrl: "blob:direct-owner", locateFile: {}, mainScriptUrlOrBlob: "blob:direct-owner" },
        runtimeClient,
        frameIntervalMs: 60_000,
      },
    );
  }, Array.from(new Uint8Array(frame)));
  await expect(page.locator('[data-synth-node-id="button"]')).toHaveText("Direct owner");
  await expect.poll(() => pendingCatalogs.length).toBe(2);

  const response = {
    schemaVersion: 1,
    catalogVersion: "revision-1",
    publisher: { id: "example", name: "Example" },
    apps: [{
      appId: "portable-app",
      displayName: "Portable App",
      author: "Example",
      category: "Instrument",
      buildId: "portable-app-build-1",
      browser: {
        abiVersion: 6,
        uiProtocolVersion: 2,
        runtimeConfigVersion: 1,
        entry: "packages/portable-app/portable-app-build-1/app.js",
        files: [{
          path: "packages/portable-app/portable-app-build-1/app.js",
          mediaType: "text/javascript",
          size: 1,
          sha256: "0123456789abcdef".repeat(4),
        }],
      },
    }],
  };
  await Promise.all(pendingCatalogs.map((route) => route.fulfill({ json: response })));

  await expect(page.locator('[data-synth-node-id="button"]')).toHaveText("Direct owner");
  await expect(page.getByRole("heading", { name: "SheafPatch" })).toHaveCount(0);
  await page.evaluate(() => (window as any).__reviewDirectApp.stop());
});

test("production bootstrap discovers catalogs without loading an application module", async ({ page }) => {
  const catalogUrl = "https://publisher.example/catalog.json";
  const requested: string[] = [];
  await page.route("http://127.0.0.1:4173/catalog-sources.json", (route) => route.fulfill({ json: [catalogUrl] }));
  await page.route(catalogUrl, (route) => route.fulfill({
    json: {
      schemaVersion: 1,
      catalogVersion: "revision-1",
      publisher: { id: "example", name: "Example" },
      apps: [{
        appId: "portable-app",
        displayName: "Portable App",
        author: "Example",
        category: "Instrument",
        buildId: "portable-app-build-1",
        browser: {
          abiVersion: 6,
          uiProtocolVersion: 2,
          runtimeConfigVersion: 1,
          entry: "packages/portable-app/portable-app-build-1/app.js",
          files: [{
            path: "packages/portable-app/portable-app-build-1/app.js",
            mediaType: "text/javascript",
            size: 1,
            sha256: "0123456789abcdef".repeat(4),
          }],
        },
      }],
    },
  }));
  page.on("request", (request) => requested.push(request.url()));

  await page.goto("http://127.0.0.1:4173/public/index.html");
  await expect(page.getByRole("button", { name: /launch portable app/i })).toBeEnabled();

  expect(requested).toContain("http://127.0.0.1:4173/catalog-sources.json");
  expect(requested).toContain(catalogUrl);
  expect(requested).not.toContain("http://127.0.0.1:4173/dist/wasm/app.js");
  expect(requested.filter((url) => /\/packages\//.test(url))).toEqual([]);
});

test("browser worker contains no concrete application branch", async ({ page }) => {
  const forbidden = /MiniApp|miniapp|synth_miniapp|Vco|FilterModule|LfoBank/;
  await blockProductAutoBoot(page);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const source = await page.evaluate(async () => (await fetch("http://127.0.0.1:4173/src/worker.ts")).text());
  expect(source).not.toMatch(forbidden);
});
