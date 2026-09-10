import { expect, test } from "@playwright/test";

test("canonical static server applies browser isolation, MIDI sysex, and same-origin microphone headers", async ({ request }) => {
  const { contentTypeForPath } = await import("../src/" + "static-server.mjs") as { contentTypeForPath(path: string): string };
  const defaultPage = await request.get("http://127.0.0.1:4173/public/index.html");
  expect(defaultPage.headers()["cross-origin-opener-policy"]).toBe("same-origin");
  expect(defaultPage.headers()["cross-origin-embedder-policy"]).toBe("require-corp");
  expect(defaultPage.headers()["permissions-policy"]).toContain("midi=(self)");
  expect(defaultPage.headers()["permissions-policy"]).toContain("microphone=(self)");

  const page = await request.get("http://127.0.0.1:4174/public/index.html");
  expect(page.status()).toBe(200);
  expect(page.headers()["cross-origin-opener-policy"]).toBe("same-origin");
  expect(page.headers()["cross-origin-embedder-policy"]).toBe("require-corp");
  expect(page.headers()["permissions-policy"]).toContain("midi=(self)");
  expect(page.headers()["permissions-policy"]).toContain("microphone=(self)");
  expect(contentTypeForPath("module.wasm")).toBe("application/wasm");

  expect((await request.get("http://127.0.0.1:4174/src/worker.ts")).status()).toBe(404);
  expect((await request.get("http://127.0.0.1:4174/../../package.json")).status()).toBe(404);
  expect((await request.get("http://127.0.0.1:4174/not-an-asset")).status()).toBe(404);
});

test("static shell identifies the generic catalog launcher without app-specific markup", async ({ page, request }) => {
  const stylesheet = await request.get("http://127.0.0.1:4173/public/synth-browser.css");
  expect(stylesheet.ok()).toBe(true);
  expect(await stylesheet.text()).not.toMatch(/miniapp|fake-browser/i);

  await page.route("**/dist/src/main.js", (route) => route.fulfill({
    status: 200,
    contentType: "application/javascript",
    body: "",
  }));
  await page.goto("http://127.0.0.1:4173/public/index.html");
  expect(await page.locator("body > main, body > script").count()).toBe(2);
  const root = page.locator("#synth-root");
  await expect(root).toHaveAttribute("data-synth-launcher", "true");
  await expect(root).toHaveAttribute("data-synth-catalog-sources", "/catalog-sources.json");
  expect(await root.evaluate((element) => element.childElementCount)).toBe(0);
  expect(await page.locator("button, canvas, select, input").count()).toBe(0);
  const styles = await page.evaluate(() => {
    const body = getComputedStyle(document.body);
    const root = getComputedStyle(document.querySelector("#synth-root")!);
    return { bodyMargin: body.margin, bodyBackground: body.backgroundColor, rootColor: root.color };
  });
  expect(styles).toEqual({ bodyMargin: "0px", bodyBackground: "rgb(18, 20, 22)", rootColor: "rgb(229, 231, 235)" });
});

test("normal generic browser flows make no backend or WebSocket requests beyond static discovery", async ({ page }) => {
  const dynamicRequests: string[] = [];
  const sockets: string[] = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.origin === "http://127.0.0.1:4174" && !url.pathname.startsWith("/dist/") && !url.pathname.startsWith("/public/"))
      dynamicRequests.push(url.pathname);
  });
  page.on("websocket", (socket) => sockets.push(socket.url()));
  await page.goto("http://127.0.0.1:4174/public/index.html");
  await page.evaluate(async () => {
    const { BrowserPersistence } = await (new Function("return import('/dist/src/persistence.js')")() as Promise<{
      BrowserPersistence: new (filesystem: unknown, identity: unknown, options: unknown, reportStatus: unknown) => unknown;
    }>);
    const { BrowserRuntimeWorker } = await (new Function("return import('/dist/src/worker.js')")() as Promise<{
      BrowserRuntimeWorker: new (loadModule: unknown, createPersistence: unknown) => { handle(command: unknown): Promise<unknown> };
    }>);
    const filesystem = {
      filesystems: { IDBFS: "idbfs" }, mkdir() {}, mount() {}, syncfs(_populate: boolean, complete: () => void) { complete(); },
    };
    const worker = new BrowserRuntimeWorker(async () => ({
      abiVersion: 6, uiProtocolVersion: 2, runtimeConfigVersion: 1,
      filesystem,
      create: () => 1, audioOutputChannels: () => 2, initialize: () => 0, prepare: () => 0, process: () => 0, messageTick: () => 0,
      buildUiFrame: () => new ArrayBuffer(0), dispatchAction: () => 0, submitMidiEndpoints: () => 0,
      dequeueMidiAction: () => undefined, deliverMidi: () => 0, dequeueMidiOutput: () => undefined, destroy: () => {},
    }), (mountedFilesystem: unknown, identity: unknown, reportStatus: unknown) =>
      new BrowserPersistence(mountedFilesystem, identity, { debounceMs: 0 }, reportStatus));
    await worker.handle({ type: "load", module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" } });
    await worker.handle({ type: "create" });
    await worker.handle({
      type: "initialize",
      identity: { publisherId: "example", appId: "portable-app", runtimeConfigVersion: 1 },
    });
    await worker.handle({ type: "prepare", sampleRate: 48000, blockSize: 128 });
    await worker.handle({ type: "midi-endpoints", endpoints: [] });
    await worker.handle({ type: "build-ui-frame" });
    await worker.handle({ type: "persistence", state: "configuration saved" });
  });

  expect(dynamicRequests).toEqual(["/catalog-sources.json"]);
  expect(sockets).toEqual([]);
});

for (const app of [
  { appId: "miniapp", button: /launch mini app/i },
  { appId: "braid-4", button: /launch braid 4/i },
] as const) {
  test(`published root discovers both rows and fetches only ${app.appId}'s immutable package`, async ({ page }) => {
    const requested: string[] = [];
    page.on("request", (request) => requested.push(new URL(request.url()).pathname));

    await page.goto("http://127.0.0.1:4175/");
    // Launcher rows sort by display name (mergeCatalogs): "1 Second Delay",
    // "Braid 4", "Mini App".
    await expect.poll(() => page.locator(".synth-launcher__app").evaluateAll((rows) =>
      rows.map((element) => (element as HTMLElement).dataset.synthAppId)))
      .toEqual(["sheaf/one-second-delay", "sheaf/braid-4", "sheaf/miniapp"]);

    const discoveryRequests = requested.filter((pathname) =>
      pathname.endsWith(".json") || pathname.includes("/packages/") || pathname.includes("/rollback/"));
    expect(discoveryRequests).toEqual([
      "/catalog-sources.json",
      "/catalogs/sheaf/catalog.json",
    ]);

    await page.getByRole("button", { name: app.button }).click();
    await expect.poll(() => requested.filter((pathname) => pathname.includes("/packages/")).sort())
      .toEqual(expect.arrayContaining([
        expect.stringMatching(new RegExp(`^/catalogs/sheaf/packages/${app.appId}/[0-9a-f]{64}/${app.appId}\\.js$`)),
        expect.stringMatching(new RegExp(`^/catalogs/sheaf/packages/${app.appId}/[0-9a-f]{64}/${app.appId}\\.wasm$`)),
      ]));
    const otherAppId = app.appId === "miniapp" ? "braid-4" : "miniapp";
    expect(requested.some((pathname) => pathname.includes(`/packages/${otherAppId}/`))).toBe(false);
    expect(requested.some((pathname) => pathname.includes("/rollback/"))).toBe(false);
  });
}

test("launcher and runtime source remain application-generic", async () => {
  const { readFile } = await (new Function("return import('node:fs/promises')")() as Promise<{
    readFile(path: URL, encoding: "utf8"): Promise<string>;
  }>);
  for (const relativePath of [
    "../src/main.ts",
    "../src/launcher.ts",
    "../src/catalog-client.ts",
    "../src/package-loader.ts",
    "../src/worker.ts",
    "../public/index.html",
  ]) {
    const source = await readFile(new URL(relativePath, import.meta.url), "utf8");
    expect(source, relativePath).not.toMatch(/miniapp/i);
    expect(source, relativePath).not.toMatch(/braid-?4/i);
    expect(source, relativePath).not.toMatch(/rollback\/direct-miniapp/i);
    expect(source, relativePath).not.toMatch(/dist\/wasm\/app\.js/i);
  }
});

for (const app of [
  { appId: "miniapp", root: "miniapp.root" },
  { appId: "braid-4", root: "braid4.root" },
] as const) {
  test(`generic ${app.appId} rollback owns one explicit package without catalog discovery`, async ({ page }) => {
    const requested: string[] = [];
    page.on("request", (request) => requested.push(new URL(request.url()).pathname));

    await page.goto(`http://127.0.0.1:4175/rollback/apps/${app.appId}/index.html`);
    await expect(page.locator(`[data-synth-node-id="${app.root}"]`)).toBeVisible({ timeout: 60_000 });

    expect(requested).toEqual(expect.arrayContaining([
      expect.stringMatching(new RegExp(`/catalogs/sheaf/packages/${app.appId}/[0-9a-f]{64}/${app.appId}\\.js$`)),
      expect.stringMatching(new RegExp(`/catalogs/sheaf/packages/${app.appId}/[0-9a-f]{64}/${app.appId}\\.wasm$`)),
    ]));
    expect(requested.some((pathname) => pathname.endsWith("catalog-sources.json"))).toBe(false);
  });
}
