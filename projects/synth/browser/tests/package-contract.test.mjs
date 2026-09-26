import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { mkdtemp, mkdir, readFile, readdir, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { promisify } from "node:util";
import { pathToFileURL } from "node:url";
import { findBrowserRoot } from "./helpers/browser-root.mjs";

const browserRoot = await findBrowserRoot();
const [
  { readAppBuildManifest },
  { assemblePackage },
  { SUPPORTED_UI_PROTOCOL_VERSION },
] = await Promise.all([
  import(pathToFileURL(path.join(browserRoot, "dist", "src", "app-build-manifest.mjs")).href),
  import(pathToFileURL(path.join(browserRoot, "dist", "src", "package-contract.mjs")).href),
  import(pathToFileURL(path.join(browserRoot, "dist", "src", "protocol.js")).href),
]);

const execFileAsync = promisify(execFile);

const REQUIRED_ARTIFACTS = Object.freeze({
  entry: "app.js",
  wasm: "app.wasm",
  pthreadWorker: "workers/pthread.js",
  wasmWorker: "workers/wasm-worker.js",
  audioWorklet: "worklets/audio.js",
});

async function fixture() {
  const root = await mkdtemp(path.join(os.tmpdir(), "synth-browser-package-"));
  const sourceDirectory = path.join(root, "emitted");
  await mkdir(path.join(sourceDirectory, "workers"), { recursive: true });
  await mkdir(path.join(sourceDirectory, "worklets"), { recursive: true });
  await writeFile(path.join(sourceDirectory, "app.js"), "export default async options => ({ options });\n");
  await writeFile(path.join(sourceDirectory, "app.wasm"), Uint8Array.from([0, 97, 115, 109, 1, 0, 0, 0]));
  await writeFile(path.join(sourceDirectory, "workers", "pthread.js"), "postMessage('pthread');\n");
  await writeFile(path.join(sourceDirectory, "workers", "wasm-worker.js"), "postMessage('wasm-worker');\n");
  await writeFile(path.join(sourceDirectory, "worklets", "audio.js"), "registerProcessor('audio', class {});\n");
  return {
    root,
    sourceDirectory,
    outputDirectory: path.join(root, "output"),
    async dispose() { await rm(root, { recursive: true, force: true }); },
  };
}

async function fileSnapshot(root) {
  const snapshot = [];
  async function visit(directory, prefix = "") {
    const entries = await readdir(directory, { withFileTypes: true });
    for (const entry of entries.sort((left, right) => left.name.localeCompare(right.name))) {
      const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
      const filename = path.join(directory, entry.name);
      if (entry.isDirectory()) await visit(filename, relative);
      else snapshot.push([relative, Buffer.from(await readFile(filename)).toString("hex")]);
    }
  }
  await visit(root);
  return snapshot;
}

test("assembles identical emitted files into byte-for-byte stable immutable packages", async () => {
  const first = await fixture();
  const second = await fixture();
  try {
    const one = await assemblePackage({
      appId: "test-app",
      sourceDirectory: first.sourceDirectory,
      outputDirectory: first.outputDirectory,
      artifacts: REQUIRED_ARTIFACTS,
    });
    const two = await assemblePackage({
      appId: "test-app",
      sourceDirectory: second.sourceDirectory,
      outputDirectory: second.outputDirectory,
      artifacts: REQUIRED_ARTIFACTS,
    });

    assert.deepEqual(one, two);
    assert.equal(one.appId, "test-app");
    assert.match(one.buildId, /^[0-9a-f]{64}$/);
    assert.deepEqual({
      abiVersion: one.browser.abiVersion,
      uiProtocolVersion: one.browser.uiProtocolVersion,
      runtimeConfigVersion: one.browser.runtimeConfigVersion,
    }, { abiVersion: 6, uiProtocolVersion: 2, runtimeConfigVersion: 1 });
    assert.equal(one.browser.entry, `packages/test-app/${one.buildId}/app.js`);
    assert.deepEqual(one.browser.files.map(({ path: filePath }) => filePath), [
      `packages/test-app/${one.buildId}/app.js`,
      `packages/test-app/${one.buildId}/app.wasm`,
      `packages/test-app/${one.buildId}/workers/pthread.js`,
      `packages/test-app/${one.buildId}/workers/wasm-worker.js`,
      `packages/test-app/${one.buildId}/worklets/audio.js`,
    ]);
    assert.deepEqual(
      await fileSnapshot(first.outputDirectory),
      await fileSnapshot(second.outputDirectory),
    );
  } finally {
    await Promise.all([first.dispose(), second.dispose()]);
  }
});

test("changes the content-derived build ID when an emitted role file changes", async () => {
  const setup = await fixture();
  try {
    const before = await assemblePackage({
      appId: "test-app",
      sourceDirectory: setup.sourceDirectory,
      outputDirectory: setup.outputDirectory,
      artifacts: REQUIRED_ARTIFACTS,
    });
    await writeFile(path.join(setup.sourceDirectory, "worklets", "audio.js"), "registerProcessor('changed', class {});\n");
    const after = await assemblePackage({
      appId: "test-app",
      sourceDirectory: setup.sourceDirectory,
      outputDirectory: setup.outputDirectory,
      artifacts: REQUIRED_ARTIFACTS,
    });

    assert.notEqual(after.buildId, before.buildId);
    assert.notEqual(after.browser.files.find((file) => file.path.endsWith("worklets/audio.js")).sha256,
      before.browser.files.find((file) => file.path.endsWith("worklets/audio.js")).sha256);
  } finally {
    await setup.dispose();
  }
});

test("changes the content-derived build ID when artifact roles change", async () => {
  const setup = await fixture();
  try {
    const before = await assemblePackage({
      appId: "test-app",
      sourceDirectory: setup.sourceDirectory,
      outputDirectory: setup.outputDirectory,
      artifacts: REQUIRED_ARTIFACTS,
    });
    const after = await assemblePackage({
      appId: "test-app",
      sourceDirectory: setup.sourceDirectory,
      outputDirectory: setup.outputDirectory,
      artifacts: {
        ...REQUIRED_ARTIFACTS,
        entry: REQUIRED_ARTIFACTS.pthreadWorker,
        pthreadWorker: REQUIRED_ARTIFACTS.entry,
      },
    });

    assert.notEqual(after.buildId, before.buildId);
    assert.equal(after.browser.entry, `packages/test-app/${after.buildId}/workers/pthread.js`);
  } finally {
    await setup.dispose();
  }
});

test("rejects emitted files that are not named by a required artifact role", async () => {
  const setup = await fixture();
  try {
    await writeFile(path.join(setup.sourceDirectory, "stale-fixture.js"), "throw new Error('stale');\n");

    await assert.rejects(
      assemblePackage({
        appId: "test-app",
        sourceDirectory: setup.sourceDirectory,
        outputDirectory: setup.outputDirectory,
        artifacts: REQUIRED_ARTIFACTS,
      }),
      /unexpected.*stale-fixture\.js|stale-fixture\.js.*not.*role/i,
    );
  } finally {
    await setup.dispose();
  }
});

test("requires every Emscripten entry, WASM, worker, and AudioWorklet role", async () => {
  const setup = await fixture();
  try {
    for (const role of Object.keys(REQUIRED_ARTIFACTS)) {
      const artifacts = { ...REQUIRED_ARTIFACTS };
      delete artifacts[role];
      await assert.rejects(
        assemblePackage({
          appId: "test-app",
          sourceDirectory: setup.sourceDirectory,
          outputDirectory: setup.outputDirectory,
          artifacts,
        }),
        new RegExp(role, "i"),
      );
    }

    await rm(path.join(setup.sourceDirectory, "workers", "pthread.js"));
    await assert.rejects(
      assemblePackage({
        appId: "test-app",
        sourceDirectory: setup.sourceDirectory,
        outputDirectory: setup.outputDirectory,
        artifacts: REQUIRED_ARTIFACTS,
      }),
      /pthreadWorker.*not.*inventoried|pthread.*missing/i,
    );
  } finally {
    await setup.dispose();
  }
});

test("never reads or writes an artifact path outside the package roots", async () => {
  const setup = await fixture();
  try {
    await writeFile(path.join(setup.root, "outside.js"), "throw new Error('outside');\n");
    for (const invalidPath of ["../outside.js", "/tmp/outside.js", "workers/../app.js", "workers\\pthread.js"]) {
      await assert.rejects(
        assemblePackage({
          appId: "test-app",
          sourceDirectory: setup.sourceDirectory,
          outputDirectory: setup.outputDirectory,
          artifacts: { ...REQUIRED_ARTIFACTS, pthreadWorker: invalidPath },
        }),
        /path|relative|normalized|package root/i,
      );
    }
    await assert.rejects(
      assemblePackage({
        appId: "../test-app",
        sourceDirectory: setup.sourceDirectory,
        outputDirectory: setup.outputDirectory,
        artifacts: REQUIRED_ARTIFACTS,
      }),
      /appId/i,
    );
  } finally {
    await setup.dispose();
  }
});

test("package-app command assembles aliased Emscripten roles and prints the catalog record", async () => {
  const setup = await fixture();
  try {
    await Promise.all([
      rm(path.join(setup.sourceDirectory, "workers"), { recursive: true }),
      rm(path.join(setup.sourceDirectory, "worklets"), { recursive: true }),
    ]);
    const command = path.join(browserRoot, "dist", "src", "package-app.mjs");
    const { stdout, stderr } = await execFileAsync(process.execPath, [
      command,
      "--app-id", "cli-app",
      "--source-dir", setup.sourceDirectory,
      "--output-dir", setup.outputDirectory,
      "--entry", "app.js",
      "--wasm", "app.wasm",
      "--pthread-worker", "app.js",
      "--wasm-worker", "app.js",
      "--audio-worklet", "app.js",
    ]);
    const record = JSON.parse(stdout);

    assert.equal(stderr, "");
    assert.match(record.buildId, /^[0-9a-f]{64}$/);
    assert.equal(record.browser.entry, `packages/cli-app/${record.buildId}/app.js`);
    assert.ok(record.browser.files.some((file) => file.path.endsWith("/app.wasm")));
    assert.deepEqual(
      JSON.parse(stdout),
      await assemblePackage({
        appId: "cli-app",
        sourceDirectory: setup.sourceDirectory,
        outputDirectory: setup.outputDirectory,
        artifacts: {
          entry: "app.js",
          wasm: "app.wasm",
          pthreadWorker: "app.js",
          wasmWorker: "app.js",
          audioWorklet: "app.js",
        },
      }),
    );
  } finally {
    await setup.dispose();
  }
});

// sru-46: three kinds of artifact advertise the UI protocol version, not two.
// The shell owns `COMMAND_BUFFER_VERSION`, but every Wasm package exports
// `synth_browser_ui_protocol_version()` independently, so a shell bumped alone
// rejects every package it loads. Both real first-party packages compile the
// one shared ABI source, so pinning that source against the shell constant is
// what keeps a stale package from reaching publication.
test("every first-party package exports the shell's UI protocol version", async () => {
  const manifest = await readAppBuildManifest({ browserRoot });
  assert.deepEqual(manifest.apps.map(({ appId }) => appId), ["braid-4", "miniapp", "one-second-delay"]);

  const builder = await readFile(path.join(browserRoot, "src", "build-browser-apps.mjs"), "utf8");
  assert.match(builder, /path\.join\(browserRoot, "cpp", "BrowserRuntimeAbi\.cpp"\)/,
    "every first-party package must compile the shared ABI source");

  const abiSource = await readFile(path.join(browserRoot, "cpp", "BrowserRuntimeAbi.cpp"), "utf8");
  const exported = /synth_browser_ui_protocol_version\(\)\s*\{\s*return\s+(\d+);/.exec(abiSource);
  assert.ok(exported, "BrowserRuntimeAbi.cpp must export a literal UI protocol version");
  assert.equal(Number(exported[1]), SUPPORTED_UI_PROTOCOL_VERSION);
});
