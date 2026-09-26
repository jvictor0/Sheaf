import { expect, test, type Page } from "@playwright/test";
import { AUDIO_INPUT_FIXTURE_DEVICE_ID, AUDIO_INPUT_FIXTURE_DEVICE_LABEL, FIXTURE_APPS, installRealFakeApp, stopRealFakeApp, synthNode } from "./helpers/fake-app.js";

type RuntimeResponse = { type: string; [key: string]: unknown };

const AUDIO_INPUT_PROBE = (FIXTURE_APPS as typeof FIXTURE_APPS & {
  audioInputProbe: { appId: string; displayName: string; uiHeight: number };
}).audioInputProbe;

const INPUT_VALUES = {
  singleChannel: [0.125],
  stereoPair: [0.125, -0.25],
  fourLiveMonoClamp: [0.125, 0.75, 0.875, 0.9375],
  fourLiveStereoClamp: [0.125, -0.25, 0.875, 0.9375],
  quadOut0Dominant: [0.25, 0.01, 0.5, 0.02],
  quadOut1Dominant: [0.01, -0.25, 0.02, 0.5],
} as const;

const AUDIO_INPUT_STATUS = {
  online: 2,
  channelCountUnreported: 7,
} as const;

// Every test in this file drives capture through `selectFixtureAudioInput`,
// which pins the operator's selected device (audio.ts's `selectedDeviceId`),
// so every resulting `getUserMedia` call names that device explicitly.
const PINNED_CAPTURE_CONSTRAINTS = {
  audio: {
    channelCount: { ideal: 4 },
    echoCancellation: false,
    noiseSuppression: false,
    autoGainControl: false,
    deviceId: { exact: AUDIO_INPUT_FIXTURE_DEVICE_ID },
  },
} as const;

// 2,000 microunits is 0.002 full-scale. That leaves room for browser
// AudioWorklet/float scheduling noise while staying far below the separation
// between the deterministic probe peaks in this file.
const PEAK_TOLERANCE_MICROUNITS = 2_000;

test.setTimeout(120_000);

test.afterEach(async ({ page }) => {
  await stopRealFakeApp(page);
});

function expectedProbePeakMicrounits(values: readonly number[], activeChannels: number): number {
  const sample = (channel: number) => channel < activeChannels ? (values[channel] ?? 0) : 0;
  const out0 = sample(0) + 0.5 * sample(2);
  const out1 = sample(1) - sample(3);
  return Math.round(Math.max(Math.abs(out0), Math.abs(out1)) * 1_000_000);
}

async function runtimeRequest<T extends RuntimeResponse = RuntimeResponse>(
  page: Page,
  command: Record<string, unknown>,
): Promise<T> {
  const response = await page.evaluate(async (command) => {
    const state = (window as any).__task4Fake;
    if (!state?.runtime) throw new Error("fake runtime handle is not exposed");
    return state.runtime.request(command);
  }, command);
  if (response.type === "error") throw new Error(String(response.error));
  return response as T;
}

async function waitForNativeStats(page: Page, predicate: (stats: { blocks: number; peakMicrounits: number; deadlineMicrounits: number }) => boolean) {
  let latest = { blocks: 0, peakMicrounits: 0, deadlineMicrounits: 0 };
  await expect.poll(async () => {
    const response = await runtimeRequest<{ type: "audio-worklet-stats"; blocks: number; peakMicrounits: number; deadlineMicrounits: number }>(
      page,
      { type: "audio-worklet-stats" },
    );
    latest = {
      blocks: response.blocks,
      peakMicrounits: response.peakMicrounits,
      deadlineMicrounits: response.deadlineMicrounits,
    };
    return predicate(response);
  }, { timeout: 10_000 }).toBe(true);
  return latest;
}

async function expectAudioStatus(page: Page, text: string): Promise<void> {
  await page.locator(synthNode("runtime.sidebar.audio")).click();
  await expect(page.locator(synthNode("runtime.audio.input"))).toBeVisible();
  await expect(page.locator(synthNode("runtime.audio.status_line"))).toHaveText(text);
}

// The operator gesture that now arms capture: open the Audio page and pick
// the fixture's one enumerated device from the real input combo. That
// selection resolves to an index in C++ (BrowserRuntimeMainServices::
// DispatchAudio / ArmPendingAudioRequest) which main.ts's post-dispatch poll
// (`consumePendingAudioRequest`) delivers to AudioBridge.acquireInputDeviceAtIndex --
// capture no longer starts merely because the application declared input
// channels. Every test below that needs live capture calls this once, right
// after `installRealFakeApp`.
//
// `AudioBridge.submitAudioDevices()` fires in the background on activation
// (never awaited by it), so the combo's options are not guaranteed to carry
// the fixture device yet at the moment this runs, and nothing but another
// dispatched action re-renders it (`frameIntervalMs` is deliberately slow in
// this fixture). Waiting on the runtime's own observed `audio-devices`
// command -- already recorded by the harness's `observingClient` -- before
// touching the combo is what makes this deterministic rather than racing
// Playwright's generic retry against that background submission.
async function selectFixtureAudioInput(page: Page): Promise<void> {
  await page.waitForFunction((label) => {
    const state = (window as unknown as { __task4Fake?: { observations?: { commands?: Array<{ type: string; devices?: Array<{ label: string }> }> } } }).__task4Fake;
    return !!state?.observations?.commands?.some((command) =>
      command.type === "audio-devices" && (command.devices ?? []).some((device) => device.label === label));
  }, AUDIO_INPUT_FIXTURE_DEVICE_LABEL, { timeout: 10_000 });
  await page.locator(synthNode("runtime.sidebar.audio")).click();
  await page.locator(`${synthNode("runtime.audio.input")} select`).selectOption({ label: AUDIO_INPUT_FIXTURE_DEVICE_LABEL });
}

async function expectNativePeak(page: Page, expectedPeakMicrounits: number): Promise<void> {
  await waitForNativeStats(page, (candidate) =>
    candidate.blocks > 0 &&
    Number.isFinite(candidate.deadlineMicrounits) &&
    Math.abs(candidate.peakMicrounits - expectedPeakMicrounits) <= PEAK_TOLERANCE_MICROUNITS);
}

async function expectExactNativePeak(page: Page, expectedPeakMicrounits: number): Promise<void> {
  await waitForNativeStats(page, (candidate) =>
    candidate.blocks > 0 &&
    Number.isFinite(candidate.deadlineMicrounits) &&
    candidate.peakMicrounits === expectedPeakMicrounits);
}

async function audioResources(page: Page): Promise<any> {
  return page.evaluate(() => (window as any).__task4Fake.resources);
}

async function expectGrantedInputAcquisition(
  page: Page,
  expected: { sourceChannels: number; physicalChannels: number; statusCode?: number },
): Promise<void> {
  await expect.poll(async () => {
    const resources = await audioResources(page);
    const registrations = resources.inputSourceRegistrations.map((registration: { physicalChannels: number; statusCode: number; nativeHandle: number }) => ({
      physicalChannels: registration.physicalChannels,
      statusCode: registration.statusCode,
      nativeHandlePositive: registration.nativeHandle > 0,
    }));
    return {
      getUserMediaCalls: resources.getUserMediaCalls,
      getUserMediaConstraints: resources.getUserMediaConstraints,
      mediaStreamSourceCreations: resources.mediaStreamSourceCreations,
      registrationCount: registrations.length,
      registrations,
      distinctNativeHandles: new Set(resources.inputSourceRegistrations.map((registration: { nativeHandle: number }) => registration.nativeHandle)).size,
      connections: resources.inputSourceConnections,
    };
  }, { timeout: 5_000 }).toEqual({
    getUserMediaCalls: 1,
    getUserMediaConstraints: [PINNED_CAPTURE_CONSTRAINTS],
    mediaStreamSourceCreations: 1,
    registrationCount: expect.any(Number),
    registrations: expect.arrayContaining([{
      physicalChannels: expected.physicalChannels,
      statusCode: expected.statusCode ?? AUDIO_INPUT_STATUS.online,
      nativeHandlePositive: true,
    }]),
    distinctNativeHandles: 1,
    connections: [{
      destination: "native-worklet",
      outputIndex: 0,
      inputIndex: 0,
      sourceChannels: expected.sourceChannels,
      physicalChannels: expected.physicalChannels,
    }],
  });
  const acquiredResources = await audioResources(page);
  expect(acquiredResources.inputSourceRegistrations.length).toBeGreaterThanOrEqual(1);
  for (const registration of acquiredResources.inputSourceRegistrations) {
    expect(registration.physicalChannels).toBe(expected.physicalChannels);
    expect(registration.statusCode).toBe(expected.statusCode ?? AUDIO_INPUT_STATUS.online);
    expect(registration.nativeHandle).toBe(acquiredResources.inputSourceRegistrations[0].nativeHandle);
  }
  const resources = await audioResources(page);
  expect(resources.inputSourceConnections).not.toEqual(expect.arrayContaining([
    expect.objectContaining({ destination: "audio-context-destination" }),
  ]));
}

async function expectNoInputSourceAcquisition(page: Page): Promise<void> {
  const resources = await audioResources(page);
  expect(resources.getUserMediaCalls).toBe(1);
  expect(resources.getUserMediaConstraints).toEqual([PINNED_CAPTURE_CONSTRAINTS]);
  expect(resources.mediaStreamSourceCreations).toBe(0);
  expect(resources.inputSourceRegistrations).toEqual([]);
  expect(resources.inputSourceConnections).toEqual([]);
}

async function expectRuntimeFunctionsLive(page: Page): Promise<void> {
  const beforeFrames = await page.evaluate(() => ((window as any).__task4Fake.observations.frames as unknown[]).length);
  await page.locator(synthNode("runtime.sidebar.file")).click();
  await expect(page.locator(synthNode("runtime.file.root"))).toBeVisible();
  await expect.poll(() => page.evaluate(() => ((window as any).__task4Fake.observations.frames as unknown[]).length))
    .toBeGreaterThan(beforeFrames);

  await expect(runtimeRequest(page, { type: "persistence-status" }))
    .resolves.toMatchObject({ type: "page-status" });
  await expect(runtimeRequest(page, { type: "midi-diagnostics" }))
    .resolves.toMatchObject({ type: "midi-diagnostics" });
  const first = await runtimeRequest<{ type: "audio-worklet-stats"; blocks: number; peakMicrounits: number; deadlineMicrounits: number }>(
    page,
    { type: "audio-worklet-stats" },
  );
  await waitForNativeStats(page, (candidate) => candidate.blocks > first.blocks);
}

for (const scenario of [
  {
    name: "one-channel source shape with one published physical channel",
    sourceChannels: 1,
    physicalChannels: 1,
    values: INPUT_VALUES.singleChannel,
    status: "Input requested 4 / active 1 - input channel shortfall",
  },
  {
    name: "two-channel source shape with two published physical channels",
    sourceChannels: 2,
    physicalChannels: 2,
    values: INPUT_VALUES.stereoPair,
    status: "Input requested 4 / active 2 - input channel shortfall",
  },
  {
    name: "four-channel source shape with four published physical channels out0",
    sourceChannels: 4,
    physicalChannels: 4,
    values: INPUT_VALUES.quadOut0Dominant,
    // No live capture diagnostic applies here, so the selection
    // acknowledgement the device combo set on dispatch shows through instead.
    status: "Input requested 4 / active 4 - Using System Default",
  },
  {
    name: "four-channel source shape with four published physical channels out1",
    sourceChannels: 4,
    physicalChannels: 4,
    values: INPUT_VALUES.quadOut1Dominant,
    // Same as the out0 case above: no live diagnostic, so the selection
    // acknowledgement shows through.
    status: "Input requested 4 / active 4 - Using System Default",
  },
] as const) {
  test(`real Wasm probe transforms ${scenario.name}`, async ({ page }) => {
    await installRealFakeApp(page, AUDIO_INPUT_PROBE, {
      audioInput: {
        capture: "deterministic",
        sourceChannels: scenario.sourceChannels,
        physicalChannels: scenario.physicalChannels,
        channelValues: scenario.values,
      },
    });

    await selectFixtureAudioInput(page);
    await expectAudioStatus(page, scenario.status);
    await expectGrantedInputAcquisition(page, scenario);
    await expectNativePeak(page, expectedProbePeakMicrounits(scenario.values, scenario.physicalChannels));
  });
}

for (const scenario of [
  {
    name: "four-live-channel source clamped to one published physical channel",
    sourceChannels: 4,
    physicalChannels: 1,
    values: INPUT_VALUES.fourLiveMonoClamp,
    status: "Input requested 4 / active 1 - input channel shortfall",
  },
  {
    name: "four-live-channel source clamped to two published physical channels",
    sourceChannels: 4,
    physicalChannels: 2,
    values: INPUT_VALUES.fourLiveStereoClamp,
    status: "Input requested 4 / active 2 - input channel shortfall",
  },
] as const) {
  test(`real Wasm probe honors published physical-count clamp for ${scenario.name}`, async ({ page }) => {
    await installRealFakeApp(page, AUDIO_INPUT_PROBE, {
      audioInput: {
        capture: "deterministic",
        sourceChannels: scenario.sourceChannels,
        physicalChannels: scenario.physicalChannels,
        channelValues: scenario.values,
      },
    });

    await selectFixtureAudioInput(page);
    await expectAudioStatus(page, scenario.status);
    await expectGrantedInputAcquisition(page, scenario);
    await expectNativePeak(page, expectedProbePeakMicrounits(scenario.values, scenario.physicalChannels));
  });
}

test("literal zero deterministic input remains exactly silent through the real Wasm callback", async ({ page }) => {
  await installRealFakeApp(page, AUDIO_INPUT_PROBE, {
    audioInput: {
      capture: "deterministic",
      sourceChannels: 1,
      physicalChannels: 1,
      channelValues: [0],
    },
  });

  await selectFixtureAudioInput(page);
  await expectAudioStatus(page, "Input requested 4 / active 1 - input channel shortfall");
  await expectGrantedInputAcquisition(page, { sourceChannels: 1, physicalChannels: 1 });
  await expectExactNativePeak(page, 0);
});

test("permission denial keeps the real Wasm runtime live with safe silent input", async ({ page }) => {
  await installRealFakeApp(page, AUDIO_INPUT_PROBE, {
    audioInput: { capture: "denied", physicalChannels: 0, channelValues: [0, 0, 0, 0] },
  });

  await selectFixtureAudioInput(page);
  await expectAudioStatus(page, "Input requested 4 / active 0 - microphone permission denied");
  await expect(page.locator(synthNode("runtime.audio.input.retry"))).toBeVisible();
  await expectNoInputSourceAcquisition(page);
  await expectExactNativePeak(page, 0);
  await expectRuntimeFunctionsLive(page);
});

test("unreported shortfall keeps deterministic input and non-audio runtime functions live", async ({ page }) => {
  const values = [0.2, -0.125, 0.8, 0.7];
  await installRealFakeApp(page, AUDIO_INPUT_PROBE, {
    audioInput: {
      capture: "deterministic",
      sourceChannels: 4,
      physicalChannels: 2,
      channelValues: values,
      omitTrackChannelCount: true,
    },
  });

  await selectFixtureAudioInput(page);
  await expectAudioStatus(page, "Input requested 4 / active 2 - microphone channel count unreported, input channel shortfall");
  await expectGrantedInputAcquisition(page, {
    sourceChannels: 4,
    physicalChannels: 2,
    statusCode: AUDIO_INPUT_STATUS.channelCountUnreported,
  });
  await expectNativePeak(page, expectedProbePeakMicrounits(values, 2));
  await expectRuntimeFunctionsLive(page);
});

// Deferred source attach — a capture registration landing while native worklet
// startup is still in flight — is not reachable through this harness. It installs
// the app on the guarded launch path, where startFromUserActivation() runs before
// any UI exists, so there is no combo for an operator to select a device from and
// nothing can register mid-startup. The claim is checked where the race is real,
// against AudioBridge directly: see audio-flow.spec.ts's deferred-input
// reconciliation cases, covering both persistent attachment failure and a
// successful deferred source staying live.

test("stream termination clears active input while output, UI, persistence, and MIDI stay live", async ({ page }) => {
  const values = INPUT_VALUES.quadOut0Dominant;
  await installRealFakeApp(page, AUDIO_INPUT_PROBE, {
    audioInput: { capture: "deterministic", sourceChannels: 4, physicalChannels: 4, channelValues: values },
  });
  await selectFixtureAudioInput(page);
  // No live diagnostic applies yet, so the selection acknowledgement shows through.
  await expectAudioStatus(page, "Input requested 4 / active 4 - Using System Default");
  await expectGrantedInputAcquisition(page, { sourceChannels: 4, physicalChannels: 4 });
  await expectNativePeak(page, expectedProbePeakMicrounits(values, 4));

  await page.evaluate(async () => {
    const input = (window as any).__task4Fake.audioInput;
    if (!input?.endCurrentTrack) throw new Error("audio input fixture cannot end the current track");
    await input.endCurrentTrack();
  });

  await expectAudioStatus(page, "Input requested 4 / active 0 - microphone stream ended");
  await expect(page.locator(synthNode("runtime.audio.input.retry"))).toBeVisible();
  await expectRuntimeFunctionsLive(page);
});

test("teardown stops a granted audio input track exactly once", async ({ page }) => {
  await installRealFakeApp(page, AUDIO_INPUT_PROBE, {
    audioInput: {
      capture: "deterministic",
      sourceChannels: 2,
      physicalChannels: 2,
      channelValues: INPUT_VALUES.stereoPair,
    },
  });

  await selectFixtureAudioInput(page);
  await expectAudioStatus(page, "Input requested 4 / active 2 - input channel shortfall");
  await expectGrantedInputAcquisition(page, { sourceChannels: 2, physicalChannels: 2 });
  const teardown = await stopRealFakeApp(page);
  expect(teardown.inputTrackStops).toBe(1);
  expect(teardown.expectedInputTrackStops).toBe(1);
});
