import { expect, test } from "@playwright/test";
import { installRealFakeApp, stopRealFakeApp, synthNode } from "./helpers/fake-app.js";

test.afterEach(async ({ page }) => {
  await stopRealFakeApp(page);
});

test("the fixture app's Export button downloads the queued file export", async ({ page }) => {
  await installRealFakeApp(page);

  const downloadPromise = page.waitForEvent("download");
  await page.locator(synthNode("fake-browser-export")).click();
  // The harness pins frameIntervalMs to 60s (see installRealFakeApp) so
  // ticking never races an assertion; a queued export is only drained on a
  // message-tick, so this test drives one directly through the same runtime
  // client the app itself uses, rather than waiting on the background timer.
  await page.evaluate(() =>
    (window as any).__task4Fake.runtime.request({
      type: "message-tick",
      timestampMicros: Math.round(performance.now() * 1000),
    }),
  );
  const download = await downloadPromise;

  expect(download.suggestedFilename()).toBe("fixture-export.txt");
  const path = await download.path();
  if (path === null) throw new Error("download did not save to a local path");
  const { readFile } = await (new Function("return import('node:fs/promises')")() as Promise<{
    readFile(path: string, encoding: string): Promise<string>;
  }>);
  expect(await readFile(path, "utf8")).toBe("fixture export\n");
});
