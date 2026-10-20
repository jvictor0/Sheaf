export const COMMAND_BUFFER_MAGIC = "SBCB";
// Declared in protocol-versions.js and re-exported here, so a .mjs that runs
// before anything is built can read them without importing compiled output.
// This stays the import site every TypeScript consumer uses.
export {
  SUPPORTED_BROWSER_ABI_VERSION,
  SUPPORTED_RUNTIME_CONFIG_VERSION,
  SUPPORTED_UI_PROTOCOL_VERSION,
} from "./protocol-versions.js";
import { SUPPORTED_UI_PROTOCOL_VERSION } from "./protocol-versions.js";
export const COMMAND_BUFFER_VERSION = SUPPORTED_UI_PROTOCOL_VERSION;

export type MidiEndpoint = { identifier: string; name: string; kind: "input" | "output" };
// Mirrors `synth_browser::BrowserAudioDevice` (BrowserAudioDevices.hpp), one
// entry from `navigator.mediaDevices.enumerateDevices()` carried across
// `synth_browser_submit_audio_devices`. `deviceId` and `label` are submitted
// exactly as the browser reports them -- including both empty on an
// unpermitted page -- since the native side, not JS, decides what is
// presentable.
export type AudioDevice = { deviceId: string; label: string; kind: "input" | "output" };
export type MidiActionType = "open-input" | "open-output" | "close-input" | "close-output" | "update-input-ref" | "update-output-ref" | "resync";
export type MidiAction = { type: MidiActionType; controllerIx: number; identifier?: string; name?: string };
export type MidiOutput = {
  controllerIx: number;
  bytes: number[];
  delivery: "immediate" | "scheduled";
  // Absolute performance.timeOrigin-relative engine microseconds. Immediate
  // feedback uses zero and the timestamp-less Web MIDI send overload.
  dueTimeMicros: number;
};
export type MidiOutputDiagnostics = {
  droppedImmediateOutputCount: number;
  droppedScheduledOutputCount: number;
  lateScheduledOutputCount: number;
};

export enum NodeKind {
  Root,
  Row,
  Section,
  ScrollArea,
  Label,
  Button,
  Toggle,
  Slider,
  ComboBox,
  TextField,
  StatusText,
  Draw,
}

export enum DrawKind {
  Fill,
  StrokeRect,
  Line,
  Arc,
  Text,
  FillEllipse,
  StrokeEllipse,
  FillRoundedRect,
  StrokeRoundedRect,
  Polyline,
  FillPolygon,
}

export type Action = { name: string; value: string };
export type Point = { x: number; y: number };
export type Bounds = { x: number; y: number; width: number; height: number };
export type Color = { r: number; g: number; b: number; a: number };
export type DrawCommand = {
  kind: DrawKind; align: number; bounds: Bounds; from: Point; to: Point; color: Color; strokeWidth: number;
  startRadians: number; endRadians: number; cornerRadius: number; text: string; textSize: number; textColor: Color; points: Point[];
};
export type TextStyle = { size: number; color: Color; align: number };
// `bounds` are parent-relative and draw geometry is node-local (sru-46).
// `color`, `textStyle`, and the container border fields are absent when the
// producer carried none, in which case the backend applies its own default
// look; `color`'s meaning is per-kind (see the contract on `synth::ui::Node`
// in `include/synth/PortableUI.hpp`) and glyph colour always comes from
// `textStyle`. There is no `variant`: it carried appearance only and version 2
// retired it.
export type Node = {
  id: string; kind: NodeKind; checked: boolean; selected: boolean; enabled: boolean; bounds: Bounds; label: string; text: string;
  selectedOption: string; value: number; minValue: number; maxValue: number; step: number;
  scrollContentWidth: number; scrollContentHeight: number; color?: Color; textStyle?: TextStyle;
  borderColor?: Color; borderWidth?: number; cornerRadius?: number;
  action?: Action; pointerDragAction?: Action; doubleClickAction?: Action;
  drawStart: number; drawCount: number; options: Array<{ id: string; label: string }>; children: string[];
};
export type CommandBufferFrame = { version: number; strings: string[]; nodes: Node[]; actions: Action[]; drawCommands: DrawCommand[]; diagnostics: Array<{ code: number; feature: string }> };

export class CommandBufferError extends Error {
  constructor(public readonly recordKind: string, public readonly index?: number, detail = "invalid command buffer") {
    super(`${detail} (${recordKind}${index === undefined ? "" : ` ${index}`})`);
    this.name = "CommandBufferError";
  }
}

class Reader {
  private offset = 0;
  constructor(private readonly bytes: Uint8Array, private readonly recordKind: string, private readonly index?: number) {}
  u8() { this.require(1); return this.bytes[this.offset++]; }
  u16() { return this.u8() | (this.u8() << 8); }
  u32() { return (this.u8() | (this.u8() << 8) | (this.u8() << 16) | (this.u8() << 24)) >>> 0; }
  i32() { return this.u32() | 0; }
  float() { this.require(4); const value = new DataView(this.bytes.buffer, this.bytes.byteOffset + this.offset, 4).getFloat32(0, true); this.offset += 4; return value; }
  section(size: number, kind: string) { this.require(size); const section = this.bytes.subarray(this.offset, this.offset + size); this.offset += size; return new Reader(section, kind); }
  empty() { return this.offset === this.bytes.length; }
  private require(size: number) { if (size > this.bytes.length - this.offset) throw new CommandBufferError(this.recordKind, this.index, "truncated command buffer"); }
}

function fail(kind: string, index: number | undefined, detail: string): never { throw new CommandBufferError(kind, index, detail); }
function finite(value: number, kind: string, index: number) { if (!Number.isFinite(value)) fail(kind, index, "non-finite numeric value"); return value; }
function bounds(reader: Reader, kind: string, index: number): Bounds {
  const result = { x: reader.float(), y: reader.float(), width: reader.float(), height: reader.float() };
  for (const value of Object.values(result)) finite(value, kind, index);
  return result;
}
function point(reader: Reader, kind: string, index: number): Point { return { x: finite(reader.float(), kind, index), y: finite(reader.float(), kind, index) }; }
function color(reader: Reader): Color { return { r: reader.u8(), g: reader.u8(), b: reader.u8(), a: reader.u8() }; }
// Optional node fields carry an explicit presence byte. Absence is never a
// sentinel value, because any sentinel is indistinguishable from a producer
// legitimately choosing it.
function presence(reader: Reader, kind: string, index: number): boolean {
  const flag = reader.u8();
  if (flag > 1) fail(kind, index, "invalid presence flag");
  return flag === 1;
}
function optionalColor(reader: Reader, kind: string, index: number): Color | undefined {
  return presence(reader, kind, index) ? color(reader) : undefined;
}
function optionalTextStyle(reader: Reader, kind: string, index: number): TextStyle | undefined {
  if (!presence(reader, kind, index)) return undefined;
  const size = finite(reader.float(), kind, index);
  const glyphColor = color(reader);
  const align = reader.u8();
  if (align > 2) fail(kind, index, "invalid text alignment");
  return { size, color: glyphColor, align };
}
function optionalFloat(reader: Reader, kind: string, index: number): number | undefined {
  return presence(reader, kind, index) ? finite(reader.float(), kind, index) : undefined;
}

export function decodeCommandBuffer(buffer: ArrayBuffer): CommandBufferFrame {
  const header = new Reader(new Uint8Array(buffer), "header");
  for (const character of COMMAND_BUFFER_MAGIC) if (header.u8() !== character.charCodeAt(0)) fail("header", undefined, "invalid command buffer magic");
  const version = header.u16();
  if (version !== COMMAND_BUFFER_VERSION) fail("header", undefined, "unsupported command buffer version");
  header.u16();
  const stringBytes = header.u32();
  const nodeBytes = header.u32();
  const actionBytes = header.u32();
  const drawBytes = header.u32();
  const diagnosticBytes = header.u32();
  const stringsReader = header.section(stringBytes, "string table");
  const nodesReader = header.section(nodeBytes, "node table");
  const actionsReader = header.section(actionBytes, "action table");
  const drawsReader = header.section(drawBytes, "draw table");
  const diagnosticsReader = header.section(diagnosticBytes, "diagnostic table");
  if (!header.empty()) fail("header", undefined, "trailing command buffer bytes");

  const stringCount = stringsReader.u32();
  const strings: string[] = [];
  for (let index = 0; index < stringCount; index++) {
    const length = stringsReader.u32();
    const value = new Uint8Array(length);
    for (let offset = 0; offset < length; offset++) value[offset] = stringsReader.u8();
    strings.push(new TextDecoder().decode(value));
  }
  if (!stringsReader.empty()) fail("string table", undefined, "invalid string table");
  const stringAt = (index: number, recordKind: string, recordIndex: number) => {
    if (index >= strings.length) fail(recordKind, recordIndex, "invalid string index");
    return strings[index];
  };

  const actionCount = actionsReader.u32();
  const actions: Action[] = [];
  for (let index = 0; index < actionCount; index++) actions.push({ name: stringAt(actionsReader.u32(), "action", index), value: stringAt(actionsReader.u32(), "action", index) });
  if (!actionsReader.empty()) fail("action table", undefined, "invalid action table");
  const actionAt = (index: number, nodeIndex: number) => {
    if (index === -1) return undefined;
    if (index < 0 || index >= actions.length) fail("node", nodeIndex, "invalid action index");
    return actions[index];
  };

  const drawCount = drawsReader.u32();
  const drawCommands: DrawCommand[] = [];
  for (let index = 0; index < drawCount; index++) {
    const kind = drawsReader.u8();
    if (kind > DrawKind.FillPolygon) fail("draw", index, "invalid draw kind");
    const align = drawsReader.u8();
    if (align > 2) fail("draw", index, "invalid text alignment");
    drawsReader.u16();
    const draw: DrawCommand = {
      kind, align, bounds: bounds(drawsReader, "draw", index), from: point(drawsReader, "draw", index), to: point(drawsReader, "draw", index), color: color(drawsReader),
      strokeWidth: finite(drawsReader.float(), "draw", index), startRadians: finite(drawsReader.float(), "draw", index), endRadians: finite(drawsReader.float(), "draw", index),
      cornerRadius: finite(drawsReader.float(), "draw", index), text: stringAt(drawsReader.u32(), "draw", index), textSize: finite(drawsReader.float(), "draw", index), textColor: color(drawsReader), points: [],
    };
    const pointCount = drawsReader.u32();
    for (let pointIndex = 0; pointIndex < pointCount; pointIndex++) draw.points.push(point(drawsReader, "draw", index));
    drawCommands.push(draw);
  }
  if (!drawsReader.empty()) fail("draw table", undefined, "invalid draw table");

  const nodeCount = nodesReader.u32();
  const nodes: Node[] = [];
  for (let index = 0; index < nodeCount; index++) {
    const id = stringAt(nodesReader.u32(), "node", index);
    const kind = nodesReader.u8();
    if (kind > NodeKind.Draw) fail("node", index, "invalid node kind");
    const checked = nodesReader.u8(); const selected = nodesReader.u8(); const enabled = nodesReader.u8();
    if (checked > 1 || selected > 1 || enabled > 1) fail("node", index, "invalid boolean value");
    const node: Node = {
      id, kind, checked: checked === 1, selected: selected === 1, enabled: enabled === 1, bounds: bounds(nodesReader, "node", index),
      label: stringAt(nodesReader.u32(), "node", index), text: stringAt(nodesReader.u32(), "node", index), selectedOption: stringAt(nodesReader.u32(), "node", index),
      value: finite(nodesReader.float(), "node", index), minValue: finite(nodesReader.float(), "node", index), maxValue: finite(nodesReader.float(), "node", index), step: finite(nodesReader.float(), "node", index),
      scrollContentWidth: finite(nodesReader.float(), "node", index), scrollContentHeight: finite(nodesReader.float(), "node", index),
      color: optionalColor(nodesReader, "node", index), textStyle: optionalTextStyle(nodesReader, "node", index),
      borderColor: optionalColor(nodesReader, "node", index),
      borderWidth: optionalFloat(nodesReader, "node", index),
      cornerRadius: optionalFloat(nodesReader, "node", index),
      action: actionAt(nodesReader.i32(), index), pointerDragAction: actionAt(nodesReader.i32(), index), doubleClickAction: actionAt(nodesReader.i32(), index),
      drawStart: nodesReader.u32(), drawCount: nodesReader.u32(), options: [], children: [],
    };
    if (node.drawStart > drawCommands.length || node.drawCount > drawCommands.length - node.drawStart) fail("node", index, "invalid draw range");
    if (node.kind === NodeKind.ScrollArea && (node.scrollContentWidth < 0 || node.scrollContentHeight < 0)) fail("node", index, "invalid scroll extent");
    const optionCount = nodesReader.u32();
    for (let optionIndex = 0; optionIndex < optionCount; optionIndex++) node.options.push({ id: stringAt(nodesReader.u32(), "node", index), label: stringAt(nodesReader.u32(), "node", index) });
    const childCount = nodesReader.u32();
    for (let childIndex = 0; childIndex < childCount; childIndex++) node.children.push(stringAt(nodesReader.u32(), "node", index));
    nodes.push(node);
  }
  if (!nodesReader.empty()) fail("node table", undefined, "invalid node table");
  const nodeIds = new Set<string>();
  nodes.forEach((node, index) => { if (nodeIds.has(node.id)) fail("node", index, "duplicate node id"); nodeIds.add(node.id); });
  nodes.forEach((node, index) => node.children.forEach((child) => { if (!nodeIds.has(child)) fail("node", index, "unknown child node"); }));

  const diagnosticCount = diagnosticsReader.u32();
  const diagnostics: Array<{ code: number; feature: string }> = [];
  for (let index = 0; index < diagnosticCount; index++) {
    const code = diagnosticsReader.u8(); diagnosticsReader.u8(); diagnosticsReader.u16();
    if (code !== 1) fail("diagnostic", index, "invalid diagnostic code");
    diagnostics.push({ code, feature: stringAt(diagnosticsReader.u32(), "diagnostic", index) });
  }
  if (!diagnosticsReader.empty()) fail("diagnostic table", undefined, "invalid diagnostic table");
  return { version, strings, nodes, actions, drawCommands, diagnostics };
}
