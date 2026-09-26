import { expect, test, type Browser, type Page } from "@playwright/test";

type FirstPartyApp = Readonly<{
  appId: string;
  button: RegExp;
  root: string;
}>;

const firstPartyApps: readonly FirstPartyApp[] = [
  { appId: "miniapp", button: /launch mini app/i, root: "miniapp.root" },
  { appId: "braid-4", button: /launch braid 4/i, root: "braid4.root" },
];

const publishedOrigin = "http://127.0.0.1:4175";

test.setTimeout(120_000);

async function installInstrumentedLauncher(page: Page): Promise<void> {
  await page.route(`${publishedOrigin}/dist/src/main.js`, (route) => route.fulfill({
    status: 200,
    contentType: "application/javascript",
    body: "",
  }));
  await page.addInitScript(() => {
    const observations = {
      contexts: 0,
      resumes: 0,
      midiRequests: 0,
      runtimes: 0,
      packages: 0,
      selectedGlobalId: "",
      commands: [] as string[],
      fallbackMessages: [] as string[],
      audioWorkletNodeNames: [] as string[],
      audioWorkletNodeOptions: [] as Array<{ numberOfInputs?: number; channelCount?: number; channelCountMode?: string; channelInterpretation?: string }>,
      audioConfigs: [] as Array<{ channels: number; inputChannels?: number }>,
      audioInputChannels: [] as number[],
      getUserMediaCalls: 0,
      mediaStreamSourceCreations: 0,
      inputSourceRegistrations: 0,
      memoryAtCallbackStart: 0,
      memoryAfterCallbackProgress: 0,
      module: undefined as any,
      runtime: undefined as any,
    };
    (window as any).__firstPartyAcceptance = observations;

    Object.defineProperty(navigator, "mediaDevices", {
      configurable: true,
      value: {
        async getUserMedia() {
          observations.getUserMediaCalls += 1;
          throw Object.assign(new Error("zero-input app requested microphone"), { name: "NotAllowedError" });
        },
      },
    });

    const nativePostMessage = MessagePort.prototype.postMessage;
    (MessagePort.prototype as any).postMessage = function(message: unknown, options?: StructuredSerializeOptions) {
      const type = message && typeof message === "object" && "type" in message
        ? String((message as { type: unknown }).type)
        : "";
      if (type === "configure-audio" || type === "render-audio") observations.fallbackMessages.push(type);
      return options === undefined
        ? nativePostMessage.call(this, message)
        : nativePostMessage.call(this, message, options);
    };

    const NativeAudioWorkletNode = globalThis.AudioWorkletNode;
    Object.defineProperty(globalThis, "AudioWorkletNode", {
      configurable: true,
      value: new Proxy(NativeAudioWorkletNode, {
        construct(target, argumentsList, newTarget) {
          observations.audioWorkletNodeNames.push(String(argumentsList[1]));
          const options = argumentsList[2] as AudioWorkletNodeOptions | undefined;
          observations.audioWorkletNodeOptions.push({
            numberOfInputs: options?.numberOfInputs,
            channelCount: options?.channelCount,
            channelCountMode: options?.channelCountMode,
            channelInterpretation: options?.channelInterpretation,
          });
          return Reflect.construct(target, argumentsList, newTarget);
        },
      }),
    });
  });

  await page.goto(`${publishedOrigin}/`);
  await page.evaluate(async () => {
    const state = (window as any).__firstPartyAcceptance;
    const root = document.querySelector<HTMLElement>("#synth-root")!;
    root.dataset.synthLauncher = "false";
    const main = await (new Function("return import('/dist/src/main.js?first-party-acceptance')")() as Promise<any>);
    const worker = await (new Function("return import('/dist/src/worker.js?first-party-acceptance')")() as Promise<any>);
    const { ActivationLease } = await (new Function("return import('/dist/src/activation.js')")() as Promise<any>);
    const { CatalogClient } = await (new Function("return import('/dist/src/catalog-client.js')")() as Promise<any>);
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);

    await main.installSheafPatchLauncher(root, {
      client: new CatalogClient({ sourcesUrl: "/catalog-sources.json" }),
      activationLeaseFactory: () => {
        if (state.contexts !== 0) throw new Error("second AudioContext");
        const context = new AudioContext();
        state.contexts += 1;
        const nativeCreateMediaStreamSource = context.createMediaStreamSource.bind(context);
        context.createMediaStreamSource = ((...args: Parameters<AudioContext["createMediaStreamSource"]>) => {
          state.mediaStreamSourceCreations += 1;
          return nativeCreateMediaStreamSource(...args);
        }) as AudioContext["createMediaStreamSource"];
        const nativeResume = context.resume.bind(context);
        context.resume = async () => {
          state.resumes += 1;
          await nativeResume();
        };
        return ActivationLease.acquire({
          audioContextFactory: () => context,
          requestMIDIAccess: async () => {
            state.midiRequests += 1;
            return { inputs: new Map(), outputs: new Map(), onstatechange: null };
          },
        });
      },
      materializePackage: async (app: any) => {
        state.packages += 1;
        state.selectedGlobalId = app.globalId;
        return materializePackage(app);
      },
      runtimeClientFactory: () => {
        state.runtimes += 1;
        const runtime = main.createDirectRuntimeClient((materialized: any) =>
          worker.loadEmscriptenRuntime(materialized, async (entryUrl: string) => {
            const imported = await import(entryUrl);
            const nativeFactory = imported.default ?? imported.createSynthBrowserModule;
            return {
              default: async (options: unknown) => {
                const module = await nativeFactory(options);
                state.module = module;
                const nativeStart = module._synth_browser_start_audio_worklet.bind(module);
                module._synth_browser_start_audio_worklet = (...args: unknown[]) => {
                  state.memoryAtCallbackStart = module.HEAPU8.buffer.byteLength;
                  return nativeStart(...args);
                };
                const nativeAudioInputChannels = module._synth_browser_audio_input_channels?.bind(module);
                if (nativeAudioInputChannels) {
                  module._synth_browser_audio_input_channels = (...args: unknown[]) => {
                    const channels = nativeAudioInputChannels(...args);
                    state.audioInputChannels.push(channels);
                    return channels;
                  };
                }
                if (typeof module._synth_browser_set_audio_input_source !== "function")
                  throw new Error("missing mandatory ABI-v4 audio input source export");
                const nativeSetAudioInputSource = module._synth_browser_set_audio_input_source.bind(module);
                module._synth_browser_set_audio_input_source = (...args: unknown[]) => {
                  state.inputSourceRegistrations += 1;
                  return nativeSetAudioInputSource(...args);
                };
                return module;
              },
            };
          }));
        const instrumented = {
          ...runtime,
          async request(command: { type: string }) {
            state.commands.push(command.type);
            const response = await runtime.request(command);
            if (command.type === "audio-config" && response.type === "audio-config") {
              state.audioConfigs.push({ channels: response.channels, inputChannels: response.inputChannels });
            }
            return response;
          },
        };
        state.runtime = instrumented;
        return instrumented;
      },
      frameIntervalMs: 25,
    });
  });
}

async function pollNativeCallback(page: Page): Promise<{
  first: { blocks: number; peakMicrounits: number; deadlineMicrounits: number };
  second: { blocks: number; peakMicrounits: number; deadlineMicrounits: number };
}> {
  return page.evaluate(async () => {
    const state = (window as any).__firstPartyAcceptance;
    const stats = async () => {
      const response = await state.runtime.request({ type: "audio-worklet-stats" });
      if (response.type !== "audio-worklet-stats") throw new Error(response.error ?? "missing native callback stats");
      return response;
    };
    const first = await stats();
    const deadline = performance.now() + 10_000;
    let second = first;
    while (performance.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 25));
      second = await stats();
      if (second.blocks > first.blocks && second.peakMicrounits > 0 && Number.isFinite(second.deadlineMicrounits)) break;
    }
    state.memoryAfterCallbackProgress = state.module.HEAPU8.buffer.byteLength;
    return { first, second };
  });
}

for (const app of firstPartyApps) {
  test(`${app.appId} launches once through the native catalog path with isolated persistence`, async ({ browser }: { browser: Browser }) => {
    const context = await browser.newContext();
    const page = await context.newPage();
    const requests: string[] = [];
    page.on("request", (request) => requests.push(new URL(request.url()).pathname));
    try {
      await installInstrumentedLauncher(page);

      // Launcher rows sort by display name (mergeCatalogs): "1 Second Delay",
      // "Braid 4", "Mini App".
      await expect.poll(() => page.locator(".synth-launcher__app").evaluateAll((rows) =>
        rows.map((row) => (row as HTMLElement).dataset.synthAppId)))
        .toEqual(["sheaf/one-second-delay", "sheaf/braid-4", "sheaf/miniapp"]);
      const discoveryRequests = requests.filter((pathname) =>
        pathname.endsWith(".json") || pathname.includes("/packages/"));
      expect(discoveryRequests).toEqual([
        "/catalog-sources.json",
        "/catalogs/sheaf/catalog.json",
      ]);

      await page.getByRole("button", { name: app.button }).click();
      await expect(page.locator(`[data-synth-node-id="${app.root}"]`)).toBeVisible({ timeout: 60_000 });

      await page.evaluate(async () => {
        const state = (window as any).__firstPartyAcceptance;
        const response = await state.runtime.request({
          type: "midi-input",
          controllerIx: 0,
          bytes: [0x90, 60, 100],
          timestampMicros: Math.round(performance.now() * 1000),
        });
        if (response.type === "error") throw new Error(response.error);
      });
      const callback = await pollNativeCallback(page);

      await page.locator('[data-synth-node-id="runtime.sidebar.file"]').click();
      await expect(page.locator('[data-synth-node-id="runtime.file.root"]')).toBeVisible();
      const expectedPatchRoot = `/data/patches/sheaf/${app.appId}`;
      const otherAppId = app.appId === "miniapp" ? "braid-4" : "miniapp";
      await expect(page.locator('[data-synth-node-id="runtime.file.patch_root"]')).toContainText(expectedPatchRoot);
      const patchName = `acceptance-${app.appId}`;
      await page.locator('[data-synth-node-id="runtime.file.save_as"]').click();
      await page.locator('[data-synth-node-id="runtime.file.browser.save_name"] input').fill(patchName);
      await page.locator('[data-synth-node-id="runtime.file.browser.confirm"]').click();
      await expect.poll(() => page.locator('[data-synth-node-id="runtime.file.patch_name"]').textContent())
        .toBe(patchName);
      await expect.poll(() => page.evaluate(({ expectedPatchRoot, patchName, otherAppId }) => {
        const filesystem = (window as any).__firstPartyAcceptance.module.FS;
        const entries = filesystem.readdir(expectedPatchRoot);
        let otherRootExists = true;
        try { filesystem.stat(`/data/patches/sheaf/${otherAppId}`); }
        catch { otherRootExists = false; }
        return { saved: entries.includes(patchName), otherRootExists };
      }, { expectedPatchRoot, patchName, otherAppId })).toEqual({ saved: true, otherRootExists: false });

      const observations = await page.evaluate(() => {
        const { module: _module, runtime: _runtime, ...serializable } = (window as any).__firstPartyAcceptance;
        return serializable;
      });
      expect(callback.second.blocks).toBeGreaterThan(callback.first.blocks);
      expect(Number.isFinite(callback.first.deadlineMicrounits)).toBe(true);
      expect(Number.isFinite(callback.second.deadlineMicrounits)).toBe(true);
      expect(callback.second.peakMicrounits).toBeGreaterThan(0);
      expect(observations.memoryAtCallbackStart).toBeGreaterThan(0);
      expect(observations.memoryAfterCallbackProgress).toBe(observations.memoryAtCallbackStart);
      expect(observations.contexts).toBe(1);
      expect(observations.resumes).toBeGreaterThanOrEqual(1);
      expect(observations.midiRequests).toBe(1);
      expect(observations.runtimes).toBe(1);
      expect(observations.packages).toBe(1);
      expect(observations.selectedGlobalId).toBe(`sheaf/${app.appId}`);
      expect(observations.commands).not.toEqual(expect.arrayContaining(["configure-audio", "render-audio"]));
      expect(observations.fallbackMessages).toEqual([]);
      expect(observations.audioWorkletNodeNames).toContain("sheaf-synth-audio");
      expect(observations.audioWorkletNodeNames).not.toContain("synth-audio-ring-buffer");
      expect(observations.audioWorkletNodeOptions).toEqual(expect.arrayContaining([
        expect.objectContaining({
          numberOfInputs: 0,
          channelCount: 2,
          channelCountMode: "explicit",
        }),
      ]));
      expect(observations.audioConfigs).toEqual(expect.arrayContaining([
        expect.objectContaining({ channels: 2, inputChannels: 0 }),
      ]));
      expect(observations.audioInputChannels).toEqual(expect.arrayContaining([0]));
      expect(observations.getUserMediaCalls).toBe(0);
      expect(observations.mediaStreamSourceCreations).toBe(0);
      expect(observations.inputSourceRegistrations).toBe(0);

      const packageRequests = requests.filter((pathname) => pathname.includes("/packages/"));
      expect(packageRequests).toEqual(expect.arrayContaining([
        expect.stringMatching(new RegExp(`/packages/${app.appId}/[0-9a-f]{64}/${app.appId}\\.js$`)),
        expect.stringMatching(new RegExp(`/packages/${app.appId}/[0-9a-f]{64}/${app.appId}\\.wasm$`)),
      ]));
      expect(packageRequests.some((pathname) => pathname.includes(`/packages/${otherAppId}/`))).toBe(false);
    } finally {
      await page.evaluate(() => dispatchEvent(new Event("pagehide"))).catch(() => {});
      await context.close();
    }
  });
}
