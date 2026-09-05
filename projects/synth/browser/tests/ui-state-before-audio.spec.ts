import { expect, test } from "@playwright/test";
import { decodeCommandBuffer } from "../src/protocol.js";
import { makeCommandBuffer, NodeKind } from "./fixtures/command-buffer.js";

// openspec/changes/ui-state-before-audio -- design.md's Testing section
// requires: "Browser-level: freshly installed app, no activation, no action
// -> first frames carry encoder draw commands/labels." The implementation
// (projects/synth/include/synth/Engine.hpp) was left without this test: the
// implementer traced that browser/src/main.ts's renderFrame() (:355-356)
// already calls "message-tick" before "build-ui-frame" unconditionally,
// including for the very first frame (main.ts:355-356, before the frame timer
// starts and before any user activation), so no browser-side CODE change was
// needed to observe the fix. That trace is correct, but it is a description
// of today's main.ts, not a test: it proves the bug is fixed, not that a
// later change can't silently unfix it. The concrete regression this file
// guards against is a future main.ts edit that reorders those two calls, or
// that gates "message-tick" behind user activation "for performance" --
// either would silently reintroduce blank encoder controls on first load
// with nothing else failing.
//
// Both tests below drive the REAL installSynthBrowserApp()/SynthBrowserApp
// bootstrap exported by main.ts -- not a hand-rolled substitute -- so an
// edit to main.ts's own call order or an activation gate added there fails
// these tests directly, which a test that merely replays the two request
// types by hand (independent of what main.ts actually does) could not
// catch.

test("freshly installed app renders populated encoder state on the first frame, with no audio activation and no dispatched action", async ({ page }) => {
  // Real miniapp WASM (the same fixture audio-flow.spec.ts's "real miniapp
  // WASM" tests use), loaded through main.ts's own createDirectRuntimeClient
  // -- a thin call-recording proxy sits in front of it so this one test can
  // assert both the request ORDER and the resulting frame CONTENT without
  // altering the real runtime's behavior in any way.
  await page.goto("http://127.0.0.1:4173/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');

  const result = await page.evaluate(async () => {
    const { installSynthBrowserApp, createDirectRuntimeClient } =
      await (new Function("return import('/dist/src/main.js')")() as Promise<any>);

    const calls: string[] = [];
    let firstFrame: number[] | undefined;
    const real = createDirectRuntimeClient();
    const runtimeClient = {
      request: async (command: { type: string }) => {
        calls.push(command.type);
        const response = await real.request(command as any);
        if (command.type === "build-ui-frame" && (response as any).type === "ui-frame" && firstFrame === undefined) {
          firstFrame = (response as any).frame;
        }
        return response;
      },
      terminate: () => real.terminate?.(),
      // startAudioWorklet intentionally NOT forwarded: this run asserts "no
      // activation", so nothing here is entitled to start audio. If a future
      // regression tried anyway, this makes it throw instead of silently
      // succeeding and invalidating the "no activation" premise.
    };

    const moduleUrl = new URL("/dist/wasm/apps/miniapp/miniapp.js", location.href).href;
    const app = await installSynthBrowserApp(document.querySelector("#synth-root")!, {
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
      // Long enough that the periodic frame timer cannot fire again before
      // this test reads its result: only the FIRST renderFrame() (main.ts
      // :222, before the timer is even armed) is under test here.
      frameIntervalMs: 24 * 60 * 60 * 1000,
      // No activationLease and no midiAccess: audio/MIDI activation never
      // runs (main.ts only starts them inside `if (this.options.midiAccess)`).
    });
    await app.stop();

    return { calls, firstFrame };
  });

  // No activation, no dispatched action: only the install/bootstrap sequence
  // ran (positive control for "no action": dispatch-action never appears).
  expect(result.calls).not.toContain("dispatch-action");

  const tickIndex = result.calls.indexOf("message-tick");
  const frameIndex = result.calls.indexOf("build-ui-frame");
  expect(tickIndex, `expected a message-tick call; saw: ${JSON.stringify(result.calls)}`).toBeGreaterThanOrEqual(0);
  expect(frameIndex, `expected a build-ui-frame call; saw: ${JSON.stringify(result.calls)}`).toBeGreaterThanOrEqual(0);
  expect(frameIndex, `message-tick must precede build-ui-frame; saw: ${JSON.stringify(result.calls)}`).toBeGreaterThan(tickIndex);

  expect(result.firstFrame, "no build-ui-frame response was captured").toBeDefined();
  const decoded = decodeCommandBuffer(Uint8Array.from(result.firstFrame!).buffer);
  const encoder = decoded.nodes.find((node) => node.id.startsWith("miniapp.encoder.") && node.pointerDragAction);
  expect(encoder, `no draggable encoder in the first built frame; nodes: ${decoded.nodes.map((node) => node.id).join(", ")}`).toBeDefined();
  const draws = decoded.drawCommands.slice(encoder!.drawStart, encoder!.drawStart + encoder!.drawCount);
  expect(draws.length, "encoder carried no draw commands on the first frame").toBeGreaterThan(0);
  expect(draws.some((draw) => typeof draw.text === "string" && draw.text.length > 0),
    "encoder draw commands carried no label/value text on the first frame").toBe(true);
});

// A second, cheap complement to the test above: it needs no compiled WASM
// (a fake runtime client stands in, exactly as runtime-core.spec.ts's
// bootstrap tests already do), so it stays fast and still exercises the
// real main.ts request ORDER specifically -- defense in depth against the
// same regression class when a full rebuild isn't in the loop.
test("first frame issues message-tick before build-ui-frame with no audio activation and no dispatched action (fake runtime)", async ({ page }) => {
  await page.goto("http://127.0.0.1:4173/dist/src/main.js");
  await page.setContent('<main id="synth-root"></main>');
  const frame = makeCommandBuffer([
    { id: "root", kind: NodeKind.Root, bounds: [0, 0, 100, 40], children: [] },
  ]);

  const calls = await page.evaluate(async (bytes) => {
    const { installSynthBrowserApp } = await (new Function("return import('/dist/src/main.js')")() as Promise<any>);
    const calls: string[] = [];
    const runtimeClient = {
      request: async (command: { type: string }) => {
        calls.push(command.type);
        if (command.type === "audio-config") return { type: "audio-config", channels: 2, inputChannels: 0 };
        if (command.type === "build-ui-frame") return { type: "ui-frame", frame: bytes };
        return { type: "ok" };
      },
      terminate() {},
      // No startAudioWorklet on this fake: activation must never be reached.
    };
    const app = await installSynthBrowserApp(document.querySelector("#synth-root")!, {
      module: { entryUrl: "blob:ui-state-before-audio", locateFile: {}, mainScriptUrlOrBlob: "blob:ui-state-before-audio" },
      runtimeClient,
      frameIntervalMs: 24 * 60 * 60 * 1000,
    });
    await app.stop();
    return calls;
  }, Array.from(new Uint8Array(frame)));

  expect(calls).not.toContain("dispatch-action");
  const tickIndex = calls.indexOf("message-tick");
  const frameIndex = calls.indexOf("build-ui-frame");
  expect(tickIndex, `expected a message-tick call; saw: ${JSON.stringify(calls)}`).toBeGreaterThanOrEqual(0);
  expect(frameIndex, `expected a build-ui-frame call; saw: ${JSON.stringify(calls)}`).toBeGreaterThanOrEqual(0);
  expect(frameIndex, `message-tick must precede build-ui-frame; saw: ${JSON.stringify(calls)}`).toBeGreaterThan(tickIndex);
});
