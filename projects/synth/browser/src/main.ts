import { AudioBridge, AudioBridgeOptions, AudioOutputRouteAction, AudioRequestControl, BrowserAudioWorker, PendingAudioRequest } from "./audio.js";
import type { AudioBridgeStart } from "./audio.js";
import { ActivationLease } from "./activation.js";
import { CatalogClient } from "./catalog-client.js";
import { runtimeIdentityForCatalogApp, validateBrowserRuntimeIdentity } from "./catalog.js";
import type { BrowserRuntimeIdentity, CatalogApp } from "./catalog.js";
import { SheafPatchLauncher } from "./launcher.js";
import { BrowserMidiManager, BrowserMidiWorkerRuntime } from "./midi.js";
import type { BrowserMidiStartResult } from "./midi.js";
import { materializePackage as materializeCatalogPackage } from "./package-loader.js";
import type { MaterializedPackage, MaterializedRuntimeModule } from "./package-loader.js";
import { BrowserUiBackend } from "./ui.js";
import type { RuntimeCommand, RuntimeModuleLoader, RuntimeResponse, RuntimeVersions } from "./worker.js";
import { BrowserRuntimeWorker, loadEmscriptenRuntime } from "./worker.js";

export type RuntimeClient = {
  request(command: RuntimeCommand): Promise<RuntimeResponse>;
  startAudioWorklet?(context?: AudioContext): Promise<{ started: true } | { started: false; diagnostic: string }>;
  // Audio input registration passes a live `AudioNode`, so only a client that
  // shares the launcher realm with the runtime can offer these.
  setAudioInputSource?(source: AudioNode, physicalChannels: number, statusCode: number): Promise<number>;
  clearAudioInputSource?(statusCode: number): Promise<void>;
  // Unload-safe clear: completes before it returns, so a `pagehide` handler can
  // use it. Only a client sharing the launcher realm can offer one.
  clearAudioInputSourceNow?(statusCode: number): void;
  // index: -1 nothing pending, -2 release/default the armed control,
  // otherwise a nonnegative index into the device list most recently
  // submitted through `request({ type: "audio-devices" })`. control: which
  // control (AudioRequestControl.input or .output) the index applies to.
  consumePendingAudioRequest?(): Promise<{ index: number; control: number }>;
  onStatus?(handler: (response: RuntimeResponse) => void): void;
  terminate?(): void | Promise<void>;
};

export type SynthBrowserAppOptions = {
  module?: MaterializedRuntimeModule;
  moduleUrl?: string;
  runtimeIdentity?: BrowserRuntimeIdentity;
  frameIntervalMs?: number;
  runtimeClient?: RuntimeClient;
  runtimeClientFactory?: () => RuntimeClient;
  runtimeModuleLoader?: RuntimeModuleLoader;
  runtimeVersions?: RuntimeVersions;
  activationLease?: ActivationLease;
  disposeModule?: () => void;
  audioOptions?: AudioBridgeOptions;
};

export type SynthBrowserLauncherOptions = {
  sourcesUrl?: string;
  client?: CatalogClient;
  select?: (app: CatalogApp) => Promise<void>;
  activationLeaseFactory?: () => ActivationLease;
  materializePackage?: (app: CatalogApp) => Promise<MaterializedPackage>;
  installApp?: typeof installSynthBrowserApp;
  runtimeClientFactory?: () => RuntimeClient;
  audioOptions?: AudioBridgeOptions;
  frameIntervalMs?: number;
};

const DEFAULT_RUNTIME_IDENTITY: BrowserRuntimeIdentity = Object.freeze({
  publisherId: "sheaf",
  appId: "direct-runtime",
  runtimeConfigVersion: 1,
});
const ROOT_OWNER = Symbol.for("sheaf.synth-browser.root-owner");

function directModuleMapping(moduleUrl: string): MaterializedRuntimeModule {
  const parsed = new URL(moduleUrl, location.href);
  const filename = parsed.pathname.slice(parsed.pathname.lastIndexOf("/") + 1);
  const stem = filename.endsWith(".js") ? filename.slice(0, -3) : filename;
  const wasmFilename = `${stem}.wasm`;
  return Object.freeze({
    entryUrl: parsed.href,
    locateFile: Object.freeze({
      [filename]: parsed.href,
      [wasmFilename]: new URL(wasmFilename, parsed).href,
    }),
    mainScriptUrlOrBlob: parsed.href,
  });
}

function claimRoot(root: HTMLElement): () => boolean {
  const ownedRoot = root as HTMLElement & { [key: symbol]: unknown };
  const owner = Object.freeze({});
  ownedRoot[ROOT_OWNER] = owner;
  // Symbol.for makes this supersession guard stable across fresh main.js evaluations.
  return () => ownedRoot[ROOT_OWNER] === owner;
}

export function createDirectRuntimeClient(loadModule: RuntimeModuleLoader = loadEmscriptenRuntime): RuntimeClient {
  const statusHandlers = new Set<(response: RuntimeResponse) => void>();
  const runtime = new BrowserRuntimeWorker(
    loadModule,
    undefined,
    (response) => statusHandlers.forEach((handler) => handler(response)),
  );
  let queue: Promise<void> = Promise.resolve();

  // Every runtime operation shares one queue: capture registration must not
  // interleave with a UI dispatch or a message tick already in flight.
  const enqueue = <T>(operation: () => Promise<T>): Promise<T> => {
    const result = queue.then(operation, operation);
    queue = result.then(() => {}, () => {});
    return result;
  };
  const request = (command: RuntimeCommand): Promise<RuntimeResponse> => enqueue(() => runtime.handle(command));

  return {
    request,
    startAudioWorklet: async (context) => {
      const response = await runtime.startAudioWorklet(context);
      if (response.type === "ok") return { started: true };
      return { started: false, diagnostic: response.type === "error" ? response.error : "audio-worklet-start-failed" };
    },
    setAudioInputSource: (source, physicalChannels, statusCode) =>
      enqueue(() => runtime.setAudioInputSource(source, physicalChannels, statusCode)),
    clearAudioInputSource: (statusCode) => enqueue(() => runtime.clearAudioInputSource(statusCode)),
    clearAudioInputSourceNow: (statusCode) => { runtime.clearAudioInputSourceSync(statusCode); },
    consumePendingAudioRequest: () => enqueue(() => runtime.consumePendingAudioRequest()),
    onStatus: (handler) => { statusHandlers.add(handler); },
    terminate: async () => { await request({ type: "destroy" }); },
  };
}

// Mirrors `synth::runtime_ui::Actions::kSidebarControllers` (RuntimePages.hpp):
// the action name `startUserActivation` gates the MIDI request on. Checked
// against the C++ literal by version-drift.test.mjs rather than kept in sync
// by hand.
const SidebarControllersAction = "runtime.sidebar.controllers" as const;

export class SynthBrowserApp {
  private readonly ui: BrowserUiBackend;
  private audio: AudioBridge | undefined;
  private readonly midi: BrowserMidiManager;
  private frameTimer: ReturnType<typeof setInterval> | undefined;
  // Governs only whether the audio half of startUserActivation re-runs; the
  // MIDI half's own call is gated on the dispatched action's name instead, so
  // it stays reachable regardless of this flag (see startUserActivation).
  private audioActivationStarted = false;
  private frameInFlight = false;
  private frameRequested = false;
  private stopped = false;
  private stopPromise: Promise<void> | undefined;
  private unloadInstalled = false;
  private readonly unload = () => { void this.stop(); };

  constructor(
    private readonly root: HTMLElement,
    private readonly runtime: RuntimeClient,
    private readonly options: Required<Pick<SynthBrowserAppOptions, "module" | "runtimeIdentity" | "frameIntervalMs">> &
      Pick<SynthBrowserAppOptions, "audioOptions" | "runtimeVersions" | "activationLease" | "disposeModule"> &
      Readonly<{ claimRuntimeRoot: () => void; midiAccess?: Awaited<ReturnType<ActivationLease["consume"]>>["midiAccess"] }>,
  ) {
    this.ui = new BrowserUiBackend(root, (action) => {
      void this.dispatchAction(action);
      void this.startUserActivation(action.name);
    });
    this.midi = new BrowserMidiManager(new BrowserMidiWorkerRuntime((command) => this.runtime.request(command)));
    this.runtime.onStatus?.((status) => {
      if (status.type === "file-export") this.offerDownload(status);
      else this.renderStatus(status);
    });
  }

  async start(): Promise<void> {
    this.renderStatus({ type: "status", status: "starting" });
    const module: MaterializedRuntimeModule = {
      entryUrl: this.options.module.entryUrl,
      locateFile: this.options.module.locateFile,
      mainScriptUrlOrBlob: this.options.module.mainScriptUrlOrBlob,
    };
    await this.expectOk(await this.runtime.request({ type: "load", module, versions: this.options.runtimeVersions }));
    await this.expectOk(await this.runtime.request({
      type: "create",
      documentTimeOriginMillis: performance.timeOrigin,
    }));
    await this.expectOk(await this.runtime.request({ type: "initialize", identity: this.options.runtimeIdentity }));
    const audioConfig = await this.runtime.request({ type: "audio-config" });
    if (audioConfig.type !== "audio-config") throw new Error("runtime did not return audio configuration");
    // The application's input request is discovered here -- after the module is
    // loaded, the runtime created, and the application initialized -- so capture
    // can never precede the application that asks for it.
    const audioWorker: BrowserAudioWorker = { audioInputChannels: async () => audioConfig.inputChannels };
    if (this.runtime.startAudioWorklet)
      audioWorker.startAudioWorklet = (context) => this.runtime.startAudioWorklet!(context);
    if (this.runtime.setAudioInputSource)
      audioWorker.setAudioInputSource = (source, physicalChannels, statusCode) =>
        this.runtime.setAudioInputSource!(source, physicalChannels, statusCode);
    if (this.runtime.clearAudioInputSource)
      audioWorker.clearAudioInputSource = (statusCode) => this.runtime.clearAudioInputSource!(statusCode);
    if (this.runtime.clearAudioInputSourceNow)
      audioWorker.clearAudioInputSourceNow = (statusCode) => { this.runtime.clearAudioInputSourceNow!(statusCode); };
    // Device submission is plain data, so every client offers it through the
    // generic `request`, unlike the AudioNode-carrying methods above.
    audioWorker.submitAudioDevices = async (devices) => {
      const response = await this.runtime.request({ type: "audio-devices", devices });
      if (response.type !== "ok") throw new Error("runtime rejected audio device snapshot");
    };
    // Both ride the same dispatch-action channel a real UI action does, so a
    // route failure or routing-unsupported report also repaints the Audio
    // page immediately and drains any pending request the same way every
    // other dispatch already does.
    audioWorker.reportOutputRouteFailed = (label) =>
      this.dispatchAction({ name: AudioOutputRouteAction.routeFailed, value: label });
    audioWorker.reportOutputRoutingUnsupported = () =>
      this.dispatchAction({ name: AudioOutputRouteAction.routingUnsupported, value: "" });
    this.audio = new AudioBridge(audioWorker, this.options.audioOptions);
    // Scoped to the app's own root rather than a global target: the gesture
    // only needs to reach this instrument, and a root-scoped listener needs
    // no explicit teardown when the app stops.
    installBrowserAudioActivation(this.root, this.audio);
    if (this.options.midiAccess) {
      this.audioActivationStarted = true;
      const [audio, midi] = await Promise.all([
        this.audio.startFromUserActivation(),
        this.midi.startWithAccess(this.options.midiAccess),
      ]);
      if (!audio.started) throw new Error(`audio activation failed: ${audio.diagnostic}`);
      if (midi.status !== "online") throw new Error(`MIDI activation failed: ${midi.reason ?? "unavailable"}`);
      this.renderStatus({ type: "status", status: "audio:online; midi:online" });
    } else {
      // No lease supplied its own MIDI access (the frogg3rs site's own boot
      // path), so check whether the browser already holds it: fire-and-forget,
      // never awaited here, so a slow or rejected query cannot delay or fail
      // boot.
      void this.startMidiIfAlreadyGranted();
    }
    this.options.claimRuntimeRoot();
    await this.renderFrame();
    this.frameTimer = setInterval(() => { this.requestFrame(); }, this.options.frameIntervalMs);
    addEventListener("pagehide", this.unload);
    addEventListener("beforeunload", this.unload);
    this.unloadInstalled = true;
    this.renderStatus({ type: "status", status: "running" });
  }

  stop(): Promise<void> {
    if (this.stopPromise) return this.stopPromise;
    this.stopped = true;
    if (this.unloadInstalled) {
      removeEventListener("pagehide", this.unload);
      removeEventListener("beforeunload", this.unload);
      this.unloadInstalled = false;
    }
    if (this.frameTimer !== undefined) clearInterval(this.frameTimer);
    this.frameTimer = undefined;
    this.ui.dispose();
    this.midi.stop();
    // Capture release is synchronous and happens here rather than in the async
    // tail: `pagehide` discards this promise, and an unloading page is not
    // required to run any continuation of it.
    this.audio?.releaseNow();
    this.stopPromise = this.finishStop();
    return this.stopPromise;
  }

  // `stop()` has already released capture synchronously; this awaits whatever
  // teardown could only finish asynchronously before the runtime handle is
  // destroyed, and the leased AudioContext the source belonged to is closed only
  // after that.
  private async finishStop(): Promise<void> {
    try {
      await this.audio?.stop();
    } finally {
      try {
        await this.runtime.terminate?.();
      } finally {
        this.options.activationLease?.dispose();
        this.options.disposeModule?.();
      }
    }
  }

  // At load, on the boot path with no activation lease, WHERE the Permissions
  // API already reports Web MIDI sysex access as granted, this starts MIDI
  // immediately through the same path the Controllers action uses, so a
  // controller set up on an earlier visit works with no prompt. On any other
  // reported state, a rejected query, or a browser with no Permissions API,
  // it leaves MIDI to the Controllers action. Never awaited by its caller, so
  // its outcome cannot delay or fail audio or boot. The manager's own status
  // is rendered here because this path runs outside `startUserActivation`,
  // which is the only other place that renders it.
  private async startMidiIfAlreadyGranted(): Promise<void> {
    const permissions = navigator.permissions;
    if (!permissions?.query) return;
    let status: PermissionStatus;
    try {
      status = await permissions.query({ name: "midi", sysex: true } as PermissionDescriptor);
    } catch {
      return;
    }
    if (status.state !== "granted") return;
    const midi = await this.midi.startFromUserActivation();
    this.renderStatus({ type: "status", status: `midi:${midi.status}` });
  }

  // Runs on every dispatched action. The audio half only re-runs while it has
  // not yet started -- once it has, `this.audio.startFromUserActivation()`
  // would short-circuit anyway, so skipping the call outright costs nothing
  // and keeps this function's own early return scoped to audio alone. The
  // MIDI half is gated on the action name instead of on that same flag, so a
  // Controllers dispatch still reaches `this.midi.startFromUserActivation()`
  // even after an earlier, non-Controllers dispatch already started audio.
  // Audio's own guard is real: a start it already completed returns
  // immediately. MIDI's guard covers the same case now: overlapping callers
  // of `this.midi.startFromUserActivation()`, including this dispatch racing
  // the load-time saved-grant start above, share the one in-flight
  // `navigator.requestMIDIAccess` call and its outcome instead of each
  // issuing their own.
  private async startUserActivation(actionName: string): Promise<void> {
    if (!this.audio) return;
    const [audio, midi] = await Promise.all([
      this.audioActivationStarted
        ? Promise.resolve<AudioBridgeStart>({ started: true })
        : this.audio.startFromUserActivation(),
      actionName === SidebarControllersAction
        ? this.midi.startFromUserActivation()
        : Promise.resolve<BrowserMidiStartResult>({ status: this.midi.status() }),
    ]);
    if (audio.started) this.audioActivationStarted = true;
    this.renderStatus({ type: "status", status: `audio:${audio.started ? "online" : audio.diagnostic}; midi:${midi.status}` });
  }

  private async dispatchAction(action: { name: string; value: string }): Promise<void> {
    const response = await this.runtime.request({ type: "dispatch-action", ...action });
    if (response.type === "ui-frame") this.ui.renderFrame(Uint8Array.from(response.frame).buffer);
    else if (response.type === "error") this.renderStatus({ type: "status", status: response.error });
    await this.consumePendingAudioRequest();
  }

  // The runtime arms a pending request only from a device selection, the
  // portable `Retry Input` action, or `Allow Microphone` (all resolve through
  // the same index/control path on the C++ side), so a device is requested or
  // routed exactly when the operator asked for it. Losing a stream never arms one,
  // which is what keeps a denied or ended capture from re-prompting off the
  // back of an unrelated UI action.
  private async consumePendingAudioRequest(): Promise<void> {
    if (this.stopped || !this.audio || !this.runtime.consumePendingAudioRequest) return;
    const { index, control } = await this.runtime.consumePendingAudioRequest();
    if (index === PendingAudioRequest.none) return;
    if (control === AudioRequestControl.output) {
      if (index === PendingAudioRequest.release) {
        await this.audio.releaseSelectedOutput();
        return;
      }
      await this.audio.acquireOutputDeviceAtIndex(index);
      return;
    }
    if (index === PendingAudioRequest.release) {
      await this.audio.releaseSelectedInput();
      return;
    }
    if (index === PendingAudioRequest.requestPermission) {
      await this.audio.requestInputPermission();
      return;
    }
    await this.audio.acquireInputDeviceAtIndex(index);
  }

  private requestFrame(): void {
    if (this.frameInFlight) {
      this.frameRequested = true;
      return;
    }
    this.frameInFlight = true;
    void this.renderFrame()
      .catch((error) => this.renderStatus({ type: "status", status: error instanceof Error ? error.message : "browser render failed" }))
      .finally(() => {
        this.frameInFlight = false;
        if (!this.frameRequested) return;
        this.frameRequested = false;
        this.requestFrame();
      });
  }

  private async renderFrame(): Promise<void> {
    await this.expectOk(await this.runtime.request({ type: "message-tick", timestampMicros: Math.round(performance.now() * 1000) }));
    const response = await this.runtime.request({ type: "build-ui-frame" });
    if (response.type === "ui-frame") this.ui.renderFrame(Uint8Array.from(response.frame).buffer);
    else if (response.type === "error") this.renderStatus({ type: "status", status: response.error });
  }

  private async expectOk(response: RuntimeResponse): Promise<void> {
    if (response.type === "error") throw new Error(response.error);
  }

  // Hands the app's exported file to the browser's own save flow: an
  // anchor click triggers the download, and the object URL is revoked on
  // the next macrotask -- revoking it before the click's download starts
  // can cancel the download in Chromium.
  private offerDownload({ fileName, mediaType, bytes }: { fileName: string; mediaType: string; bytes: ArrayBuffer }): void {
    const url = URL.createObjectURL(new Blob([bytes], { type: mediaType }));
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = fileName;
    anchor.rel = "noopener";
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  }

  private renderStatus(response: RuntimeResponse): void {
    const text = response.type === "page-status" ? response.status : response.type === "status" ? response.status : undefined;
    if (!text) return;
    this.root.dataset.synthStatus = text;
  }
}

export async function installSynthBrowserApp(root: HTMLElement, options: SynthBrowserAppOptions = {}): Promise<SynthBrowserApp> {
  const moduleUrl = options.moduleUrl ?? root.dataset.synthModule;
  if (!options.module && !moduleUrl) throw new Error("a materialized runtime module or explicit moduleUrl is required");
  const module = options.module ?? directModuleMapping(moduleUrl!);
  const runtimeIdentity = validateBrowserRuntimeIdentity(options.runtimeIdentity ?? DEFAULT_RUNTIME_IDENTITY);
  let app: SynthBrowserApp | undefined;
  try {
    const resources = options.activationLease ? await options.activationLease.consume() : undefined;
    const runtime = options.runtimeClient ?? options.runtimeClientFactory?.() ??
      createDirectRuntimeClient(options.runtimeModuleLoader ?? loadEmscriptenRuntime);
    app = new SynthBrowserApp(root, runtime, {
      module,
      runtimeIdentity,
      frameIntervalMs: options.frameIntervalMs ?? 1000 / 30,
      audioOptions: resources ? { ...options.audioOptions, audioContext: resources.audioContext } : options.audioOptions,
      runtimeVersions: options.runtimeVersions,
      activationLease: options.activationLease,
      disposeModule: options.disposeModule,
      midiAccess: resources?.midiAccess,
      claimRuntimeRoot: () => { claimRoot(root); },
    });
    await app.start();
    return app;
  } catch (error) {
    if (app) await app.stop();
    else {
      options.activationLease?.dispose();
      options.disposeModule?.();
    }
    throw error;
  }
}

// Acts on a bridge the caller already owns and drives, rather than
// constructing its own. Stays listening across a failed attempt and removes
// itself only once the bridge actually reports started; re-invoking
// `startFromUserActivation` before then is safe, since it short-circuits on
// its own once running.
export function installBrowserAudioActivation(target: EventTarget, bridge: AudioBridge): void {
  const onPointerDown = () => {
    void bridge.startFromUserActivation().then((result) => {
      if (result.started) target.removeEventListener("pointerdown", onPointerDown);
    });
  };
  target.addEventListener("pointerdown", onPointerDown);
}

export async function installSheafPatchLauncher(
  root: HTMLElement,
  options: SynthBrowserLauncherOptions = {},
): Promise<SheafPatchLauncher> {
  const ownsRoot = claimRoot(root);
  const sourcesUrl = options.sourcesUrl ?? root.dataset.synthCatalogSources ?? "/catalog-sources.json";
  const select = options.select ?? ((app: CatalogApp) => {
    // This callback is invoked synchronously by SheafPatchLauncher's DOM event
    // handler. Acquire both gesture-bound resources before entering async work.
    const activationLease = options.activationLeaseFactory?.() ?? ActivationLease.acquire();
    return launchCatalogApplication(root, app, activationLease, options);
  });
  const launcher = new SheafPatchLauncher(root, {
    client: options.client ?? new CatalogClient({ sourcesUrl }),
    select,
    ownsRoot,
  });
  await launcher.start();
  return launcher;
}

async function launchCatalogApplication(
  root: HTMLElement,
  app: CatalogApp,
  activationLease: ActivationLease,
  options: SynthBrowserLauncherOptions,
): Promise<void> {
  let materialized: MaterializedPackage | undefined;
  try {
    materialized = await (options.materializePackage ?? materializeCatalogPackage)(app);
    await (options.installApp ?? installSynthBrowserApp)(root, {
      module: materialized,
      runtimeVersions: {
        abiVersion: app.browser.abiVersion,
        uiProtocolVersion: app.browser.uiProtocolVersion,
        runtimeConfigVersion: app.browser.runtimeConfigVersion,
      },
      runtimeIdentity: runtimeIdentityForCatalogApp(app),
      activationLease,
      disposeModule: () => materialized!.dispose(),
      runtimeClientFactory: options.runtimeClientFactory,
      audioOptions: options.audioOptions,
      frameIntervalMs: options.frameIntervalMs,
    });
  } catch (error) {
    materialized?.dispose();
    activationLease.dispose();
    throw error;
  }
}

const root = document.querySelector<HTMLElement>("#synth-root");
if (root?.dataset.synthLauncher === "true") {
  void installSheafPatchLauncher(root).catch((error) => {
    root.dataset.synthStatus = error instanceof Error ? error.message : "catalog launcher startup failed";
  });
} else if (root?.dataset.synthAuto === "true") {
  void installSynthBrowserApp(root).catch((error) => {
    root.dataset.synthStatus = error instanceof Error ? error.message : "browser runtime startup failed";
  });
}
