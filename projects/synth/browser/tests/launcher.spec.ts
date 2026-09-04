import { expect, test, type Page, type Route } from "@playwright/test";

const digest = "0123456789abcdef".repeat(4);
const sourcesUrl = "http://127.0.0.1:4173/catalog-sources.json";
const firstCatalogUrl = "https://publisher.example/releases/catalog.json";

function app(appId: string, displayName: string) {
  const buildId = `${appId}-build-1`;
  const entry = `packages/${appId}/${buildId}/${appId}.js`;
  return {
    appId,
    displayName,
    author: "Ada Example",
    category: "Instrument",
    buildId,
    browser: {
      abiVersion: 6,
      uiProtocolVersion: 2,
      runtimeConfigVersion: 1,
      entry,
      files: [{ path: entry, mediaType: "text/javascript", size: 1, sha256: digest }],
    },
  };
}

function catalog(publisherId: string, apps = [app("two", "Borealis"), app("one", "Aurora")]) {
  return {
    schemaVersion: 1,
    catalogVersion: "revision-1",
    publisher: { id: publisherId, name: "Example Audio" },
    apps,
  };
}

async function routeSources(page: Page, urls: string[]) {
  await page.route(sourcesUrl, (route) => route.fulfill({ json: urls }));
}

test("shows loading then accessible app metadata without requesting package files", async ({ page }) => {
  const pending: { route?: Route } = {};
  const packageRequests: string[] = [];
  await routeSources(page, [firstCatalogUrl]);
  await page.route(firstCatalogUrl, async (route) => { pending.route = route; });
  page.on("request", (request) => {
    const pathname = new URL(request.url()).pathname;
    if (pathname.includes("/packages/") || pathname.startsWith("/dist/wasm/"))
      packageRequests.push(request.url());
  });

  await page.goto("http://127.0.0.1:4173/public/index.html");
  await expect(page.getByRole("heading", { name: "SheafPatch" })).toBeVisible();
  await expect(page.getByRole("status")).toHaveText(/loading trusted catalogs/i);
  await expect.poll(() => Boolean(pending.route)).toBe(true);
  await pending.route!.fulfill({ json: catalog("publisher") });

  const row = page.getByRole("listitem").filter({ hasText: "Aurora" });
  await expect(row.getByRole("button", { name: /launch aurora/i })).toBeEnabled();
  await expect(row).toContainText("Example Audio");
  await expect(row).toContainText("Ada Example");
  await expect(row).toContainText("Instrument");
  await expect(row).toContainText("Compatible");
  await expect(page.getByRole("listitem").filter({ hasText: "Borealis" })
    .getByRole("button", { name: /launch borealis/i })).toBeEnabled();
  await expect.poll(() => page.locator(".synth-launcher__app").evaluateAll((rows) =>
    rows.map((element) => (element as HTMLElement).dataset.synthAppId)))
    .toEqual(["publisher/one", "publisher/two"]);
  expect(packageRequests).toEqual([]);
});

test("preserves healthy apps, diagnoses a failed source, and refreshes stable URLs on retry", async ({ page }) => {
  const failedCatalogUrl = "https://offline.example/catalog.json";
  let failedAttempts = 0;
  let healthyAttempts = 0;
  await routeSources(page, [firstCatalogUrl, failedCatalogUrl]);
  await page.route(firstCatalogUrl, (route) => {
    healthyAttempts += 1;
    const apps = healthyAttempts === 1
      ? [app("one", "Aurora")]
      : [app("one", "Aurora"), app("two", "Borealis")];
    return route.fulfill({ json: catalog("publisher", apps) });
  });
  await page.route(failedCatalogUrl, (route) => {
    failedAttempts += 1;
    return route.fulfill({ status: 503, body: "offline" });
  });

  await page.goto("http://127.0.0.1:4173/public/index.html");
  await expect(page.getByRole("button", { name: /launch aurora/i })).toBeEnabled();
  const diagnostic = page.getByRole("listitem").filter({ hasText: failedCatalogUrl });
  await expect(diagnostic).toContainText(/unavailable/i);
  await page.getByRole("button", { name: /retry catalogs/i }).click();

  await expect(page.getByRole("button", { name: /launch borealis/i })).toBeEnabled();
  expect(healthyAttempts).toBe(2);
  expect(failedAttempts).toBe(2);
});

test("preserves the last healthy app list when source-list revalidation fails", async ({ page }) => {
  const failedCatalogUrl = "https://offline.example/catalog.json";
  let sourceAttempts = 0;
  await page.route(sourcesUrl, (route) => {
    sourceAttempts += 1;
    if (sourceAttempts === 1) return route.fulfill({ json: [firstCatalogUrl, failedCatalogUrl] });
    return route.fulfill({ status: 503, body: "source list offline" });
  });
  await page.route(firstCatalogUrl, (route) => route.fulfill({ json: catalog("publisher") }));
  await page.route(failedCatalogUrl, (route) => route.fulfill({ status: 503, body: "offline" }));

  await page.goto("http://127.0.0.1:4173/public/index.html");
  await expect(page.getByRole("button", { name: /launch aurora/i })).toBeEnabled();
  await page.getByRole("button", { name: /retry catalogs/i }).click();

  await expect(page.getByRole("status")).toContainText(/catalog discovery failed.*HTTP 503/i);
  await expect(page.getByRole("button", { name: /launch aurora/i })).toBeEnabled();
  expect(sourceAttempts).toBe(2);
});

test("locks selection after success without rendering an in-page return control", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/dist/src/launcher.js");
  await page.setContent('<main id="launcher-root"></main>');
  await page.evaluate(async () => {
    const { SheafPatchLauncher } = await (new Function("return import('/dist/src/launcher.js')")() as Promise<any>);
    const selected: string[] = [];
    (window as any).__launcherTest = { selected };
    const makeApp = (appId: string, displayName: string) => ({
      globalId: `example/${appId}`,
      catalogUrl: "https://publisher.example/catalog.json",
      publisher: { id: "example", name: "Example Audio" },
      appId,
      displayName,
      author: "Ada Example",
      category: "Instrument",
      buildId: `${appId}-build-1`,
      browser: {
        abiVersion: 6,
        uiProtocolVersion: 2,
        runtimeConfigVersion: 1,
        entry: `${appId}.js`,
        entryUrl: `https://publisher.example/${appId}.js`,
        files: [],
      },
    });
    const client = {
      loadSources: async () => ({
        apps: [makeApp("one", "Aurora"), makeApp("two", "Borealis")],
        diagnostics: [],
        duplicateDiagnostics: [],
      }),
    };
    const launcher = new SheafPatchLauncher(document.querySelector("#launcher-root"), {
      client,
      select: async (app: any) => { selected.push(app.globalId); },
    });
    await launcher.start();
  });

  await page.getByRole("button", { name: /launch aurora/i }).click();
  await expect(page.getByRole("button", { name: /launch aurora/i })).toBeDisabled();
  await expect(page.getByRole("button", { name: /launch borealis/i })).toBeDisabled();
  await expect(page.getByRole("button", { name: /back to launcher/i })).toHaveCount(0);

  expect(await page.evaluate(() => (window as any).__launcherTest)).toEqual({
    selected: ["example/one"],
  });
});

test("does not overwrite runtime DOM that replaces the exact pending-selection shell", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/dist/src/launcher.js");
  await page.setContent('<main id="launcher-root"></main>');
  await page.evaluate(async () => {
    const { SheafPatchLauncher } = await (new Function("return import('/dist/src/launcher.js')")() as Promise<any>);
    let finishSelection!: () => void;
    const selection = new Promise<void>((resolve) => { finishSelection = resolve; });
    (window as any).__finishSelection = finishSelection;
    const application = {
      globalId: "example/one",
      catalogUrl: "https://publisher.example/catalog.json",
      publisher: { id: "example", name: "Example Audio" },
      appId: "one",
      displayName: "Aurora",
      author: "Ada Example",
      category: "Instrument",
      buildId: "one-build-1",
      browser: {
        abiVersion: 6,
        uiProtocolVersion: 2,
        runtimeConfigVersion: 1,
        entry: "one.js",
        entryUrl: "https://publisher.example/one.js",
        files: [],
      },
    };
    const launcher = new SheafPatchLauncher(document.querySelector("#launcher-root"), {
      client: { loadSources: async () => ({ apps: [application], diagnostics: [], duplicateDiagnostics: [] }) },
      select: async () => selection,
    });
    await launcher.start();
  });

  await page.getByRole("button", { name: /launch aurora/i }).click();
  await expect(page.getByRole("button", { name: /loading aurora/i })).toBeDisabled();
  await page.evaluate(() => {
    document.querySelector("#launcher-root")!.innerHTML =
      '<section data-runtime-owner="true"><div class="synth-launcher">Runtime-owned class collision</div></section>';
    (window as any).__finishSelection();
  });

  await expect(page.locator('[data-runtime-owner="true"]')).toContainText("Runtime-owned class collision");
  await expect(page.getByRole("heading", { name: "SheafPatch" })).toHaveCount(0);
});

test("reports package selection failure on its row and allows retry", async ({ page }) => {
  await routeSources(page, [firstCatalogUrl]);
  await page.route(firstCatalogUrl, (route) => route.fulfill({ json: catalog("publisher") }));
  await page.goto("http://127.0.0.1:4173/public/index.html");

  await page.getByRole("button", { name: /launch aurora/i }).click();

  const row = page.getByRole("listitem").filter({ hasText: "Aurora" });
  await expect(row).toContainText(/package file .* fetch failed/i);
  await expect(row.getByRole("button", { name: /retry aurora/i })).toBeEnabled();
});
