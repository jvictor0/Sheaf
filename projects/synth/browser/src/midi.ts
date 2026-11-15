import type { MidiAction, MidiEndpoint, MidiOutput, MidiOutputDiagnostics } from "./protocol.js";
import type { RuntimeCommand, RuntimeResponse } from "./worker.js";

export interface BrowserMidiRuntime {
  submitEndpoints(endpoints: MidiEndpoint[]): Promise<MidiAction[]>;
  deliverMidi(controllerIx: number, bytes: number[], timestampMicros: number): Promise<void>;
  dequeueMidiOutput(): Promise<MidiOutput | undefined>;
  midiDiagnostics?(): Promise<MidiOutputDiagnostics>;
}

export class BrowserMidiWorkerRuntime implements BrowserMidiRuntime {
  constructor(private readonly request: (command: RuntimeCommand) => Promise<RuntimeResponse>) {}

  async submitEndpoints(endpoints: MidiEndpoint[]): Promise<MidiAction[]> {
    const response = await this.request({ type: "midi-endpoints", endpoints });
    if (response.type !== "midi-actions") throw new Error("runtime rejected MIDI endpoint snapshot");
    return response.actions;
  }

  async deliverMidi(controllerIx: number, bytes: number[], timestampMicros: number): Promise<void> {
    const response = await this.request({ type: "midi-input", controllerIx, bytes, timestampMicros });
    if (response.type !== "ok") throw new Error("runtime rejected MIDI bytes");
  }

  async dequeueMidiOutput(): Promise<MidiOutput | undefined> {
    const response = await this.request({ type: "drain-midi-output" });
    if (response.type !== "midi-output") throw new Error("runtime rejected MIDI output drain");
    return response.output;
  }

  async midiDiagnostics(): Promise<MidiOutputDiagnostics> {
    const response = await this.request({ type: "midi-diagnostics" });
    if (response.type !== "midi-diagnostics") throw new Error("runtime rejected MIDI diagnostics");
    return response.diagnostics;
  }
}

type MidiMessage = { data: Uint8Array; timeStamp: number };
export type MidiPort = { close?(): Promise<unknown> | void };
type MidiInputPort = MidiPort & { id: string; name?: string | null; state?: string; onmidimessage: ((message: MidiMessage) => void) | null };
type MidiOutputPort = MidiPort & {
  id: string;
  name?: string | null;
  state?: string;
  send(bytes: number[] | Uint8Array, timestamp?: number): void;
  clear?: () => void;
};
export type MidiAccess = {
  inputs: { values(): IterableIterator<MidiInputPort> };
  outputs: { values(): IterableIterator<MidiOutputPort> };
  onstatechange: ((event: unknown) => void) | null;
};

export type BrowserMidiStatus = "offline" | "requesting" | "online";
export type BrowserMidiStartResult = { status: BrowserMidiStatus; reason?: string };
export type BrowserMidiManagerOptions = {
  requestMIDIAccess?: (options: { sysex: true }) => Promise<MidiAccess>;
  setInterval?: (handler: () => void, milliseconds: number) => ReturnType<typeof setInterval>;
  clearInterval?: (handle: ReturnType<typeof setInterval>) => void;
  pollIntervalMs?: number;
  drainIntervalMs?: number;
  nowMicros?: () => number;
  timeOriginMillis?: number;
};

export type BrowserMidiDiagnostics = {
  droppedImmediateOutputCount: number;
  droppedScheduledOutputCount: number;
  bridgeLateScheduledOutputCount: number;
  lateScheduledOutputCount: number;
  sendErrorCount: number;
};

type InputBinding = { identifier: string; port: MidiInputPort; handler: (message: MidiMessage) => void };
type OutputBinding = { identifier: string; port: MidiOutputPort };

export class BrowserMidiManager {
  private access: MidiAccess | undefined;
  private statusValue: BrowserMidiStatus = "offline";
  private pollTimer: ReturnType<typeof setInterval> | undefined;
  private drainTimer: ReturnType<typeof setInterval> | undefined;
  private readonly inputs = new Map<number, InputBinding>();
  private readonly outputs = new Map<number, OutputBinding>();
  private queue: Promise<void> = Promise.resolve();
  private drainInFlight: Promise<void> | undefined;
  private drainRequested = false;
  private lateScheduledOutputCount = 0;
  private sendErrorCount = 0;
  private activationInFlight: Promise<BrowserMidiStartResult> | undefined;
  private bridgeDiagnostics: MidiOutputDiagnostics = {
    droppedImmediateOutputCount: 0,
    droppedScheduledOutputCount: 0,
    lateScheduledOutputCount: 0,
  };

  constructor(private readonly runtime: BrowserMidiRuntime, private readonly options: BrowserMidiManagerOptions = {}) {}

  status(): BrowserMidiStatus { return this.statusValue; }
  diagnostics(): BrowserMidiDiagnostics {
    return {
      droppedImmediateOutputCount: this.bridgeDiagnostics.droppedImmediateOutputCount,
      droppedScheduledOutputCount: this.bridgeDiagnostics.droppedScheduledOutputCount,
      bridgeLateScheduledOutputCount: this.bridgeDiagnostics.lateScheduledOutputCount,
      lateScheduledOutputCount: this.lateScheduledOutputCount,
      sendErrorCount: this.sendErrorCount,
    };
  }

  // Two callers can both reach this before either settles: a Controllers
  // click while an earlier Controllers click's prompt is still open, or the
  // load-time saved-grant start racing a Controllers click. `this.access`
  // guards re-entry only after a grant resolves, which is too late for
  // either caller already in flight, so a second caller shares the first
  // caller's own in-flight request and its eventual status instead of
  // issuing a second `requestMIDIAccess` call.
  async startFromUserActivation(): Promise<BrowserMidiStartResult> {
    if (this.access) return { status: this.statusValue };
    if (this.activationInFlight) return this.activationInFlight;
    this.statusValue = "requesting";
    const activation = this.requestAccessAndStart().finally(() => {
      this.activationInFlight = undefined;
    });
    this.activationInFlight = activation;
    return activation;
  }

  private async requestAccessAndStart(): Promise<BrowserMidiStartResult> {
    try {
      const access = this.options.requestMIDIAccess
        ? await this.options.requestMIDIAccess({ sysex: true })
        : await (navigator.requestMIDIAccess({ sysex: true }) as unknown as Promise<MidiAccess>);
      return await this.startWithAccess(access);
    } catch (error) {
      this.statusValue = "offline";
      return { status: this.statusValue, reason: error instanceof Error ? error.message : "Web MIDI unavailable" };
    }
  }

  async startWithAccess(access: MidiAccess): Promise<BrowserMidiStartResult> {
    if (this.access) {
      if (this.access !== access) throw new Error("MIDI manager already owns different access");
      return { status: this.statusValue };
    }
    this.statusValue = "requesting";
    this.access = access;
    try {
      access.onstatechange = () => { void this.poll(); };
      const schedule = this.options.setInterval ?? setInterval;
      this.pollTimer = schedule(() => { void this.poll(); }, this.options.pollIntervalMs ?? 500);
      this.drainTimer = schedule(() => { void this.drainOutputs(); }, this.options.drainIntervalMs ?? 16);
      await this.poll();
      this.statusValue = "online";
      return { status: this.statusValue };
    } catch (error) {
      this.stop();
      return { status: "offline", reason: error instanceof Error ? error.message : "Web MIDI unavailable" };
    }
  }

  poll(): Promise<void> {
    if (!this.access) return Promise.resolve();
    const next = this.queue.then(() => this.reconcile(), () => this.reconcile());
    this.queue = next.catch(() => {});
    return next;
  }

  async drainOutputs(): Promise<void> {
    if (!this.access) return;
    if (this.drainInFlight) {
      this.drainRequested = true;
      return this.drainInFlight;
    }
    const drain = async () => {
      do {
        this.drainRequested = false;
        await this.drainOutputsNow();
      } while (this.drainRequested && this.access);
    };
    this.drainInFlight = drain().finally(() => {
      this.drainInFlight = undefined;
    });
    return this.drainInFlight;
  }

  stop(): void {
    if (this.pollTimer !== undefined) {
      (this.options.clearInterval ?? clearInterval)(this.pollTimer);
      this.pollTimer = undefined;
    }
    if (this.drainTimer !== undefined) {
      (this.options.clearInterval ?? clearInterval)(this.drainTimer);
      this.drainTimer = undefined;
    }
    if (this.access) this.access.onstatechange = null;
    for (const controllerIx of this.inputs.keys()) this.closeInput(controllerIx);
    for (const controllerIx of [...this.outputs.keys()]) this.closeOutput(controllerIx);
    this.access = undefined;
    this.statusValue = "offline";
  }

  private async drainOutputsNow(): Promise<void> {
    // Bound each drain pass so a MIDI burst cannot monopolize the browser task;
    // the drain timer will pick up any deferred messages.
    for (let count = 0; count < 256; count++) {
      const output = await this.runtime.dequeueMidiOutput();
      if (!output) break;
      const port = this.outputs.get(output.controllerIx)?.port;
      if (!port || port.state === "disconnected") continue;
      try {
        if (output.delivery === "scheduled") {
          const nowMicros = this.options.nowMicros?.() ?? Math.round(performance.now() * 1000);
          if (nowMicros > output.dueTimeMicros) this.lateScheduledOutputCount += 1;
          // Web MIDI DOMHighResTimeStamp is in milliseconds relative to
          // performance.timeOrigin, exactly the engine epoch divided by 1000.
          port.send(output.bytes, output.dueTimeMicros / 1000);
        } else {
          port.send(output.bytes);
        }
      } catch {
        this.sendErrorCount += 1;
      }
    }
    if (this.runtime.midiDiagnostics) {
      try {
        this.bridgeDiagnostics = await this.runtime.midiDiagnostics();
      } catch {}
    }
  }

  private async reconcile(): Promise<void> {
    const access = this.access;
    if (!access) return;
    const endpoints: MidiEndpoint[] = [];
    for (const port of access.inputs.values()) {
      if (port.state !== "disconnected") endpoints.push({ identifier: port.id, name: port.name ?? "", kind: "input" });
    }
    for (const port of access.outputs.values()) {
      if (port.state !== "disconnected") endpoints.push({ identifier: port.id, name: port.name ?? "", kind: "output" });
    }
    const actions = await this.runtime.submitEndpoints(endpoints);
    for (const action of actions) this.applyAction(action, access);
    await this.drainOutputs();
  }

  private applyAction(action: MidiAction, access: MidiAccess): void {
    switch (action.type) {
      case "open-input": {
        const port = this.inputFor(access, action.identifier);
        if (!port) return;
        const existing = this.inputs.get(action.controllerIx);
        if (existing?.identifier === port.id && existing.port === port) return;
        this.closeInput(action.controllerIx);
        const handler = (message: MidiMessage) => {
          const timestampMicros = this.normalizeInputTimestamp(message.timeStamp);
          void this.runtime.deliverMidi(action.controllerIx, Array.from(message.data), timestampMicros);
        };
        port.onmidimessage = handler;
        this.inputs.set(action.controllerIx, { identifier: port.id, port, handler });
        return;
      }
      case "close-input":
        this.closeInput(action.controllerIx);
        return;
      case "open-output": {
        const port = this.outputFor(access, action.identifier);
        if (!port) return;
        const existing = this.outputs.get(action.controllerIx);
        if (existing?.identifier === port.id && existing.port === port) return;
        this.closeOutput(action.controllerIx);
        this.outputs.set(action.controllerIx, { identifier: port.id, port });
        return;
      }
      case "close-output":
        this.closeOutput(action.controllerIx);
        return;
      case "update-input-ref":
      case "update-output-ref":
      case "resync":
        return;
    }
  }

  private closeInput(controllerIx: number): void {
    const binding = this.inputs.get(controllerIx);
    if (!binding) return;
    if (binding.port.onmidimessage === binding.handler) binding.port.onmidimessage = null;
    this.inputs.delete(controllerIx);
  }

  private closeOutput(controllerIx: number): void {
    const binding = this.outputs.get(controllerIx);
    if (!binding) return;
    try {
      binding.port.clear?.();
    } catch {
      this.sendErrorCount += 1;
    }
    this.outputs.delete(controllerIx);
  }

  private inputFor(access: MidiAccess, identifier: string | undefined): MidiInputPort | undefined {
    return [...access.inputs.values()].find((port) => port.id === identifier && port.state !== "disconnected");
  }

  private outputFor(access: MidiAccess, identifier: string | undefined): MidiOutputPort | undefined {
    return [...access.outputs.values()].find((port) => port.id === identifier && port.state !== "disconnected");
  }

  private normalizeInputTimestamp(timestampMillis: number): number {
    if (!Number.isFinite(timestampMillis)) {
      return this.options.nowMicros?.() ?? Math.round(performance.now() * 1000);
    }
    const timeOriginMillis = this.options.timeOriginMillis ?? performance.timeOrigin;
    // Modern Event.timeStamp is already time-origin-relative. Some legacy
    // implementations expose absolute DOM milliseconds; normalize those by
    // subtracting the explicitly shared origin before converting to micros.
    const relativeMillis = timestampMillis >= timeOriginMillis
      ? timestampMillis - timeOriginMillis
      : timestampMillis;
    if (!Number.isFinite(relativeMillis) || relativeMillis < 0) {
      return this.options.nowMicros?.() ?? Math.round(performance.now() * 1000);
    }
    return Math.round(relativeMillis * 1000);
  }
}
