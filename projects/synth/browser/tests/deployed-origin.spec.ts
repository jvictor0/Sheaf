import { expect, test } from "@playwright/test";

const processInfo = (globalThis as any).process as { env: Record<string, string | undefined> };
const remoteCatalogUrl = processInfo.env.SYNTH_BROWSER_REMOTE_CATALOG_URL;
const expectedCatalogVersion = processInfo.env.SYNTH_BROWSER_EXPECTED_CATALOG_VERSION;
const deployedApps = [
  { appId: "miniapp", button: /launch mini app/i, root: "miniapp.root" },
  { appId: "braid-4", button: /launch braid 4/i, root: "braid4.root" },
] as const;

test.skip(!remoteCatalogUrl, "SYNTH_BROWSER_REMOTE_CATALOG_URL is not set");
test.setTimeout(90_000);

for (const app of deployedApps) {
  test(`${app.appId} launches once from the complete deployed catalog`, async ({ page }) => {
    expect(
      expectedCatalogVersion,
      "SYNTH_BROWSER_EXPECTED_CATALOG_VERSION must accompany the remote catalog URL",
    ).toBeTruthy();
    const remoteOrigin = new URL(remoteCatalogUrl!).origin;
    const remoteRequests: string[] = [];
    page.on("request", (request) => {
      const url = new URL(request.url());
      if (url.origin === remoteOrigin) remoteRequests.push(url.pathname);
    });
    await page.route("http://127.0.0.1:4173/catalog-sources.json", (route) =>
      route.fulfill({ json: [remoteCatalogUrl] }));
    await page.addInitScript(() => {
      const observations = {
        contexts: 0,
        resumes: 0,
        midiRequests: 0,
        workerUrls: [] as string[],
        audioWorkletModules: [] as string[],
        audioWorkletNodeNames: [] as string[],
        audioConnections: 0,
        blobFetches: [] as string[],
        fallbackMessages: [] as string[],
      };
      (window as any).__synthDeployedOrigin = observations;

      const nativeFetch = globalThis.fetch.bind(globalThis);
      globalThis.fetch = ((input: RequestInfo | URL, init?: RequestInit) => {
        const url = typeof input === "string" || input instanceof URL ? String(input) : input.url;
        if (url.startsWith("blob:")) observations.blobFetches.push(url);
        return nativeFetch(input, init);
      }) as typeof fetch;

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

      const NativeWorker = globalThis.Worker;
      Object.defineProperty(globalThis, "Worker", {
        configurable: true,
        value: new Proxy(NativeWorker, {
          construct(target, argumentsList, newTarget) {
            observations.workerUrls.push(String(argumentsList[0]));
            return Reflect.construct(target, argumentsList, newTarget);
          },
        }),
      });

      const NativeAudioContext = globalThis.AudioContext;
      Object.defineProperty(globalThis, "AudioContext", {
        configurable: true,
        value: new Proxy(NativeAudioContext, {
          construct(target, argumentsList, newTarget) {
            const context = Reflect.construct(target, argumentsList, newTarget) as AudioContext;
            observations.contexts += 1;
            const nativeResume = context.resume.bind(context);
            context.resume = async () => {
              observations.resumes += 1;
              return nativeResume();
            };
            const nativeAddModule = context.audioWorklet.addModule.bind(context.audioWorklet);
            context.audioWorklet.addModule = async (url: string | URL, options?: WorkletOptions) => {
              observations.audioWorkletModules.push(String(url));
              return nativeAddModule(url, options);
            };
            return context;
          },
        }),
      });

      const NativeAudioWorkletNode = globalThis.AudioWorkletNode;
      Object.defineProperty(globalThis, "AudioWorkletNode", {
        configurable: true,
        value: new Proxy(NativeAudioWorkletNode, {
          construct(target, argumentsList, newTarget) {
            const node = Reflect.construct(target, argumentsList, newTarget) as AudioWorkletNode;
            observations.audioWorkletNodeNames.push(String(argumentsList[1]));
            const nativeConnect = node.connect.bind(node);
            node.connect = ((...args: Parameters<typeof node.connect>) => {
              observations.audioConnections += 1;
              return nativeConnect(...args);
            }) as typeof node.connect;
            return node;
          },
        }),
      });

      Object.defineProperty(navigator, "requestMIDIAccess", {
        configurable: true,
        value: async () => {
          observations.midiRequests += 1;
          return { inputs: new Map(), outputs: new Map(), onstatechange: null };
        },
      });
    });

    await page.goto("http://127.0.0.1:4173/public/index.html");
    expect(await page.evaluate(() => crossOriginIsolated)).toBe(true);
    const deployedCatalog = await page.evaluate(async (catalogUrl) => {
      const response = await fetch(catalogUrl, { mode: "cors", credentials: "omit", cache: "no-store" });
      if (!response.ok) throw new Error(`remote catalog returned HTTP ${response.status}`);
      const catalog = await response.json();
      return {
        catalogVersion: catalog.catalogVersion,
        appIds: catalog.apps.map((entry: { appId: string }) => entry.appId),
      };
    }, remoteCatalogUrl!);
    // readAppBuildManifest sorts by appId before build-first-party-catalog.mjs
    // writes the raw catalog, so the published catalog.json's own apps[] order
    // is alphabetical by appId, not the launcher's display-name order below.
    expect(deployedCatalog).toEqual({
      catalogVersion: expectedCatalogVersion,
      appIds: ["braid-4", "miniapp", "one-second-delay"],
    });

    const rows = page.locator(".synth-launcher__app");
    // Launcher rows sort by display name (mergeCatalogs): "1 Second Delay",
    // "Braid 4", "Mini App".
    await expect.poll(() => rows.evaluateAll((elements) =>
      elements.map((element) => (element as HTMLElement).dataset.synthAppId)))
      .toEqual(["sheaf/one-second-delay", "sheaf/braid-4", "sheaf/miniapp"]);
    await page.getByRole("button", { name: app.button }).click();
    await expect(page.locator(`[data-synth-node-id="${app.root}"]`)).toBeVisible({ timeout: 60_000 });

    const observations = await page.evaluate(() => (window as any).__synthDeployedOrigin);
    expect(observations.contexts).toBe(1);
    expect(observations.resumes).toBeGreaterThanOrEqual(1);
    expect(observations.midiRequests).toBe(1);
    expect(observations.workerUrls.some((url: string) => url.startsWith("blob:"))).toBe(true);
    expect(observations.blobFetches.length).toBeGreaterThan(0);
    expect(observations.audioWorkletModules).not.toEqual(expect.arrayContaining([
      expect.stringMatching(/\/dist\/src\/audio-worklet\.js$/),
    ]));
    expect(observations.audioWorkletNodeNames).toContain("sheaf-synth-audio");
    expect(observations.audioWorkletNodeNames).not.toContain("synth-audio-ring-buffer");
    expect(observations.audioConnections).toBe(1);
    expect(observations.fallbackMessages).toEqual([]);

    const otherAppId = app.appId === "miniapp" ? "braid-4" : "miniapp";
    expect(remoteRequests).toEqual(expect.arrayContaining([
      new URL(remoteCatalogUrl!).pathname,
      expect.stringMatching(new RegExp(`/packages/${app.appId}/[0-9a-f]{64}/${app.appId}\\.js$`)),
      expect.stringMatching(new RegExp(`/packages/${app.appId}/[0-9a-f]{64}/${app.appId}\\.wasm$`)),
    ]));
    expect(remoteRequests.some((pathname) => pathname.includes(`/packages/${otherAppId}/`))).toBe(false);

    await page.evaluate(() => dispatchEvent(new Event("pagehide")));
  });
}
