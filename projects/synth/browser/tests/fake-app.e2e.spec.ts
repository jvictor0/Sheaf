import { expect, test, type Page } from "@playwright/test";
import { installTwisterPair, removeTwisterPair, TWISTER_DEVICE_NAME } from "./helpers/fake-midi.js";
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

const TWISTER_DISPLAY_NAME = "MIDI Fighter Twister";
const TWISTER_WIZARD_DEFAULTS = [
  "Hold Reset",
  "Hold Random",
  "Hold Random Mod",
  "Next Bank",
  "Start",
  "Previous Bank",
] as const;
const TWISTER_WIZARD_CHOICES = [
  "Toggle Reset",
  "Hold Reset",
  "Toggle Random",
  "Hold Random",
  "Toggle Random Mod",
  "Hold Random Mod",
  "Toggle Gesture Select",
  "Hold Gesture Select",
  "Bank Select",
  "Next Bank",
  "Previous Bank",
  "Start",
  "Continue",
  "Stop",
  "Clock",
  "Scene Select",
] as const;

// The wizard form owns its own node namespace; the Controllers page renames
// only its root. These are the ids the MF Twister form actually publishes.
const WIZARD_SLOT = "controller-wizard.twister.encoder-slot";
const WIZARD_MESSAGES = '[data-synth-node-id^="controller-wizard.twister.button."][data-synth-node-id$=".message"]';
const WIZARD_ARGUMENTS = '[data-synth-node-id^="controller-wizard.twister.button."][data-synth-node-id$=".argument"]';
const WIZARD_CANDIDATES = '[data-synth-node-id^="runtime.controllers.wizard.chooser.candidate."]';

function wizardSlot(page: Page) {
  return page.locator(synthNode(WIZARD_SLOT));
}

function wizardMessage(page: Page, index: number) {
  return page.locator(synthNode(`controller-wizard.twister.button.${index}.message`));
}

function wizardArgument(page: Page, index: number) {
  return page.locator(synthNode(`controller-wizard.twister.button.${index}.argument`));
}

function wizardCandidate(page: Page, index: number) {
  return page.locator(WIZARD_CANDIDATES).nth(index);
}

function controllerRow(page: Page, index: number) {
  return page.locator(synthNode(`runtime.controllers.row.${index}`));
}

const CONTROLLER_SECTION = {
  encoders: 0,
  systemMessages: 1,
} as const;

const MAPPING_FIELD = {
  addressType: 28,
  channel: 0,
  blockStartCc: 16,
  blockEndCc: 17,
  slotIx: 2,
  blockStartPos: 18,
  button: 15,
  messageKind: 6,
  messageArg: 7,
} as const;

function controllerSectionBody(page: Page, controllerIx: number, section: number) {
  return page.locator(synthNode(`runtime.controllers.row.${controllerIx}.section.${section}.body`));
}

function mappingField(page: Page, controllerIx: number, section: number, rowIx: number, field: number) {
  return page.locator(
    synthNode(`runtime.controllers.row.${controllerIx}.section.${section}.body.mapping.${rowIx}.field.${field}`),
  );
}

async function expectMappingRowCount(page: Page, controllerIx: number, section: number, count: number): Promise<void> {
  const rowIdPattern = `^runtime\\.controllers\\.row\\.${controllerIx}\\.section\\.${section}\\.body\\.mapping\\.\\d+$`;
  await expect.poll(async () =>
    controllerSectionBody(page, controllerIx, section)
      .locator("[data-synth-node-id]")
      .evaluateAll((nodes, pattern) =>
        nodes.filter((node) => new RegExp(pattern).test((node as HTMLElement).dataset.synthNodeId ?? "")).length,
      rowIdPattern),
  ).toBe(count);
}

async function latestMidiEndpoints(page: Page): Promise<Array<{ identifier: string; name: string; kind: string }>> {
  return page.evaluate(() => {
    const commands = ((window as any).__task4Fake?.observations.commands ?? []) as Array<any>;
    const snapshot = commands.filter((command) => command.type === "midi-endpoints").at(-1);
    return snapshot?.endpoints ?? [];
  });
}

async function waitForTwisterEndpointSnapshot(page: Page, ordinal: number): Promise<void> {
  await expect.poll(() => latestMidiEndpoints(page)).toEqual(expect.arrayContaining([
    { identifier: `twister-in-${ordinal}`, name: TWISTER_DEVICE_NAME, kind: "input" },
    { identifier: `twister-out-${ordinal}`, name: TWISTER_DEVICE_NAME, kind: "output" },
  ]));
}

async function waitForTwisterEndpointRemoval(page: Page, ordinal: number): Promise<void> {
  await expect.poll(async () => (await latestMidiEndpoints(page)).some((endpoint) =>
    endpoint.identifier === `twister-in-${ordinal}` || endpoint.identifier === `twister-out-${ordinal}`),
  ).toBe(false);
}

async function selectedWizardMessageLabels(page: Page): Promise<string[]> {
  return page.locator(WIZARD_MESSAGES).evaluateAll((controls) =>
    controls.map((control) => {
      const select = control.querySelector("select");
      return select?.selectedOptions[0]?.textContent?.trim() ?? control.textContent?.trim() ?? "";
    }),
  );
}

async function wizardMessageChoices(page: Page, index: number): Promise<string[]> {
  return wizardMessage(page, index).locator("select option").evaluateAll((options) =>
    options.map((option) => option.textContent?.trim() ?? ""),
  );
}

async function assertTwisterWizardDefaults(page: Page): Promise<void> {
  await expect(wizardSlot(page)).toHaveCount(1);
  await expect(wizardSlot(page).locator("input")).toHaveValue("0");
  await expect(page.locator(WIZARD_MESSAGES)).toHaveCount(6);
  await expect(page.locator(WIZARD_ARGUMENTS)).toHaveCount(6);
  // The form names its own controls: each column states its physical CC range
  // and each row names its side button with the same one-based wording the
  // refusal status uses ("Button 2" for index 1).
  await expect(page.locator(synthNode("controller-wizard.twister.column.0.heading"))).toHaveText("Left (CC 8-10)");
  await expect(page.locator(synthNode("controller-wizard.twister.column.1.heading"))).toHaveText("Right (CC 11-13)");
  for (let index = 0; index < 6; index += 1)
    await expect(page.locator(synthNode(`controller-wizard.twister.button.${index}.label`))).toHaveText(`Button ${index + 1}`);
  expect(await selectedWizardMessageLabels(page)).toEqual([...TWISTER_WIZARD_DEFAULTS]);
  expect(await wizardMessageChoices(page, 0)).toEqual([...TWISTER_WIZARD_CHOICES]);
  for (let index = 0; index < 6; index += 1)
    await expect(wizardArgument(page, index).locator("input")).toBeDisabled();
}

async function assertTwisterWizardColumnGeometry(page: Page): Promise<void> {
  const boxes = await Promise.all([...Array(6).keys()].map((index) => wizardMessage(page, index).boundingBox()));
  expect(boxes.every(Boolean), JSON.stringify(boxes)).toBe(true);
  const [left0, left1, left2, right0, right1, right2] = boxes as NonNullable<(typeof boxes)[number]>[];
  for (const box of [left1, left2]) expect(Math.abs(box.x - left0.x)).toBeLessThanOrEqual(1);
  for (const box of [right1, right2]) expect(Math.abs(box.x - right0.x)).toBeLessThanOrEqual(1);
  for (const [left, right] of [[left0, right0], [left1, right1], [left2, right2]] as const) {
    expect(right.x).toBeGreaterThan(left.x + left.width);
    expect(Math.abs(right.y - left.y)).toBeLessThanOrEqual(1);
  }
  expect(left1.y).toBeGreaterThan(left0.y);
  expect(left2.y).toBeGreaterThan(left1.y);
}

async function expandControllerSection(page: Page, controllerIx: number, section: number): Promise<void> {
  const sectionToggle = page.locator(synthNode(`runtime.controllers.row.${controllerIx}.section.${section}.toggle`));
  if (await sectionToggle.count() === 0)
    await page.locator(synthNode(`runtime.controllers.row.${controllerIx}.disclosure`)).click();
  await expect(sectionToggle).toBeVisible();
  if (await controllerSectionBody(page, controllerIx, section).count() === 0)
    await sectionToggle.click();
  await expect(controllerSectionBody(page, controllerIx, section)).toBeVisible();
}

async function expectMappingInputValue(
  page: Page,
  controllerIx: number,
  section: number,
  rowIx: number,
  field: number,
  value: string,
): Promise<void> {
  await expect(mappingField(page, controllerIx, section, rowIx, field).locator("input")).toHaveValue(value);
}

async function expectMappingSelectLabel(
  page: Page,
  controllerIx: number,
  section: number,
  rowIx: number,
  field: number,
  label: string,
): Promise<void> {
  // Compare the selected option's own label; substring filtering would let
  // "Hold Random" match "Hold Random Mod".
  await expect.poll(async () =>
    mappingField(page, controllerIx, section, rowIx, field)
      .locator("select")
      .evaluate((select) => (select as HTMLSelectElement).selectedOptions[0]?.textContent?.trim() ?? ""),
  ).toBe(label);
}

async function assertTwisterEncoderMappings(page: Page, controllerIx: number, slot: string): Promise<void> {
  await expandControllerSection(page, controllerIx, CONTROLLER_SECTION.encoders);
  await expectMappingRowCount(page, controllerIx, CONTROLLER_SECTION.encoders, 4);

  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 0, MAPPING_FIELD.channel, "0");
  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 0, MAPPING_FIELD.blockStartCc, "0");
  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 0, MAPPING_FIELD.blockEndCc, "16");
  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 0, MAPPING_FIELD.slotIx, slot);
  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 0, MAPPING_FIELD.blockStartPos, "0");

  await expectMappingSelectLabel(page, controllerIx, CONTROLLER_SECTION.encoders, 1, MAPPING_FIELD.addressType, "CC");
  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 1, MAPPING_FIELD.channel, "1");
  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 1, MAPPING_FIELD.blockStartCc, "0");
  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 1, MAPPING_FIELD.blockEndCc, "16");
  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 1, MAPPING_FIELD.slotIx, slot);
  await expectMappingInputValue(page, controllerIx, CONTROLLER_SECTION.encoders, 1, MAPPING_FIELD.blockStartPos, "0");
}

async function savedRuntimeConfig(page: Page): Promise<any | undefined> {
  return page.evaluate(() => {
    const fs = (window as any).__task4FakeRuntimeFs;
    if (!fs) throw new Error("runtime filesystem is unavailable");
    try {
      return JSON.parse(fs.readFile("/data/config.json", { encoding: "utf8" }));
    } catch {
      return undefined;
    }
  });
}

function expectedEncoderInputMappings(channel: number, slot: string) {
  return [...Array(16).keys()].map((position) => ({
    control: { channel, cc: position, type: "cc" },
    slotIx: Number(slot),
    position,
  }));
}

function expectedEncoderOutputMappings(slot: string) {
  return [...Array(16).keys()].map((position) => ({
    slotIx: Number(slot),
    position,
    cc: position,
  }));
}

async function assertSavedTwisterEncoderMappings(page: Page, controllerIx: number, slot: string): Promise<void> {
  await expect.poll(async () => {
    const config = await savedRuntimeConfig(page);
    const profile = config?.midiInstrument?.controllers?.[controllerIx]?.profile;
    return profile === undefined ? undefined : {
      turns: profile.encoderInput?.turns,
      pushes: profile.encoderInput?.pushes,
      outputProtocol: profile.encoderOutput?.protocol,
      outputs: profile.encoderOutput?.mappings,
    };
  }).toEqual({
    turns: expectedEncoderInputMappings(0, slot),
    pushes: expectedEncoderInputMappings(1, slot),
    outputProtocol: "twister",
    outputs: expectedEncoderOutputMappings(slot),
  });
}

type SideAssociation = { button: string; message: string; argument: string };

async function twisterSideAssociations(page: Page, controllerIx: number): Promise<SideAssociation[]> {
  const associations = await Promise.all([...Array(6).keys()].map(async (rowIx): Promise<SideAssociation> => {
    const field = (id: number) => mappingField(page, controllerIx, CONTROLLER_SECTION.systemMessages, rowIx, id);
    const argument = field(MAPPING_FIELD.messageArg).locator("input");
    return {
      button: await field(MAPPING_FIELD.button).locator("input").inputValue(),
      message: await field(MAPPING_FIELD.messageKind)
        .locator("select")
        .evaluate((select) => (select as HTMLSelectElement).selectedOptions[0]?.textContent?.trim() ?? ""),
      argument: await argument.count() === 1 ? await argument.inputValue() : "",
    };
  }));
  // The low-level editor presents system rows in its own canonical message
  // order, so compare the associations by side button rather than by row.
  return associations.sort((left, right) => Number(left.button) - Number(right.button));
}

async function assertTwisterSideAssociations(page: Page, controllerIx: number, slot: string): Promise<void> {
  await expandControllerSection(page, controllerIx, CONTROLLER_SECTION.systemMessages);
  await expectMappingRowCount(page, controllerIx, CONTROLLER_SECTION.systemMessages, 6);
  await expect.poll(() => twisterSideAssociations(page, controllerIx)).toEqual(
    TWISTER_WIZARD_DEFAULTS.map((message, button) => ({
      button: String(button),
      message,
      // Only the two relative bank messages carry the controller-wide slot.
      argument: message === "Next Bank" || message === "Previous Bank" ? slot : "",
    })),
  );
}

// Endpoint reconciliation completes on the browser Web MIDI poll, and this
// harness renders a frame per dispatched action rather than on a fast timer, so
// reopen the page until the reconciled selection is on screen.
async function expectReconciledEndpoints(page: Page, index: number, ordinal: number): Promise<void> {
  const selected = async () => Promise.all((["input", "output"] as const).map((side) =>
    page.locator(synthNode(`runtime.controllers.row.${index}.${side}`)).locator("select").inputValue()));
  const expected = [`twister-in-${ordinal}`, `twister-out-${ordinal}`];
  await expect.poll(async () => {
    const values = await selected();
    if (values.every((value, valueIx) => value === expected[valueIx])) return values;
    await page.locator(synthNode("runtime.controllers.back")).click();
    await page.locator(synthNode("runtime.sidebar.controllers")).click();
    return selected();
  }, { timeout: 15_000 }).toEqual(expected);
}

async function assertActiveTwisterRecord(page: Page, index: number, name: string, ordinal: number, slot: string): Promise<void> {
  const row = controllerRow(page, index);
  await expect(row.locator(synthNode(`runtime.controllers.row.${index}.name`))).toHaveText(name);
  await expect(row.locator(synthNode(`runtime.controllers.row.${index}.device`))).toHaveText(TWISTER_DISPLAY_NAME);
  await expectReconciledEndpoints(page, index, ordinal);
  // Blacklist is offered only when the record's persisted
  // wizard id resolves in the registry, so its presence is how an installed
  // opaque wizard id is observable through production UI.
  await expect(row.locator(synthNode(`runtime.controllers.row.${index}.blacklist`))).toBeVisible();
  await assertTwisterEncoderMappings(page, index, slot);
  await assertTwisterSideAssociations(page, index, slot);
  await assertSavedTwisterEncoderMappings(page, index, slot);
}

async function expectControllersWarning(page: Page, visible: boolean): Promise<void> {
  const warning = page.locator(synthNode("runtime.sidebar.controllers.warning"));
  if (visible) await expect(warning).toBeVisible();
  else await expect(warning).toHaveCount(0);
}

async function stopAndRelaunchRealFakeApp(page: Page): Promise<void> {
  await stopRealFakeApp(page);
  await installRealFakeApp(page);
}

test("controller wizard unique candidate configures a default Twister profile in exactly three clicks", async ({ page }) => {
  await page.setViewportSize({ width: 1200, height: 800 });
  await installRealFakeApp(page);
  await installTwisterPair(page, 1);
  await waitForTwisterEndpointSnapshot(page, 1);

  await page.locator('[data-synth-node-id="runtime.sidebar.controllers"]').click();
  await page.locator('[data-synth-node-id="runtime.controllers.wizard.launch"]').click();
  await assertTwisterWizardDefaults(page);
  await assertTwisterWizardColumnGeometry(page);
  await page.locator('[data-synth-node-id="runtime.controllers.wizard.submit"]').click();

  await assertActiveTwisterRecord(page, 0, TWISTER_DISPLAY_NAME, 1, "0");
  await expectControllersWarning(page, false);
  await stopAndRelaunchRealFakeApp(page);
  await installTwisterPair(page, 1);
  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await assertActiveTwisterRecord(page, 0, TWISTER_DISPLAY_NAME, 1, "0");
});

test("controller wizard disables configuration when no candidate exists", async ({ page }) => {
  await installRealFakeApp(page);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await expect(page.locator(synthNode("runtime.controllers.wizard.launch"))).toBeDisabled();
  await expect(page.locator(synthNode("runtime.controllers.available.heading"))).toHaveText("Available controllers");
  await expect(page.locator(synthNode("runtime.controllers.available.empty"))).toContainText(
    "No recognized unconfigured controller pair is present",
  );
});

test("controller wizard presents a chooser for duplicate available Twisters", async ({ page }) => {
  await installRealFakeApp(page);
  await installTwisterPair(page, 1);
  await installTwisterPair(page, 2);
  await waitForTwisterEndpointSnapshot(page, 1);
  await waitForTwisterEndpointSnapshot(page, 2);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await page.locator(synthNode("runtime.controllers.wizard.launch")).click();
  await expect(page.locator(WIZARD_CANDIDATES)).toHaveCount(2);
  for (const ordinal of [1, 2] as const) {
    const choice = wizardCandidate(page, ordinal - 1);
    await expect(choice).toContainText(TWISTER_DISPLAY_NAME);
    await expect(choice).toContainText(`twister-in-${ordinal}`);
    await expect(choice).toContainText(`twister-out-${ordinal}`);
  }
  await wizardCandidate(page, 1).click();
  await assertTwisterWizardDefaults(page);
});

test("controller wizard uses deterministic names for duplicate submitted Twisters", async ({ page }) => {
  await installRealFakeApp(page);
  await installTwisterPair(page, 1);
  await installTwisterPair(page, 2);
  await waitForTwisterEndpointSnapshot(page, 1);
  await waitForTwisterEndpointSnapshot(page, 2);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await page.locator(synthNode("runtime.controllers.wizard.launch")).click();
  await wizardCandidate(page, 1).click();
  await page.locator(synthNode("runtime.controllers.wizard.submit")).click();
  await assertActiveTwisterRecord(page, 0, TWISTER_DISPLAY_NAME, 2, "0");
  await page.locator(synthNode("runtime.controllers.wizard.launch")).click();
  await page.locator(synthNode("runtime.controllers.wizard.submit")).click();
  await assertActiveTwisterRecord(page, 1, `${TWISTER_DISPLAY_NAME} 2`, 1, "0");
});

test("controller wizard ignores an available row and restores warning after blacklist removal", async ({ page }) => {
  await installRealFakeApp(page);
  await installTwisterPair(page, 1);
  await waitForTwisterEndpointSnapshot(page, 1);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await page.locator(synthNode("runtime.controllers.available.0.ignore")).click();
  await expect(controllerRow(page, 0)).toContainText("Released");
  await expect(controllerRow(page, 0)).toContainText("twister-in-1");
  await expect(controllerRow(page, 0).locator('[data-synth-node-id*=".mapping."]')).toHaveCount(0);
  await expectControllersWarning(page, false);
  await page.locator(synthNode("runtime.controllers.row.0.remove_blacklist")).click();
  await expect(controllerRow(page, 0)).toHaveCount(0);
  // The available row names the recognized controller by its registry
  // descriptor and its paired endpoints by their device names. The two strings
  // differ, so asserting the descriptor name is what proves the row identifies
  // the controller rather than echoing the device.
  await expect(page.locator(synthNode("runtime.controllers.available.0.name"))).toHaveText(TWISTER_DISPLAY_NAME);
  await expect(page.locator(synthNode("runtime.controllers.available.0.endpoints"))).toContainText(TWISTER_DEVICE_NAME);
  await expectControllersWarning(page, true);
});

test("controller wizard ignores from a new-candidate form", async ({ page }) => {
  await installRealFakeApp(page);
  await installTwisterPair(page, 1);
  await waitForTwisterEndpointSnapshot(page, 1);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await page.locator(synthNode("runtime.controllers.wizard.launch")).click();
  await page.locator(synthNode("runtime.controllers.wizard.ignore")).click();
  await expect(controllerRow(page, 0)).toContainText("Released");
  await expect(controllerRow(page, 0)).toContainText("twister-in-1");
  await expectControllersWarning(page, false);
});

test("controller wizard stale and invalid submit preserve entered values", async ({ page }) => {
  await installRealFakeApp(page);
  await installTwisterPair(page, 1);
  await waitForTwisterEndpointSnapshot(page, 1);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await page.locator(synthNode("runtime.controllers.wizard.launch")).click();
  await page.locator(`${synthNode(WIZARD_SLOT)} input`).fill("4");
  await wizardMessage(page, 0).locator("select").selectOption({ label: "Bank Select" });
  await wizardArgument(page, 0).locator("input").fill("7");
  await wizardMessage(page, 1).locator("select").selectOption({ label: "Scene Select" });
  await wizardArgument(page, 1).locator("input").fill("999999999999999999999999999999999999999");
  await page.locator(synthNode("runtime.controllers.wizard.submit")).click();
  // The refusal names the offending button; buttons are labelled from 1.
  await expect(page.locator(synthNode("runtime.controllers.wizard.status"))).toContainText("Button 2");
  await expect(page.locator(`${synthNode(WIZARD_SLOT)} input`)).toHaveValue("4");
  await expect(wizardArgument(page, 0).locator("input")).toHaveValue("7");
  await expect(wizardArgument(page, 1).locator("input")).toHaveValue("999999999999999999999999999999999999999");
  // Make the form valid so the next refusal can only be the stale candidate.
  await wizardArgument(page, 1).locator("input").fill("5");
  await removeTwisterPair(page, 1);
  await waitForTwisterEndpointRemoval(page, 1);
  await page.locator(synthNode("runtime.controllers.wizard.submit")).click();
  await expect(page.locator(synthNode("runtime.controllers.wizard.status"))).toContainText("reconnect");
  await expect(page.locator(`${synthNode(WIZARD_SLOT)} input`)).toHaveValue("4");
  await expect(wizardArgument(page, 0).locator("input")).toHaveValue("7");
  await expect(wizardArgument(page, 1).locator("input")).toHaveValue("5");
  await expect(controllerRow(page, 0)).toHaveCount(0);
});

test("controller wizard supports rename and delete on active records", async ({ page }) => {
  await installRealFakeApp(page);
  await installTwisterPair(page, 1);
  await waitForTwisterEndpointSnapshot(page, 1);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await page.locator(synthNode("runtime.controllers.wizard.launch")).click();
  await page.locator(synthNode("runtime.controllers.wizard.submit")).click();
  // Rename lives in the row's expanded editor, so the disclosure must be
  // opened before the rename controls exist to interact with.
  await page.locator(synthNode("runtime.controllers.row.0.disclosure")).click();
  await page.locator(`${synthNode("runtime.controllers.row.0.rename_draft")} input`).fill("Studio Twister");
  await page.locator(synthNode("runtime.controllers.row.0.rename")).click();
  await expect(controllerRow(page, 0)).toContainText("Studio Twister");
  await page.locator(synthNode("runtime.controllers.row.0.delete")).click();
  await expect(controllerRow(page, 0)).toHaveCount(0);
  await expectControllersWarning(page, true);
});

test("controller wizard actions are absent on a manually added record", async ({ page }) => {
  await installRealFakeApp(page);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await page.locator(`${synthNode("runtime.controllers.add_preset")} select`).selectOption({ label: "Custom" });
  await page.locator(synthNode("runtime.controllers.add_button")).click();

  const row = controllerRow(page, 0);
  await expect(row.locator(synthNode("runtime.controllers.row.0.name"))).toHaveText("Custom");
  // A Custom row adds through no preset and binds no endpoints, so it
  // renders neither a preset device name nor a stored endpoint label -- the
  // page's no-input placeholder is what it actually shows.
  await expect(row.locator(synthNode("runtime.controllers.row.0.device"))).toHaveText("(none)");
  await expect(row.locator(synthNode("runtime.controllers.row.0.delete"))).toBeVisible();
  // Rename lives in the row's expanded editor, rendered as a tree sibling of
  // the row's own header container rather than nested under it, so this one
  // is queried from the page, not scoped through `row`.
  await page.locator(synthNode("runtime.controllers.row.0.disclosure")).click();
  await expect(page.locator(synthNode("runtime.controllers.row.0.rename"))).toBeVisible();
  // A manual record carries no persisted wizard id, so the registry-gated
  // lifecycle actions are not offered even though its kind is Generic.
  await expect(row.locator(synthNode("runtime.controllers.row.0.reconfigure"))).toHaveCount(0);
  await expect(row.locator(synthNode("runtime.controllers.row.0.blacklist"))).toHaveCount(0);
});

test("controller wizard retains dormant profile when an active record is blacklisted", async ({ page }) => {
  await installRealFakeApp(page);
  await installTwisterPair(page, 1);
  await waitForTwisterEndpointSnapshot(page, 1);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await page.locator(synthNode("runtime.controllers.wizard.launch")).click();
  await page.locator(`${synthNode(WIZARD_SLOT)} input`).fill("4");
  await page.locator(synthNode("runtime.controllers.wizard.submit")).click();
  await assertActiveTwisterRecord(page, 0, TWISTER_DISPLAY_NAME, 1, "4");

  await page.locator(synthNode("runtime.controllers.row.0.blacklist")).click();
  await expect(controllerRow(page, 0)).toContainText("Released");
  await expect(controllerRow(page, 0).locator('[data-synth-node-id*=".mapping."]')).toHaveCount(0);
  await expect(controllerRow(page, 0).locator(synthNode("runtime.controllers.row.0.input"))).toHaveCount(0);
  // The retained dormant profile is observable through the blacklisted row's
  // wizard: Configure seeds Encoder Slot 4 instead of the default 0.
  await page.locator(synthNode("runtime.controllers.row.0.configure")).click();
  await expect(page.locator(`${synthNode(WIZARD_SLOT)} input`)).toHaveValue("4");
  await page.locator(synthNode("runtime.controllers.wizard.submit")).click();
  await assertActiveTwisterRecord(page, 0, TWISTER_DISPLAY_NAME, 1, "4");
});

test("controller wizard configures a blacklisted record through its wizard", async ({ page }) => {
  await installRealFakeApp(page);
  await installTwisterPair(page, 1);
  await waitForTwisterEndpointSnapshot(page, 1);

  await page.locator(synthNode("runtime.sidebar.controllers")).click();
  await page.locator(synthNode("runtime.controllers.available.0.ignore")).click();
  await expect(controllerRow(page, 0)).toContainText("Released");
  await page.locator(synthNode("runtime.controllers.row.0.configure")).click();
  await assertTwisterWizardDefaults(page);
  await page.locator(synthNode("runtime.controllers.wizard.submit")).click();
  await assertActiveTwisterRecord(page, 0, TWISTER_DISPLAY_NAME, 1, "0");
});

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
