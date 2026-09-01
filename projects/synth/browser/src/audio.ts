import { MAX_BROWSER_AUDIO_INPUT_CHANNELS, browserAudioInputLimitDiagnostic } from "./audio-input-limits.js";
import type { AudioDevice } from "./protocol.js";

export type BrowserAudioWorker = {
  startAudioWorklet?: (context?: AudioContext) => Promise<AudioBridgeStart>;
  audioInputChannels?: () => Promise<number>;
  // Resolves to the module-local native handle the source is registered under,
  // so the bridge can retain the exact identity the native graph holds.
  setAudioInputSource?: (source: AudioNode, physicalChannels: number, statusCode: number) => Promise<number>;
  clearAudioInputSource?: (statusCode: number) => Promise<void>;
  // The unload-safe clear. A page being unloaded is not required to run any
  // promise continuation, so teardown needs a path that completes before it
  // returns.
  clearAudioInputSourceNow?: (statusCode: number) => void;
  // Carries a device snapshot across `synth_browser_submit_audio_devices`.
  submitAudioDevices?: (devices: AudioDevice[]) => Promise<void>;
  // Carries a rejected `setSinkId` back to the native selection state through
  // the existing dispatch-action channel (`runtime.audio.output.route_failed`
  // -- synth::runtime_ui::Actions::kAudioOutputRouteFailed, RuntimePages.hpp)
  // rather than a new export, naming the device the failed route was for.
  reportOutputRouteFailed?: (label: string) => Promise<void>;
  // Carries the one-time discovery that this browser exposes no way to route
  // to a specific output device at all, through the same dispatch-action
  // channel (`runtime.audio.output.routing_unsupported` --
  // Actions::kAudioOutputRoutingUnsupported).
  reportOutputRoutingUnsupported?: () => Promise<void>;
};

export type AudioBridgeStart = { started: true } | { started: false; diagnostic: string };

export type AudioBridgeOptions = {
  audioContext?: AudioContext;
};

// Mirrors `synth_browser::BrowserAudioInputStatus`. The numeric values are the
// ABI status codes the native runtime validates and the Audio page renders, so
// entries may be appended but never reordered.
export const AudioInputStatusCode = {
  notRequested: 0,
  requesting: 1,
  online: 2,
  permissionDenied: 3,
  apiUnavailable: 4,
  prerequisiteBlocked: 5,
  streamEnded: 6,
  channelCountUnreported: 7,
  insecureContext: 8,
  permissionsPolicyBlocked: 9,
  audioContextUnavailable: 10,
} as const;

// Mirrors `synth_browser::kNoPendingAudioRequest` / `kReleaseAudioRequest` /
// `kRequestPermissionAudioRequest` (BrowserAudioDevices.hpp), the three
// sentinel values the index half of
// `synth_browser_consume_pending_audio_request` can return alongside a
// nonnegative device-list index.
export const PendingAudioRequest = {
  none: -1,
  release: -2,
  requestPermission: -3,
} as const;

// Mirrors `synth_browser::BrowserAudioDeviceKind` (BrowserAudioDevices.hpp),
// the control half of `synth_browser_consume_pending_audio_request`'s
// result: which control (audio input or audio output) the reported index
// applies to.
export const AudioRequestControl = {
  input: 0,
  output: 1,
} as const;

// Mirrors `synth::runtime_ui::Actions::kAudioOutputRouteFailed` /
// `kAudioOutputRoutingUnsupported` (RuntimePages.hpp): the action names
// `reportOutputRouteFailed` / `reportOutputRoutingUnsupported` dispatch back
// through the existing dispatch-action channel (main.ts) rather than a new
// export.
export const AudioOutputRouteAction = {
  routeFailed: "runtime.audio.output.route_failed",
  routingUnsupported: "runtime.audio.output.routing_unsupported",
} as const;

export type AudioInputState = {
  requestedChannels: number;
  activeChannels: number;
  statusCode: number;
  // A stable kebab-case reason. Each missing prerequisite has its own published
  // status code, so this refines rather than replaces the Audio page's line.
  diagnostic: string;
  // The module-local handle the current source is registered under, or 0 when
  // nothing is registered.
  nativeHandle: number;
};

// The capture the bridge currently owns, including the exact native handle the
// registration returned. Registration itself stays in `worker.ts`, which owns
// the module-local object cache and is the only layer that hands an `AudioNode`
// to Emscripten; the bridge retains the identity that cache minted rather than
// registering a second time to learn it.
type RegisteredAudioInput = {
  node: AudioNode;
  stream: MediaStream;
  nativeHandle: number;
  physicalChannels: number;
};

// Pinned by sbw-4: an *ideal* channel count so a device that cannot supply the
// request degrades to a shortfall instead of failing, and voice processing off
// so the browser does not silently downmix a multichannel interface to mono.
// `deviceId` names the operator's selection (browser's `MediaDeviceInfo`
// deviceId); omitted, the request opens the system default, matching prior
// behaviour.
function captureConstraints(requestedChannels: number, deviceId?: string): MediaStreamConstraints {
  return {
    audio: {
      channelCount: { ideal: requestedChannels },
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
      ...(deviceId ? { deviceId: { exact: deviceId } } : {}),
    },
  };
}

function classifyCaptureFailure(error: unknown): { statusCode: number; diagnostic: string } {
  const name = error instanceof Error ? error.name : "";
  if (name === "NotAllowedError" || name === "PermissionDeniedError")
    return { statusCode: AudioInputStatusCode.permissionDenied, diagnostic: "permission-denied" };
  if (name === "SecurityError")
    return { statusCode: AudioInputStatusCode.permissionsPolicyBlocked, diagnostic: "capture-blocked" };
  return {
    statusCode: AudioInputStatusCode.apiUnavailable,
    diagnostic: `capture-failed:${name || "unknown"}`,
  };
}

function positiveChannelCount(value: unknown): number | undefined {
  return typeof value === "number" && Number.isInteger(value) && value > 0 ? value : undefined;
}

export class AudioBridge {
  private started = false;
  private stopped = false;
  private requestedInputChannels = 0;
  // Set when enumeration threw. The first submission runs BEFORE the requested
  // channel count is known (start() fires it and only then awaits discovery),
  // and releaseInput reports nothing while that count is still zero -- so a
  // failure at startup, the moment it matters most, would otherwise be
  // swallowed by the very guard that keeps a zero-input app quiet.
  private pendingEnumerationDiagnostic = "";
  private input: RegisteredAudioInput | undefined;
  private inputStatusCode: number = AudioInputStatusCode.notRequested;
  private inputDiagnostic = "";
  // Capture transitions are serialized. `track.onended` can fire while a retry
  // or a teardown is already in flight, and two overlapping acquisitions would
  // register two sources against one worklet input bus.
  private inputWork: Promise<void> = Promise.resolve();
  // The browser deviceId of the operator's current input selection, or
  // undefined for the system default. Read by `captureConstraints` on every
  // (re)acquisition, including `retryInput`.
  private selectedDeviceId: string | undefined;
  // The exact array most recently handed to `submitAudioDevices`, kept so an
  // index reported back through the pending-request poll (main.ts) can be
  // resolved to a deviceId without a second enumeration.
  private submittedDevices: AudioDevice[] = [];
  private deviceChangeHandler: (() => void) | undefined;

  constructor(private readonly worker: BrowserAudioWorker, private readonly options: AudioBridgeOptions = {}) {}

  async startFromUserActivation(): Promise<AudioBridgeStart> {
    if (!globalThis.crossOriginIsolated || typeof SharedArrayBuffer === "undefined")
      return { started: false, diagnostic: "cross-origin-isolation-required" };
    if (this.stopped) return { started: false, diagnostic: "audio-bridge-stopped" };
    if (this.started) return { started: true };
    if (!this.worker.startAudioWorklet)
      return { started: false, diagnostic: "native-audio-worklet-required" };
    // `enumerateDevices` neither prompts nor requires permission (unlike
    // `getUserMedia` below), so it runs here unconditionally -- an app with
    // no input request still shows an output device combo, which needs this
    // submission too -- and again on every `devicechange`.
    void this.submitAudioDevices();
    this.installDeviceChangeListener();
    // Discovery first: a zero-input application must reach native startup
    // without `getUserMedia` ever being touched (sbw-4).
    this.requestedInputChannels = await this.discoverRequestedInputChannels();
    // Now that the count is known, a failure recorded by the submission above
    // can actually be published. Nothing is retried here -- only reported.
    if (this.pendingEnumerationDiagnostic && this.requestedInputChannels > 0) {
      await this.releaseInput(AudioInputStatusCode.apiUnavailable, this.pendingEnumerationDiagnostic);
      this.pendingEnumerationDiagnostic = "";
    }
    if (this.requestedInputChannels > MAX_BROWSER_AUDIO_INPUT_CHANNELS) {
      this.inputStatusCode = AudioInputStatusCode.apiUnavailable;
      this.inputDiagnostic = browserAudioInputLimitDiagnostic(this.requestedInputChannels);
      return { started: false, diagnostic: this.inputDiagnostic };
    }
    // Capture is armed only by the operator selecting a device -- the
    // pending-request poll in main.ts -- never automatically here, so
    // opening the page raises no permission prompt.
    const result = await this.worker.startAudioWorklet(this.options.audioContext);
    if (this.stopped)
      return { started: false, diagnostic: "audio-bridge-stopped" };
    this.started = result.started;
    if (!result.started) {
      // Releases whatever this attempt opened, but does not permanently latch
      // `stopped`: a native start can fail for a reason a later real gesture
      // can still overcome (an autoplay-refused resume that a later trusted
      // gesture will honour), so the bridge must stay retryable. Only a
      // deliberate `releaseNow()`/`stop()` is terminal.
      this.releaseResourcesNow();
      return result;
    }
    // Capture can no longer be in flight this early on the guarded (Launch
    // click) startup path: no UI exists yet for the operator to select a
    // device from, so `this.input` is unset here. On the no-activation-lease
    // path, the first dispatched action can arm `startFromUserActivation`
    // and its own pending-request poll concurrently (main.ts), so a narrow
    // race still exists; this check is kept, unmodified, as the same
    // defensive reconcile it always was for that case.
    if (this.input)
      void this.serializeInputWork(() => this.reconcileInputAfterWorkletStart());
    return result;
  }

  // The user-initiated path back from an offline capture (sbw-4): it reacquires
  // into the existing AudioContext, worklet node, engine, and application, and
  // is never called from the realtime callback or on a timer.
  async retryInput(): Promise<void> {
    if (this.requestedInputChannels > MAX_BROWSER_AUDIO_INPUT_CHANNELS) return;
    await this.serializeInputWork(() => (this.stopped ? Promise.resolve() : this.acquireInput()));
  }

  // The device-selection path back from offline, or the operator's first
  // selection: called from the pending-request poll (main.ts) when the C++
  // runtime reports a nonnegative index. `index` addresses the array this
  // bridge most recently handed to `submitAudioDevices`; enumeration can
  // change underneath between that submission and this call (a device
  // unplugged, a `devicechange` resubmission landing in between), so an
  // index no longer in range is treated as stale and dropped rather than
  // acquiring whatever now sits at that position -- the next submission
  // resyncs the native side onto the current list.
  async acquireInputDeviceAtIndex(index: number): Promise<void> {
    if (this.stopped) return;
    const device = this.submittedDevices[index];
    if (!device) return;
    if (this.requestedInputChannels > MAX_BROWSER_AUDIO_INPUT_CHANNELS) return;
    this.selectedDeviceId = device.deviceId;
    await this.serializeInputWork(() => (this.stopped ? Promise.resolve() : this.acquireInput()));
  }

  // The operator selected No Input at the reported pending request: releases
  // whatever capture is held and clears the pinned selection, so a later
  // Retry falls back to the system default rather than repinning a device
  // the operator just turned off.
  async releaseSelectedInput(): Promise<void> {
    this.selectedDeviceId = undefined;
    await this.serializeInputWork(() =>
      this.stopped ? Promise.resolve() : this.releaseInput(AudioInputStatusCode.notRequested, ""));
  }

  // The output-selection path back from the pending request: called from the
  // poll (main.ts) when the C++ runtime reports a nonnegative index for the
  // output control. `index` addresses the same array `acquireInputDeviceAtIndex`
  // does -- the one this bridge most recently handed to `submitAudioDevices`
  // -- so a stale index (the device unplugged between submission and this
  // call) is dropped the same way: the next submission resyncs the native
  // side onto the current list.
  async acquireOutputDeviceAtIndex(index: number): Promise<void> {
    if (this.stopped) return;
    const device = this.submittedDevices[index];
    if (!device) return;
    await this.routeOutput(device);
  }

  // The operator selected System Default at the reported pending request:
  // reverts output to the platform default sink. Unlike input there is no
  // capture to release -- output keeps playing through whatever sink is
  // already current -- so this only has work to do when routing is actually
  // supported.
  async releaseSelectedOutput(): Promise<void> {
    if (this.stopped) return;
    await this.routeOutput(undefined);
  }

  // Routes output to `device` (the platform default, for `undefined`) via the
  // Web Audio Output Devices API on the context this bridge already holds.
  // `submitAudioDevices` already keeps an unroutable device off the list an
  // operator can select from, so reaching here with a real `device` should
  // mean routing is supported; the same check is repeated regardless, since a
  // caller must never assume a capability instead of testing for it.
  //
  // `setSinkId` can reject -- the named device can vanish between
  // enumeration and this call, or the browser can otherwise refuse it -- and
  // this bridge keeps no "currently routed device" state of its own, so a
  // rejection simply leaves whatever routing already existed: nothing here
  // ever claims a route before the promise it came from has resolved, so
  // there is nothing for THIS bridge to roll back. The eagerly-claimed
  // selection the native side recorded on dispatch is a separate, falsifiable
  // claim, though: `reportOutputRouteFailed` carries the rejection back so
  // that claim can revert instead of continuing to name a device nothing is
  // routed to. Only a named `device` can leave a wrong claim standing --
  // releasing to the platform default already shows System Default -- so the
  // report only fires for that case.
  private async routeOutput(device: AudioDevice | undefined): Promise<void> {
    if (!this.outputRoutingSupported()) return;
    const context = this.options.audioContext as AudioContext & { setSinkId(sinkId: string): Promise<void> };
    try {
      await context.setSinkId(device?.deviceId ?? "");
    } catch {
      if (!device) return;
      try {
        await this.worker.reportOutputRouteFailed?.(device.label);
      } catch {
        // A destroyed or unavailable runtime cannot record this either; the
        // stale claim it leaves standing is no worse than before this report
        // existed, and a later selection or report still corrects it.
      }
    }
  }

  // Runtime feature detection only, never user-agent sniffing: true only when
  // this bridge holds a context and that context exposes `setSinkId` as a
  // function. Checked at the point of use rather than cached, so it can
  // never go stale relative to when the context was created; shared by
  // `submitAudioDevices` (deciding what to offer) and `routeOutput`
  // (deciding whether to call it).
  private outputRoutingSupported(): boolean {
    const context = this.options.audioContext as (AudioContext & { setSinkId?: unknown }) | undefined;
    return !!context && typeof context.setSinkId === "function";
  }

  // Unload-safe teardown: everything releasable without yielding happens before
  // this returns, because a page being unloaded or frozen into the back/forward
  // cache is not required to run any promise continuation. Idempotent, and an
  // acquisition still in flight observes `stopped` and releases what it holds.
  // Permanent: once this returns, no later gesture can start this bridge
  // again. A start attempt that merely failed to start uses
  // `releaseResourcesNow` below instead, so it stays retryable.
  releaseNow(): void {
    if (this.stopped) return;
    this.stopped = true;
    this.releaseResourcesNow();
  }

  async stop(): Promise<void> {
    this.releaseNow();
    await this.whenInputSettled();
  }

  // The resource release a deliberate stop and a start attempt that merely
  // failed to start both need -- release any input, drop the device-change
  // listener -- without the permanent `stopped` latch, which only
  // `releaseNow()` sets.
  private releaseResourcesNow(): void {
    this.started = false;
    this.removeDeviceChangeListener();
    this.releaseInputNow(AudioInputStatusCode.notRequested, "");
  }

  inputState(): AudioInputState {
    return {
      requestedChannels: this.requestedInputChannels,
      activeChannels: this.input?.physicalChannels ?? 0,
      statusCode: this.inputStatusCode,
      diagnostic: this.inputDiagnostic,
      nativeHandle: this.input?.nativeHandle ?? 0,
    };
  }

  // Resolves once every capture transition queued so far has finished. Capture
  // loss arrives asynchronously through `track.onended`, so hosts and tests need
  // a way to observe the settled state instead of guessing at timers.
  whenInputSettled(): Promise<void> {
    return this.inputWork;
  }

  private async discoverRequestedInputChannels(): Promise<number> {
    if (!this.worker.audioInputChannels) return 0;
    const requested = await this.worker.audioInputChannels();
    return positiveChannelCount(requested) ?? 0;
  }

  // Enumerates and resubmits the full device list -- never a cached list,
  // and never filtered on account of permission or labeling: matching
  // whatever `navigator.mediaDevices.enumerateDevices` currently reports,
  // including an unpermitted page's placeholder entries (both `deviceId` and
  // `label` empty), is the native side's job to decide presentable
  // (BuildBrowserAudioSnapshot), not this bridge's.
  //
  // The one narrow exception is routability itself, which native cannot
  // decide because it has no way to ask the browser: an output device this
  // bridge cannot route to at all (`outputRoutingSupported()` false) is
  // dropped here rather than submitted and left to be silently unroutable
  // if selected -- the same reasoning that already keeps a zero-input
  // application from ever calling `getUserMedia`, generalized to a control
  // this bridge can enumerate but not act on. Dropping the devices is not
  // reporting the absence, though, so an unsupported browser also reports
  // that fact explicitly (`reportOutputRoutingUnsupported`) rather than
  // leaving the operator to infer it from a combo that only ever offers
  // System Default.
  private async submitAudioDevices(): Promise<void> {
    if (this.stopped) return;
    const submit = this.worker.submitAudioDevices;
    if (!submit) return;
    const mediaDevices = navigator.mediaDevices;
    if (typeof mediaDevices?.enumerateDevices !== "function") return;
    let infos: MediaDeviceInfo[];
    try {
      infos = await mediaDevices.enumerateDevices();
    } catch {
      // The degradation is deliberate and stays: a browser that refuses to
      // enumerate leaves the instrument running rather than failing the page.
      // What changes is that the reason is no longer indistinguishable from a
      // machine with no devices -- the two look identical in the combo and
      // call for different responses, and the silence once turned a
      // `ReferenceError` in a fixture into a long hunt for a missing device.
      //
      // Reported only while nothing is captured. A live capture's own status
      // is the more useful thing for the status line to be saying, and
      // overwriting it here would tear down a working stream to report a
      // failure that did not affect it.
      if (!this.input) {
        this.pendingEnumerationDiagnostic = "device-enumeration-failed";
        await this.releaseInput(AudioInputStatusCode.apiUnavailable, "device-enumeration-failed");
      }
      return;
    }
    if (this.stopped) return;
    const outputRoutable = this.outputRoutingSupported();
    if (!outputRoutable) {
      try {
        await this.worker.reportOutputRoutingUnsupported?.();
      } catch {
        // A destroyed or unavailable runtime cannot record this either; the
        // next `devicechange` (or explicit call) retries, same as the device
        // snapshot itself below.
      }
    }
    const devices: AudioDevice[] = infos
      .filter((info) => info.kind === "audioinput" || (info.kind === "audiooutput" && outputRoutable))
      .map((info) => ({
        deviceId: info.deviceId,
        label: info.label,
        kind: info.kind === "audiooutput" ? "output" : "input",
      }));
    this.submittedDevices = devices;
    try {
      await submit(devices);
    } catch {
      // A destroyed or unavailable runtime cannot record this snapshot; the
      // next `devicechange` (or explicit call) retries.
    }
  }

  private installDeviceChangeListener(): void {
    if (this.deviceChangeHandler) return;
    const mediaDevices = navigator.mediaDevices;
    if (typeof mediaDevices?.addEventListener !== "function") return;
    this.deviceChangeHandler = () => { void this.submitAudioDevices(); };
    mediaDevices.addEventListener("devicechange", this.deviceChangeHandler);
  }

  private removeDeviceChangeListener(): void {
    if (!this.deviceChangeHandler) return;
    navigator.mediaDevices?.removeEventListener?.("devicechange", this.deviceChangeHandler);
    this.deviceChangeHandler = undefined;
  }

  private serializeInputWork(work: () => Promise<void>): Promise<void> {
    const next = this.inputWork.then(work, work);
    this.inputWork = next.then(() => {}, () => {});
    return this.inputWork;
  }

  // Asks the browser for capture permission without selecting anything. A page
  // holding no permission enumerates its inputs with empty labels, so it can
  // offer no device to acquire and retry re-requests the empty selection; this
  // is the only route from that state to a labelled list.
  //
  // `getUserMedia` is the only call that prompts, so permission cannot be
  // earned without opening a device for the interval the browser requires.
  // That interval ends here: the stream is never handed to `this.input`, never
  // wired to a node, and every track is stopped before this returns. No
  // `deviceId` is requested, so nothing is being selected -- which is what
  // keeps this distinct from a capture nobody chose.
  async requestInputPermission(): Promise<void> {
    await this.serializeInputWork(() =>
      (this.stopped ? Promise.resolve() : this.requestInputPermissionNow()));
  }

  private async requestInputPermissionNow(): Promise<void> {
    if (this.stopped) return;
    if (this.requestedInputChannels <= 0) return;
    const mediaDevices = navigator.mediaDevices;
    if (typeof mediaDevices?.getUserMedia !== "function") {
      await this.releaseInput(AudioInputStatusCode.apiUnavailable, "media-devices-unavailable");
      return;
    }
    let stream: MediaStream;
    try {
      stream = await mediaDevices.getUserMedia({ audio: true });
    } catch (error) {
      if (this.stopped) return;
      const failure = classifyCaptureFailure(error);
      await this.releaseInput(failure.statusCode, failure.diagnostic);
      return;
    }
    // Before any awaited work below, so a grant never leaves a device open on
    // an early return or a rejected resubmission.
    for (const track of stream.getTracks()) track.stop();
    if (this.stopped) return;
    // Labels populate only once permission is held, so the list is rebuilt
    // from a fresh enumeration. The selection is not touched: earning a label
    // is not choosing a device, and `inputDeviceName` stays empty.
    await this.submitAudioDevices();
  }

  private async acquireInput(): Promise<void> {
    if (this.stopped) return;
    if (this.requestedInputChannels <= 0) return;
    if (this.requestedInputChannels > MAX_BROWSER_AUDIO_INPUT_CHANNELS) {
      // Native startup rejects this configuration outright; prompting for a
      // microphone the application can never be started with would be gratuitous.
      await this.releaseInput(
        AudioInputStatusCode.apiUnavailable,
        browserAudioInputLimitDiagnostic(this.requestedInputChannels),
      );
      return;
    }
    const registerSource = this.worker.setAudioInputSource?.bind(this.worker);
    if (!registerSource) {
      await this.releaseInput(AudioInputStatusCode.apiUnavailable, "input-registration-unavailable");
      return;
    }
    const context = this.options.audioContext;
    if (!context) {
      await this.releaseInput(AudioInputStatusCode.audioContextUnavailable, "audio-context-unavailable");
      return;
    }
    if (!globalThis.isSecureContext) {
      await this.releaseInput(AudioInputStatusCode.insecureContext, "insecure-context");
      return;
    }
    const mediaDevices = navigator.mediaDevices;
    if (typeof mediaDevices?.getUserMedia !== "function") {
      await this.releaseInput(AudioInputStatusCode.apiUnavailable, "media-devices-unavailable");
      return;
    }

    await this.releaseInput(AudioInputStatusCode.requesting, "");
    let stream: MediaStream;
    try {
      stream = await mediaDevices.getUserMedia(captureConstraints(this.requestedInputChannels, this.selectedDeviceId));
    } catch (error) {
      if (this.stopped) return;
      const failure = classifyCaptureFailure(error);
      await this.releaseInput(failure.statusCode, failure.diagnostic);
      return;
    }
    // Provisional ownership starts here: the stream is this bridge's to release
    // on every path below that does not hand it to `this.input`, and the end
    // handler is installed before any awaited work so a track that ends while
    // registration is in flight is observed rather than left falsely online.
    const track = stream.getAudioTracks()[0];
    let endedDuringAcquisition = false;
    if (track) track.onended = () => { endedDuringAcquisition = true; };
    let source: MediaStreamAudioSourceNode | undefined;
    let acquired: RegisteredAudioInput | undefined;
    try {
      // A stop or a second retry can win the race against the permission prompt.
      if (this.stopped) return;
      if (!track) {
        await this.releaseInput(AudioInputStatusCode.streamEnded, "no-audio-track");
        return;
      }
      source = context.createMediaStreamSource(stream);
      const reported = positiveChannelCount(track.getSettings?.().channelCount);
      const statusCode = reported === undefined
        ? AudioInputStatusCode.channelCountUnreported
        : AudioInputStatusCode.online;
      // D5's fallback chain: the track's own setting, else the source node's count,
      // else one channel. The result is clamped to the request so a device with
      // more channels than the application addresses never inflates the active count.
      const derived = reported ?? positiveChannelCount(source.channelCount) ?? 1;
      const physicalChannels = Math.min(derived, this.requestedInputChannels);
      const nativeHandle = await registerSource(source, physicalChannels, statusCode);
      // A stop -- including an unload -- can land while this registration is in
      // flight. `releaseNow()` cleared whatever was published when it ran, not
      // the claim the call above has just published, so that late claim has to
      // be taken back down here or the native side keeps a handle and a count
      // for a source nobody owns any more.
      if (this.stopped) {
        await this.releaseInput(AudioInputStatusCode.notRequested, "");
        return;
      }
      if (endedDuringAcquisition || track.readyState === "ended") {
        // The registration landed on a source whose track is already gone, so the
        // native claim has to come back down before the source does.
        await this.releaseInput(AudioInputStatusCode.streamEnded, "stream-ended");
        return;
      }
      acquired = { node: source, stream, nativeHandle, physicalChannels };
      this.input = acquired;
      this.inputStatusCode = statusCode;
      this.inputDiagnostic = reported === undefined ? "channel-count-unreported" : "";
      track.onended = () => {
        void this.serializeInputWork(() => this.handleStreamEnded(track));
      };
    } catch {
      // Source construction or registration threw. The native side may or may
      // not have taken the source, so drop the claim rather than assume which.
      await this.releaseInput(AudioInputStatusCode.apiUnavailable, "input-registration-failed");
    } finally {
      if (!acquired) {
        source?.disconnect();
        stopStream(stream);
      }
    }
  }

  private async reconcileInputAfterWorkletStart(): Promise<void> {
    if (this.stopped || !this.input) return;
    const input = this.input;
    const registerSource = this.worker.setAudioInputSource?.bind(this.worker);
    if (!registerSource) {
      await this.releaseInput(AudioInputStatusCode.apiUnavailable, "input-registration-unavailable");
      return;
    }
    try {
      const nativeHandle = await registerSource(input.node, input.physicalChannels, this.inputStatusCode);
      if (this.stopped) {
        await this.releaseInput(AudioInputStatusCode.notRequested, "");
        return;
      }
      if (this.input === input)
        input.nativeHandle = nativeHandle;
    } catch {
      if (!this.stopped && this.input === input)
        await this.releaseInput(AudioInputStatusCode.apiUnavailable, "input-registration-failed");
    }
  }

  private async handleStreamEnded(track: MediaStreamTrack): Promise<void> {
    // A handler left over from a stream this bridge has already replaced or
    // released must not knock the current capture offline.
    if (!this.input || !this.input.stream.getTracks().includes(track)) return;
    await this.releaseInput(AudioInputStatusCode.streamEnded, "stream-ended");
  }

  // Clears the native active count first, so no audio callback can read a source
  // that is about to be disconnected, then releases the media resources and
  // leaves the output callback and AudioContext untouched. Used inside the
  // capture chain, where awaiting the clear keeps it strictly ordered ahead of
  // the disconnect.
  private async releaseInput(statusCode: number, diagnostic: string): Promise<void> {
    if (this.requestedInputChannels <= 0) return;
    this.inputStatusCode = statusCode;
    this.inputDiagnostic = diagnostic;
    try {
      await this.worker.clearAudioInputSource?.(statusCode);
    } catch {
      // A destroyed or unavailable runtime cannot hold the media resources open.
    }
    this.releaseCaptureResources();
  }

  // The same release with the same clear-then-disconnect-then-stop order, done
  // without yielding. A host that has no synchronous clear still gets the media
  // released here and the native clear queued behind it -- late, but not lost.
  private releaseInputNow(statusCode: number, diagnostic: string): void {
    if (this.requestedInputChannels <= 0) return;
    this.inputStatusCode = statusCode;
    this.inputDiagnostic = diagnostic;
    let cleared = false;
    try {
      if (this.worker.clearAudioInputSourceNow) {
        this.worker.clearAudioInputSourceNow(statusCode);
        cleared = true;
      }
    } catch {
      // A destroyed or unavailable runtime cannot hold the media resources open.
      cleared = true;
    }
    if (!cleared) {
      void this.serializeInputWork(async () => {
        try {
          await this.worker.clearAudioInputSource?.(statusCode);
        } catch {
          // Same: teardown never fails on account of an absent runtime.
        }
      });
    }
    this.releaseCaptureResources();
  }

  private releaseCaptureResources(): void {
    const input = this.input;
    if (!input) return;
    this.input = undefined;
    input.node.disconnect();
    stopStream(input.stream);
  }
}

function stopStream(stream: MediaStream): void {
  for (const track of stream.getTracks()) {
    track.onended = null;
    track.stop();
  }
}
