import { expect, test, type Page } from "@playwright/test";
import { BrowserUiBackend, CommandBufferError, decodeCommandBuffer } from "../src/ui.js";
import { DrawKind, NodeKind, makeCommandBuffer } from "./fixtures/command-buffer.js";

const frame = makeCommandBuffer([
  { id: "root", kind: NodeKind.Root, bounds: [0, 0, 320, 160], children: ["scroll", "button", "toggle", "slider", "combo", "field", "draw"] },
  { id: "scroll", kind: NodeKind.ScrollArea, bounds: [0, 0, 100, 40], scrollContentWidth: 100, scrollContentHeight: 160, children: ["row"] },
  { id: "row", kind: NodeKind.Row, bounds: [0, 0, 90, 20], children: ["bottom"], doubleClickAction: { name: "generic.row", value: "open" } },
  { id: "bottom", kind: NodeKind.Label, bounds: [0, 120, 90, 20], text: "Bottom content" },
  { id: "button", kind: NodeKind.Button, bounds: [110, 0, 80, 20], label: "Activate", action: { name: "generic.button", value: "press" } },
  { id: "toggle", kind: NodeKind.Toggle, bounds: [110, 25, 80, 20], label: "Enabled", checked: false, action: { name: "generic.toggle", value: "" } },
  { id: "slider", kind: NodeKind.Slider, bounds: [110, 50, 80, 20], value: 3, minValue: 0, maxValue: 10, step: 1, action: { name: "generic.slider", value: "" }, pointerDragAction: { name: "generic.drag", value: "axis:0" } },
  { id: "combo", kind: NodeKind.ComboBox, bounds: [110, 75, 80, 20], selectedOption: "one", options: [{ id: "one", label: "One" }, { id: "two", label: "Two" }], action: { name: "generic.combo", value: "" } },
  { id: "field", kind: NodeKind.TextField, bounds: [110, 100, 80, 20], text: "before", action: { name: "generic.text", value: "" } },
  { id: "draw", kind: NodeKind.Draw, bounds: [210, 0, 80, 40], doubleClickAction: { name: "generic.draw", value: "open" }, draws: [
    { kind: DrawKind.Fill, color: [20, 30, 40, 255] },
    { kind: DrawKind.StrokeRect, bounds: [1, 2, 20, 15], color: [255, 0, 0, 255] },
    { kind: DrawKind.Line, from: { x: 0, y: 0 }, to: { x: 30, y: 20 }, color: [0, 255, 0, 255] },
    { kind: DrawKind.Arc, bounds: [4, 4, 12, 12], startRadians: 0, endRadians: 3.14 },
    { kind: DrawKind.Text, bounds: [2, 2, 40, 10], text: "draw" },
    { kind: DrawKind.FillEllipse, bounds: [3, 3, 10, 8] }, { kind: DrawKind.StrokeEllipse, bounds: [3, 3, 10, 8] },
    { kind: DrawKind.FillRoundedRect, bounds: [2, 2, 10, 8], cornerRadius: 2 }, { kind: DrawKind.StrokeRoundedRect, bounds: [2, 2, 10, 8], cornerRadius: 2 },
    { kind: DrawKind.Polyline, points: [{ x: 0, y: 0 }, { x: 5, y: 5 }] }, { kind: DrawKind.FillPolygon, points: [{ x: 0, y: 0 }, { x: 5, y: 0 }, { x: 2, y: 4 }] },
  ] },
]);

test.beforeEach(async ({ page }) => {
  await page.route("**/dist/src/main.js", (route) => route.fulfill({
    status: 200,
    contentType: "application/javascript",
    body: "",
  }));
});

test("renders portable controls, canvas draws, and reachable scroll content", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { backend: InstanceType<typeof BrowserUiBackend> };
    browserWindow.backend = new BrowserUiBackend(document.querySelector("#synth-root")!);
    browserWindow.backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(frame)));
  await expect(page.locator('[data-synth-node-id="button"][data-synth-node-kind="button"]')).toHaveText("Activate");
  await expect(page.locator('[data-synth-node-id="draw"] canvas')).toBeVisible();
  const scroll = page.locator('[data-synth-node-id="scroll"]');
  await expect(scroll).toHaveJSProperty("scrollHeight", 160);
  await scroll.evaluate((element) => { element.scrollTop = element.scrollHeight; });
  await expect(page.locator('[data-synth-node-id="bottom"]')).toBeInViewport();
  await page.locator('[data-synth-node-id="button"]').evaluate((element) => { (window as unknown as { firstButton: Element }).firstButton = element; });
  await page.evaluate(async (bytes) => {
    const browserWindow = window as unknown as { backend: { renderFrame(buffer: ArrayBuffer): void } };
    browserWindow.backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(frame)));
  expect(await page.locator('[data-synth-node-id="button"]').evaluate((element) => element === (window as unknown as { firstButton: Element }).firstButton)).toBeTruthy();
  expect(await page.locator("[data-synth-node-id]").evaluateAll((nodes) => nodes.some((node) => /miniapp|fake/i.test(node.outerHTML)))).toBeFalsy();
});

test("dispatches portable controls and double-click actions", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const actions = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { actions: unknown[] };
    browserWindow.actions = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: unknown) => browserWindow.actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    return browserWindow.actions;
  }, Array.from(new Uint8Array(frame)));
  expect(actions).toEqual([]);
  await page.locator('[data-synth-node-id="button"]').click();
  await page.locator('[data-synth-node-id="toggle"] input').check();
  await page.locator('[data-synth-node-id="slider"] input').fill("7");
  await page.locator('[data-synth-node-id="combo"] select').selectOption("two");
  await page.locator('[data-synth-node-id="field"] input').fill("after");
  await page.locator('[data-synth-node-id="row"]').dblclick();
  await page.locator('[data-synth-node-id="draw"] canvas').dblclick();
  const dispatched = await page.evaluate(() => (window as unknown as { actions: unknown[] }).actions);
  expect(dispatched).toEqual([
    { name: "generic.button", value: "press" }, { name: "generic.toggle", value: "1" }, { name: "generic.slider", value: "7" },
    { name: "generic.combo", value: "two" }, { name: "generic.text", value: "after" }, { name: "generic.row", value: "open" },
    { name: "generic.draw", value: "open" },
  ]);
});

test("dispatches real mouse double-clicks on draggable draw controls", async ({ page }) => {
  const encoderFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 120, 120], children: ["encoder"] },
    { id: "encoder", kind: NodeKind.Draw, bounds: [20, 20, 60, 60],
      pointerDragAction: { name: "generic.drag", value: "axis:0" },
      doubleClickAction: { name: "generic.reset", value: "axis" },
      draws: [{ kind: DrawKind.FillEllipse, bounds: [20, 20, 60, 60], color: [20, 80, 90, 255] }] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { actions: unknown[] };
    browserWindow.actions = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: unknown) => browserWindow.actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(encoderFrame)));

  const box = await page.locator('[data-synth-node-id="encoder"]').boundingBox();
  expect(box).not.toBeNull();
  await page.mouse.dblclick(box!.x + box!.width / 2, box!.y + box!.height / 2);

  const dispatched = await page.evaluate(() => (window as unknown as { actions: unknown[] }).actions);
  expect(dispatched).toEqual([{ name: "generic.reset", value: "axis" }]);
});

// ---------------------------------------------------------------------------
// sru-52: pointer gestures over Draw and Button nodes. Mirrors the gesture
// tests at the end of juce/PortableJuceBackendTests.cpp.
// ---------------------------------------------------------------------------

type GestureAction = { name: string; value: string };
type GestureSpec = Partial<{ enabled: boolean; action: GestureAction; pointerDragAction: GestureAction; doubleClickAction: GestureAction }>;
const canvasDraws = [{ kind: DrawKind.Fill, color: [20, 30, 40, 255] as [number, number, number, number] }];

// One 100x100 canvas under a 200x200 root, carrying whatever combination of
// actions the case under test needs.
const canvasFrame = (canvas: GestureSpec) => makeCommandBuffer([
  { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 200], children: ["canvas"] },
  { id: "canvas", kind: NodeKind.Draw, bounds: [0, 0, 100, 100], draws: canvasDraws, ...canvas },
]);

async function renderRecording(page: Page, buffer: ArrayBuffer) {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { actions: { name: string }[] };
    browserWindow.actions = [];
    new BrowserUiBackend(document.querySelector("#synth-root")!,
      (action: { name: string }) => browserWindow.actions.push(action))
      .renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(buffer)));
}

const dispatchedNames = (page: Page) =>
  page.evaluate(() => (window as unknown as { actions: { name: string }[] }).actions.map((action) => action.name));

const forgetDispatched = (page: Page) =>
  page.evaluate(() => { (window as unknown as { actions: unknown[] }).actions.length = 0; });

async function gestureCentreOf(page: Page, id: string) {
  const box = (await page.locator(`[data-node-id="${id}"]`).boundingBox())!;
  // A node resolved to zero extent could never be clicked, which would make
  // every gesture assertion below vacuously true.
  expect(box.width, `${id} clickable width`).toBeGreaterThan(0);
  expect(box.height, `${id} clickable height`).toBeGreaterThan(0);
  return { x: box.x + box.width / 2, y: box.y + box.height / 2 };
}

// The node a press at this point actually reaches, which is not always the node
// whose centre it is: a node that intercepts nothing lets the press fall through.
const nodeIdAtPoint = (page: Page, point: { x: number; y: number }) =>
  page.evaluate(({ x, y }) =>
    document.elementFromPoint(x, y)?.closest("[data-node-id]")?.getAttribute("data-node-id"), point);

// 30 px right: `30 * 0.0025` is 0.075, well past `continuePointerDrag`'s
// threshold, which mirrors `kPointerDragThreshold`.
async function dragPastThreshold(page: Page, from: { x: number; y: number }) {
  await page.mouse.move(from.x, from.y);
  await page.mouse.down();
  await page.mouse.move(from.x + 30, from.y);
  await page.mouse.up();
}

// One Draw node and one Button node carrying the same click and double-click
// actions, for the parity assertions.
const clickAndDoubleClickFrame = makeCommandBuffer([
  { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 200], children: ["canvas", "btn"] },
  { id: "canvas", kind: NodeKind.Draw, bounds: [0, 0, 100, 100], draws: canvasDraws,
    action: { name: "click", value: "" }, doubleClickAction: { name: "dbl", value: "" } },
  { id: "btn", kind: NodeKind.Button, bounds: [0, 100, 100, 100], label: "Btn",
    action: { name: "click", value: "" }, doubleClickAction: { name: "dbl", value: "" } },
]);

test("dispatches a plain click from a click-only draw node", async ({ page }) => {
  await renderRecording(page, canvasFrame({ action: { name: "canvas.click", value: "" } }));
  const centre = await gestureCentreOf(page, "canvas");
  await page.mouse.click(centre.x, centre.y);
  expect(await dispatchedNames(page)).toEqual(["canvas.click"]);
});

test("dispatches the same single-click sequence from a draw node as from a button", async ({ page }) => {
  await renderRecording(page, clickAndDoubleClickFrame);
  const canvasCentre = await gestureCentreOf(page, "canvas");
  await page.mouse.click(canvasCentre.x, canvasCentre.y);
  const fromDraw = await dispatchedNames(page);
  await forgetDispatched(page);
  const buttonCentre = await gestureCentreOf(page, "btn");
  await page.mouse.click(buttonCentre.x, buttonCentre.y);
  const fromButton = await dispatchedNames(page);

  expect(fromDraw).toEqual(fromButton);
  expect(fromDraw).toEqual(["click"]);
});

// The exact ordered list pins both halves of sru-52's drag clause at once: the
// pointer-drag action is dispatched, and no click action is. Deliberately not
// compared against a Button — design.md D10b's parity clause covers click and
// double-click only, because a JUCE Button has no pointer-drag path and no
// producer gives one a drag action.
test("dispatches no click from a draw drag past the drag threshold", async ({ page }) => {
  await renderRecording(page, canvasFrame({
    action: { name: "canvas.click", value: "" },
    pointerDragAction: { name: "canvas.drag", value: "" },
  }));
  const centre = await gestureCentreOf(page, "canvas");
  await dragPastThreshold(page, centre);
  // The DOM fires a native `click` for a press and release inside one element
  // however far the pointer travelled between them, so the drag has to consume
  // it — otherwise one gesture would be both a drag and a click.
  expect(await dispatchedNames(page)).toEqual(["canvas.drag"]);
});

// The suppression is per gesture, not sticky. Every other case here is one
// gesture on a freshly rendered node, so none of them would notice a suppression
// that outlived the drag that raised it.
test("keeps dispatching a click after a drag on the same draw node", async ({ page }) => {
  await renderRecording(page, canvasFrame({
    action: { name: "canvas.click", value: "" },
    pointerDragAction: { name: "canvas.drag", value: "" },
  }));
  const centre = await gestureCentreOf(page, "canvas");
  const box = (await page.locator('[data-node-id="canvas"]').boundingBox())!;

  await dragPastThreshold(page, centre);
  expect(await dispatchedNames(page)).toEqual(["canvas.drag"]);
  await forgetDispatched(page);
  await page.mouse.click(centre.x, centre.y);
  expect(await dispatchedNames(page)).toEqual(["canvas.click"]);
  await forgetDispatched(page);

  // And again for a drag that leaves the node: the captured pointer retargets the
  // release to the element, so this gesture still ends in a native `click` that
  // its own drag has to consume — and the click after it still dispatches.
  await page.mouse.move(centre.x, centre.y);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width + 40, centre.y);
  await page.mouse.up();
  expect(await dispatchedNames(page)).toEqual(["canvas.drag"]);
  await forgetDispatched(page);
  await page.mouse.click(centre.x, centre.y);
  expect(await dispatchedNames(page)).toEqual(["canvas.click"]);
});

test("dispatches nothing from a disabled draw node", async ({ page }) => {
  await renderRecording(page, canvasFrame({ enabled: false, action: { name: "canvas.click", value: "" } }));
  const centre = await gestureCentreOf(page, "canvas");
  // Disabled is a dispatch rule, not an interception one: the node still takes
  // the press. Without this, an empty action list would also be the result of
  // wrongly dropping `pointer-events` and letting the press land on a parent
  // that dispatches nothing anyway — a different bug wearing the same result.
  expect(await page.locator('[data-node-id="canvas"]').evaluate((element) => getComputedStyle(element).pointerEvents)).toBe("auto");
  expect(await nodeIdAtPoint(page, centre)).toBe("canvas");
  await page.mouse.click(centre.x, centre.y);
  expect(await dispatchedNames(page)).toEqual([]);
});

test("passes a click over an inert draw node through to the node behind it", async ({ page }) => {
  // sru-25: translucent visualizer underlays must keep passing clicks through
  // to the encoders beneath them. The underlay is declared last, so it would
  // take the click if it intercepted anything.
  await renderRecording(page, makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 200], children: ["encoder", "underlay"] },
    { id: "encoder", kind: NodeKind.Draw, bounds: [0, 0, 100, 100], draws: canvasDraws,
      action: { name: "encoder.click", value: "" } },
    { id: "underlay", kind: NodeKind.Draw, bounds: [0, 0, 100, 100], draws: canvasDraws },
  ]));
  const centre = await gestureCentreOf(page, "underlay");
  expect(await page.locator('[data-node-id="underlay"]').evaluate((element) => getComputedStyle(element).pointerEvents)).toBe("none");
  expect(await nodeIdAtPoint(page, centre)).toBe("encoder");
  await page.mouse.click(centre.x, centre.y);
  expect(await dispatchedNames(page)).toEqual(["encoder.click"]);
});

test("dispatches no click when a press on a draw node is released off it", async ({ page }) => {
  // Not a click in either backend: the DOM fires `click` on the common ancestor
  // of the press and the release, and `juce::Button` needs `isOver` at mouse-up.
  await renderRecording(page, canvasFrame({ action: { name: "canvas.click", value: "" } }));
  const centre = await gestureCentreOf(page, "canvas");
  const box = (await page.locator('[data-node-id="canvas"]').boundingBox())!;
  await page.mouse.move(centre.x, centre.y);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width + 40, box.y + box.height + 40);
  await page.mouse.up();
  expect(await dispatchedNames(page)).toEqual([]);
});

test("dispatches the same double-click sequence from a draw node as from a button", async ({ page }) => {
  await renderRecording(page, clickAndDoubleClickFrame);

  const canvasCentre = await gestureCentreOf(page, "canvas");
  await page.mouse.dblclick(canvasCentre.x, canvasCentre.y);
  const fromDraw = await dispatchedNames(page);
  await forgetDispatched(page);
  const buttonCentre = await gestureCentreOf(page, "btn");
  await page.mouse.dblclick(buttonCentre.x, buttonCentre.y);
  const fromButton = await dispatchedNames(page);

  expect(fromDraw).toEqual(fromButton);
  // Pinned as a literal, read off the observed sequence: the DOM's separate
  // `click` and `dblclick` listeners see both presses of a double click, so the
  // double-click action lands third and last. The JUCE backend, which derives
  // each click from a `mouseUp`, observes the same three in the same order.
  expect(fromDraw).toEqual(["click", "click", "dbl"]);
});

test("appends control values to action prefixes without losing option ids", async ({ page }) => {
  const prefixedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 320, 120], children: ["combo", "field", "toggle"] },
    { id: "combo", kind: NodeKind.ComboBox, bounds: [0, 0, 120, 24], selectedOption: "dev-a",
      options: [{ id: "dev-a", label: "Device A" }, { id: "dev-b", label: "Device B" }],
      action: { name: "controller.endpoint", value: "0:input" } },
    { id: "field", kind: NodeKind.TextField, bounds: [0, 32, 120, 24], text: "",
      action: { name: "mapping.value", value: "0:encoders:2:5" } },
    { id: "toggle", kind: NodeKind.Toggle, bounds: [0, 64, 120, 24], label: "Feedback", checked: false,
      action: { name: "mapping.value", value: "0:system_messages:1:25" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { actions: unknown[] };
    browserWindow.actions = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: unknown) => browserWindow.actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(prefixedFrame)));
  await page.locator('[data-synth-node-id="combo"] select').selectOption("dev-b");
  await page.locator('[data-synth-node-id="field"] input').fill("42");
  await page.locator('[data-synth-node-id="toggle"] input').check();
  const dispatched = await page.evaluate(() => (window as unknown as { actions: unknown[] }).actions);
  expect(dispatched).toEqual([
    { name: "controller.endpoint", value: "0:input:dev-b" },
    { name: "mapping.value", value: "0:encoders:2:5:42" },
    { name: "mapping.value", value: "0:system_messages:1:25:1" },
  ]);
});

test("dispatches each generic text input event with one separator-appended value", async ({ page }) => {
  const draftFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 80], children: ["draft"] },
    { id: "draft", kind: NodeKind.TextField, bounds: [0, 0, 160, 24], text: "",
      action: { name: "generic.rename_draft", value: "0:6d616e75616c" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { actions: unknown[] };
    browserWindow.actions = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: unknown) => browserWindow.actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(draftFrame)));
  await page.locator('[data-synth-node-id="draft"] input').pressSequentially("abc");
  const dispatched = await page.evaluate(() => (window as unknown as { actions: unknown[] }).actions);
  expect(dispatched).toEqual([
    { name: "generic.rename_draft", value: "0:6d616e75616c:a" },
    { name: "generic.rename_draft", value: "0:6d616e75616c:ab" },
    { name: "generic.rename_draft", value: "0:6d616e75616c:abc" },
  ]);
});

const disabledFrame = makeCommandBuffer([
  { id: "root", kind: NodeKind.Root, bounds: [0, 0, 320, 240], children: ["button", "toggle", "slider", "combo", "field", "row", "draw"] },
  { id: "button", kind: NodeKind.Button, bounds: [10, 10, 80, 20], label: "Submit", enabled: false, action: { name: "generic.button", value: "press" } },
  { id: "toggle", kind: NodeKind.Toggle, bounds: [10, 40, 80, 20], label: "Feedback", checked: false, enabled: false, action: { name: "generic.toggle", value: "" } },
  { id: "slider", kind: NodeKind.Slider, bounds: [10, 70, 80, 20], value: 3, minValue: 0, maxValue: 10, step: 1, enabled: false, action: { name: "generic.slider", value: "" } },
  { id: "combo", kind: NodeKind.ComboBox, bounds: [10, 100, 80, 20], selectedOption: "two", enabled: false,
    options: [{ id: "one", label: "One" }, { id: "two", label: "Two" }], action: { name: "generic.combo", value: "" } },
  { id: "field", kind: NodeKind.TextField, bounds: [10, 130, 80, 20], text: "before", enabled: false, action: { name: "generic.text", value: "" } },
  { id: "row", kind: NodeKind.Row, bounds: [120, 10, 80, 20], enabled: false, doubleClickAction: { name: "generic.row", value: "open" } },
  { id: "draw", kind: NodeKind.Draw, bounds: [120, 40, 80, 40], enabled: false,
    pointerDragAction: { name: "generic.drag", value: "axis:0" }, doubleClickAction: { name: "generic.draw", value: "open" } },
]);

test("renders disabled native controls and keeps their portable values", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(disabledFrame)));

  for (const [id, control] of [["button", ""], ["toggle", " input"], ["slider", " input"], ["combo", " select"], ["field", " input"]] as const)
    await expect(page.locator(`[data-synth-node-id="${id}"]${control}`)).toBeDisabled();
  for (const id of ["button", "toggle", "slider", "combo", "field", "row", "draw"])
    await expect(page.locator(`[data-synth-node-id="${id}"]`)).toHaveAttribute("aria-disabled", "true");
  await expect(page.locator('[data-synth-node-id="combo"] select')).toHaveValue("two");
  await expect(page.locator('[data-synth-node-id="combo"] select option')).toHaveText(["One", "Two"]);
  await expect(page.locator('[data-synth-node-id="field"] input')).toHaveValue("before");
  await expect(page.locator('[data-synth-node-id="slider"] input')).toHaveValue("3");
});

test("suppresses actions from disabled native controls", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const dispatched = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const actions: Array<{ name: string; value: string }> = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const controlIn = (id: string) => document.querySelector<HTMLInputElement | HTMLSelectElement>(`[data-synth-node-id="${id}"] :is(input, select)`)!;
    document.querySelector<HTMLButtonElement>('[data-synth-node-id="button"]')!.click();
    const toggle = controlIn("toggle") as HTMLInputElement;
    toggle.checked = true;
    toggle.dispatchEvent(new Event("change", { bubbles: true }));
    const slider = controlIn("slider") as HTMLInputElement;
    slider.value = "9";
    slider.dispatchEvent(new Event("input", { bubbles: true }));
    const combo = controlIn("combo") as HTMLSelectElement;
    combo.value = "one";
    combo.dispatchEvent(new Event("change", { bubbles: true }));
    const field = controlIn("field") as HTMLInputElement;
    field.value = "after";
    field.dispatchEvent(new Event("input", { bubbles: true }));
    return actions;
  }, Array.from(new Uint8Array(disabledFrame)));

  expect(dispatched).toEqual([]);
});

test("suppresses double-click and drag actions from disabled semantic nodes", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const actions: Array<{ name: string; value: string }> = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const row = document.querySelector<HTMLElement>('[data-synth-node-id="row"]')!;
    const draw = document.querySelector<HTMLElement>('[data-synth-node-id="draw"]')!;
    const captures: number[] = [];
    draw.setPointerCapture = (pointerId) => { captures.push(pointerId); };
    draw.releasePointerCapture = () => {};
    row.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    draw.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    draw.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, pointerId: 5, clientX: 10, clientY: 10 }));
    draw.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 5, clientX: 40, clientY: 10 }));
    draw.dispatchEvent(new PointerEvent("pointerup", { bubbles: true, pointerId: 5, clientX: 40, clientY: 10 }));
    await new Promise((resolve) => requestAnimationFrame(() => resolve(undefined)));
    return { actions, captures };
  }, Array.from(new Uint8Array(disabledFrame)));

  expect(result).toEqual({ actions: [], captures: [] });
});

test("stops an in-flight drag when its node becomes disabled", async ({ page }) => {
  const draggableFrames = ([true, false] as const).map((enabled) => makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 100], children: ["drag"] },
    { id: "drag", kind: NodeKind.Draw, bounds: [10, 10, 40, 40], enabled, pointerDragAction: { name: "generic.drag", value: "axis:0" } },
  ]));
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async ([enabledBytes, disabledBytes]) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const actions: Array<{ name: string; value: string }> = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(enabledBytes).buffer);
    const drag = document.querySelector<HTMLElement>('[data-synth-node-id="drag"]')!;
    const releases: number[] = [];
    drag.setPointerCapture = () => {};
    drag.releasePointerCapture = (pointerId) => { releases.push(pointerId); };
    drag.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, pointerId: 6, clientX: 10, clientY: 10 }));
    backend.renderFrame(new Uint8Array(disabledBytes).buffer);
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 6, clientX: 60, clientY: 10 }));
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 6, clientX: 110, clientY: 10 }));
    return { actions, releases };
  }, [Array.from(new Uint8Array(draggableFrames[0])), Array.from(new Uint8Array(draggableFrames[1]))]);

  expect(result).toEqual({ actions: [], releases: [6] });
});

test("keeps dispatching once a previously disabled node becomes enabled", async ({ page }) => {
  const enabledFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 320, 240], children: ["button", "combo", "field", "row"] },
    { id: "button", kind: NodeKind.Button, bounds: [10, 10, 80, 20], label: "Submit", action: { name: "generic.button", value: "press" } },
    { id: "combo", kind: NodeKind.ComboBox, bounds: [10, 40, 80, 20], selectedOption: "two",
      options: [{ id: "one", label: "One" }, { id: "two", label: "Two" }], action: { name: "generic.combo", value: "" } },
    { id: "field", kind: NodeKind.TextField, bounds: [10, 70, 80, 20], text: "before", action: { name: "generic.text", value: "" } },
    { id: "row", kind: NodeKind.Row, bounds: [120, 10, 80, 20], doubleClickAction: { name: "generic.row", value: "open" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async ({ disabled, enabled }) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { actions: unknown[]; backend: { renderFrame(buffer: ArrayBuffer): void } };
    browserWindow.actions = [];
    browserWindow.backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: unknown) => browserWindow.actions.push(action));
    browserWindow.backend.renderFrame(new Uint8Array(disabled).buffer);
    browserWindow.backend.renderFrame(new Uint8Array(enabled).buffer);
  }, { disabled: Array.from(new Uint8Array(disabledFrame)), enabled: Array.from(new Uint8Array(enabledFrame)) });

  await page.locator('[data-synth-node-id="button"]').click();
  await page.locator('[data-synth-node-id="combo"] select').selectOption("one");
  await page.locator('[data-synth-node-id="field"] input').fill("after");
  await page.locator('[data-synth-node-id="row"]').dblclick();
  expect(await page.evaluate(() => (window as unknown as { actions: unknown[] }).actions)).toEqual([
    { name: "generic.button", value: "press" },
    { name: "generic.combo", value: "one" },
    { name: "generic.text", value: "after" },
    { name: "generic.row", value: "open" },
  ]);
  for (const id of ["button", "combo", "field", "row"])
    await expect(page.locator(`[data-synth-node-id="${id}"]`)).not.toHaveAttribute("aria-disabled");
});

test("preserves focused combo boxes across stale frame refreshes", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { backend: InstanceType<typeof BrowserUiBackend> };
    browserWindow.backend = new BrowserUiBackend(document.querySelector("#synth-root")!);
    browserWindow.backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(frame)));

  const snapshot = await page.evaluate((bytes) => {
    const select = document.querySelector<HTMLSelectElement>('[data-synth-node-id="combo"] select')!;
    select.focus();
    select.value = "two";
    const firstOption = select.options[0];
    (window as unknown as { firstComboOption: HTMLOptionElement }).firstComboOption = firstOption;
    const browserWindow = window as unknown as { backend: { renderFrame(buffer: ArrayBuffer): void } };
    browserWindow.backend.renderFrame(new Uint8Array(bytes).buffer);
    return {
      value: select.value,
      firstOptionPreserved: select.options[0] === (window as unknown as { firstComboOption: HTMLOptionElement }).firstComboOption,
      optionLabels: Array.from(select.options).map((option) => option.textContent),
    };
  }, Array.from(new Uint8Array(frame)));

  expect(snapshot).toEqual({
    value: "two",
    firstOptionPreserved: true,
    optionLabels: ["One", "Two"],
  });
});

test("maps portable arc angles into the canvas angle basis", async ({ page }) => {
  const arcFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 120, 120], children: ["draw"] },
    { id: "draw", kind: NodeKind.Draw, bounds: [0, 0, 80, 80], draws: [
      { kind: DrawKind.Arc, bounds: [10, 10, 40, 40], startRadians: 0, endRadians: Math.PI / 2 },
    ] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const calls = await page.evaluate(async (bytes) => {
    const arcCalls: Array<{ start: number; end: number }> = [];
    const originalArc = CanvasRenderingContext2D.prototype.arc;
    CanvasRenderingContext2D.prototype.arc = function(...args: Parameters<CanvasRenderingContext2D["arc"]>) {
      arcCalls.push({ start: args[3], end: args[4] });
      return originalArc.apply(this, args);
    };
    try {
      const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
      const backend = new BrowserUiBackend(document.querySelector("#synth-root")!);
      backend.renderFrame(new Uint8Array(bytes).buffer);
      return arcCalls;
    } finally {
      CanvasRenderingContext2D.prototype.arc = originalArc;
    }
  }, Array.from(new Uint8Array(arcFrame)));
  expect(calls).toHaveLength(1);
  expect(calls[0].start).toBeCloseTo(-Math.PI / 2, 6);
  expect(calls[0].end).toBeCloseTo(0, 6);
});

test("captures pointer drags and dispatches accepted incremental two-axis deltas", async ({ page }) => {
  const dragFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 100], children: ["drag", "plain"] },
    { id: "drag", kind: NodeKind.Draw, bounds: [10, 10, 40, 40], pointerDragAction: { name: "generic.drag", value: "0:3:0" } },
    { id: "plain", kind: NodeKind.Button, bounds: [60, 10, 60, 30], label: "Plain" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const actions: Array<{ name: string; value: string }> = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const drag = document.querySelector<HTMLElement>('[data-synth-node-id="drag"]')!;
    const plain = document.querySelector<HTMLElement>('[data-synth-node-id="plain"]')!;
    const captures: number[] = [];
    const releases: number[] = [];
    drag.setPointerCapture = (pointerId) => { captures.push(pointerId); };
    drag.releasePointerCapture = (pointerId) => { releases.push(pointerId); };
    plain.setPointerCapture = (pointerId) => { captures.push(pointerId + 100); };

    plain.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, pointerId: 4, clientX: 1, clientY: 1 }));
    drag.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, pointerId: 7, clientX: 10, clientY: 20 }));
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 7, clientX: 18, clientY: 16 }));
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 7, clientX: 20, clientY: 20 }));
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 7, clientX: 20.2, clientY: 20 }));
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 7, clientX: 20.6, clientY: 20 }));
    drag.dispatchEvent(new PointerEvent("pointerup", { bubbles: true, pointerId: 7, clientX: 20.6, clientY: 20 }));
    return { actions, captures, releases };
  }, Array.from(new Uint8Array(dragFrame)));

  expect(result.captures).toEqual([7]);
  expect(result.releases).toEqual([7]);
  expect(result.actions).toHaveLength(1);
  expect(result.actions[0].name).toBe("generic.drag");
  expect(Number(result.actions[0].value.split(":").at(-1))).toBeCloseTo(0.0265, 10);
});

test("compensates pointer movement for the current surface scale", async ({ page }) => {
  const dragFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 100], children: ["drag"] },
    { id: "drag", kind: NodeKind.Draw, bounds: [10, 10, 40, 40], pointerDragAction: { name: "generic.drag", value: "axis:0" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const host = document.querySelector<HTMLElement>("#synth-root")!;
    host.style.width = "100px";
    const actions: Array<{ name: string; value: string }> = [];
    const backend = new BrowserUiBackend(host, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const drag = document.querySelector<HTMLElement>('[data-synth-node-id="drag"]')!;
    drag.setPointerCapture = () => {};
    drag.releasePointerCapture = () => {};
    drag.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, pointerId: 3, clientX: 10, clientY: 20 }));
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 3, clientX: 14, clientY: 18 }));
    drag.dispatchEvent(new PointerEvent("pointerup", { bubbles: true, pointerId: 3, clientX: 14, clientY: 18 }));
    return actions;
  }, Array.from(new Uint8Array(dragFrame)));

  expect(result).toHaveLength(1);
  expect(result[0].name).toBe("generic.drag");
  expect(Number(result[0].value.slice("axis:".length))).toBeCloseTo(0.03, 10);
});

test("keeps captured drags alive outside and clears them on cancel and lost capture", async ({ page }) => {
  const dragFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 100], children: ["drag"] },
    { id: "drag", kind: NodeKind.Draw, bounds: [10, 10, 20, 20], pointerDragAction: { name: "generic.drag", value: "axis:0" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const actions: Array<{ name: string; value: string }> = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const drag = document.querySelector<HTMLElement>('[data-synth-node-id="drag"]')!;
    const captures: number[] = [];
    const releases: number[] = [];
    drag.setPointerCapture = (pointerId) => { captures.push(pointerId); };
    drag.releasePointerCapture = (pointerId) => { releases.push(pointerId); };
    const send = (type: string, pointerId: number, clientX: number) =>
      drag.dispatchEvent(new PointerEvent(type, { bubbles: true, pointerId, clientX, clientY: 10 }));

    send("pointerdown", 1, 10);
    send("pointermove", 1, 1000);
    send("pointercancel", 1, 1000);
    send("pointermove", 1, 1010);
    send("pointerdown", 2, 10);
    send("pointermove", 2, 20);
    send("lostpointercapture", 2, 20);
    send("pointermove", 2, 30);
    return { actions, captures, releases };
  }, Array.from(new Uint8Array(dragFrame)));

  expect(result.actions).toHaveLength(2);
  expect(Number(result.actions[0].value.slice("axis:".length))).toBeCloseTo(2.475, 10);
  expect(Number(result.actions[1].value.slice("axis:".length))).toBeCloseTo(0.025, 10);
  expect(result.captures).toEqual([1, 2]);
  expect(result.releases).toEqual([1]);
});

test("does not begin a drag when pointer capture fails", async ({ page }) => {
  const dragFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 100], children: ["drag"] },
    { id: "drag", kind: NodeKind.Draw, bounds: [10, 10, 20, 20], pointerDragAction: { name: "generic.drag", value: "axis:0" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const actions: Array<{ name: string; value: string }> = [];
    const errors: string[] = [];
    window.addEventListener("error", (event) => { errors.push(event.message); event.preventDefault(); });
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const drag = document.querySelector<HTMLElement>('[data-synth-node-id="drag"]')!;
    drag.setPointerCapture = () => { throw new DOMException("capture unavailable", "InvalidStateError"); };
    drag.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, pointerId: 4, clientX: 10, clientY: 10 }));
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 4, clientX: 20, clientY: 10 }));
    await new Promise((resolve) => setTimeout(resolve, 0));
    return { actions, errors };
  }, Array.from(new Uint8Array(dragFrame)));

  expect(result).toEqual({ actions: [], errors: [] });
});

test("allows only one pointer to drive each drag element", async ({ page }) => {
  const dragFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 100], children: ["drag"] },
    { id: "drag", kind: NodeKind.Draw, bounds: [10, 10, 20, 20], pointerDragAction: { name: "generic.drag", value: "axis:0" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const actions: Array<{ name: string; value: string }> = [];
    const captures: number[] = [];
    const releases: number[] = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const drag = document.querySelector<HTMLElement>('[data-synth-node-id="drag"]')!;
    drag.setPointerCapture = (pointerId) => { captures.push(pointerId); };
    drag.releasePointerCapture = (pointerId) => { releases.push(pointerId); };
    const send = (type: string, pointerId: number, clientX: number) =>
      drag.dispatchEvent(new PointerEvent(type, { bubbles: true, pointerId, clientX, clientY: 10 }));

    send("pointerdown", 1, 10);
    send("pointerdown", 2, 10);
    send("pointermove", 2, 30);
    send("pointermove", 1, 14);
    send("pointerup", 2, 30);
    send("pointerup", 1, 14);
    return { actions, captures, releases };
  }, Array.from(new Uint8Array(dragFrame)));

  expect(result.captures).toEqual([1]);
  expect(result.releases).toEqual([1]);
  expect(result.actions).toHaveLength(1);
  expect(result.actions[0].name).toBe("generic.drag");
  expect(Number(result.actions[0].value.slice("axis:".length))).toBeCloseTo(0.01, 10);
});

test("uses the current surface scale for each accepted drag increment", async ({ page }) => {
  const dragFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 100], children: ["drag"] },
    { id: "drag", kind: NodeKind.Draw, bounds: [10, 10, 20, 20], pointerDragAction: { name: "generic.drag", value: "axis:0" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const actions = await page.evaluate(async (bytes) => {
    let resize: ResizeObserverCallback = () => {};
    class ResizeObserverSpy {
      constructor(callback: ResizeObserverCallback) { resize = callback; }
      observe() {}
      disconnect() {}
    }
    (window as unknown as { ResizeObserver: typeof ResizeObserver }).ResizeObserver = ResizeObserverSpy as unknown as typeof ResizeObserver;
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const host = document.querySelector<HTMLElement>("#synth-root")!;
    host.style.width = "200px";
    const actions: Array<{ name: string; value: string }> = [];
    const backend = new BrowserUiBackend(host, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const drag = document.querySelector<HTMLElement>('[data-synth-node-id="drag"]')!;
    drag.setPointerCapture = () => {};
    drag.releasePointerCapture = () => {};
    drag.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, pointerId: 8, clientX: 10, clientY: 5 }));
    host.style.width = "100px";
    resize([], {} as ResizeObserver);
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 8, clientX: 14, clientY: 5 }));
    drag.dispatchEvent(new PointerEvent("pointerup", { bubbles: true, pointerId: 8, clientX: 14, clientY: 5 }));
    return actions;
  }, Array.from(new Uint8Array(dragFrame)));

  expect(actions).toHaveLength(1);
  expect(actions[0].name).toBe("generic.drag");
  expect(Number(actions[0].value.slice("axis:".length))).toBeCloseTo(0.02, 10);
});

// A host page (e.g. a stacked-mobile layout) may put its OWN transform on an
// ancestor between the surface root and a drag node -- distinct from
// whatever `fitSurface` last computed as `surfaceScale`. Before
// `effectiveScaleFor`'s introduction, drag delta was divided by
// `surfaceScale` alone, which never sees that extra transform: with the
// surface root left at `scale(1)` (host width == design width, so
// surfaceScale computes to 1) and an ancestor transform of `scale(2)`
// layered on top with no further `fitSurface` call, the OLD formula would
// have used a stale divisor of 1 and produced a delta twice the correct
// size. This is the durable regression net for `effectiveScaleFor`
// (ui.ts): the drag element's OWN rect (which reflects the ancestor
// transform) divided by its wire width, not the root-only scalar.
test("keeps drag sensitivity synced to a transform applied by the host to an ancestor other than the surface root", async ({ page }) => {
  const dragFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 100], children: ["drag"] },
    { id: "drag", kind: NodeKind.Draw, bounds: [10, 10, 40, 40], pointerDragAction: { name: "generic.drag", value: "axis:0" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const host = document.querySelector<HTMLElement>("#synth-root")!;
    host.style.width = "200px";
    const actions: Array<{ name: string; value: string }> = [];
    const backend = new BrowserUiBackend(host, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const drag = document.querySelector<HTMLElement>('[data-synth-node-id="drag"]')!;
    // Simulate a host-applied per-block/ancestor transform (the surface
    // root, `drag`'s parent here) that `fitSurface` never re-derives
    // `surfaceScale` from -- exactly the shape of a shell-side per-block
    // container transform layered on top of (or replacing) the root's own
    // fitSurface scale.
    drag.parentElement!.style.transformOrigin = "0 0";
    drag.parentElement!.style.transform = "scale(2)";
    drag.setPointerCapture = () => {};
    drag.releasePointerCapture = () => {};
    drag.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, pointerId: 11, clientX: 10, clientY: 20 }));
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 11, clientX: 14, clientY: 18 }));
    drag.dispatchEvent(new PointerEvent("pointerup", { bubbles: true, pointerId: 11, clientX: 14, clientY: 18 }));
    return actions;
  }, Array.from(new Uint8Array(dragFrame)));

  expect(result).toHaveLength(1);
  expect(result[0].name).toBe("generic.drag");
  // Correct (transform-robust) value: effective scale is 2 (root's own
  // scale(1) x the ancestor's scale(2)), so delta = ((4/2) - (-2/2)) * 0.0025.
  // The pre-fix formula (dividing by the stale surfaceScale=1) would have
  // produced 0.015 instead -- verified by reverting effectiveScaleFor's
  // introduction and re-running this exact test during development.
  expect(Number(result[0].value.slice("axis:".length))).toBeCloseTo(0.0075, 10);
});

test("preserves focused edits while a stale frame is rendered", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { backend: InstanceType<typeof BrowserUiBackend> };
    browserWindow.backend = new BrowserUiBackend(document.querySelector("#synth-root")!);
    browserWindow.backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(frame)));

  const slider = page.locator('[data-synth-node-id="slider"] input');
  const field = page.locator('[data-synth-node-id="field"] input');
  await slider.focus();
  await slider.fill("7");
  await page.evaluate(async (bytes) => {
    (window as unknown as { backend: { renderFrame(buffer: ArrayBuffer): void } }).backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(frame)));
  await expect(slider).toHaveValue("7");

  await field.focus();
  await field.fill("after");
  await page.evaluate(async (bytes) => {
    (window as unknown as { backend: { renderFrame(buffer: ArrayBuffer): void } }).backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(frame)));
  await expect(field).toHaveValue("after");
});

test("fills explicit draw bounds without changing whole-canvas fills", async ({ page }) => {
  const fillFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 20, 20], children: ["draw"] },
    { id: "draw", kind: NodeKind.Draw, bounds: [0, 0, 20, 20], draws: [
      { kind: DrawKind.Fill, color: [0, 0, 0, 255] },
      { kind: DrawKind.Fill, bounds: [5, 5, 10, 10], color: [255, 0, 0, 255] },
    ] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const pixels = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const context = document.querySelector<HTMLCanvasElement>('[data-synth-node-id="draw"] canvas')!.getContext("2d")!;
    return {
      outside: Array.from(context.getImageData(1, 1, 1, 1).data),
      inside: Array.from(context.getImageData(6, 6, 1, 1).data),
    };
  }, Array.from(new Uint8Array(fillFrame)));
  expect(pixels).toEqual({ outside: [0, 0, 0, 255], inside: [255, 0, 0, 255] });
});

test("draws arcs with isolated round cap and join state", async ({ page }) => {
  const arcFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 40, 40], children: ["draw"] },
    { id: "draw", kind: NodeKind.Draw, bounds: [0, 0, 40, 40], draws: [
      { kind: DrawKind.Arc, bounds: [5, 5, 10, 10], startRadians: 0, endRadians: 0.0001 },
      { kind: DrawKind.Line, from: { x: 0, y: 0 }, to: { x: 10, y: 10 } },
    ] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const calls = await page.evaluate(async (bytes) => {
    const calls: string[] = [];
    let lineCap = "butt";
    let lineJoin = "miter";
    const stack: Array<[string, string]> = [];
    const context = {
      fillStyle: "", strokeStyle: "", lineWidth: 1, font: "", textAlign: "left",
      get lineCap() { return lineCap; },
      set lineCap(value: string) { lineCap = value; calls.push(`lineCap:${value}`); },
      get lineJoin() { return lineJoin; },
      set lineJoin(value: string) { lineJoin = value; calls.push(`lineJoin:${value}`); },
      save() { stack.push([lineCap, lineJoin]); calls.push("save"); },
      restore() { [lineCap, lineJoin] = stack.pop()!; calls.push("restore"); },
      translate() {}, beginPath() {}, moveTo() {}, lineTo() {},
      arc() { calls.push("arc"); },
      stroke() { calls.push(`stroke:${lineCap}:${lineJoin}`); },
      fillRect() {}, strokeRect() {}, fillText() {}, ellipse() {}, fill() {}, roundRect() {},
    };
    (HTMLCanvasElement.prototype as unknown as { getContext: () => unknown }).getContext = () => context;
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    return calls;
  }, Array.from(new Uint8Array(arcFrame)));

  expect(calls).toEqual([
    "save", "lineCap:round", "lineJoin:round", "arc", "stroke:round:round", "restore", "stroke:butt:miter",
  ]);
});

// Re-pinned from the retired auto-flow contract: the cursor that placed these
// unbounded controls is gone, so each renders at its parent's origin with zero
// extent (sprs-6) rather than at a backend-chosen position and size.
test("does not flow unbounded controls after explicit draw content", async ({ page }) => {
  const layoutFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 320, 240], children: ["draw", "label", "button", "slider"] },
    { id: "draw", kind: NodeKind.Draw, bounds: [20, 16, 280, 80] },
    { id: "label", kind: NodeKind.Label, text: "Controls" },
    { id: "button", kind: NodeKind.Button, label: "Trigger" },
    { id: "slider", kind: NodeKind.Slider, value: 0.5, minValue: 0, maxValue: 1, step: 0.01 },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const bounds = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const read = (id: string) => {
      const element = document.querySelector<HTMLElement>(`[data-synth-node-id="${id}"]`)!;
      const rect = element.getBoundingClientRect();
      return {
        rect: { x: rect.x, y: rect.y, width: rect.width, height: rect.height },
        extent: { left: element.style.left, top: element.style.top, width: element.style.width, height: element.style.height },
      };
    };
    return { root: read("root"), draw: read("draw"), label: read("label"), button: read("button"), slider: read("slider") };
  }, Array.from(new Uint8Array(layoutFrame)));

  expect(bounds.root.rect).toEqual({ x: 0, y: 0, width: 320, height: 240 });
  expect(bounds.draw.rect).toEqual({ x: 20, y: 16, width: 280, height: 80 });
  for (const unbounded of [bounds.label, bounds.button, bounds.slider]) {
    // Each is placed at its parent's origin with zero extent (sprs-6) — and the
    // *rendered* extent is zero too, not just the inline style. A `<button>`'s
    // border-box floor is its own border and padding, which would otherwise
    // render 26x2 pixels for a node the tree says has none.
    expect(unbounded.extent).toEqual({ left: "0px", top: "0px", width: "0px", height: "0px" });
    expect(unbounded.rect).toEqual({ x: 0, y: 0, width: 0, height: 0 });
  }
});

// Re-pinned from the retired auto-sizing contract: the backend no longer grows a
// control to contain its label. It clips the label inside the resolved extent
// (sru-49) so no neighbour moves.
test("clips a long toggle label inside its resolved extent", async ({ page }) => {
  const labelFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 320, 120], children: ["toggle", "button"] },
    { id: "toggle", kind: NodeKind.Toggle, bounds: [0, 0, 60, 24], label: "Long Modifier", checked: false },
    { id: "button", kind: NodeKind.Button, bounds: [70, 0, 80, 24], label: "Following" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const layout = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const toggle = document.querySelector('[data-synth-node-id="toggle"]')! as HTMLElement;
    const button = document.querySelector('[data-synth-node-id="button"]')! as HTMLElement;
    const toggleBounds = toggle.getBoundingClientRect();
    const buttonBounds = button.getBoundingClientRect();
    return {
      toggleRight: toggleBounds.right,
      toggleWidth: toggleBounds.width,
      buttonLeft: buttonBounds.left,
      toggleClientWidth: toggle.clientWidth,
      toggleScrollWidth: toggle.scrollWidth,
      toggleOverflow: getComputedStyle(toggle).overflowX,
    };
  }, Array.from(new Uint8Array(labelFrame)));

  expect(layout.toggleWidth).toBe(60);
  expect(layout.toggleRight).toBe(60);
  // The label genuinely does not fit, and the backend clips it rather than
  // widening the node, so the following control keeps its own resolved x.
  expect(layout.toggleScrollWidth).toBeGreaterThan(layout.toggleClientWidth);
  expect(layout.toggleOverflow).toBe("hidden");
  expect(layout.buttonLeft).toBe(70);
});

// Re-pinned from the retired auto-flow contract. The numbers are unchanged
// because they are now the producer's resolved bounds rather than the cursor's
// output: 376 wide within a 400 root at margin 12, and the next control 8 below.
test("fits long status text inside its resolved extent without moving the next control", async ({ page }) => {
  const text = "x".repeat(80);
  const labelFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 400, 60], children: ["status", "button"] },
    { id: "status", kind: NodeKind.StatusText, bounds: [12, 12, 376, 22], text },
    { id: "button", kind: NodeKind.Button, bounds: [12, 42, 72, 28], label: "Following" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const layout = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const statusElement = document.querySelector<HTMLElement>('[data-synth-node-id="status"]')!;
    const status = statusElement.getBoundingClientRect();
    const button = document.querySelector<HTMLElement>('[data-synth-node-id="button"]')!.getBoundingClientRect();
    const statusStyle = getComputedStyle(statusElement);
    return {
      status: { left: status.left, right: status.right, width: status.width, bottom: status.bottom },
      statusClientWidth: statusElement.clientWidth,
      statusScrollWidth: statusElement.scrollWidth,
      statusOverflow: statusStyle.overflowX,
      statusTextOverflow: statusStyle.textOverflow,
      button: { left: button.left, top: button.top },
    };
  }, Array.from(new Uint8Array(labelFrame)));

  expect(layout.status.width).toBe(376);
  expect(layout.status.right).toBe(388);
  // The text overruns its resolved extent, and the backend truncates it there
  // rather than expanding the node, so the next control does not move.
  expect(layout.statusScrollWidth).toBeGreaterThan(layout.statusClientWidth);
  expect(layout.statusOverflow).toBe("hidden");
  expect(layout.statusTextOverflow).toBe("ellipsis");
  expect(layout.button.top).toBeGreaterThanOrEqual(layout.status.bottom + 8);
});

test("clips realistic label and status text to their resolved height", async ({ page }) => {
  const labelFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 260, 120], children: ["label", "status", "button"] },
    { id: "label", kind: NodeKind.Label, bounds: [12, 12, 236, 22], text: "MIDI OUTPUT DEVICE CONFIGURATION STATUS" },
    { id: "status", kind: NodeKind.StatusText, bounds: [12, 42, 236, 22], text: "SYSTEM DEFAULT AUDIO OUTPUT IS READY FOR PERFORMANCE" },
    { id: "button", kind: NodeKind.Button, bounds: [12, 72, 72, 28], label: "Following" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const layout = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const read = (id: string) => {
      const element = document.querySelector<HTMLElement>(`[data-synth-node-id="${id}"]`)!;
      const style = getComputedStyle(element);
      const bounds = element.getBoundingClientRect();
      return {
        top: bounds.top, bottom: bounds.bottom,
        clientHeight: element.clientHeight, scrollHeight: element.scrollHeight,
        whiteSpace: style.whiteSpace, overflow: style.overflow, textOverflow: style.textOverflow,
      };
    };
    return { label: read("label"), status: read("status"), button: read("button") };
  }, Array.from(new Uint8Array(labelFrame)));

  for (const text of [layout.label, layout.status]) {
    expect(text.whiteSpace).toBe("nowrap");
    expect(text.overflow).toBe("hidden");
    expect(text.textOverflow).toBe("ellipsis");
    expect(text.scrollHeight).toBeLessThanOrEqual(text.clientHeight);
  }
  expect(layout.status.top).toBeGreaterThanOrEqual(layout.label.bottom + 8);
  expect(layout.button.top).toBeGreaterThanOrEqual(layout.status.bottom + 8);
});

// Re-pinned from the retired "host height includes flowed content" contract:
// the host height is the resolved root extent, and content the producer places
// past it does not grow the host (sprs-6).
test("derives the host height from the resolved root extent, not from content", async ({ page }) => {
  const overflowFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 30], children: ["label", "button"] },
    { id: "label", kind: NodeKind.Label, bounds: [0, 0, 200, 22], text: "A label that consumes the available row width" },
    { id: "button", kind: NodeKind.Button, bounds: [0, 40, 72, 28], label: "Below" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const layout = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const host = document.querySelector<HTMLElement>("#synth-root")!.getBoundingClientRect();
    const button = document.querySelector<HTMLElement>('[data-synth-node-id="button"]')!.getBoundingClientRect();
    return { hostBottom: host.bottom, hostHeight: host.height, buttonBottom: button.bottom };
  }, Array.from(new Uint8Array(overflowFrame)));

  expect(layout.hostHeight).toBe(30);
  expect(layout.buttonBottom).toBeGreaterThan(layout.hostBottom);
});

test("keeps scroll descendants out of the outer surface extent", async ({ page }) => {
  const scrollFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 300, 160], children: ["scroll"] },
    { id: "scroll", kind: NodeKind.ScrollArea, bounds: [10, 10, 100, 100], scrollContentWidth: 100, scrollContentHeight: 2000, children: ["deep"] },
    { id: "deep", kind: NodeKind.Label, bounds: [10, 1900, 80, 20], text: "Deep content" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const layout = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const host = document.querySelector<HTMLElement>("#synth-root")!;
    const scroll = document.querySelector<HTMLElement>('[data-synth-node-id="scroll"]')!;
    const deep = document.querySelector<HTMLElement>('[data-synth-node-id="deep"]')!;
    scroll.scrollTop = deep.offsetTop;
    const scrollBounds = scroll.getBoundingClientRect();
    const deepBounds = deep.getBoundingClientRect();
    return {
      hostHeight: host.getBoundingClientRect().height,
      scrollHeight: scroll.scrollHeight,
      deepVisible: deepBounds.top >= scrollBounds.top && deepBounds.bottom <= scrollBounds.bottom,
    };
  }, Array.from(new Uint8Array(scrollFrame)));

  expect(layout.hostHeight).toBe(160);
  expect(layout.scrollHeight).toBe(2000);
  expect(layout.deepVisible).toBeTruthy();
});

test("places nested sidebar descendants from parent-relative bounds", async ({ page }) => {
  const compositeFrame = makeCommandBuffer([
    { id: "main", kind: NodeKind.Root, bounds: [0, 0, 996, 200], children: ["app", "sidebar"] },
    { id: "app", kind: NodeKind.Root, bounds: [0, 0, 900, 200], children: ["app-status", "app-button"] },
    { id: "app-status", kind: NodeKind.StatusText, bounds: [12, 12, 876, 22], text: "x".repeat(160) },
    { id: "app-button", kind: NodeKind.Button, bounds: [12, 42, 72, 28], label: "Next" },
    { id: "sidebar", kind: NodeKind.Root, bounds: [900, 0, 96, 200], children: ["side-button"] },
    { id: "side-button", kind: NodeKind.Button, bounds: [0, 0, 96, 28], label: "Side" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const layout = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const read = (id: string) => {
      const element = document.querySelector<HTMLElement>(`[data-synth-node-id="${id}"]`)!;
      const rect = element.getBoundingClientRect();
      return { styleLeft: element.style.left, left: rect.left, right: rect.right, top: rect.top };
    };
    return { main: read("main"), appStatus: read("app-status"), appButton: read("app-button"), sidebar: read("sidebar"), sideButton: read("side-button") };
  }, Array.from(new Uint8Array(compositeFrame)));

  expect(layout.sidebar.styleLeft).toBe("900px");
  expect(layout.sideButton.styleLeft).toBe("0px");
  expect(layout.sidebar.left).toBe(900);
  expect(layout.sideButton.left).toBe(900);
  expect(layout.appStatus.right).toBeLessThanOrEqual(900);
  expect(layout.appButton.right).toBeLessThanOrEqual(900);
  expect(layout.appButton.top).toBeGreaterThan(layout.appStatus.top);
});

test("keeps the scale transform only on the current parentless root", async ({ page }) => {
  const firstFrame = makeCommandBuffer([
    { id: "app", kind: NodeKind.Root, bounds: [0, 0, 200, 100] },
  ]);
  const compositeFrame = makeCommandBuffer([
    { id: "main", kind: NodeKind.Root, bounds: [0, 0, 300, 100], children: ["app", "sidebar"] },
    { id: "app", kind: NodeKind.Root, bounds: [0, 0, 200, 100] },
    { id: "sidebar", kind: NodeKind.Root, bounds: [200, 0, 100, 100] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const transforms = await page.evaluate(async ({ first, composite }) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const host = document.querySelector<HTMLElement>("#synth-root")!;
    host.style.width = "100px";
    const backend = new BrowserUiBackend(host);
    backend.renderFrame(new Uint8Array(first).buffer);
    const firstTransform = document.querySelector<HTMLElement>('[data-synth-node-id="app"]')!.style.transform;
    backend.renderFrame(new Uint8Array(composite).buffer);
    const main = document.querySelector<HTMLElement>('[data-synth-node-id="main"]')!;
    const app = document.querySelector<HTMLElement>('[data-synth-node-id="app"]')!;
    return { firstTransform, mainTransform: main.style.transform, appTransform: app.style.transform, appWidth: app.getBoundingClientRect().width };
  }, { first: Array.from(new Uint8Array(firstFrame)), composite: Array.from(new Uint8Array(compositeFrame)) });

  expect(transforms.firstTransform).toBe("scale(0.5)");
  expect(transforms.mainTransform).toMatch(/^scale\(0\.333/);
  expect(transforms.appTransform).toBe("");
  expect(transforms.appWidth).toBeCloseTo(200 / 3, 4);
});

test("rejects cyclic node graphs without recursive overflow", async ({ page }) => {
  const cyclicFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 100], children: ["a"] },
    { id: "a", kind: NodeKind.Row, bounds: [0, 0, 10, 10], children: ["b"] },
    { id: "b", kind: NodeKind.Row, bounds: [0, 0, 10, 10], children: ["a"] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const message = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    try {
      new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
      return "no error";
    } catch (error) {
      return error instanceof Error ? `${error.name}: ${error.message}` : String(error);
    }
  }, Array.from(new Uint8Array(cyclicFrame)));

  expect(message).toMatch(/cycle/i);
});

test("reports stable generic errors for malformed node trees", async ({ page }) => {
  const cases = [
    {
      expected: "duplicate node id (node 1)",
      bytes: makeCommandBuffer([
        { id: "same", kind: NodeKind.Root, bounds: [0, 0, 10, 10] },
        { id: "same", kind: NodeKind.Root, bounds: [0, 0, 10, 10] },
      ]),
    },
    {
      expected: "unknown child node (node 0)",
      bytes: makeCommandBuffer([{ id: "root", kind: NodeKind.Root, bounds: [0, 0, 10, 10], children: ["missing"] }]),
    },
    {
      expected: "node child has multiple parents",
      bytes: makeCommandBuffer([
        { id: "root", kind: NodeKind.Root, bounds: [0, 0, 10, 10], children: ["a", "b"] },
        { id: "a", kind: NodeKind.Row, bounds: [0, 0, 10, 10], children: ["child"] },
        { id: "b", kind: NodeKind.Row, bounds: [0, 0, 10, 10], children: ["child"] },
        { id: "child", kind: NodeKind.Label, bounds: [0, 0, 10, 10] },
      ]),
    },
    {
      expected: "browser UI frame requires one parentless root, found 2",
      bytes: makeCommandBuffer([
        { id: "one", kind: NodeKind.Root, bounds: [0, 0, 10, 10] },
        { id: "two", kind: NodeKind.Root, bounds: [0, 0, 10, 10] },
      ]),
    },
    {
      expected: "browser UI frame requires one parentless root, found 0",
      bytes: makeCommandBuffer([
        { id: "a", kind: NodeKind.Row, bounds: [0, 0, 10, 10], children: ["b"] },
        { id: "b", kind: NodeKind.Row, bounds: [0, 0, 10, 10], children: ["a"] },
      ]),
    },
    {
      expected: "parentless browser UI node must be a root",
      bytes: makeCommandBuffer([{ id: "row", kind: NodeKind.Row, bounds: [0, 0, 10, 10] }]),
    },
  ];
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const messages = await page.evaluate(async (serializedCases) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    return serializedCases.map(({ bytes }) => {
      try {
        new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
        return "no error";
      } catch (error) {
        return error instanceof Error ? error.message : String(error);
      }
    });
  }, cases.map(({ bytes }) => ({ bytes: Array.from(new Uint8Array(bytes)) })));

  expect(messages).toEqual(cases.map(({ expected }) => expected));
});

test("a version-mismatched buffer fails loudly and renders no frame", async ({ page }) => {
  const staleFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 40, 40], children: ["label"] },
    { id: "label", kind: NodeKind.Label, bounds: [0, 0, 40, 20], text: "stale" },
  ], [], 1);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const host = document.querySelector("#synth-root")!;
    let message = "no error";
    try {
      new BrowserUiBackend(host).renderFrame(new Uint8Array(bytes).buffer);
    } catch (error) {
      message = error instanceof Error ? error.message : String(error);
    }
    return { message, childCount: host.childElementCount, html: host.innerHTML };
  }, Array.from(new Uint8Array(staleFrame)));

  expect(result.message).toMatch(/unsupported command buffer version/);
  expect(result.childCount).toBe(0);
  expect(result.html).not.toContain("stale");
});

test("dispose disconnects resize observation and releases active pointer capture", async ({ page }) => {
  const dragFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 100], children: ["drag"] },
    { id: "drag", kind: NodeKind.Draw, bounds: [0, 0, 20, 20], pointerDragAction: { name: "generic.drag", value: "axis:0" } },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    let disconnects = 0;
    class ResizeObserverSpy {
      observe() {}
      disconnect() { disconnects++; }
    }
    (window as unknown as { ResizeObserver: typeof ResizeObserver }).ResizeObserver = ResizeObserverSpy as unknown as typeof ResizeObserver;
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const actions: Array<{ name: string; value: string }> = [];
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!, (action: { name: string; value: string }) => actions.push(action));
    backend.renderFrame(new Uint8Array(bytes).buffer);
    const drag = document.querySelector<HTMLElement>('[data-synth-node-id="drag"]')!;
    const releases: number[] = [];
    drag.setPointerCapture = () => {};
    drag.releasePointerCapture = (pointerId) => { releases.push(pointerId); };
    drag.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, pointerId: 9, clientX: 1, clientY: 1 }));
    backend.dispose();
    drag.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 9, clientX: 20, clientY: 1 }));
    return { actions, disconnects, releases };
  }, Array.from(new Uint8Array(dragFrame)));

  expect(result).toEqual({ actions: [], disconnects: 1, releases: [9] });
});

test("dispose is idempotent and renderFrame cannot revive a disposed backend", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const result = await page.evaluate(async (bytes) => {
    let disconnects = 0;
    class ResizeObserverSpy {
      observe() {}
      disconnect() { disconnects++; }
    }
    (window as unknown as { ResizeObserver: typeof ResizeObserver }).ResizeObserver = ResizeObserverSpy as unknown as typeof ResizeObserver;
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const backend = new BrowserUiBackend(document.querySelector("#synth-root")!);
    backend.dispose();
    backend.dispose();
    let message = "no error";
    try {
      backend.renderFrame(new Uint8Array(bytes).buffer);
    } catch (error) {
      message = error instanceof Error ? error.message : String(error);
    }
    return { disconnects, message };
  }, Array.from(new Uint8Array(frame)));

  expect(result).toEqual({ disconnects: 1, message: "cannot render a disposed browser UI backend" });
});

// Re-pinned for node-local draw geometry (sru-46): the command that used to
// carry the node's surface bounds [40, 50, 20, 20] now carries the same
// rectangle at the node's own origin, and the same canvas pixel is painted.
test("paints node-local draw commands into positioned canvases", async ({ page }) => {
  const positionedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 100], children: ["draw"] },
    { id: "draw", kind: NodeKind.Draw, bounds: [40, 50, 20, 20], draws: [
      { kind: DrawKind.Fill, bounds: [0, 0, 20, 20], color: [12, 34, 56, 255] },
    ] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const pixel = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const context = document.querySelector<HTMLCanvasElement>('[data-synth-node-id="draw"] canvas')!.getContext("2d")!;
    return Array.from(context.getImageData(1, 1, 1, 1).data);
  }, Array.from(new Uint8Array(positionedFrame)));

  expect(pixel).toEqual([12, 34, 56, 255]);
});

// Re-pinned for node-local draw geometry: both endpoints are node-local by
// definition, so there is nothing left to classify. The surface line the old
// fixture drew from (35, 90) to (185, 90) inside a node at (30, 70) is the same
// line written node-locally, and it lands on the same canvas pixel.
test("paints line endpoints in node-local coordinates", async ({ page }) => {
  const positionedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 220, 180], children: ["draw"] },
    { id: "draw", kind: NodeKind.Draw, bounds: [30, 70, 160, 100], draws: [
      { kind: DrawKind.Line,
        from: { x: 5, y: 20 },
        to: { x: 155, y: 20 },
        color: [40, 100, 230, 255],
        strokeWidth: 3 },
    ] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const pixel = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const context = document.querySelector<HTMLCanvasElement>('[data-synth-node-id="draw"] canvas')!.getContext("2d")!;
    return Array.from(context.getImageData(5, 20, 1, 1).data);
  }, Array.from(new Uint8Array(positionedFrame)));

  expect(pixel).toEqual([40, 100, 230, 255]);
});

// Re-pinned likewise: every command in the buffer is node-local, so no command
// can reinterpret the space its neighbours are drawn in.
test("paints every command in a draw-node buffer with node-local geometry", async ({ page }) => {
  const positionedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 220, 180], children: ["draw"] },
    { id: "draw", kind: NodeKind.Draw, bounds: [30, 70, 160, 100], draws: [
      { kind: DrawKind.Line,
        from: { x: 5, y: 20 },
        to: { x: 70, y: 20 },
        color: [230, 170, 30, 255],
        strokeWidth: 3 },
      { kind: DrawKind.Line,
        from: { x: 70, y: 30 },
        to: { x: 155, y: 30 },
        color: [40, 100, 230, 255],
        strokeWidth: 3 },
    ] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const pixel = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const context = document.querySelector<HTMLCanvasElement>('[data-synth-node-id="draw"] canvas')!.getContext("2d")!;
    return Array.from(context.getImageData(5, 20, 1, 1).data);
  }, Array.from(new Uint8Array(positionedFrame)));

  expect(pixel).toEqual([230, 170, 30, 255]);
});

// A geometry-free command used to be the input that decided how the whole
// buffer was classified. Now it simply carries no geometry, and the commands
// around it are unaffected.
test("paints a geometry-free command without displacing the rest of the buffer", async ({ page }) => {
  const positionedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 100], children: ["draw"] },
    { id: "draw", kind: NodeKind.Draw, bounds: [30, 40, 40, 30], draws: [
      { kind: DrawKind.FillEllipse, color: [10, 20, 30, 255] },
      { kind: DrawKind.Fill, bounds: [0, 0, 40, 30], color: [30, 180, 70, 255] },
    ] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const pixel = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const context = document.querySelector<HTMLCanvasElement>('[data-synth-node-id="draw"] canvas')!.getContext("2d")!;
    return Array.from(context.getImageData(1, 1, 1, 1).data);
  }, Array.from(new Uint8Array(positionedFrame)));

  expect(pixel).toEqual([30, 180, 70, 255]);
});

// The guard on the deleted draw classifier. Every other paint fixture keeps its
// geometry inside the node, where restoring `drawCommandsLookLocal` plus
// `context.translate(-bounds.x, -bounds.y)` is an arithmetic identity and would
// still pass. These three commands are chosen so it is not: the 60-wide fill
// overhangs a 40-wide node and the third has a negative origin, so
// `boundsLookLocal` fails for them and the old code classified the *whole*
// buffer as surface-absolute — painting every command translated by minus the
// node origin, i.e. off the canvas entirely. Under the node-local contract
// (sru-46) the canvas origin is the node origin, and the overhang is simply
// clipped by the canvas. If any of these pixels reads transparent, a coordinate
// classifier has come back.
test("paints an overhanging draw buffer node-locally with no classifier fallback", async ({ page }) => {
  const overhangingFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 100], children: ["draw"] },
    { id: "draw", kind: NodeKind.Draw, bounds: [30, 40, 40, 30], draws: [
      { kind: DrawKind.Fill, bounds: [0, 0, 60, 30], color: [200, 40, 40, 255] },
      { kind: DrawKind.Fill, bounds: [-10, 20, 20, 6], color: [40, 200, 40, 255] },
      { kind: DrawKind.Fill, bounds: [0, 0, 10, 10], color: [40, 40, 200, 255] },
    ] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const pixels = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const canvas = document.querySelector<HTMLCanvasElement>('[data-synth-node-id="draw"] canvas')!;
    const context = canvas.getContext("2d")!;
    const at = (x: number, y: number) => Array.from(context.getImageData(x, y, 1, 1).data);
    return {
      size: [canvas.width, canvas.height],
      // Only the overhanging fill reaches here, and only because it was not
      // shifted by the node origin.
      overhang: at(35, 25),
      // Inside the negative-origin fill's visible remainder.
      negativeOrigin: at(2, 22),
      // The last fill, painted over the first two at the node's own origin.
      lastAtOrigin: at(1, 1),
    };
  }, Array.from(new Uint8Array(overhangingFrame)));

  expect(pixels).toEqual({
    size: [40, 30],
    overhang: [200, 40, 40, 255],
    negativeOrigin: [40, 200, 40, 255],
    lastAtOrigin: [40, 40, 200, 255],
  });
});

test("fits a fixed portable surface into a narrow browser viewport", async ({ page }) => {
  await page.setViewportSize({ width: 342, height: 500 });
  const fixedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 900, 560], children: ["button"] },
    { id: "button", kind: NodeKind.Button, bounds: [780, 500, 100, 40], label: "Edge" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const bounds = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const root = document.querySelector('[data-synth-node-id="root"]')!.getBoundingClientRect();
    const button = document.querySelector('[data-synth-node-id="button"]')!.getBoundingClientRect();
    const host = document.querySelector("#synth-root")!.getBoundingClientRect();
    return {
      viewportWidth: window.innerWidth,
      root: { left: root.left, right: root.right, width: root.width, height: root.height },
      button: { right: button.right, bottom: button.bottom },
      host: { width: host.width, height: host.height },
    };
  }, Array.from(new Uint8Array(fixedFrame)));

  expect(bounds.root.left).toBe(0);
  expect(bounds.root.right).toBeCloseTo(bounds.viewportWidth, 4);
  expect(bounds.root.width).toBeCloseTo(342, 4);
  expect(bounds.root.height).toBeCloseTo(560 * 342 / 900, 4);
  expect(bounds.button.right).toBeLessThanOrEqual(bounds.root.right);
  expect(bounds.button.bottom).toBeLessThanOrEqual(bounds.root.left + bounds.root.height);
  expect(bounds.host.height).toBeCloseTo(bounds.root.height, 2);
});

async function renderAndRead(page: Page, buffer: ArrayBuffer, ids: string[]) {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  return page.evaluate(async ({ bytes, ids }) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    return Object.fromEntries(ids.map((id) => {
      const element = document.querySelector<HTMLElement>(`[data-node-id="${id}"]`)!;
      const rect = element.getBoundingClientRect();
      return [id, {
        styleLeft: element.style.left,
        styleTop: element.style.top,
        styleWidth: element.style.width,
        styleHeight: element.style.height,
        surfaceX: rect.x,
        surfaceY: rect.y,
        renderedWidth: rect.width,
        renderedHeight: rect.height,
        overflow: getComputedStyle(element).overflowX,
      }];
    }));
  }, { bytes: Array.from(new Uint8Array(buffer)), ids });
}

type FixtureNode = Parameters<typeof makeCommandBuffer>[0][number];

// Keep this representative fixture in sync with BackendGeometryPropertyTree in
// PortableJuceBackendTests.cpp.
const backendGeometryPropertyNodes: FixtureNode[] = [
  { id: "root", kind: NodeKind.Root, bounds: [0, 0, 500, 260], children: ["section", "scroll"] },
  { id: "section", kind: NodeKind.Section, bounds: [20, 16, 220, 130], children: ["row", "status"] },
  { id: "row", kind: NodeKind.Row, bounds: [7, 11, 190, 70], children: ["label", "button", "toggle", "slider", "draw", "overhang"] },
  { id: "label", kind: NodeKind.Label, bounds: [4, 3, 48, 18], text: "Label" },
  { id: "button", kind: NodeKind.Button, bounds: [60, 4, 64, 24], label: "Go" },
  { id: "toggle", kind: NodeKind.Toggle, bounds: [130, 4, 54, 24], label: "On" },
  { id: "slider", kind: NodeKind.Slider, bounds: [60, 36, 96, 24], value: 0.5, minValue: 0, maxValue: 1, step: 0.01 },
  { id: "draw", kind: NodeKind.Draw, bounds: [160, 34, 24, 24], draws: [
    { kind: DrawKind.Fill, bounds: [0, 0, 24, 24], color: [1, 2, 3, 255] },
  ] },
  { id: "overhang", kind: NodeKind.Label, bounds: [180, 50, 40, 24], text: "Overhang" },
  { id: "status", kind: NodeKind.StatusText, bounds: [7, 90, 190, 22], text: "Status" },
  { id: "scroll", kind: NodeKind.ScrollArea, bounds: [260, 20, 120, 90], scrollContentWidth: 240, scrollContentHeight: 220, children: ["scroll-row", "scroll-draw", "zero"] },
  { id: "scroll-row", kind: NodeKind.Row, bounds: [8, 30, 200, 32], children: ["combo", "field"] },
  { id: "combo", kind: NodeKind.ComboBox, bounds: [4, 4, 75, 24], selectedOption: "one", options: [{ id: "one", label: "One" }, { id: "two", label: "Two" }] },
  { id: "field", kind: NodeKind.TextField, bounds: [86, 4, 88, 24], text: "value" },
  { id: "scroll-draw", kind: NodeKind.Draw, bounds: [20, 125, 50, 35], draws: [
    { kind: DrawKind.Fill, bounds: [0, 0, 50, 35], color: [4, 5, 6, 255] },
  ] },
  { id: "zero", kind: NodeKind.Label, bounds: [150, 10, 0, 0], text: "unresolved" },
];

// Keep this parity fixture in sync with BackendStyleParityTree in
// MiniAppJuceBackendParityTests.cpp. Boundless FillEllipse is deliberately
// excluded; see the Task 3.12 note in openspec/.../tasks.md.
const backendStyleParityNodes: FixtureNode[] = [
  { id: "root", kind: NodeKind.Root, bounds: [0, 0, 500, 280], color: [8, 9, 10, 255], children: ["section", "scroll", "square"] },
  { id: "section", kind: NodeKind.Section, bounds: [20, 16, 220, 160], color: [20, 30, 40, 255],
    borderColor: [200, 210, 220, 255], borderWidth: 4, cornerRadius: 8,
    children: ["row", "status", "disabled"] },
  { id: "row", kind: NodeKind.Row, bounds: [7, 11, 190, 70], children: ["label", "button", "toggle", "slider", "draw"] },
  { id: "label", kind: NodeKind.Label, bounds: [4, 3, 48, 18], text: "Label",
    color: [10, 10, 10, 255], textStyle: { size: 14, color: [240, 240, 240, 255], align: 0 } },
  { id: "button", kind: NodeKind.Button, bounds: [60, 4, 64, 24], label: "Go",
    color: [0, 120, 0, 255], textStyle: { size: 13, color: [244, 245, 246, 255], align: 1 } },
  { id: "toggle", kind: NodeKind.Toggle, bounds: [130, 4, 54, 24], label: "On", checked: true, color: [0, 120, 0, 255] },
  { id: "slider", kind: NodeKind.Slider, bounds: [60, 36, 96, 24], value: 0.5, minValue: 0, maxValue: 1, step: 0.01, color: [10, 80, 160, 255] },
  { id: "draw", kind: NodeKind.Draw, bounds: [160, 34, 24, 24], color: [250, 0, 0, 255], draws: [
    { kind: DrawKind.Fill, bounds: [0, 0, 24, 24], color: [1, 2, 3, 255] },
  ] },
  { id: "status", kind: NodeKind.StatusText, bounds: [7, 90, 190, 22], text: "Status",
    textStyle: { size: 15, color: [180, 200, 220, 255], align: 0 } },
  { id: "disabled", kind: NodeKind.Button, bounds: [7, 118, 96, 24], label: "Disabled", enabled: false, color: [40, 80, 120, 255] },
  { id: "scroll", kind: NodeKind.ScrollArea, bounds: [260, 20, 120, 90],
    color: [45, 55, 65, 255], borderColor: [180, 190, 200, 255], borderWidth: 4, cornerRadius: 10,
    scrollContentWidth: 240, scrollContentHeight: 220, children: ["scroll-row", "scroll-draw", "zero"] },
  { id: "scroll-row", kind: NodeKind.Row, bounds: [8, 30, 200, 32], children: ["combo", "field"] },
  { id: "combo", kind: NodeKind.ComboBox, bounds: [4, 4, 75, 24], selectedOption: "one",
    options: [{ id: "one", label: "One" }, { id: "two", label: "Two" }], color: [120, 20, 80, 255] },
  { id: "field", kind: NodeKind.TextField, bounds: [86, 4, 88, 24], text: "value",
    color: [30, 70, 90, 255], textStyle: { size: 13, color: [230, 235, 240, 255], align: 0 } },
  { id: "scroll-draw", kind: NodeKind.Draw, bounds: [20, 125, 50, 35], draws: [
    { kind: DrawKind.Fill, bounds: [0, 0, 50, 35], color: [4, 5, 6, 255] },
  ] },
  { id: "zero", kind: NodeKind.Label, bounds: [150, 10, 0, 0], text: "unresolved" },
  { id: "square", kind: NodeKind.Section, bounds: [400, 20, 40, 40], color: [33, 44, 55, 255] },
];

function foldAncestorOrigins(nodes: FixtureNode[], id: string, scrollOffsets: Record<string, { x: number; y: number }>) {
  const byId = new Map(nodes.map((node) => [node.id!, node]));
  const parentById = new Map<string, string>();
  for (const node of nodes)
    for (const child of node.children ?? []) parentById.set(child, node.id!);

  const start = byId.get(id)!;
  const bounds = start.bounds ?? [0, 0, 0, 0];
  let x = bounds[0];
  let y = bounds[1];
  for (let parentId = parentById.get(id); parentId; parentId = parentById.get(parentId)) {
    const parent = byId.get(parentId)!;
    const parentBounds = parent.bounds ?? [0, 0, 0, 0];
    x += parentBounds[0];
    y += parentBounds[1];
    if (parent.kind === NodeKind.ScrollArea) {
      const offset = scrollOffsets[parent.id!] ?? { x: 0, y: 0 };
      x -= offset.x;
      y -= offset.y;
    }
  }
  return { x, y };
}

function expectedGeometry(nodes: FixtureNode[]) {
  return Object.fromEntries(nodes.map((node) => {
    const folded = foldAncestorOrigins(nodes, node.id!, {});
    const bounds = node.bounds ?? [0, 0, 0, 0];
    return [node.id!, { x: folded.x, y: folded.y, width: bounds[2], height: bounds[3] }];
  }));
}

test("renders every representative node at the fold of its ancestor origins", async ({ page }) => {
  await page.setViewportSize({ width: 250, height: 420 });
  const propertyFrame = makeCommandBuffer(backendGeometryPropertyNodes);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const rendered = await page.evaluate(async ({ bytes, ids }) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const scroll = document.querySelector<HTMLElement>('[data-node-id="scroll"]')!;
    scroll.scrollLeft = 13;
    scroll.scrollTop = 19;
    const root = document.querySelector<HTMLElement>('[data-node-id="root"]')!;
    const rootRect = root.getBoundingClientRect();
    return {
      scale: rootRect.width / 500,
      nodes: Object.fromEntries(ids.map((id) => {
        const element = document.querySelector<HTMLElement>(`[data-node-id="${id}"]`)!;
        const rect = element.getBoundingClientRect();
        return [id, {
          x: rect.left - rootRect.left,
          y: rect.top - rootRect.top,
          width: rect.width,
          height: rect.height,
        }];
      })),
    };
  }, { bytes: Array.from(new Uint8Array(propertyFrame)), ids: backendGeometryPropertyNodes.map((node) => node.id!) });

  expect(rendered.scale).toBeCloseTo(0.5, 4);
  for (const node of backendGeometryPropertyNodes) {
    const expected = foldAncestorOrigins(backendGeometryPropertyNodes, node.id!, { scroll: { x: 13, y: 19 } });
    const bounds = node.bounds ?? [0, 0, 0, 0];
    expect(rendered.nodes[node.id!].x, `${node.id} surface x`).toBeCloseTo(expected.x * rendered.scale, 4);
    expect(rendered.nodes[node.id!].y, `${node.id} surface y`).toBeCloseTo(expected.y * rendered.scale, 4);
    expect(rendered.nodes[node.id!].width, `${node.id} rendered width`).toBeCloseTo(bounds[2] * rendered.scale, 4);
    expect(rendered.nodes[node.id!].height, `${node.id} rendered height`).toBeCloseTo(bounds[3] * rendered.scale, 4);
  }
});

test("matches JUCE backend geometry and carried style assignments", async ({ page }) => {
  await page.setViewportSize({ width: 620, height: 420 });
  const parityFrame = makeCommandBuffer(backendStyleParityNodes);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const rendered = await page.evaluate(async ({ bytes, ids }) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const root = document.querySelector<HTMLElement>('[data-node-id="root"]')!;
    const rootRect = root.getBoundingClientRect();
    const readRect = (id: string) => {
      const rect = document.querySelector<HTMLElement>(`[data-node-id="${id}"]`)!.getBoundingClientRect();
      return { x: rect.left - rootRect.left, y: rect.top - rootRect.top, width: rect.width, height: rect.height };
    };
    const style = (selector: string) => getComputedStyle(document.querySelector<HTMLElement>(selector)!);
    const element = (id: string) => document.querySelector<HTMLElement>(`[data-node-id="${id}"]`)!;
    return {
      scale: rootRect.width / 500,
      geometry: Object.fromEntries(ids.map((id) => [id, readRect(id)])),
      styles: {
        rootBackground: style('[data-node-id="root"]').backgroundColor,
        sectionBackground: style('[data-node-id="section"]').backgroundColor,
        sectionBorderColor: element("section").style.getPropertyValue("--synth-border-color"),
        sectionBorderWidth: element("section").style.getPropertyValue("--synth-border-width"),
        sectionBorderRadius: style('[data-node-id="section"]').borderRadius,
        sectionBorderShadow: style('[data-node-id="section"]').boxShadow,
        scrollBackground: style('[data-node-id="scroll"]').backgroundColor,
        scrollBorderColor: element("scroll").style.getPropertyValue("--synth-border-color"),
        scrollBorderWidth: element("scroll").style.getPropertyValue("--synth-border-width"),
        scrollBorderRadius: style('[data-node-id="scroll"]').borderRadius,
        scrollBorderShadow: style('[data-node-id="scroll"]').boxShadow,
        squareBackground: style('[data-node-id="square"]').backgroundColor,
        squareBorderRadius: style('[data-node-id="square"]').borderRadius,
        labelBackground: style('[data-node-id="label"]').backgroundColor,
        labelGlyph: style('[data-node-id="label"]').color,
        labelSize: style('[data-node-id="label"]').fontSize,
        buttonFill: style('[data-node-id="button"]').backgroundColor,
        buttonGlyph: style('[data-node-id="button"]').color,
        toggleAccent: style('[data-node-id="toggle"] input').accentColor,
        sliderAccent: style('[data-node-id="slider"] input').accentColor,
        comboBackground: style('[data-node-id="combo"] select').backgroundColor,
        fieldBackground: style('[data-node-id="field"] input').backgroundColor,
        fieldGlyph: style('[data-node-id="field"] input').color,
        disabledFill: style('[data-node-id="disabled"]').backgroundColor,
        disabledOpacity: style('[data-node-id="disabled"]').opacity,
        drawBackground: style('[data-node-id="draw"]').backgroundColor,
        drawCarriedFill: element("draw").style.getPropertyValue("--synth-fill"),
      },
    };
  }, { bytes: Array.from(new Uint8Array(parityFrame)), ids: backendStyleParityNodes.map((node) => node.id!) });

  expect(rendered.scale).toBe(1);
  expect(rendered.geometry).toEqual(expectedGeometry(backendStyleParityNodes));
  expect(rendered.styles).toEqual({
    rootBackground: "rgb(8, 9, 10)",
    sectionBackground: "rgb(20, 30, 40)",
    sectionBorderColor: "rgba(200, 210, 220, 1)",
    sectionBorderWidth: "4px",
    sectionBorderRadius: "8px",
    sectionBorderShadow: "rgb(200, 210, 220) 0px 0px 0px 4px inset",
    scrollBackground: "rgb(45, 55, 65)",
    scrollBorderColor: "rgba(180, 190, 200, 1)",
    scrollBorderWidth: "4px",
    scrollBorderRadius: "10px",
    scrollBorderShadow: "rgb(180, 190, 200) 0px 0px 0px 4px inset",
    squareBackground: "rgb(33, 44, 55)",
    squareBorderRadius: "0px",
    labelBackground: "rgb(10, 10, 10)",
    labelGlyph: "rgb(240, 240, 240)",
    labelSize: "14px",
    buttonFill: "rgb(0, 120, 0)",
    buttonGlyph: "rgb(244, 245, 246)",
    toggleAccent: "rgb(31, 136, 31)",
    sliderAccent: "rgb(10, 80, 160)",
    comboBackground: "rgb(120, 20, 80)",
    fieldBackground: "rgb(30, 70, 90)",
    fieldGlyph: "rgb(230, 235, 240)",
    disabledFill: "rgb(29, 59, 88)",
    disabledOpacity: "0.58",
    drawBackground: "rgba(0, 0, 0, 0)",
    drawCarriedFill: "initial",
  });
});

test("offsets a child by its wire bounds with no parent subtraction", async ({ page }) => {
  const nestedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 400, 300], children: ["parent"] },
    { id: "parent", kind: NodeKind.Section, bounds: [50, 40, 200, 100], children: ["child"] },
    { id: "child", kind: NodeKind.Label, bounds: [10, 10, 80, 20], text: "Child" },
  ]);
  const layout = await renderAndRead(page, nestedFrame, ["parent", "child"]);

  expect(layout.child.styleLeft).toBe("10px");
  expect(layout.child.styleTop).toBe("10px");
  // And the surface position is the plain fold of the ancestor origins.
  expect(layout.child.surfaceX).toBe(60);
  expect(layout.child.surfaceY).toBe(50);
});

test("keeps an overhanging child parent-relative", async ({ page }) => {
  // `explicitBoundsAreParentLocal` reclassified a child that did not fit its
  // parent as surface-absolute. There is no classifier left to do so.
  const overhangingFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 400, 300], children: ["parent"] },
    { id: "parent", kind: NodeKind.Section, bounds: [50, 40, 100, 50], children: ["child"] },
    { id: "child", kind: NodeKind.Label, bounds: [10, 10, 200, 20], text: "Overhang" },
  ]);
  const layout = await renderAndRead(page, overhangingFrame, ["child"]);

  expect(layout.child.styleLeft).toBe("10px");
  expect(layout.child.styleTop).toBe("10px");
  expect(layout.child.surfaceX).toBe(60);
  expect(layout.child.surfaceY).toBe(50);
});

test("does not flow a node without resolved bounds", async ({ page }) => {
  const unresolvedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 400, 300], children: ["parent"] },
    { id: "parent", kind: NodeKind.Section, bounds: [50, 40, 200, 100], children: ["orphan", "field"] },
    { id: "orphan", kind: NodeKind.Label, bounds: [0, 0, 0, 0], text: "Unresolved" },
    // A `<select>`-bearing kind whose own border and padding, and whose unsized
    // child control, would each render pixels the tree does not describe.
    { id: "field", kind: NodeKind.ComboBox, bounds: [0, 0, 0, 0], selectedOption: "one",
      options: [{ id: "one", label: "A long option label" }] },
  ]);
  const layout = await renderAndRead(page, unresolvedFrame, ["orphan", "field"]);

  expect(layout.orphan.styleLeft).toBe("0px");
  expect(layout.orphan.styleTop).toBe("0px");
  expect(layout.orphan.styleWidth).toBe("0px");
  expect(layout.orphan.styleHeight).toBe("0px");
  // It renders at its parent's origin with zero extent, never flowed or sized.
  expect(layout.orphan.surfaceX).toBe(50);
  expect(layout.orphan.surfaceY).toBe(40);
  // Zero-based extent is the *rendered* extent, not only the inline style: the
  // border-box floor of a control's own border and padding is exactly the kind
  // of backend-supplied size sprs-6 rules out.
  for (const zeroExtent of [layout.orphan, layout.field]) {
    expect(zeroExtent.renderedWidth).toBe(0);
    expect(zeroExtent.renderedHeight).toBe(0);
    // And the toolkit's own unsized inner control is clipped to that extent
    // rather than spilling out of a box the tree gave no room to.
    expect(zeroExtent.overflow).toBe("hidden");
  }
});

test("emits both the prefixed and the unprefixed node id and kind attributes", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const attributes = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const elements = [...document.querySelectorAll<HTMLElement>("[data-synth-node-id]")];
    return {
      count: elements.length,
      consistent: elements.every((element) =>
        element.dataset.nodeId === element.dataset.synthNodeId &&
        element.dataset.nodeKind === element.dataset.synthNodeKind),
      scrollAreaKind: document.querySelector<HTMLElement>('[data-node-id="scroll"]')!.dataset.nodeKind,
    };
  }, Array.from(new Uint8Array(frame)));

  expect(attributes).toEqual({ count: 10, consistent: true, scrollAreaKind: "scroll-area" });
});

test("renders one carried colour on the surface each node kind assigns it", async ({ page }) => {
  const carried: [number, number, number, number] = [0, 200, 0, 255];
  const styledFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 400, 300], color: [8, 9, 10, 255],
      children: ["section", "button", "toggle", "slider", "combo", "field", "label", "draw"] },
    { id: "section", kind: NodeKind.Section, bounds: [0, 0, 120, 24], color: carried },
    { id: "button", kind: NodeKind.Button, bounds: [0, 30, 120, 24], label: "Go", color: carried },
    { id: "toggle", kind: NodeKind.Toggle, bounds: [0, 60, 120, 24], label: "On", color: carried },
    { id: "slider", kind: NodeKind.Slider, bounds: [0, 90, 120, 24], color: carried },
    { id: "combo", kind: NodeKind.ComboBox, bounds: [0, 120, 120, 24], selectedOption: "one",
      options: [{ id: "one", label: "One" }], color: carried },
    { id: "field", kind: NodeKind.TextField, bounds: [0, 150, 120, 24], text: "x", color: carried },
    { id: "label", kind: NodeKind.Label, bounds: [0, 180, 120, 24], text: "Label",
      color: [10, 10, 10, 255], textStyle: { size: 14, color: [240, 240, 240, 255], align: 0 } },
    { id: "draw", kind: NodeKind.Draw, bounds: [0, 210, 120, 24], color: carried,
      draws: [{ kind: DrawKind.Fill, color: [1, 2, 3, 255] }] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const painted = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const styleOf = (selector: string) => getComputedStyle(document.querySelector<HTMLElement>(selector)!);
    return {
      rootBackground: styleOf('[data-node-id="root"]').backgroundColor,
      sectionBackground: styleOf('[data-node-id="section"]').backgroundColor,
      buttonFill: styleOf('[data-node-id="button"]').backgroundColor,
      toggleAccent: styleOf('[data-node-id="toggle"] input').accentColor,
      sliderAccent: styleOf('[data-node-id="slider"] input').accentColor,
      comboBackground: styleOf('[data-node-id="combo"] select').backgroundColor,
      fieldBackground: styleOf('[data-node-id="field"] input').backgroundColor,
      labelBackground: styleOf('[data-node-id="label"]').backgroundColor,
      labelGlyph: styleOf('[data-node-id="label"]').color,
      labelSize: styleOf('[data-node-id="label"]').fontSize,
      drawBackground: styleOf('[data-node-id="draw"]').backgroundColor,
    };
  }, Array.from(new Uint8Array(styledFrame)));

  expect(painted).toEqual({
    rootBackground: "rgb(8, 9, 10)",
    sectionBackground: "rgb(0, 200, 0)",
    buttonFill: "rgb(0, 200, 0)",
    toggleAccent: "rgb(0, 200, 0)",
    sliderAccent: "rgb(0, 200, 0)",
    comboBackground: "rgb(0, 200, 0)",
    fieldBackground: "rgb(0, 200, 0)",
    // The label's node colour backs it; its glyphs come from textStyle.
    labelBackground: "rgb(10, 10, 10)",
    labelGlyph: "rgb(240, 240, 240)",
    labelSize: "14px",
    // A `Draw` node's colour paints nothing: its commands carry their own.
    drawBackground: "rgba(0, 0, 0, 0)",
  });
});

test("renders a container fill and rounded border across padding and gaps", async ({ page }) => {
  const frame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 220, 160], children: ["panel"] },
    { id: "panel", kind: NodeKind.Section, bounds: [20, 20, 160, 100],
      color: [20, 30, 40, 255], borderColor: [200, 210, 220, 255], borderWidth: 4, cornerRadius: 8,
      children: ["first", "second"] },
    { id: "first", kind: NodeKind.Label, bounds: [16, 16, 54, 20], text: "One", color: [70, 80, 90, 255] },
    { id: "second", kind: NodeKind.Label, bounds: [16, 52, 54, 20], text: "Two", color: [90, 100, 110, 255] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const rendered = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const panel = document.querySelector<HTMLElement>('[data-node-id="panel"]')!;
    const first = document.querySelector<HTMLElement>('[data-node-id="first"]')!;
    const second = document.querySelector<HTMLElement>('[data-node-id="second"]')!;
    const panelStyle = getComputedStyle(panel);
    const rectOf = (element: HTMLElement) => {
      const rect = element.getBoundingClientRect();
      const root = document.querySelector<HTMLElement>('[data-node-id="root"]')!.getBoundingClientRect();
      return { x: rect.left - root.left, y: rect.top - root.top, width: rect.width, height: rect.height };
    };
    return {
      panel: {
        background: panelStyle.backgroundColor,
        borderColor: panel.style.getPropertyValue("--synth-border-color"),
        borderWidth: panel.style.getPropertyValue("--synth-border-width"),
        borderRadius: panelStyle.borderRadius,
        borderShadow: panelStyle.boxShadow,
      },
      panelRect: rectOf(panel),
      firstRect: rectOf(first),
      secondRect: rectOf(second),
    };
  }, Array.from(new Uint8Array(frame)));

  expect(rendered.panel).toEqual({
    background: "rgb(20, 30, 40)",
    borderColor: "rgba(200, 210, 220, 1)",
    borderWidth: "4px",
    borderRadius: "8px",
    borderShadow: "rgb(200, 210, 220) 0px 0px 0px 4px inset",
  });
  const paddingPoint = { x: rendered.panelRect.x + 8, y: rendered.panelRect.y + 8 };
  const gapPoint = { x: rendered.panelRect.x + 32, y: rendered.panelRect.y + 44 };
  for (const [name, point] of Object.entries({ paddingPoint, gapPoint })) {
    expect(point.x, `${name} is inside panel x`).toBeGreaterThan(rendered.panelRect.x);
    expect(point.y, `${name} is inside panel y`).toBeGreaterThan(rendered.panelRect.y);
    expect(point.x, `${name} is inside panel right`).toBeLessThan(rendered.panelRect.x + rendered.panelRect.width);
    expect(point.y, `${name} is inside panel bottom`).toBeLessThan(rendered.panelRect.y + rendered.panelRect.height);
    for (const child of [rendered.firstRect, rendered.secondRect]) {
      expect(point.x < child.x || point.x > child.x + child.width ||
             point.y < child.y || point.y > child.y + child.height,
             `${name} is not painted by either child`).toBe(true);
    }
  }
});

test("derives selected and disabled presentation from the carried colour", async ({ page }) => {
  const stateFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 200], children: ["plain", "selected", "disabled", "unstyled", "unstyled-disabled"] },
    { id: "plain", kind: NodeKind.Button, bounds: [0, 0, 80, 24], label: "Plain", color: [0, 120, 0, 255] },
    { id: "selected", kind: NodeKind.Button, bounds: [0, 30, 80, 24], label: "Selected", selected: true, color: [0, 120, 0, 255] },
    { id: "disabled", kind: NodeKind.Button, bounds: [0, 60, 80, 24], label: "Disabled", enabled: false, color: [0, 120, 0, 255] },
    { id: "unstyled", kind: NodeKind.Button, bounds: [0, 90, 80, 24], label: "Unstyled" },
    { id: "unstyled-disabled", kind: NodeKind.Button, bounds: [0, 120, 80, 24], label: "Unstyled off", enabled: false },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const fills = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const elementOf = (id: string) => document.querySelector<HTMLElement>(`[data-node-id="${id}"]`)!;
    const fillOf = (id: string) => getComputedStyle(elementOf(id)).backgroundColor;
    return {
      plain: fillOf("plain"),
      selected: fillOf("selected"),
      disabled: fillOf("disabled"),
      unstyled: fillOf("unstyled"),
      // Chromium serializes a computed alpha to two decimals, which cannot
      // distinguish adjacent alpha bytes. Read the derived value exactly.
      disabledCarried: elementOf("disabled").style.getPropertyValue("--synth-fill"),
      disabledOpacity: getComputedStyle(elementOf("disabled")).opacity,
      unstyledDisabledFill: fillOf("unstyled-disabled"),
      unstyledDisabledOpacity: getComputedStyle(elementOf("unstyled-disabled")).opacity,
    };
  }, Array.from(new Uint8Array(stateFrame)));

  expect(fills.plain).toBe("rgb(0, 120, 0)");
  // `juce::Colour(0, 120, 0).brighter(0.14f)`, so both backends derive the same
  // selected fill from the same carried colour rather than substituting one.
  expect(fills.selected).toBe("rgb(31, 136, 31)");
  // `juce::Colour(0, 120, 0).darker(0.35f)` and nothing else — the whole of
  // `StateColourFor`'s disabled fold. Channel 120 becomes 88 and the carried
  // alpha is untouched, so the two backends produce the same four bytes.
  expect(fills.disabled).toBe("rgb(0, 88, 0)");
  expect(fills.disabledCarried).toBe("rgba(0, 88, 0, 1)");
  // The element dim on top of it is `component.setAlpha(0.58f)`'s counterpart,
  // which the JUCE backend applies to every disabled node whether it carries a
  // colour or not. So it is unconditional here too: one colour fold, one dim.
  expect(fills.disabledOpacity).toBe("0.58");
  // Carrying no colour keeps the backend's own flat default in full, dimmed by
  // the same one dim.
  expect(fills.unstyled).toBe("rgb(37, 42, 47)");
  expect(fills.unstyledDisabledFill).toBe("rgb(37, 42, 47)");
  expect(fills.unstyledDisabledOpacity).toBe("0.58");
});

// The two remaining sru-45 derived states. Both are folds of the carried colour
// rather than palette substitutions. Neither is a cross-backend parity claim:
// JUCE has no hover state, and its pressed fill is `buttonOnColourId`.
test("derives hover and pressed presentation from the carried colour", async ({ page }) => {
  const stateFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 100], children: ["carried", "unstyled"] },
    { id: "carried", kind: NodeKind.Button, bounds: [0, 0, 80, 24], label: "Carried", color: [0, 120, 0, 255] },
    { id: "unstyled", kind: NodeKind.Button, bounds: [0, 40, 80, 24], label: "Unstyled" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(stateFrame)));
  const fillOf = (id: string) => page.locator(`[data-node-id="${id}"]`)
    .evaluate((element) => getComputedStyle(element).backgroundColor);

  const carried = page.locator('[data-node-id="carried"]');
  await carried.hover();
  // `brighter(0.14f)` of the carried fill, the same fold selected uses.
  expect(await fillOf("carried")).toBe("rgb(31, 136, 31)");
  const box = (await carried.boundingBox())!;
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  // Pressed brightens the *carried* colour by 0.24 rather than compounding with
  // the hover fold, so holding a selected control does not stack two folds.
  expect(await fillOf("carried")).toBe("rgb(49, 146, 49)");
  await page.mouse.up();

  // A node carrying nothing keeps the stylesheet's own hover look.
  await page.locator('[data-node-id="unstyled"]').hover();
  expect(await fillOf("unstyled")).toBe("rgb(48, 56, 62)");
});

test("reads a checked toggle as selected when deriving its carried accent", async ({ page }) => {
  const toggleFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 100], children: ["off", "on"] },
    { id: "off", kind: NodeKind.Toggle, bounds: [0, 0, 120, 24], label: "Off", checked: false, color: [0, 120, 0, 255] },
    { id: "on", kind: NodeKind.Toggle, bounds: [0, 30, 120, 24], label: "On", checked: true, color: [0, 120, 0, 255] },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const accents = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const accentOf = (id: string) => getComputedStyle(document.querySelector<HTMLElement>(`[data-node-id="${id}"] input`)!).accentColor;
    return { off: accentOf("off"), on: accentOf("on") };
  }, Array.from(new Uint8Array(toggleFrame)));

  expect(accents.off).toBe("rgb(0, 120, 0)");
  // The same `brighter(0.14f)` fold the JUCE backend applies to a checked toggle.
  expect(accents.on).toBe("rgb(31, 136, 31)");
});

test("does not leak a container's carried fill into an unstyled descendant", async ({ page }) => {
  const nestedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 100], children: ["section"] },
    { id: "section", kind: NodeKind.Section, bounds: [0, 0, 200, 60], color: [0, 200, 0, 255], children: ["label", "button"] },
    { id: "label", kind: NodeKind.Label, bounds: [0, 0, 100, 20], text: "Inside" },
    { id: "button", kind: NodeKind.Button, bounds: [0, 24, 80, 24], label: "Inside" },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  const backgrounds = await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
    const backgroundOf = (id: string) => getComputedStyle(document.querySelector<HTMLElement>(`[data-node-id="${id}"]`)!).backgroundColor;
    return { section: backgroundOf("section"), label: backgroundOf("label"), button: backgroundOf("button") };
  }, Array.from(new Uint8Array(nestedFrame)));

  expect(backgrounds).toEqual({
    section: "rgb(0, 200, 0)",
    label: "rgba(0, 0, 0, 0)",
    button: "rgb(37, 42, 47)",
  });
});

test("preserves semantic nodes and reports structural buffer errors", () => {
  const decoded = decodeCommandBuffer(frame);
  expect(decoded.nodes).toHaveLength(10);
  expect(() => decodeCommandBuffer(new ArrayBuffer(4))).toThrow(CommandBufferError);
});

test("carries node colour and text style behind explicit presence flags", () => {
  const styledFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 100], children: ["styled", "plain", "transparent"] },
    {
      id: "styled", kind: NodeKind.Button, bounds: [10, 20, 80, 24],
      color: [0, 200, 0, 255], textStyle: { size: 16, color: [255, 255, 255, 255], align: 1 },
    },
    { id: "plain", kind: NodeKind.Button, bounds: [10, 50, 80, 24] },
    { id: "transparent", kind: NodeKind.Section, bounds: [10, 80, 80, 24], color: [0, 0, 0, 0] },
  ]);
  const byId = new Map(decodeCommandBuffer(styledFrame).nodes.map((node) => [node.id, node]));

  expect(byId.get("styled")!.bounds).toEqual({ x: 10, y: 20, width: 80, height: 24 });
  expect(byId.get("styled")!.color).toEqual({ r: 0, g: 200, b: 0, a: 255 });
  expect(byId.get("styled")!.textStyle).toEqual({ size: 16, color: { r: 255, g: 255, b: 255, a: 255 }, align: 1 });
  expect(byId.get("plain")!.color).toBeUndefined();
  expect(byId.get("plain")!.textStyle).toBeUndefined();
  // A sentinel colour would make this indistinguishable from "absent".
  expect(byId.get("transparent")!.color).toEqual({ r: 0, g: 0, b: 0, a: 0 });
});

test("rejects a command buffer whose version is not the shell's", () => {
  const staleFrame = makeCommandBuffer([{ id: "root", kind: NodeKind.Root, bounds: [0, 0, 10, 10] }], [], 1);
  expect(() => decodeCommandBuffer(staleFrame)).toThrow(/unsupported command buffer version/);
});

// Mirror of `TestCorruptPresenceFlagIsRejected` in
// `tests/browser_command_buffer_tests.cpp`. A presence byte the encoder can
// never produce means the reader has lost its place in the node section, so
// reading the next four bytes as a colour would turn a desynchronised read
// into a plausible-looking frame. Both decoders must reject it.
test("rejects a corrupt presence flag instead of inventing a colour", () => {
  const encodeRoot = (color?: [number, number, number, number]) => new Uint8Array(makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 400, 300], color },
  ]));
  const unstyled = encodeRoot();
  // Buffer layout: 4 magic + 2 version + 2 reserved + 5 section lengths, then
  // the string section, then the node section's u32 node count, then the first
  // node record. Within a record the colour presence byte follows the id
  // (u32), four u8 flags, the bounds (4 floats), three string indices (u32),
  // and six floats. Pinning it here means a field-order change breaks this
  // test rather than silently passing.
  const stringSectionLength = new DataView(unstyled.buffer, unstyled.byteOffset).getUint32(8, true);
  const colourPresence = 28 + stringSectionLength + 4 + 60;

  // The same offset reads 0 unstyled and 1 styled, which is what proves the
  // arithmetic lands on the presence byte rather than on some other zero.
  expect(unstyled[colourPresence]).toBe(0);
  expect(encodeRoot([1, 2, 3, 255])[colourPresence]).toBe(1);

  const corrupt = encodeRoot();
  corrupt[colourPresence] = 2;
  expect(() => decodeCommandBuffer(corrupt.buffer)).toThrow(/invalid presence flag/);
});

// ---------------------------------------------------------------------------
// sru-59: Slider value readout, every backend. Mirrors the JUCE backend's
// TextBoxBelow readout (PortableJuceBackend.hpp:1160-1178) with a browser
// `<output>` sibling of the `<input type="range">` (ui.ts's Slider branches
// in createElement/updateControl). Covers design.md's seven listed
// scenarios, PLUS the pinned-format byte-parity table and the step===0
// (preflight defect-2) case that tasks.md calls out separately.
// ---------------------------------------------------------------------------

// Reused across the initial-value/update/drag-seam tests below (reuse over
// re-creation): a fractional step so the two-decimal formatting is visible,
// matching the `value: 0.5, minValue: 0, maxValue: 1, step: 0.01` slider
// shape already established elsewhere in this file (e.g. line 902, 1512).
const fractionalSliderFrame = makeCommandBuffer([
  { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 40], children: ["slider"] },
  { id: "slider", kind: NodeKind.Slider, bounds: [0, 0, 160, 20], value: 0.5, minValue: 0, maxValue: 1, step: 0.01 },
]);

test("shows a readout element for a Slider node", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(frame)));
  await expect(page.locator('[data-synth-node-id="slider"] output')).toBeAttached();
});

test("shows the formatted initial value", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(fractionalSliderFrame)));
  await expect(page.locator('[data-synth-node-id="slider"] output')).toHaveText("0.50");
});

test("follows a wire value update", async ({ page }) => {
  const updatedFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 40], children: ["slider"] },
    { id: "slider", kind: NodeKind.Slider, bounds: [0, 0, 160, 20], value: 0.75, minValue: 0, maxValue: 1, step: 0.01 },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    const browserWindow = window as unknown as { backend: InstanceType<typeof BrowserUiBackend> };
    browserWindow.backend = new BrowserUiBackend(document.querySelector("#synth-root")!);
    browserWindow.backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(fractionalSliderFrame)));
  await expect(page.locator('[data-synth-node-id="slider"] output')).toHaveText("0.50");
  await page.evaluate(async (bytes) => {
    (window as unknown as { backend: { renderFrame(buffer: ArrayBuffer): void } }).backend.renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(updatedFrame)));
  await expect(page.locator('[data-synth-node-id="slider"] output')).toHaveText("0.75");
});

test("follows a simulated drag input event between frames", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(fractionalSliderFrame)));
  await expect(page.locator('[data-synth-node-id="slider"] output')).toHaveText("0.50");
  // No renderFrame call between the drag and this assertion: the readout can
  // only have updated through the `input` event seam (ui.ts's Slider create
  // branch), not through `updateControl`, which only runs on the next frame
  // -- exactly the seam design.md's Risks section requires (a drag must not
  // freeze the readout between frames).
  // A direct value assignment + dispatched `input` event, not `.fill()`:
  // `step` crosses the wire as a float32 (protocol.ts's `Reader.float()`),
  // so the DOM input's own `step` attribute is `0.01`'s nearest float32
  // ("0.009999999776482582"), which Playwright's `fill()` validates 0.75
  // against and rejects as a "Malformed value" -- a wire-precision artifact
  // of the fixture, unrelated to the seam this test exists to prove. Direct
  // assignment mirrors "suppresses actions from disabled native controls"
  // above, which drives the same input the same way.
  await page.locator('[data-synth-node-id="slider"] input').evaluate((input: HTMLInputElement) => {
    input.value = "0.75";
    input.dispatchEvent(new Event("input", { bubbles: true }));
  });
  await expect(page.locator('[data-synth-node-id="slider"] output')).toHaveText("0.75");
});

test("formats an integer step without decimals and a fractional step with them", async ({ page }) => {
  const mixedStepFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 80], children: ["whole", "fractional"] },
    { id: "whole", kind: NodeKind.Slider, bounds: [0, 0, 160, 20], value: 120, minValue: 20, maxValue: 300, step: 1 },
    { id: "fractional", kind: NodeKind.Slider, bounds: [0, 24, 160, 20], value: 0.5, minValue: 0, maxValue: 1, step: 0.01 },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(mixedStepFrame)));
  await expect(page.locator('[data-synth-node-id="whole"] output')).toHaveText("120");
  await expect(page.locator('[data-synth-node-id="fractional"] output')).toHaveText("0.50");
});

test("keeps showing its value while disabled", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(disabledFrame)));
  await expect(page.locator('[data-synth-node-id="slider"] input')).toBeDisabled();
  await expect(page.locator('[data-synth-node-id="slider"] output')).toHaveText("3");
});

test("does not intercept pointer events meant for the input", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(frame)));
  const output = page.locator('[data-synth-node-id="slider"] output');
  expect(await output.evaluate((element) => getComputedStyle(element).pointerEvents)).toBe("none");
  const outputBox = (await output.boundingBox())!;
  expect(outputBox.width, "readout hit-test width").toBeGreaterThan(0);

  // The readout owns a strip at the BOTTOM of the node box and the track is
  // shortened above it (sru-59 layout, mirroring JUCE's TextBoxBelow) -- so
  // the readout no longer sits over the track at all. Two things still have
  // to hold, and this test asserts both:
  //   1. a press anywhere on the TRACK resolves to the input, and
  //   2. `pointer-events: none` genuinely lets a press inside the readout's
  //      own box fall THROUGH it -- it must never be the hit target itself,
  //      which is what would silently swallow drags if the strip were ever
  //      overlapped or grown.
  // (Before the layout fix this test hit-tested the readout's centre and
  // expected INPUT, because the readout was overlaid ON the track -- an
  // assertion about a layout that produced unreadable digits across the
  // filled track and under the thumb.)
  const input = page.locator('[data-synth-node-id="slider"] input');
  const inputBox = (await input.boundingBox())!;
  expect(inputBox.height, "track keeps usable height above the readout").toBeGreaterThan(0);
  expect(
    Math.round(outputBox.y),
    "readout starts at or below the track's bottom edge (no overlap)",
  ).toBeGreaterThanOrEqual(Math.round(inputBox.y + inputBox.height) - 1);

  const trackPoint = { x: inputBox.x + inputBox.width / 2, y: inputBox.y + inputBox.height / 2 };
  expect(
    await page.evaluate((p) => document.elementFromPoint(p.x, p.y)?.tagName, trackPoint),
    "a press on the track resolves to the input",
  ).toBe("INPUT");

  const readoutPoint = { x: outputBox.x + outputBox.width / 2, y: outputBox.y + outputBox.height / 2 };
  expect(
    await page.evaluate((p) => document.elementFromPoint(p.x, p.y)?.tagName, readoutPoint),
    "the readout is never itself the hit target",
  ).not.toBe("OUTPUT");
});

// Byte-parity table (design.md, tasks.md 1.2): each expected string is the
// traced JUCE mechanism's OWN output for the identical binary double, not a
// hand-picked expectation. Verified by compiling a literal port of
// `juce_Slider.cpp:145-162` (updateRange's decimal-count loop) and
// `juce_String.cpp:472-503` (StackArrayStream::writeDouble, behind
// getTextFromValue's `String(val, N)` at juce_Slider.cpp:1655) standalone
// and running it against these exact rows.
//
// Row "0.5/step 1" is the one row where design.md's first-cut N=0 formula --
// `String(Math.round(value))` -- diverges from JUCE: it collapses 0.5 to
// "1" (and -0.5 to "0", losing the sign and the value entirely), because
// JUCE's own mechanism only switches to fixed-point formatting for N > 0
// (`if (numDecPlaces > 0) { fixed; precision(N); }` in writeDouble); for
// N === 0 the stream keeps iostream's own default (non-fixed) formatting
// instead of rounding to an integer, so an off-step value still shows its
// fraction. `formatSliderValue` in ui.ts uses `String(value)` for N === 0
// instead of `Math.round`, which is the JUCE-authority adjustment
// design.md's own contingency clause calls for -- this table is the record
// of that divergence and the fix it forced.
const byteParityRows: Array<[label: string, value: number, minValue: number, maxValue: number, step: number, expected: string]> = [
  ["toFixed rounding boundary 2.675/step 0.01", 2.675, 0, 10, 0.01, "2.67"],
  ["half-integer tie 0.5/step 1 (JUCE-authority adjustment)", 0.5, 0, 10, 1, "0.5"],
  ["negative half-integer tie -0.5/step 1", -0.5, -10, 10, 1, "-0.5"],
  ["negative value -4.2/step 0.1", -4.2, -10, 10, 0.1, "-4.2"],
  ["range min endpoint 0/step 0.01", 0, 0, 1, 0.01, "0.00"],
  ["range max endpoint 1/step 0.01", 1, 0, 1, 0.01, "1.00"],
  ["negative range min endpoint -1/step 0.01", -1, -1, 1, 0.01, "-1.00"],
  ["whole-number value 3/step 1 (no divergence)", 3, 0, 10, 1, "3"],
];

test("formats byte-parity rounding boundaries against the traced JUCE mechanism", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async () => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    (window as unknown as { backend: InstanceType<typeof BrowserUiBackend> }).backend =
      new BrowserUiBackend(document.querySelector("#synth-root")!);
  });
  for (const [label, value, minValue, maxValue, step, expected] of byteParityRows) {
    const rowFrame = makeCommandBuffer([
      { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 40], children: ["slider"] },
      { id: "slider", kind: NodeKind.Slider, bounds: [0, 0, 160, 20], value, minValue, maxValue, step },
    ]);
    await page.evaluate(async (bytes) => {
      (window as unknown as { backend: { renderFrame(buffer: ArrayBuffer): void } }).backend.renderFrame(new Uint8Array(bytes).buffer);
    }, Array.from(new Uint8Array(rowFrame)));
    await expect(page.locator('[data-synth-node-id="slider"] output'), label).toHaveText(expected);
  }
});

// preflight defect-2 fix: the general trailing-zero loop, run literally on a
// scaled value of 0 (from step 0), would collapse to N=0. JUCE's own
// updateRange() instead guards the loop with
// `if (! approximatelyEqual (interval, 0.0))` and leaves numDecimalPlaces at
// its untouched default of 7 when the guard is false. No current producer
// sends step 0 (braid-4 and the miniapp both send 0.001f) but the algorithm
// must be total, per design.md.
test("formats a continuous (step 0) slider at JUCE's default 7 decimal places", async ({ page }) => {
  const continuousFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 40], children: ["slider"] },
    { id: "slider", kind: NodeKind.Slider, bounds: [0, 0, 160, 20], value: 0.5, minValue: 0, maxValue: 1, step: 0 },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(continuousFrame)));
  await expect(page.locator('[data-synth-node-id="slider"] output')).toHaveText("0.5000000");
});

// Postflight (2026-08-19): the readout strip's first cut was a hardcoded 14px
// carved out of the node's own box, which on a node at-or-below that height
// spilled past the wire-set bounds (breaking sru-59's "renders within the
// node's bounds") and drove `calc(100% - 14px)` to a 0-height, silently
// undraggable track. No fixture exercised it -- every slider here was 20px or
// taller. These two pin both sides of the adaptive floor.
test("keeps the readout inside the bounds and the track usable on a short slider node", async ({ page }) => {
  const shortFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 40], children: ["slider"] },
    { id: "slider", kind: NodeKind.Slider, bounds: [0, 0, 160, 12], value: 5, minValue: 0, maxValue: 10, step: 1 },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(shortFrame)));
  const node = page.locator('[data-synth-node-id="slider"]');
  const input = page.locator('[data-synth-node-id="slider"] input');
  const output = page.locator('[data-synth-node-id="slider"] output');
  // 12px * 0.4 = 4px, under the 8px legibility floor -> no strip at all.
  expect(await output.evaluate((element) => getComputedStyle(element).display)).toBe("none");
  const nodeBox = (await node.boundingBox())!;
  const inputBox = (await input.boundingBox())!;
  expect(inputBox.height, "track keeps the whole box when the strip is dropped").toBeGreaterThan(0);
  expect(Math.round(inputBox.height), "track fills the node").toBe(Math.round(nodeBox.height));
  const centre = { x: inputBox.x + inputBox.width / 2, y: inputBox.y + inputBox.height / 2 };
  expect(
    await page.evaluate((p) => document.elementFromPoint(p.x, p.y)?.tagName, centre),
    "a short slider is still draggable",
  ).toBe("INPUT");
});

test("shrinks the readout strip rather than overflowing a mid-height slider node", async ({ page }) => {
  const midFrame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 200, 60], children: ["slider"] },
    { id: "slider", kind: NodeKind.Slider, bounds: [0, 0, 160, 24], value: 5, minValue: 0, maxValue: 10, step: 1 },
  ]);
  await page.goto("http://127.0.0.1:4173/public/index.html");
  await page.evaluate(async (bytes) => {
    const { BrowserUiBackend } = await import("../dist/src/" + "ui.js");
    new BrowserUiBackend(document.querySelector("#synth-root")!).renderFrame(new Uint8Array(bytes).buffer);
  }, Array.from(new Uint8Array(midFrame)));
  const node = page.locator('[data-synth-node-id="slider"]');
  const input = page.locator('[data-synth-node-id="slider"] input');
  const output = page.locator('[data-synth-node-id="slider"] output');
  const nodeBox = (await node.boundingBox())!;
  const inputBox = (await input.boundingBox())!;
  const outputBox = (await output.boundingBox())!;
  // 24px * 0.4 = 9px strip (under the 14px cap, over the 8px floor).
  expect(Math.round(outputBox.height), "strip scales with the node").toBe(9);
  expect(await output.evaluate((element) => getComputedStyle(element).display)).not.toBe("none");
  expect(
    Math.round(outputBox.y + outputBox.height),
    "readout stays inside the node's own bounds",
  ).toBeLessThanOrEqual(Math.round(nodeBox.y + nodeBox.height) + 1);
  expect(inputBox.height, "track keeps the majority of the box").toBeGreaterThan(nodeBox.height / 2);
});
