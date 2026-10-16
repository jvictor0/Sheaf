// Reads a constant-returning exported function straight out of a compiled
// `.wasm` module.
//
// Why this exists: the first-party packages advertise their UI
// protocol version twice -- once in the catalog metadata the publisher writes,
// and once in `synth_browser_ui_protocol_version()` compiled into the module.
// Only the second one is what the shell actually calls. A stale Wasm build meets
// the current decoder and throws `invalid presence flag`, and the catalog says
// nothing is wrong, because the catalog was written from the manifest rather
// than from the artifact. So the publisher reads the artifact.
//
// The emscripten JS glue cannot be loaded here to ask the module directly: it is
// built with `-sENVIRONMENT=web,worker` and does not run under Node. Decoding
// the binary is the remaining option, and it is a strict one -- anything other
// than a body of exactly `i32.const <n>; end` is an error rather than a guess.

const WASM_MAGIC = Object.freeze([0x00, 0x61, 0x73, 0x6d]);
const SECTION_IMPORT = 2;
const SECTION_FUNCTION = 3;
const SECTION_EXPORT = 7;
const SECTION_CODE = 10;
const EXTERNAL_KIND_FUNCTION = 0;
const OPCODE_I32_CONST = 0x41;
const OPCODE_END = 0x0b;

class Reader {
  constructor(bytes, offset = 0, end = bytes.length) {
    this.bytes = bytes;
    this.offset = offset;
    this.end = end;
  }

  get done() {
    return this.offset >= this.end;
  }

  u8() {
    if (this.offset >= this.end) throw new Error("wasm read past end of section");
    return this.bytes[this.offset++];
  }

  varU32() {
    let result = 0;
    let shift = 0;
    for (;;) {
      const byte = this.u8();
      result |= (byte & 0x7f) << shift;
      if ((byte & 0x80) === 0) return result >>> 0;
      shift += 7;
      if (shift > 35) throw new Error("wasm varuint32 is longer than five bytes");
    }
  }

  varI32() {
    let result = 0;
    let shift = 0;
    for (;;) {
      const byte = this.u8();
      result |= (byte & 0x7f) << shift;
      shift += 7;
      if ((byte & 0x80) === 0) {
        if (shift < 32 && (byte & 0x40) !== 0) result |= -(1 << shift);
        return result;
      }
      if (shift > 35) throw new Error("wasm varint32 is longer than five bytes");
    }
  }

  name() {
    const length = this.varU32();
    if (this.offset + length > this.end) throw new Error("wasm name runs past end of section");
    const value = new TextDecoder().decode(this.bytes.subarray(this.offset, this.offset + length));
    this.offset += length;
    return value;
  }

  skip(count) {
    this.offset += count;
    if (this.offset > this.end) throw new Error("wasm read past end of section");
  }
}

function sections(bytes) {
  if (bytes.length < 8) throw new Error("not a wasm module: shorter than a header");
  for (let index = 0; index < WASM_MAGIC.length; ++index)
    if (bytes[index] !== WASM_MAGIC[index]) throw new Error("not a wasm module: bad magic");

  const found = [];
  const reader = new Reader(bytes, 8);
  while (!reader.done) {
    const id = reader.u8();
    const size = reader.varU32();
    const start = reader.offset;
    if (start + size > bytes.length) throw new Error(`wasm section ${id} runs past end of module`);
    found.push({ id, start, end: start + size });
    reader.offset = start + size;
  }
  return found;
}

function skipLimits(reader) {
  const flags = reader.u8();
  reader.varU32();
  if ((flags & 0x01) !== 0) reader.varU32();
}

function countImportedFunctions(bytes, section) {
  if (section === undefined) return 0;
  const reader = new Reader(bytes, section.start, section.end);
  const count = reader.varU32();
  let functions = 0;
  for (let index = 0; index < count; ++index) {
    reader.name();
    reader.name();
    const kind = reader.u8();
    if (kind === EXTERNAL_KIND_FUNCTION) {
      reader.varU32();
      ++functions;
    } else if (kind === 1) {
      reader.u8();
      skipLimits(reader);
    } else if (kind === 2) {
      skipLimits(reader);
    } else if (kind === 3) {
      reader.u8();
      reader.u8();
    } else {
      throw new Error(`unknown wasm import kind ${kind}`);
    }
  }
  return functions;
}

function exportedFunctionIndex(bytes, section, exportName) {
  if (section === undefined) return undefined;
  const reader = new Reader(bytes, section.start, section.end);
  const count = reader.varU32();
  for (let index = 0; index < count; ++index) {
    const name = reader.name();
    const kind = reader.u8();
    const target = reader.varU32();
    if (kind === EXTERNAL_KIND_FUNCTION && name === exportName) return target;
  }
  return undefined;
}

function functionBody(bytes, section, bodyIndex) {
  if (section === undefined) throw new Error("wasm module has no code section");
  const reader = new Reader(bytes, section.start, section.end);
  const count = reader.varU32();
  if (bodyIndex >= count) throw new Error("wasm export points past the end of the code section");
  for (let index = 0; index < count; ++index) {
    const size = reader.varU32();
    const start = reader.offset;
    if (index === bodyIndex) return new Reader(bytes, start, start + size);
    reader.skip(size);
  }
  throw new Error("wasm code section ended before the exported body");
}

/**
 * The i32 constant an exported no-argument function returns, or `undefined` when
 * the module does not export that name as a function.
 *
 * Throws when the export exists but its body is anything other than a single
 * `i32.const`: a version accessor that grew a branch is a version accessor this
 * cannot read, and reporting a guess would be worse than refusing.
 */
export function readExportedI32Constant(moduleBytes, exportName) {
  const bytes = moduleBytes instanceof Uint8Array ? moduleBytes : new Uint8Array(moduleBytes);
  const found = sections(bytes);
  const byId = (id) => found.find((section) => section.id === id);

  const functionIndex = exportedFunctionIndex(bytes, byId(SECTION_EXPORT), exportName);
  if (functionIndex === undefined) return undefined;

  const importedFunctions = countImportedFunctions(bytes, byId(SECTION_IMPORT));
  if (functionIndex < importedFunctions)
    throw new Error(`${exportName} is re-exported from an import and has no body here`);
  if (byId(SECTION_FUNCTION) === undefined) throw new Error("wasm module has no function section");

  const body = functionBody(bytes, byId(SECTION_CODE), functionIndex - importedFunctions);
  const localGroups = body.varU32();
  for (let group = 0; group < localGroups; ++group) {
    body.varU32();
    body.u8();
  }
  const opcode = body.u8();
  if (opcode !== OPCODE_I32_CONST)
    throw new Error(
      `${exportName} does not return a single i32 constant (first opcode 0x${opcode.toString(16)})`,
    );
  const value = body.varI32();
  const end = body.u8();
  if (end !== OPCODE_END || !body.done)
    throw new Error(`${exportName} does not return a single i32 constant (body continues past it)`);
  return value;
}
