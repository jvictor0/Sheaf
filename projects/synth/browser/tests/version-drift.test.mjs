// The browser ABI version, the two protocol versions beside it, and the
// pending-audio-request sentinels are each ONE fact that has to be spelled in
// more than one language. TypeScript cannot share a literal with C++, so every
// mirror is checked here against the definition rather than maintained by
// hand. A comment saying "keep in sync" is not a mechanism; this is.
//
// The failure this prevents has already happened: a bump moved a stub in
// static-server.mjs and left the synthesized fixture catalog below it on the
// old number, and the resulting test failure was indistinguishable from a real
// defect until someone read the server process's start time.
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  SUPPORTED_BROWSER_ABI_VERSION,
  SUPPORTED_RUNTIME_CONFIG_VERSION,
  SUPPORTED_UI_PROTOCOL_VERSION,
} from "../src/protocol.js";

// Three levels up from the COMPILED location (dist/tests) reaches
// projects/synth, the same shape github-pages-workflow.test.mjs uses to find
// the repository root from there.
const synthRoot = fileURLToPath(new URL("../../../", import.meta.url));

async function read(relativeToSynthRoot) {
  return readFile(path.join(synthRoot, relativeToSynthRoot), "utf8");
}

// Reads the integer a zero-argument C++ accessor returns. Two shapes occur and
// both are real: a bare literal, and a named constant defined elsewhere in the
// tree (the runtime config version already has one). A named constant is
// resolved to its definition rather than skipped, because skipping it would
// make this check silently weaker for exactly the accessor that is already
// single-sourced. Anything neither shape covers fails loudly here rather than
// being passed over.
function cxxReturnedValue(source, symbol, resolveConstant) {
  const pattern = new RegExp(
    `${symbol}\\s*\\(\\s*\\)\\s*\\{\\s*return\\s+([A-Za-z_:][\\w:]*|\\d+)\\s*;\\s*\\}`,
  );
  const match = pattern.exec(source);
  assert.ok(match, `could not read what ${symbol} returns; has its shape changed?`);
  const returned = match[1];
  if (/^\d+$/.test(returned)) return Number(returned);
  return resolveConstant(returned.split("::").pop());
}

test("the C++ version accessors return what protocol.ts defines", async () => {
  const abi = await read("browser/cpp/BrowserRuntimeAbi.cpp");
  const persistence = await read("include/synth/browser/BrowserPersistence.hpp");
  const resolveConstant = (name) => {
    for (const source of [abi, persistence]) {
      const match = new RegExp(`${name}\\s*=\\s*(\\d+)\\s*;`).exec(source);
      if (match) return Number(match[1]);
    }
    assert.fail(`could not resolve the C++ constant ${name} to a literal`);
  };
  assert.equal(
    cxxReturnedValue(abi, "synth_browser_abi_version", resolveConstant),
    SUPPORTED_BROWSER_ABI_VERSION,
    "BrowserRuntimeAbi.cpp's ABI version disagrees with protocol.ts",
  );
  assert.equal(
    cxxReturnedValue(abi, "synth_browser_ui_protocol_version", resolveConstant),
    SUPPORTED_UI_PROTOCOL_VERSION,
    "BrowserRuntimeAbi.cpp's UI protocol version disagrees with protocol.ts",
  );
  assert.equal(
    cxxReturnedValue(abi, "synth_browser_runtime_config_version", resolveConstant),
    SUPPORTED_RUNTIME_CONFIG_VERSION,
    "BrowserRuntimeAbi.cpp's runtime config version disagrees with protocol.ts",
  );
});

test("the C++ contract test asserts the same ABI version", async () => {
  const contract = await read("tests/browser_runtime_contract_tests.cpp");
  const match = /synth_browser_abi_version\(\)\s*==\s*(\d+)/.exec(contract);
  assert.ok(match, "could not find the contract test's ABI assertion");
  assert.equal(
    Number(match[1]),
    SUPPORTED_BROWSER_ABI_VERSION,
    "the C++ contract test pins an ABI version protocol.ts no longer defines",
  );
});

// The sentinels the pending-audio-request slot can carry. C++ owns them and
// audio.ts mirrors them; nothing but this check couples the two, and the set
// grows (permission-request was added after release), so drift is a live risk
// rather than a theoretical one.
test("the pending-audio-request sentinels agree across the ABI boundary", async () => {
  const cxx = await read("include/synth/browser/BrowserAudioDevices.hpp");
  const ts = await read("browser/src/audio.ts");

  const cxxSentinel = (name) => {
    const match = new RegExp(`${name}\\s*=\\s*(-?\\d+)\\s*;`).exec(cxx);
    assert.ok(match, `could not read C++ sentinel ${name}`);
    return Number(match[1]);
  };
  const tsSentinel = (name) => {
    const match = new RegExp(`\\b${name}:\\s*(-?\\d+)`).exec(ts);
    assert.ok(match, `could not read TS sentinel ${name}`);
    return Number(match[1]);
  };

  for (const [cxxName, tsName] of [
    ["kNoPendingAudioRequest", "none"],
    ["kReleaseAudioRequest", "release"],
    ["kRequestPermissionAudioRequest", "requestPermission"],
  ]) {
    assert.equal(
      tsSentinel(tsName),
      cxxSentinel(cxxName),
      `audio.ts's PendingAudioRequest.${tsName} disagrees with ${cxxName}`,
    );
  }
});

// A fixture that hard-codes the version is the same defect as a hand-maintained
// mirror, and harder to notice because it is not near the definition. Every
// `abiVersion: <literal>` in the tree is checked, so a new fixture written with
// a stale number fails here rather than at whatever it happens to assert.
test("no fixture declares an ABI version the definition does not", async () => {
  const { execFileSync } = await import("node:child_process");
  const listing = execFileSync(
    "grep",
    ["-rn", "--exclude-dir=dist", "--exclude-dir=node_modules", "-E", "abiVersion:\\s*[0-9]+", "."],
    { cwd: synthRoot, encoding: "utf8" },
  );
  const offenders = [];
  for (const line of listing.split("\n")) {
    if (!line) continue;
    const value = /abiVersion:\s*([0-9]+)/.exec(line);
    if (!value) continue;
    if (Number(value[1]) !== SUPPORTED_BROWSER_ABI_VERSION) offenders.push(line.trim());
  }
  assert.deepEqual(
    offenders,
    [],
    `these sites declare an ABI version other than ${SUPPORTED_BROWSER_ABI_VERSION}`,
  );
});

// The JS side stubs the same three accessors the C++ side defines, and every
// stub hard-codes the number rather than reading it. A fixture that disagrees
// with the definition is the defect this file exists for; the shape it is
// written in does not change that, and the check above matches only the
// catalog spelling. Searched by the accessor names, which no stub can avoid
// sharing, rather than by any one syntactic form.
const ACCESSOR_DEFINITIONS = {
  abi: SUPPORTED_BROWSER_ABI_VERSION,
  ui_protocol: SUPPORTED_UI_PROTOCOL_VERSION,
  runtime_config: SUPPORTED_RUNTIME_CONFIG_VERSION,
};

// Two shapes occur: a property holding an arrow, and a method with a body.
// Both end in the literal the stub reports.
const STUB_SHAPES = [
  /_synth_browser_(abi|ui_protocol|runtime_config)_version\s*:\s*\(\s*\)\s*=>\s*(\d+)/,
  /_synth_browser_(abi|ui_protocol|runtime_config)_version\s*\(\s*\)\s*\{.*?return\s+(\d+)\s*;/,
];

test("no test double stubs a version the definition does not", async () => {
  const { execFileSync } = await import("node:child_process");
  const listing = execFileSync(
    "grep",
    [
      "-rn",
      "--exclude-dir=dist",
      "--exclude-dir=node_modules",
      "-E",
      "_synth_browser_(abi|ui_protocol|runtime_config)_version",
      ".",
    ],
    { cwd: synthRoot, encoding: "utf8" },
  );
  const offenders = [];
  const seen = new Set();
  for (const line of listing.split("\n")) {
    if (!line) continue;
    for (const shape of STUB_SHAPES) {
      const match = shape.exec(line);
      if (!match) continue;
      const [, accessor, literal] = match;
      seen.add(accessor);
      if (Number(literal) !== ACCESSOR_DEFINITIONS[accessor]) offenders.push(line.trim());
      break;
    }
  }
  assert.deepEqual(offenders, [], "these stubs report a version protocol.ts does not define");
  // POSITIVE CONTROL: a stub whose shape drifted out of both patterns above
  // would leave this test scanning nothing and passing. Every accessor must
  // still be found somewhere.
  assert.deepEqual(
    Object.keys(ACCESSOR_DEFINITIONS).filter((accessor) => !seen.has(accessor)),
    [],
    "no stub of these accessors was found at all; has the shape changed?",
  );
});
