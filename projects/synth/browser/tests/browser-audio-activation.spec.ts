import { expect, test, type Page } from "@playwright/test";

// Reproduces the browser autoplay restriction this change fixes, end to end,
// through the real installSynthBrowserApp()/SynthBrowserApp bootstrap and the
// real compiled miniapp WASM -- not a hand-rolled substitute. Automation
// (Playwright, headless Chromium) grants autoplay unconditionally, so a test
// that just loads the page and clicks proves nothing here: a fresh
// AudioContext is born already running there, and even
// navigator.userActivation.isActive reads true unconditionally (verified by
// hand), which is exactly the permissiveness that lets this class of bug ship
// behind a green suite. Both tests below force the restriction a browser
// applies on an origin it has not engaged with: the context starts suspended,
// and its resume() is patched to take effect only while the event currently
// driving it is a trusted one (tracked directly via `isTrusted`, since
// isActive is not a usable signal in this environment) -- otherwise it stays
// suspended, the same way a real refusal leaves it.
//
// miniapp's bank-select action is queued and drained only inside the real
// native AudioWorklet callback (MiniAppCore.hpp's ProcessBlock reads the
// selection from its UI bus there), the same "drained only on the audio
// thread" shape the reported bug's own bank-select mechanism has. So a bank
// switch only becomes observable once that callback actually runs, making it
// a faithful stand-in for "the page switches".

async function installRiggedMiniapp(page: Page) {
  await page.goto("http://127.0.0.1:4174/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');

  return page.evaluate(async () => {
    const { installSynthBrowserApp, createDirectRuntimeClient } =
      await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const { decodeCommandBuffer } = await (new Function("return import('/dist/src/protocol.js')")() as Promise<any>);

    const gesture = { active: false };
    const trackGesture = (event: Event) => { gesture.active = event.isTrusted; };
    document.addEventListener("pointerdown", trackGesture, true);
    document.addEventListener("click", trackGesture, true);

    const audioContext = new AudioContext();
    await audioContext.suspend();
    const nativeResume = audioContext.resume.bind(audioContext);
    audioContext.resume = async () => {
      if (gesture.active) await nativeResume();
      // Otherwise: silently stay suspended, exactly like a real refusal.
    };

    const real = createDirectRuntimeClient();
    const state: { latestFrame?: { nodes: any[]; drawCommands: any[] } } = {};
    const runtimeClient = {
      request: async (command: { type: string }) => {
        const response = await real.request(command);
        if (command.type === "build-ui-frame" && response.type === "ui-frame")
          state.latestFrame = decodeCommandBuffer(Uint8Array.from(response.frame).buffer);
        return response;
      },
      startAudioWorklet: (context?: AudioContext) => real.startAudioWorklet(context),
      terminate: () => real.terminate?.(),
    };

    const moduleUrl = new URL("/dist/wasm/apps/miniapp/miniapp.js", location.href).href;
    const root = document.querySelector("#synth-root") as HTMLElement;
    const app = await installSynthBrowserApp(root, {
      module: {
        entryUrl: moduleUrl,
        locateFile: {
          "miniapp.js": moduleUrl,
          "miniapp.wasm": new URL("/dist/wasm/apps/miniapp/miniapp.wasm", location.href).href,
        },
        mainScriptUrlOrBlob: moduleUrl,
      },
      runtimeIdentity: { publisherId: "sheaf", appId: "miniapp", runtimeConfigVersion: 1 },
      runtimeClientFactory: () => runtimeClient,
      audioOptions: { audioContext },
      frameIntervalMs: 50,
      // No activationLease: this is the direct-boot path (frogg3rs'
      // site-boot.mjs), where the bug lives -- audio only ever starts from a
      // later gesture, never synchronously inside start().
    });

    const encoderSignature = () => {
      const frame = state.latestFrame!;
      const encoder = frame.nodes.find((node) => node.id === "miniapp.encoder.0");
      const draws = frame.drawCommands.slice(encoder.drawStart, encoder.drawStart + encoder.drawCount);
      return JSON.stringify(draws.map((draw: any) => [draw.kind, draw.bounds, draw.startRadians, draw.endRadians, draw.text]));
    };
    // miniapp.vco.scope is a plain Draw surface with no action attached
    // (MiniAppUI.hpp's EmitWaveform never gives it one) -- confirmed here so
    // a click on it below cannot also reach startUserActivation() through
    // BrowserUiBackend's own action dispatch, keeping it a clean probe of
    // installBrowserAudioActivation's listener alone.
    const scopeNode = state.latestFrame!.nodes.find((node) => node.id === "miniapp.vco.scope");
    const scopeHasNoAction = !!scopeNode && !scopeNode.pointerDragAction && !scopeNode.doubleClickAction;

    (window as any).__activationTest = { app, root, audioContext, encoderSignature };
    return { encoderSignature: encoderSignature(), scopeHasNoAction };
  });
}

test("a real gesture on the app surface starts audio through the newly wired listener, and the page switch it enables becomes visible", async ({ page }) => {
  test.setTimeout(30_000);
  const initial = await installRiggedMiniapp(page);
  expect(initial.scopeHasNoAction).toBe(true);

  // Nothing has attempted activation yet, so this exercises
  // installBrowserAudioActivation's own pointerdown listener in isolation:
  // the scope surface under the click point carries no action, so it sets
  // `pointer-events: none` (ui.ts's acceptsPointerEvents) and the click
  // actually lands on #synth-root itself -- BrowserUiBackend's click-driven
  // startUserActivation() path is never reached from this click at all.
  await page.locator("#synth-root").click();

  const afterSurfaceClick = await page.evaluate(async () => {
    const { root, audioContext } = (window as any).__activationTest;
    const deadline = performance.now() + 5_000;
    while (performance.now() < deadline && audioContext.state !== "running")
      await new Promise((resolve) => setTimeout(resolve, 50));
    return { audioState: audioContext.state, status: root.dataset.synthStatus };
  });
  expect(afterSurfaceClick.audioState).toBe("running");

  // Now that the worklet is actually running, a bank-select click's message
  // can actually be drained: this is "the page switches".
  await page.locator('[data-synth-node-id="miniapp.bank.lfo"]').click();

  const afterBankClick = await page.evaluate(async (beforeSignature) => {
    const { encoderSignature } = (window as any).__activationTest;
    const deadline = performance.now() + 5_000;
    while (performance.now() < deadline && encoderSignature() === beforeSignature)
      await new Promise((resolve) => setTimeout(resolve, 50));
    return { afterSignature: encoderSignature() };
  }, initial.encoderSignature);

  expect(afterBankClick.afterSignature).not.toBe(initial.encoderSignature);
  await page.evaluate(() => (window as any).__activationTest.app.stop());
});

// The task doc's own acceptance criterion, end to end: the real-world
// sequence is boot-time attempt fails (nothing has clicked yet, autoplay
// refuses the resume) -> the operator's first real click must still recover,
// with the bank switch it carries taking effect. Recovering here needed
// fixing the same defect at three latches this session traced in turn:
// startUserActivation()'s latch must not block a second attempt from being
// made at all (main.ts); AudioBridge must not permanently latch itself
// `stopped` after a merely-failed start (audio.ts); and the native
// BrowserRuntime.hpp entry point must not treat "already attempted" as
// "already running" and skip retrying the actual resume (its own
// `audioContext_ != 0` guard). One honest caveat: this same click also
// exercises installBrowserAudioActivation's own pointerdown listener
// (Test 1's mechanism) at the same time, since pointerdown always bubbles to
// #synth-root regardless of which control is clicked -- so this test cannot,
// by itself, prove the main.ts latch fix in isolation from that wiring; see
// this session's report for how each was verified independently instead.
test("a later gesture actually starts audio and the bank page switches, after an earlier non-gesture attempt was refused", async ({ page }) => {
  test.setTimeout(30_000);
  const initial = await installRiggedMiniapp(page);

  // A UI action fires with no user gesture behind it, exactly like the
  // reported bug's own trigger: `.click()` runs every DOM listener a real
  // click would (so this reaches startUserActivation() and dispatches the
  // bank-select action), but the event it fires carries no user activation,
  // so the emulated restriction correctly refuses the resume() it drives.
  const before = await page.evaluate(async () => {
    const { root, audioContext } = (window as any).__activationTest;
    const bankLfoButton = document.querySelector('[data-synth-node-id="miniapp.bank.lfo"]') as HTMLElement;
    bankLfoButton.click();

    // The runtime only gives up on a stalled worklet after a fixed 5-second
    // deadline (worker.ts), so this cannot resolve any sooner than that.
    const deadline = performance.now() + 8_000;
    while (performance.now() < deadline && !(root.dataset.synthStatus ?? "").includes("did not make progress"))
      await new Promise((resolve) => setTimeout(resolve, 50));
    return { status: root.dataset.synthStatus, audioState: audioContext.state };
  });
  expect(before.status).toContain("did not make progress");
  expect(before.audioState).toBe("suspended");

  // The operator's first REAL gesture: Playwright's input is a trusted,
  // OS-level click (unlike the script-fired one above), on the same bank
  // button, so this click both starts audio and carries the bank switch that
  // the earlier failed attempt already queued (miniapp queues it regardless
  // of whether activation succeeds -- see MiniAppCore.hpp's ProcessBlock).
  await page.locator('[data-synth-node-id="miniapp.bank.lfo"]').click();

  const after = await page.evaluate(async (beforeSignature) => {
    const { root, audioContext, encoderSignature, app } = (window as any).__activationTest;
    const deadline = performance.now() + 8_000;
    while (performance.now() < deadline &&
           (audioContext.state !== "running" || encoderSignature() === beforeSignature))
      await new Promise((resolve) => setTimeout(resolve, 50));
    const result = { status: root.dataset.synthStatus, audioState: audioContext.state, afterSignature: encoderSignature() };
    await app.stop();
    return result;
  }, initial.encoderSignature);

  expect(after.audioState).toBe("running");
  expect(after.status).toContain("audio:online");
  expect(after.afterSignature).not.toBe(initial.encoderSignature);
});
