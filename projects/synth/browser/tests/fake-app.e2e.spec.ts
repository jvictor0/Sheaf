import { expect, test, type Page } from "@playwright/test";
import { installRealFakeApp, stopRealFakeApp, synthNode, type FrameObservation } from "./helpers/fake-app.js";

const fakeAcceptance = { shell: false, sync: false, gestures: false, narrow: false };


test.afterAll(async () => {
  if (!Object.values(fakeAcceptance).every(Boolean)) return;
  const processInfo = (globalThis as any).process as { env: Record<string, string | undefined>; ppid: number };
  const gateFile = processInfo.env.SYNTH_BROWSER_FAKE_GATE ?? `/tmp/sheaf-synth-browser-fake-app-${processInfo.ppid}`;
  const { writeFile } = await (new Function("return import('node:fs/promises')")() as Promise<{
    writeFile(path: string, contents: string): Promise<void>;
  }>);
  await writeFile(gateFile, "passed\n");
});


test.afterEach(async ({ page }) => {
  await stopRealFakeApp(page);
});

async function assertNoContentSidebarOverlap(page: Page): Promise<void> {
  const geometry = await page.evaluate(() => {
    const content = document.querySelector<HTMLElement>('[data-synth-node-id="fake-browser-root"]')!.getBoundingClientRect();
    const sidebar = document.querySelector<HTMLElement>('[data-synth-node-id="runtime.sidebar.root"]')!.getBoundingClientRect();
    const composite = document.querySelector<HTMLElement>('[data-synth-node-id="runtime.main.root"]')!.getBoundingClientRect();
    return {
      contentRight: content.right,
      sidebarLeft: sidebar.left,
      sidebarRight: sidebar.right,
      compositeRight: composite.right,
      viewportWidth: document.documentElement.clientWidth,
    };
  });
  expect(geometry.contentRight).toBeLessThanOrEqual(geometry.sidebarLeft + 0.5);
  expect(geometry.sidebarRight).toBeLessThanOrEqual(geometry.compositeRight + 0.5);
  expect(geometry.compositeRight).toBeLessThanOrEqual(geometry.viewportWidth + 0.5);
}

test("real fake-app WASM renders and refreshes the shared runtime shell", async ({ page }) => {
  await page.setViewportSize({ width: 1200, height: 800 });
  await installRealFakeApp(page);

  await expect(page.locator('[data-synth-node-id="runtime.main.root"]')).toHaveCount(1);
  await expect(page.locator('[data-synth-node-id="fake-browser-root"]')).toBeVisible();
  await expect(page.locator('[data-synth-node-id="runtime.sidebar.root"]')).toBeVisible();
  await assertNoContentSidebarOverlap(page);

  for (const pageName of ["audio", "controllers", "sync", "file"] as const) {
    await page.locator(`[data-synth-node-id="runtime.sidebar.${pageName}"]`).click();
    await expect(page.locator(`[data-synth-node-id="runtime.${pageName}.root"]`)).toBeVisible();
    await expect(page.locator('[data-synth-node-id="fake-browser-root"]')).toHaveCount(0);
    await page.locator(`[data-synth-node-id="runtime.${pageName}.back"]`).click();
    await expect(page.locator('[data-synth-node-id="fake-browser-root"]')).toBeVisible();
  }

  const frameShape = await page.evaluate(() => {
    const frames = (window as any).__task4Fake.observations.frames as FrameObservation[];
    const initial = frames[0];
    const childIds = new Set(initial.nodes.flatMap((node) => node.children));
    return {
      roots: initial.nodes.filter((node) => !childIds.has(node.id)).map((node) => node.id),
      frameCount: frames.length,
    };
  });
  expect(frameShape.roots).toEqual(["runtime.main.root"]);
  expect(frameShape.frameCount).toBeGreaterThanOrEqual(7);
  const resources = await page.evaluate(() => (window as any).__task4Fake.resources);
  expect(resources).toMatchObject({
    contexts: 1,
    closes: 0,
    midiRequests: 1,
    runtimeClients: 1,
    nodeConnects: 1,
    nodeDisconnects: 0,
    materializations: 1,
    packageDisposals: 0,
  });
  expect(resources.resumes).toBeGreaterThanOrEqual(1);
  expect(await page.evaluate(() =>
    (window as any).__task4Fake.observations.commands.filter((command: { type: string }) => command.type === "load").length,
  )).toBe(1);
  fakeAcceptance.shell = true;
});

test("real fake-app WASM stages, validates, saves, and reopens Sync", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await installRealFakeApp(page);
  await page.locator('[data-synth-node-id="runtime.sidebar.sync"]').click();
  await expect(page.locator('[data-synth-node-id="runtime.sync.root"]')).toBeVisible();

  const sendClock = page.locator('[data-synth-node-id="runtime.sync.send_clock"] input');
  const receiveClock = page.locator('[data-synth-node-id="runtime.sync.receive_clock"] input');
  const sendTransport = page.locator('[data-synth-node-id="runtime.sync.send_transport"] input');
  const receiveTransport = page.locator('[data-synth-node-id="runtime.sync.receive_transport"] input');
  const ppqn = page.locator('[data-synth-node-id="runtime.sync.ppqn"] input');
  await expect(sendClock).not.toBeChecked();
  await expect(ppqn).toHaveValue("24");
  await sendClock.check();
  await receiveClock.check();
  await sendTransport.check();
  await receiveTransport.check();

  await ppqn.fill("96x");
  await expect(page.locator('[data-synth-node-id="runtime.sync.validation"]')).toContainText("1 to 960");
  await ppqn.fill("96");
  // A cleared validation error removes the node rather than emptying it. The
  // page used to emit the band unconditionally and only swap its text style,
  // which left a full-width strip painting no glyphs -- sru-48's "no text
  // conveys no information" criterion, fixed in task 6.2. Asserting absence is
  // stronger than asserting emptiness: an empty band would fail this too.
  await expect(page.locator('[data-synth-node-id="runtime.sync.validation"]')).toHaveCount(0);
  await expect(page.locator('[data-synth-node-id="runtime.sync.warning"]')).toContainText("nonstandard");
  await expect(page.locator('[data-synth-node-id="runtime.sync.bpm"]')).toContainText("Current BPM: 120.00");
  await expect(page.locator('[data-synth-node-id="runtime.sync.lock"]')).toContainText("Internal");
  await expect(page.locator('[data-synth-node-id="runtime.sync.source"]')).toContainText("no external source");

  const containment = await page.evaluate(() => {
    const root = document.querySelector<HTMLElement>('[data-synth-node-id="runtime.sync.root"]')!.getBoundingClientRect();
    return [...document.querySelectorAll<HTMLElement>('[data-synth-node-id^="runtime.sync."]')]
      .every((element) => {
        const bounds = element.getBoundingClientRect();
        return bounds.left >= root.left - 0.5 && bounds.top >= root.top - 0.5 &&
          bounds.right <= root.right + 0.5 && bounds.bottom <= root.bottom + 0.5;
      });
  });
  expect(containment).toBe(true);

  await page.locator('[data-synth-node-id="runtime.sync.back"]').click();
  await expect(page.locator('[data-synth-node-id="fake-browser-root"]')).toBeVisible();
  await page.locator('[data-synth-node-id="runtime.sidebar.sync"]').click();
  await expect(page.locator('[data-synth-node-id="runtime.sync.ppqn"] input')).toHaveValue("96");
  await expect(page.locator('[data-synth-node-id="runtime.sync.send_clock"] input')).toBeChecked();
  await expect(page.locator('[data-synth-node-id="runtime.sync.receive_clock"] input')).toBeChecked();
  await expect(page.locator('[data-synth-node-id="runtime.sync.send_transport"] input')).toBeChecked();
  await expect(page.locator('[data-synth-node-id="runtime.sync.receive_transport"] input')).toBeChecked();

  const actions = await page.evaluate(() =>
    (window as any).__task4Fake.observations.commands
      .filter((command: { type: string }) => command.type === "dispatch-action")
      .map((command: { name?: string; value?: string }) => [command.name, command.value]),
  );
  expect(actions).toEqual(expect.arrayContaining([
    ["runtime.sync.send_clock", "1"],
    ["runtime.sync.receive_clock", "1"],
    ["runtime.sync.send_transport", "1"],
    ["runtime.sync.receive_transport", "1"],
    ["runtime.sync.ppqn", "96"],
    ["runtime.sync.back", ""],
  ]));
  fakeAcceptance.sync = true;
});

test("real fake-app WASM dispatches incremental drag and double-click actions", async ({ page }) => {
  await installRealFakeApp(page);
  const gesture = await page.evaluate(() => {
    const frame = ((window as any).__task4Fake.observations.frames as FrameObservation[])[0];
    const drag = frame.nodes.find((node) => node.pointerDragAction);
    const doubleClick = frame.nodes.find((node) => node.doubleClickAction);
    return {
      dragId: drag?.id,
      dragAction: drag?.pointerDragAction?.name,
      doubleClickId: doubleClick?.id,
      doubleClickAction: doubleClick?.doubleClickAction?.name,
    };
  });
  expect(gesture.dragId).toBeTruthy();
  expect(gesture.doubleClickId).toBeTruthy();

  const dragTarget = page.locator(`[data-synth-node-id="${gesture.dragId}"]`);
  const box = await dragTarget.boundingBox();
  expect(box).not.toBeNull();
  await page.mouse.move(box!.x + 20, box!.y + 20);
  await page.mouse.down();
  await page.mouse.move(box!.x + 27, box!.y + 18, { steps: 2 });
  await page.mouse.move(box!.x + 35, box!.y + 14, { steps: 2 });
  await page.mouse.up();
  await page.locator(`[data-synth-node-id="${gesture.doubleClickId}"] canvas`).dispatchEvent("dblclick");

  await expect.poll(() => page.evaluate(() =>
    (window as any).__task4Fake.observations.commands.filter((command: { type: string }) => command.type === "dispatch-action").length,
  )).toBeGreaterThanOrEqual(3);
  const actions = await page.evaluate(() =>
    (window as any).__task4Fake.observations.commands.filter((command: { type: string }) => command.type === "dispatch-action"),
  );
  expect(actions.length, JSON.stringify(actions)).toBeGreaterThanOrEqual(3);
  expect(actions.filter((action: { name?: string }) => action.name === gesture.dragAction).length).toBeGreaterThanOrEqual(2);
  expect(actions.some((action: { name?: string }) => action.name === gesture.doubleClickAction)).toBe(true);
  fakeAcceptance.gestures = true;
});

test("real fake-app shared shell remains non-overlapping at narrow width", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await installRealFakeApp(page);
  await expect(page.locator('[data-synth-node-id="runtime.main.root"]')).toBeVisible();
  await assertNoContentSidebarOverlap(page);
  fakeAcceptance.narrow = true;
});
