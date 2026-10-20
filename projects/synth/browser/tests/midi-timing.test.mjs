import assert from "node:assert/strict";
import test from "node:test";

import { BrowserMidiManager } from "../src/midi.js";
import { BrowserRuntimeWorker, emscriptenRuntimeFacade } from "../src/worker.js";

function intervalOptions(overrides = {}) {
  return {
    setInterval: () => 1,
    clearInterval: () => {},
    ...overrides,
  };
}

test("normalizes relative, legacy absolute, and invalid Web MIDI timestamps", async () => {
  const delivered = [];
  const input = {
    id: "clock-in",
    name: "Clock Input",
    state: "connected",
    onmidimessage: null,
  };
  const access = {
    inputs: new Map([[input.id, input]]),
    outputs: new Map(),
    onstatechange: null,
  };
  const manager = new BrowserMidiManager({
    submitEndpoints: async () => [{
      type: "open-input",
      controllerIx: 4,
      identifier: input.id,
      name: input.name,
    }],
    deliverMidi: async (controllerIx, bytes, timestampMicros) => {
      delivered.push({ controllerIx, bytes, timestampMicros });
    },
    dequeueMidiOutput: async () => undefined,
  }, intervalOptions({
    requestMIDIAccess: async () => access,
    timeOriginMillis: 1_700_000_000_000,
    nowMicros: () => 19_000,
  }));

  assert.deepEqual(await manager.startFromUserActivation(), { status: "online" });
  input.onmidimessage({ data: Uint8Array.of(0xFA), timeStamp: 17.25 });
  input.onmidimessage({ data: Uint8Array.of(0xF8), timeStamp: 1_700_000_000_018.5 });
  input.onmidimessage({ data: Uint8Array.of(0xFC), timeStamp: Number.NaN });
  await Promise.resolve();

  assert.deepEqual(delivered, [
    { controllerIx: 4, bytes: [0xFA], timestampMicros: 17_250 },
    { controllerIx: 4, bytes: [0xF8], timestampMicros: 18_500 },
    { controllerIx: 4, bytes: [0xFC], timestampMicros: 19_000 },
  ]);
  manager.stop();
});

test("uses stored deadlines for scheduled output and keeps immediate output compatible", async () => {
  const sent = [];
  const output = {
    id: "clock-out",
    name: "Clock Output",
    state: "connected",
    send(bytes, timestamp) {
      sent.push({ bytes: Array.from(bytes), timestamp });
    },
  };
  const access = {
    inputs: new Map(),
    outputs: new Map([[output.id, output]]),
    onstatechange: null,
  };
  const queue = [
    { controllerIx: 2, bytes: [0xFA], delivery: "scheduled", dueTimeMicros: 1_250_000 },
    { controllerIx: 2, bytes: [0xF8], delivery: "scheduled", dueTimeMicros: 1_250_000 },
    { controllerIx: 2, bytes: [0x90, 0x40, 0x7F], delivery: "immediate", dueTimeMicros: 0 },
  ];
  const manager = new BrowserMidiManager({
    submitEndpoints: async () => [{
      type: "open-output",
      controllerIx: 2,
      identifier: output.id,
      name: output.name,
    }],
    deliverMidi: async () => {},
    dequeueMidiOutput: async () => queue.shift(),
  }, intervalOptions({
    requestMIDIAccess: async () => access,
    nowMicros: () => 1_300_000,
  }));

  assert.deepEqual(await manager.startFromUserActivation(), { status: "online" });
  assert.deepEqual(sent, [
    { bytes: [0xFA], timestamp: 1_250 },
    { bytes: [0xF8], timestamp: 1_250 },
    { bytes: [0x90, 0x40, 0x7F], timestamp: undefined },
  ]);
  assert.deepEqual(manager.diagnostics(), {
    droppedImmediateOutputCount: 0,
    droppedScheduledOutputCount: 0,
    bridgeLateScheduledOutputCount: 0,
    lateScheduledOutputCount: 2,
    sendErrorCount: 0,
  });
  manager.stop();
});

test("clears Web MIDI future submissions when an output disconnects", async () => {
  const pending = [];
  let clearCount = 0;
  const output = {
    id: "clock-out",
    name: "Clock Output",
    state: "connected",
    send(bytes, timestamp) {
      pending.push({ bytes: Array.from(bytes), timestamp });
    },
    clear() {
      clearCount += 1;
      pending.length = 0;
    },
  };
  const access = {
    inputs: new Map(),
    outputs: new Map([[output.id, output]]),
    onstatechange: null,
  };
  let connected = true;
  const queue = [
    { controllerIx: 0, bytes: [0xF8], delivery: "scheduled", dueTimeMicros: 125_000 },
  ];
  const manager = new BrowserMidiManager({
    submitEndpoints: async () => connected
      ? [{ type: "open-output", controllerIx: 0, identifier: output.id, name: output.name }]
      : [{ type: "close-output", controllerIx: 0 }],
    deliverMidi: async () => {},
    dequeueMidiOutput: async () => queue.shift(),
  }, intervalOptions({
    requestMIDIAccess: async () => access,
    nowMicros: () => 100_000,
  }));

  await manager.startFromUserActivation();
  assert.deepEqual(pending, [{ bytes: [0xF8], timestamp: 125 }]);
  connected = false;
  output.state = "disconnected";
  await manager.poll();

  assert.equal(clearCount, 1);
  assert.deepEqual(pending, []);
  manager.stop();
});

test("contains Web MIDI send and clear failures while exposing them diagnostically", async () => {
  const output = {
    id: "failing-output",
    name: "Failing Output",
    state: "connected",
    send() { throw new Error("send failed"); },
    clear() { throw new Error("clear failed"); },
  };
  const access = {
    inputs: new Map(),
    outputs: new Map([[output.id, output]]),
    onstatechange: null,
  };
  const queue = [
    { controllerIx: 0, bytes: [0xF8], delivery: "scheduled", dueTimeMicros: 125_000 },
  ];
  const manager = new BrowserMidiManager({
    submitEndpoints: async () => [{
      type: "open-output", controllerIx: 0, identifier: output.id, name: output.name,
    }],
    deliverMidi: async () => {},
    dequeueMidiOutput: async () => queue.shift(),
  }, intervalOptions({ requestMIDIAccess: async () => access, nowMicros: () => 100_000 }));

  assert.deepEqual(await manager.startFromUserActivation(), { status: "online" });
  assert.equal(manager.diagnostics().sendErrorCount, 1);
  assert.doesNotThrow(() => manager.stop());
  assert.equal(manager.diagnostics().sendErrorCount, 2);
});

test("decodes the bounded C++ MIDI output ABI descriptor", () => {
  const memory = new ArrayBuffer(1024);
  const heap = new Uint8Array(memory);
  heap.set([0xFB], 768);
  let nextAllocation = 32;
  const module = {
    FS: {},
    HEAPU8: heap,
    HEAPF32: new Float32Array(memory),
    _malloc(size) {
      const pointer = nextAllocation;
      nextAllocation += size;
      return pointer;
    },
    _free() {},
    lengthBytesUTF8: (value) => new TextEncoder().encode(value).length,
    stringToUTF8() {},
    emscriptenRegisterAudioObject: () => 1,
    _synth_browser_abi_version: () => 6,
    _synth_browser_ui_protocol_version: () => 2,
    _synth_browser_runtime_config_version: () => 1,
    _synth_browser_create: () => 1,
    _synth_browser_set_timestamp_epoch_offset: () => 0,
    _synth_browser_audio_output_channels: () => 2,
    _synth_browser_initialize: () => 0,
    _synth_browser_prepare: () => 0,
    _synth_browser_process: () => 0,
    _synth_browser_start_audio_worklet: () => 0,
    _synth_browser_audio_input_channels: () => 0,
    _synth_browser_set_audio_input_source: () => 0,
    _synth_browser_clear_audio_input_source: () => 0,
    _synth_browser_consume_pending_audio_request: () => -1,
    _synth_browser_message_tick: () => 0,
    _synth_browser_build_ui_frame: () => 0,
    _synth_browser_dispatch_action: () => 0,
    _synth_browser_submit_midi_endpoints: () => 0,
    _synth_browser_dequeue_midi_action: () => 0,
    _synth_browser_deliver_midi: () => 0,
    _synth_browser_dequeue_midi_output(_handle, descriptor) {
      const view = new DataView(memory);
      view.setUint32(descriptor, 7, true);
      view.setUint32(descriptor + 4, 1, true);
      view.setUint32(descriptor + 8, 1, true);
      view.setBigUint64(descriptor + 16, 9_876_543n, true);
      return 768;
    },
    _synth_browser_midi_diagnostics(_handle, descriptor) {
      const view = new DataView(memory);
      view.setBigUint64(descriptor, 3n, true);
      view.setBigUint64(descriptor + 8, 5n, true);
      view.setBigUint64(descriptor + 16, 7n, true);
      return 0;
    },
    _synth_browser_destroy() {},
  };

  assert.deepEqual(emscriptenRuntimeFacade(module).dequeueMidiOutput(1), {
    controllerIx: 7,
    bytes: [0xFB],
    delivery: "scheduled",
    dueTimeMicros: 9_876_543,
  });
  assert.deepEqual(emscriptenRuntimeFacade(module).midiDiagnostics(1), {
    droppedImmediateOutputCount: 3,
    droppedScheduledOutputCount: 5,
    lateScheduledOutputCount: 7,
  });
});

test("normalizes a distinct worker time origin into the document engine epoch", async () => {
  const offsets = [];
  const worker = new BrowserRuntimeWorker(async () => ({
    abiVersion: 6,
    uiProtocolVersion: 2,
    runtimeConfigVersion: 1,
    create: () => 3,
    setTimestampEpochOffset: (_handle, offsetMicros) => { offsets.push(offsetMicros); return 0; },
    audioOutputChannels: () => 2,
    initialize: () => 0,
    prepare: () => 0,
    process: () => 0,
    messageTick: () => 0,
    buildUiFrame: () => new ArrayBuffer(0),
    dispatchAction: () => 0,
    submitMidiEndpoints: () => 0,
    dequeueMidiAction: () => undefined,
    deliverMidi: () => 0,
    dequeueMidiOutput: () => undefined,
    midiDiagnostics: () => ({
      droppedImmediateOutputCount: 0,
      droppedScheduledOutputCount: 0,
      lateScheduledOutputCount: 0,
    }),
    destroy: () => {},
  }), undefined, undefined, () => 1_700_000_000_250);

  await worker.handle({
    type: "load",
    module: { entryUrl: "blob:test", locateFile: {}, mainScriptUrlOrBlob: "blob:test" },
  });
  assert.deepEqual(await worker.handle({
    type: "create",
    documentTimeOriginMillis: 1_700_000_000_000,
  }), { type: "created", handle: 3 });
  assert.deepEqual(offsets, [250_000]);
});
