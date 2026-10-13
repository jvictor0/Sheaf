import { expect, test } from "@playwright/test";
import type { MidiOutput } from "../src/protocol.js";
import { makeCommandBuffer, NodeKind } from "./fixtures/command-buffer.js";

// Shared by the two tests below that install a real `SynthBrowserApp` (through
// `main.ts`'s own dispatch wiring, not a directly constructed
// `BrowserMidiManager`) against a fake runtime module: one non-Controllers
// button and one Controllers-sidebar button, so a click can be dispatched by
// name.
function genericAndControllersFrameBytes(): number[] {
  const frame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 80], children: ["generic", "controllers"] },
    { id: "generic", kind: NodeKind.Button, bounds: [0, 0, 200, 40], label: "Generic", action: { name: "generic.trigger", value: "pressed" } },
    { id: "controllers", kind: NodeKind.Button, bounds: [0, 40, 200, 40], label: "Controllers", action: { name: "runtime.sidebar.controllers", value: "" } },
  ]);
  return Array.from(new Uint8Array(frame));
}

test("requests MIDI only when the Controllers sidebar action dispatches", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    const { installSynthBrowserApp } = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const midiRequests: unknown[] = [];
    (navigator as any).requestMIDIAccess = async (options: unknown) => {
      midiRequests.push(options);
      return { inputs: new Map(), outputs: new Map(), onstatechange: null };
    };
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
        create() { return 11; },
        audioOutputChannels() { return 1; },
        audioInputChannels() { return 0; },
        initialize() { return 0; },
        prepare() { return 0; },
        process() { return 0; },
        // `worker.ts`'s `startAudioWorklet` polls `audioWorkletStats().blocks`
        // for an increase over its pre-call reading before it resolves "ok";
        // `blocks` must actually move from 0 to 1 once started; a stub that
        // reports 1 unconditionally never satisfies that poll and stalls for
        // its whole 5s deadline instead of resolving on the first tick.
        startAudioWorklet() { audioStarted = true; return 0; },
        audioWorkletStats() { return { blocks: audioStarted ? 1 : 0, peakMicrounits: 0, deadlineMicrounits: 1 }; },
        messageTick() { return 0; },
        buildUiFrame() { return Uint8Array.from(bytes).buffer; },
        dispatchAction() { return 0; },
        submitMidiEndpoints() { return 0; },
        dequeueMidiAction() { return undefined; },
        deliverMidi() { return 0; },
        dequeueMidiOutput() { return undefined; },
        destroy() {},
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
    document.querySelector<HTMLElement>('[data-synth-node-id="generic"]')!.click();
    await new Promise((resolve) => setTimeout(resolve, 20));
    const afterGeneric = {
      midiRequests: midiRequests.length,
      status: (document.querySelector("#synth-root") as HTMLElement).dataset.synthStatus,
    };
    document.querySelector<HTMLElement>('[data-synth-node-id="controllers"]')!.click();
    await new Promise((resolve) => setTimeout(resolve, 20));
    const afterControllers = { midiRequests: midiRequests.length };
    app.stop();
    return { afterGeneric, afterControllers };
  }, genericAndControllersFrameBytes());

  expect(result.afterGeneric).toEqual({ midiRequests: 0, status: "audio:online; midi:offline" });
  expect(result.afterControllers).toEqual({ midiRequests: 1 });
});

test("starts MIDI at load only when the Permissions API already reports it granted", async ({ page }) => {
  const bytes = genericAndControllersFrameBytes();

  async function runWithPermissionState(state: "granted" | "prompt" | "throws") {
    await page.goto("http://127.0.0.1:4173/public/index.html");
    return page.evaluate(async ({ bytes, state }) => {
      const { installSynthBrowserApp } = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
      const midiRequests: unknown[] = [];
      (navigator as any).requestMIDIAccess = async (options: unknown) => {
        midiRequests.push(options);
        return { inputs: new Map(), outputs: new Map(), onstatechange: null };
      };
      // `navigator.permissions` is a getter-only accessor on the real
      // Navigator prototype; a plain assignment silently no-ops instead of
      // replacing it, so the stub is installed with `defineProperty`.
      Object.defineProperty(navigator, "permissions", {
        configurable: true,
        value: {
          query: async () => {
            if (state === "throws") throw new Error("permissions query unavailable");
            return { state };
          },
        },
      });
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
          create() { return 11; },
          audioOutputChannels() { return 1; },
          audioInputChannels() { return 0; },
          initialize() { return 0; },
          prepare() { return 0; },
          process() { return 0; },
          startAudioWorklet() { audioStarted = true; return 0; },
          audioWorkletStats() { return { blocks: audioStarted ? 1 : 0, peakMicrounits: 0, deadlineMicrounits: 1 }; },
          messageTick() { return 0; },
          buildUiFrame() { return Uint8Array.from(bytes).buffer; },
          dispatchAction() { return 0; },
          submitMidiEndpoints() { return 0; },
          dequeueMidiAction() { return undefined; },
          deliverMidi() { return 0; },
          dequeueMidiOutput() { return undefined; },
          destroy() {},
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
      await new Promise((resolve) => setTimeout(resolve, 20));
      const root = document.querySelector("#synth-root") as HTMLElement;
      const afterLoad = { midiRequests: midiRequests.length, status: root.dataset.synthStatus };
      let afterControllers: { midiRequests: number } | undefined;
      if (state === "prompt") {
        document.querySelector<HTMLElement>('[data-synth-node-id="controllers"]')!.click();
        await new Promise((resolve) => setTimeout(resolve, 20));
        afterControllers = { midiRequests: midiRequests.length };
      }
      app.stop();
      return { afterLoad, afterControllers };
    }, { bytes, state });
  }

  const granted = await runWithPermissionState("granted");
  const prompt = await runWithPermissionState("prompt");
  const throws = await runWithPermissionState("throws");

  expect(granted.afterLoad).toEqual({ midiRequests: 1, status: "midi:online" });
  expect(prompt.afterLoad).toEqual({ midiRequests: 0, status: "running" });
  expect(prompt.afterControllers).toEqual({ midiRequests: 1 });
  // A rejected query must not stall or fail boot: `start()`'s own final
  // render is reached and remains "running" (a throw would leave it either
  // unset or holding an error status instead).
  expect(throws.afterLoad).toEqual({ midiRequests: 0, status: "running" });
});

const scheduledOutputContract = {
  controllerIx: 1,
  bytes: [0xfb],
  delivery: "scheduled",
  dueTimeMicros: 1_250_000,
} satisfies MidiOutput;

test("forwards generic MIDI commands through the runtime worker", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserRuntimeWorker } = await (new Function("return import('/dist/src/worker.js')")() as Promise<{
      BrowserRuntimeWorker: new (loadModule: unknown) => { handle(command: unknown): Promise<unknown> };
    }>);
    const calls: unknown[] = [];
    const actions = [{ type: "open-input", controllerIx: 1, identifier: "in-b", name: "Input B" }];
    const worker = new BrowserRuntimeWorker(async () => ({
      abiVersion: 6,
      uiProtocolVersion: 2,
      runtimeConfigVersion: 1,
      create: () => 7,
      audioOutputChannels: () => 2,
      initialize: () => 0,
      prepare: () => 0,
      process: () => 0,
      messageTick: () => 0,
      buildUiFrame: () => new ArrayBuffer(0),
      dispatchAction: () => 0,
      submitMidiEndpoints: (_handle: number, endpoints: unknown) => { calls.push(["endpoints", endpoints]); return 0; },
      dequeueMidiAction: () => actions.shift(),
      deliverMidi: (_handle: number, controllerIx: number, bytes: number[], timestampMicros: number) => { calls.push(["input", controllerIx, bytes, timestampMicros]); return 0; },
      dequeueMidiOutput: () => ({ controllerIx: 1, bytes: [0xfb], delivery: "scheduled", dueTimeMicros: 1_250_000 }),
      destroy: () => {},
    }));
    await worker.handle({ type: "load", module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" } });
    await worker.handle({ type: "create" });
    const endpointResponse = await worker.handle({ type: "midi-endpoints", endpoints: [{ identifier: "in-b", name: "Input B", kind: "input" }] });
    const inputResponse = await worker.handle({ type: "midi-input", controllerIx: 1, bytes: [0xf0, 0x7d, 0x33, 0xf7], timestampMicros: 42 });
    const outputResponse = await worker.handle({ type: "drain-midi-output" });
    return { endpointResponse, inputResponse, outputResponse, calls };
  });

  expect(result.endpointResponse).toEqual({ type: "midi-actions", actions: [{ type: "open-input", controllerIx: 1, identifier: "in-b", name: "Input B" }] });
  expect(result.inputResponse).toEqual({ type: "ok" });
  expect(result.outputResponse).toEqual({ type: "midi-output", output: scheduledOutputContract });
  expect(result.calls).toEqual([
    ["endpoints", [{ identifier: "in-b", name: "Input B", kind: "input" }]],
    ["input", 1, [0xf0, 0x7d, 0x33, 0xf7], 42],
  ]);
});

test("normalizes Web MIDI timestamps into the shared time-origin-relative microsecond epoch", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserMidiManager } = await (new Function("return import('/dist/src/midi.js')")() as Promise<any>);
    class InputPort {
      readonly state = "connected";
      onmidimessage: ((event: { data: Uint8Array; timeStamp: number }) => void) | null = null;
      constructor(readonly id: string, readonly name: string) {}
      emit(bytes: number[], timeStamp: number) {
        this.onmidimessage?.({ data: Uint8Array.from(bytes), timeStamp });
      }
    }
    const input = new InputPort("in-clock", "Clock Input");
    const access = { inputs: new Map([[input.id, input]]), outputs: new Map(), onstatechange: null };
    const delivered: Array<{ controllerIx: number; bytes: number[]; timestampMicros: number }> = [];
    const runtime = {
      submitEndpoints: async () => [
        { type: "open-input", controllerIx: 3, identifier: input.id, name: input.name },
      ],
      deliverMidi: async (controllerIx: number, bytes: number[], timestampMicros: number) => {
        delivered.push({ controllerIx, bytes, timestampMicros });
      },
      dequeueMidiOutput: async () => undefined,
    };
    const manager = new BrowserMidiManager(runtime, {
      requestMIDIAccess: async () => access,
      setInterval: () => 1,
      clearInterval: () => {},
      timeOriginMillis: 1_700_000_000_000,
      nowMicros: () => 19_000,
    });
    await manager.startFromUserActivation();
    input.emit([0xfa], 17.25);
    input.emit([0xf8], 1_700_000_000_018.5);
    input.emit([0xfc], Number.NaN);
    await new Promise((resolve) => setTimeout(resolve, 0));
    manager.stop();
    return delivered;
  });

  expect(result).toEqual([
    { controllerIx: 3, bytes: [0xfa], timestampMicros: 17_250 },
    { controllerIx: 3, bytes: [0xf8], timestampMicros: 18_500 },
    { controllerIx: 3, bytes: [0xfc], timestampMicros: 19_000 },
  ]);
});

test("uses stored deadlines for Web MIDI scheduling and reports timer-throttled lateness", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserMidiManager } = await (new Function("return import('/dist/src/midi.js')")() as Promise<any>);
    class OutputPort {
      readonly state = "connected";
      readonly sent: Array<{ bytes: number[]; timestamp?: number }> = [];
      constructor(readonly id: string, readonly name: string) {}
      send(bytes: number[] | Uint8Array, timestamp?: number) {
        this.sent.push({ bytes: Array.from(bytes), timestamp });
      }
    }
    const output = new OutputPort("out-clock", "Clock Output");
    const access = { inputs: new Map(), outputs: new Map([[output.id, output]]), onstatechange: null };
    const queue = [
      { controllerIx: 0, bytes: [0xfa], delivery: "scheduled", dueTimeMicros: 1_250_000 },
      { controllerIx: 0, bytes: [0xf8], delivery: "scheduled", dueTimeMicros: 1_250_000 },
      { controllerIx: 0, bytes: [0x90, 0x40, 0x7f], delivery: "immediate", dueTimeMicros: 0 },
    ];
    const runtime = {
      submitEndpoints: async () => [
        { type: "open-output", controllerIx: 0, identifier: output.id, name: output.name },
      ],
      deliverMidi: async () => {},
      dequeueMidiOutput: async () => queue.shift(),
    };
    const manager = new BrowserMidiManager(runtime, {
      requestMIDIAccess: async () => access,
      setInterval: () => 1,
      clearInterval: () => {},
      nowMicros: () => 1_300_000,
    });
    await manager.startFromUserActivation();
    const diagnostics = manager.diagnostics();
    manager.stop();
    return { sent: output.sent, diagnostics };
  });

  expect(result.sent).toEqual([
    { bytes: [0xfa], timestamp: 1_250 },
    { bytes: [0xf8], timestamp: 1_250 },
    { bytes: [0x90, 0x40, 0x7f] },
  ]);
  expect(result.diagnostics).toEqual({
    droppedImmediateOutputCount: 0,
    droppedScheduledOutputCount: 0,
    bridgeLateScheduledOutputCount: 0,
    lateScheduledOutputCount: 2,
    sendErrorCount: 0,
  });
});

test("requests Web MIDI sysex permission and remains offline when it is denied", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserMidiManager } = await (new Function("return import('/dist/src/midi.js')")() as Promise<{
      BrowserMidiManager: new (runtime: unknown, options: unknown) => { startFromUserActivation(): Promise<unknown>; status(): string };
    }>);
    const requests: unknown[] = [];
    const uiAndAudioRemainRunning = { ui: true, audio: true };
    const runtime = { submitEndpoints: async () => [], deliverMidi: async () => {}, dequeueMidiOutput: async () => undefined };
    const manager = new BrowserMidiManager(runtime, {
      requestMIDIAccess: async (options: unknown) => {
        requests.push(options);
        throw new Error("sysex denied");
      },
      setInterval: () => 1,
      clearInterval: () => {},
    });
    const start = await manager.startFromUserActivation();
    return { requests, start, status: manager.status(), uiAndAudioRemainRunning };
  });

  expect(result.requests).toEqual([{ sysex: true }]);
  expect(result.start).toEqual({ status: "offline", reason: "sysex denied" });
  expect(result.status).toBe("offline");
  expect(result.uiAndAudioRemainRunning).toEqual({ ui: true, audio: true });
});

test("overlapping activation calls share one MIDI request and report the same outcome", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserMidiManager } = await (new Function("return import('/dist/src/midi.js')")() as Promise<{
      BrowserMidiManager: new (runtime: unknown, options: unknown) => { startFromUserActivation(): Promise<{ status: string }>; status(): string };
    }>);
    const requests: unknown[] = [];
    let resolveAccess: (access: unknown) => void;
    const access = new Promise((resolve) => { resolveAccess = resolve; });
    const runtime = { submitEndpoints: async () => [], deliverMidi: async () => {}, dequeueMidiOutput: async () => undefined };
    const manager = new BrowserMidiManager(runtime, {
      requestMIDIAccess: async (options: unknown) => {
        requests.push(options);
        return access;
      },
      setInterval: () => 1,
      clearInterval: () => {},
    });
    // Both calls fire before either has a `MIDIAccess` to guard re-entry on,
    // the same race a second Controllers click (or the load-time saved-grant
    // start) produces against a still-open permission prompt.
    const first = manager.startFromUserActivation();
    const second = manager.startFromUserActivation();
    resolveAccess!({ inputs: new Map(), outputs: new Map(), onstatechange: null });
    const [firstResult, secondResult] = await Promise.all([first, second]);
    return { requestCount: requests.length, firstResult, secondResult, status: manager.status() };
  });

  expect(result.requestCount).toBe(1);
  expect(result.firstResult).toEqual({ status: "online" });
  expect(result.secondResult).toEqual({ status: "online" });
  expect(result.status).toBe("online");
});

test("reconciles leased MIDI access without requesting permission a second time", async ({ page }) => {
  await page.goto("http://127.0.0.1:4174/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserMidiManager } = await (new Function("return import('/dist/src/midi.js')")() as Promise<any>);
    const calls: unknown[] = [];
    const access = {
      inputs: new Map([["input", { id: "input", name: "Input", state: "connected", onmidimessage: null }]]),
      outputs: new Map([["output", { id: "output", name: "Output", state: "connected", send() {} }]]),
      onstatechange: null,
    };
    const manager = new BrowserMidiManager({
      async submitEndpoints(endpoints: unknown) { calls.push(["endpoints", endpoints]); return []; },
      async deliverMidi() {},
      async dequeueMidiOutput() { return undefined; },
    }, {
      requestMIDIAccess: async () => { calls.push("permission-request"); return access; },
      setInterval: () => 1,
      clearInterval: () => {},
    });
    const started = await manager.startWithAccess(access);
    manager.stop();
    return { started, calls };
  });

  expect(result.started).toEqual({ status: "online" });
  expect(result.calls).toEqual([["endpoints", [
    { identifier: "input", name: "Input", kind: "input" },
    { identifier: "output", name: "Output", kind: "output" },
  ]]]);
});

test("routes sysex between selected ports and their independent controller slots", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserMidiManager } = await (new Function("return import('/dist/src/midi.js')")() as Promise<{
      BrowserMidiManager: new (runtime: unknown, options: unknown) => { startFromUserActivation(): Promise<unknown>; drainOutputs(): Promise<void>; stop(): void };
    }>);
    class InputPort {
      readonly type = "input";
      readonly state = "connected";
      onmidimessage: ((event: { data: Uint8Array; timeStamp: number }) => void) | null = null;
      constructor(readonly id: string, readonly name: string) {}
      emit(bytes: number[]) { this.onmidimessage?.({ data: Uint8Array.from(bytes), timeStamp: 17 }); }
    }
    class OutputPort {
      readonly type = "output";
      readonly state = "connected";
      readonly sent: number[][] = [];
      constructor(readonly id: string, readonly name: string) {}
      send(bytes: number[] | Uint8Array) { this.sent.push(Array.from(bytes)); }
    }
    const inputA = new InputPort("in-a", "Input A");
    const inputB = new InputPort("in-b", "Input B");
    const outputA = new OutputPort("out-a", "Output A");
    const outputB = new OutputPort("out-b", "Output B");
    const access = { inputs: new Map([[inputA.id, inputA], [inputB.id, inputB]]), outputs: new Map([[outputA.id, outputA], [outputB.id, outputB]]), onstatechange: null };
    const delivered: Array<{ controllerIx: number; bytes: number[]; timestampMicros: number }> = [];
    const outputQueue = [{ controllerIx: 1, bytes: [0xf0, 0x7d, 0x44, 0xf7] }];
    const runtime = {
      submitEndpoints: async () => [
        { type: "open-input", controllerIx: 0, identifier: "in-a", name: "Input A" },
        { type: "open-input", controllerIx: 1, identifier: "in-b", name: "Input B" },
        { type: "open-output", controllerIx: 0, identifier: "out-a", name: "Output A" },
        { type: "open-output", controllerIx: 1, identifier: "out-b", name: "Output B" },
        { type: "resync", controllerIx: 0 },
        { type: "resync", controllerIx: 1 },
      ],
      deliverMidi: async (controllerIx: number, bytes: number[], timestampMicros: number) => { delivered.push({ controllerIx, bytes, timestampMicros }); },
      dequeueMidiOutput: async () => outputQueue.shift(),
    };
    const manager = new BrowserMidiManager(runtime, {
      requestMIDIAccess: async () => access,
      setInterval: () => 1,
      clearInterval: () => {},
      nowMicros: () => 123,
    });
    await manager.startFromUserActivation();
    inputB.emit([0xf0, 0x7d, 0x33, 0xf7]);
    await new Promise((resolve) => setTimeout(resolve, 0));
    await manager.drainOutputs();
    const handlerA = inputA.onmidimessage !== null;
    const handlerB = inputB.onmidimessage !== null;
    manager.stop();
    return { delivered, sentA: outputA.sent, sentB: outputB.sent, handlerA, handlerB };
  });

  expect(result.handlerA).toBe(true);
  expect(result.handlerB).toBe(true);
  expect(result.delivered).toEqual([{ controllerIx: 1, bytes: [0xf0, 0x7d, 0x33, 0xf7], timestampMicros: 17000 }]);
  expect(result.sentA).toEqual([]);
  expect(result.sentB).toEqual([[0xf0, 0x7d, 0x44, 0xf7]]);
});

test("polling recovers missed port changes without remapping another slot", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserMidiManager } = await (new Function("return import('/dist/src/midi.js')")() as Promise<{
      BrowserMidiManager: new (runtime: unknown, options: unknown) => { startFromUserActivation(): Promise<unknown>; poll(): Promise<void>; stop(): void };
    }>);
    class InputPort {
      readonly type = "input";
      state = "connected";
      onmidimessage: ((event: { data: Uint8Array; timeStamp: number }) => void) | null = null;
      constructor(readonly id: string, readonly name: string) {}
    }
    class OutputPort {
      readonly type = "output";
      state = "connected";
      constructor(readonly id: string, readonly name: string) {}
      send(_bytes: number[] | Uint8Array) {}
    }
    const inputA = new InputPort("in-a", "Input A");
    const inputB = new InputPort("in-b", "Input B");
    const outputA = new OutputPort("out-a", "Output A");
    const outputB = new OutputPort("out-b", "Output B");
    const access = { inputs: new Map([[inputA.id, inputA], [inputB.id, inputB]]), outputs: new Map([[outputA.id, outputA], [outputB.id, outputB]]), onstatechange: null };
    const snapshots: string[][] = [];
    let pass = 0;
    const runtime = {
      submitEndpoints: async (endpoints: Array<{ identifier: string }>) => {
        snapshots.push(endpoints.map((endpoint) => endpoint.identifier).sort());
        pass += 1;
        if (pass === 1) return [
          { type: "open-input", controllerIx: 0, identifier: "in-a", name: "Input A" },
          { type: "open-input", controllerIx: 1, identifier: "in-b", name: "Input B" },
          { type: "open-output", controllerIx: 0, identifier: "out-a", name: "Output A" },
          { type: "open-output", controllerIx: 1, identifier: "out-b", name: "Output B" },
        ];
        if (pass === 2) return [
          { type: "close-input", controllerIx: 1 },
          { type: "close-output", controllerIx: 1 },
        ];
        return [
          { type: "open-input", controllerIx: 1, identifier: "in-b", name: "Input B" },
          { type: "open-output", controllerIx: 1, identifier: "out-b", name: "Output B" },
        ];
      },
      deliverMidi: async () => {},
      dequeueMidiOutput: async () => undefined,
    };
    const manager = new BrowserMidiManager(runtime, {
      requestMIDIAccess: async () => access,
      setInterval: () => 1,
      clearInterval: () => {},
    });
    await manager.startFromUserActivation();
    access.inputs.delete("in-b");
    access.outputs.delete("out-b");
    await manager.poll();
    const slotAStillBoundAfterOffline = inputA.onmidimessage !== null;
    const slotBUnboundAfterOffline = inputB.onmidimessage === null;
    access.inputs.set("in-b", inputB);
    access.outputs.set("out-b", outputB);
    await manager.poll();
    const slotAStillBoundAfterReconnect = inputA.onmidimessage !== null;
    const slotBRebound = inputB.onmidimessage !== null;
    manager.stop();
    return { snapshots, slotAStillBoundAfterOffline, slotBUnboundAfterOffline, slotAStillBoundAfterReconnect, slotBRebound };
  });

  expect(result.snapshots).toEqual([
    ["in-a", "in-b", "out-a", "out-b"],
    ["in-a", "out-a"],
    ["in-a", "in-b", "out-a", "out-b"],
  ]);
  expect(result.slotAStillBoundAfterOffline).toBe(true);
  expect(result.slotBUnboundAfterOffline).toBe(true);
  expect(result.slotAStillBoundAfterReconnect).toBe(true);
  expect(result.slotBRebound).toBe(true);
});

test("drains outbound MIDI on a fast cadence without polling endpoint snapshots", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserMidiManager } = await (new Function("return import('/dist/src/midi.js')")() as Promise<{
      BrowserMidiManager: new (runtime: unknown, options: unknown) => { startFromUserActivation(): Promise<unknown>; stop(): void };
    }>);
    class OutputPort {
      readonly type = "output";
      readonly state = "connected";
      readonly sent: number[][] = [];
      constructor(readonly id: string, readonly name: string) {}
      send(bytes: number[] | Uint8Array) { this.sent.push(Array.from(bytes)); }
    }
    const output = new OutputPort("out-a", "Output A");
    const access = { inputs: new Map(), outputs: new Map([[output.id, output]]), onstatechange: null };
    const timers: Array<{ milliseconds: number; handler: () => void }> = [];
    let endpointSnapshots = 0;
    const outputQueue: Array<{ controllerIx: number; bytes: number[] }> = [];
    const runtime = {
      submitEndpoints: async () => {
        endpointSnapshots += 1;
        return [{ type: "open-output", controllerIx: 0, identifier: "out-a", name: "Output A" }];
      },
      deliverMidi: async () => {},
      dequeueMidiOutput: async () => outputQueue.shift(),
    };
    const manager = new BrowserMidiManager(runtime, {
      requestMIDIAccess: async () => access,
      setInterval: (handler: () => void, milliseconds: number) => {
        timers.push({ handler, milliseconds });
        return timers.length;
      },
      clearInterval: () => {},
    });
    await manager.startFromUserActivation();
    outputQueue.push({ controllerIx: 0, bytes: [0xf0, 0x7d, 0x66, 0xf7] });
    const drainTimer = timers.find((timer) => timer.milliseconds === 16);
    drainTimer?.handler();
    await new Promise((resolve) => setTimeout(resolve, 0));
    manager.stop();
    return { timerIntervals: timers.map((timer) => timer.milliseconds), endpointSnapshots, sent: output.sent };
  });

  expect(result.timerIntervals).toEqual([500, 16]);
  expect(result.endpointSnapshots).toBe(1);
  expect(result.sent).toEqual([[0xf0, 0x7d, 0x66, 0xf7]]);
});

test("real miniapp WASM keeps two Web MIDI controller slots independent through reconnect", async ({ page }) => {
  await page.route("**/dist/src/main.js*", (route) => {
    if (new URL(route.request().url()).search) return route.continue();
    return route.fulfill({ status: 200, contentType: "application/javascript", body: "" });
  });
  await page.goto("http://127.0.0.1:4174/public/index.html");
  const result = await page.evaluate(async () => {
    const { BrowserMidiManager, BrowserMidiWorkerRuntime } = await (new Function("return import('/dist/src/midi.js')")() as Promise<any>);
    const worker = new Worker("/dist/src/worker.js", { type: "module" });
    const observations: Array<{ command: any; response: any }> = [];
    let queue: Promise<void> = Promise.resolve();
    const request = (command: any): Promise<any> => {
      const run = () => new Promise<any>((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error(`timed out waiting for ${command.type}`)), 10_000);
        const receive = (event: MessageEvent<any>) => {
          if (event.data.type === "page-status") return;
          clearTimeout(timeout);
          worker.removeEventListener("message", receive);
          observations.push({ command, response: event.data });
          event.data.type === "error" ? reject(new Error(`${command.type}: ${event.data.error}`)) : resolve(event.data);
        };
        worker.addEventListener("message", receive);
        worker.postMessage(command);
      });
      const response = queue.then(run, run);
      queue = response.then(() => {}, () => {});
      return response;
    };

    const moduleUrl = new URL("/dist/wasm/apps/miniapp/miniapp.js", location.href).href;
    await request({
      type: "load",
      module: {
        entryUrl: moduleUrl,
        locateFile: {
          "miniapp.js": moduleUrl,
          "miniapp.wasm": new URL("/dist/wasm/apps/miniapp/miniapp.wasm", location.href).href,
        },
        mainScriptUrlOrBlob: moduleUrl,
      },
    });
    await request({ type: "create" });
    await request({
      type: "initialize",
      identity: { publisherId: "sheaf", appId: "miniapp", runtimeConfigVersion: 1 },
    });
    const endpoints = [
      { identifier: "in-a", name: "Input A", kind: "input" },
      { identifier: "in-b", name: "Input B", kind: "input" },
      { identifier: "out-a", name: "Output A", kind: "output" },
      { identifier: "out-b", name: "Output B", kind: "output" },
    ];
    await request({ type: "midi-endpoints", endpoints });
    await request({ type: "message-tick", timestampMicros: 1_000 });
    // The shared controller page starts with slot 0; add slot 1 for the second
    // device pair. `add_controller` reads the page's own preset draft rather
    // than the value it is dispatched with, so the preset is selected first
    // through `add_preset_draft`; WRLD.Bldr is the preset whose feedback
    // includes a SysEx frame, which this test needs from slot 1 below.
    await request({ type: "dispatch-action", name: "runtime.controllers.add_preset_draft", value: "library.wrldbldr" });
    await request({ type: "dispatch-action", name: "runtime.controllers.add_controller", value: "" });
    for (const [controllerIx, input, output] of [[0, "in-a", "out-a"], [1, "in-b", "out-b"]] as const) {
      await request({ type: "dispatch-action", name: "runtime.controllers.endpoint_select", value: `${controllerIx}:input:${input}` });
      await request({ type: "dispatch-action", name: "runtime.controllers.endpoint_select", value: `${controllerIx}:output:${output}` });
    }

    class InputPort {
      readonly type = "input";
      state = "connected";
      onmidimessage: ((event: { data: Uint8Array; timeStamp: number }) => void) | null = null;
      constructor(readonly id: string, readonly name: string) {}
      emit(bytes: number[]) { this.onmidimessage?.({ data: Uint8Array.from(bytes), timeStamp: 17 }); }
    }
    class OutputPort {
      readonly type = "output";
      state = "connected";
      readonly sent: number[][] = [];
      constructor(readonly id: string, readonly name: string) {}
      send(bytes: number[] | Uint8Array) { this.sent.push(Array.from(bytes)); }
    }
    const inputA = new InputPort("in-a", "Input A");
    const inputB = new InputPort("in-b", "Input B");
    const outputA = new OutputPort("out-a", "Output A");
    const outputB = new OutputPort("out-b", "Output B");
    const access = {
      inputs: new Map([[inputA.id, inputA], [inputB.id, inputB]]),
      outputs: new Map([[outputA.id, outputA], [outputB.id, outputB]]),
      onstatechange: null,
    };
    const permissions: unknown[] = [];
    const manager = new BrowserMidiManager(new BrowserMidiWorkerRuntime(request), {
      requestMIDIAccess: async (options: unknown) => { permissions.push(options); return access; },
      setInterval: () => 1,
      clearInterval: () => {},
    });
    const started = await manager.startFromUserActivation();
    const boundAtStart = { slotA: inputA.onmidimessage !== null, slotB: inputB.onmidimessage !== null };
    const endpointResponses = observations.filter(({ command }) => command.type === "midi-endpoints");
    for (let pass = 0; pass < 10; pass += 1) {
      await request({ type: "message-tick", timestampMicros: 2_000 + pass });
      await new Promise((resolve) => setTimeout(resolve, 10));
      await manager.drainOutputs();
    }
    outputA.sent.length = 0;
    outputB.sent.length = 0;

    inputB.emit([0xf0, 0x7d, 0x33, 0xf7]);
    for (let attempt = 0; attempt < 100 && !observations.some(({ command }) => command.type === "midi-input"); attempt += 1)
      await new Promise((resolve) => setTimeout(resolve, 5));
    const inbound = observations.filter(({ command }) => command.type === "midi-input").at(-1);

    access.inputs.delete(inputB.id);
    access.outputs.delete(outputB.id);
    await manager.poll();
    const afterDisconnect = {
      slotAStillBound: inputA.onmidimessage !== null,
      slotBWentOffline: inputB.onmidimessage === null,
    };

    access.inputs.set(inputB.id, inputB);
    access.outputs.set(outputB.id, outputB);
    await manager.poll();
    const reconnected = { slotA: inputA.onmidimessage !== null, slotB: inputB.onmidimessage !== null };
    await request({ type: "message-tick", timestampMicros: 3_000 });
    await new Promise((resolve) => setTimeout(resolve, 20));
    await manager.drainOutputs();

    const outboundA = outputA.sent.map((bytes) => bytes);
    const outboundB = outputB.sent.map((bytes) => bytes);
    manager.stop();
    await request({ type: "destroy" });
    worker.terminate();
    return { permissions, started, boundAtStart, endpointResponses, inbound, afterDisconnect, reconnected, outboundA, outboundB };
  });

  expect(result.permissions).toEqual([{ sysex: true }]);
  expect(result.started).toEqual({ status: "online" });
  expect(result.boundAtStart, JSON.stringify(result.endpointResponses)).toEqual({ slotA: true, slotB: true });
  expect(result.inbound).toBeDefined();
  expect(result.inbound!.command).toMatchObject({ type: "midi-input", controllerIx: 1, bytes: [0xf0, 0x7d, 0x33, 0xf7] });
  expect(result.inbound!.response).toEqual({ type: "ok" });
  expect(result.afterDisconnect).toEqual({ slotAStillBound: true, slotBWentOffline: true });
  expect(result.reconnected).toEqual({ slotA: true, slotB: true });
  expect(result.outboundA).toEqual([]);
  expect(result.outboundB.length).toBeGreaterThan(0);
  expect(result.outboundB.some((bytes: number[]) => bytes[0] === 0xf0 && bytes.at(-1) === 0xf7)).toBe(true);
});
