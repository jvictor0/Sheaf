import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtemp, mkdir, readFile, readdir, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { buildBrowserApps } from "../src/build-browser-apps.mjs";

async function fixture() {
  const browserRoot = await mkdtemp(path.join(os.tmpdir(), "sheaf-browser-builder-"));
  const sourceRoot = path.join(browserRoot, "fixtures");
  for (const name of ["alpha", "beta"]) {
    await mkdir(path.join(sourceRoot, name), { recursive: true });
    await writeFile(path.join(sourceRoot, name, `${name}.hpp`), "#pragma once\n");
  }
  const manifestPath = path.join(browserRoot, "fixture-apps.json");
  await writeFile(manifestPath, `${JSON.stringify({
    schemaVersion: 1,
    publisher: { id: "test", name: "Test" },
    apps: ["beta", "alpha"].map((name) => ({
      appId: name,
      displayName: name.toUpperCase(),
      author: "Test",
      category: "Fixture",
      header: `${name}.hpp`,
      cppType: `fixture::${name}`,
      includeDirs: [`fixtures/${name}`],
    })),
  }, null, 2)}\n`);
  return { browserRoot, sourceRoot, manifestPath };
}

function outputPath(args) {
  const outputIndex = args.indexOf("-o");
  assert.notEqual(outputIndex, -1);
  return args[outputIndex + 1];
}

test("builds every record through one argument-vector compiler policy and writes structured emissions", async () => {
  const fx = await fixture();
  const calls = [];
  const runCommand = async (executable, args, options) => {
    calls.push({ executable, args, options });
    assert.equal(options.shell, false);
    const output = outputPath(args);
    await mkdir(path.dirname(output), { recursive: true });
    await writeFile(output, "export default function Module() {}\n");
    await writeFile(output.replace(/\.js$/, ".wasm"), "wasm");
  };

  const report = await buildBrowserApps({
    browserRoot: fx.browserRoot,
    manifestPath: fx.manifestPath,
    allowedSourceRoots: [fx.sourceRoot],
    runCommand,
  });

  assert.equal(calls.length, 2);
  assert.deepEqual(report.apps.map(({ appId }) => appId), ["alpha", "beta"]);
  assert.match(report.manifestDigest, /^[a-f0-9]{64}$/);
  assert.deepEqual(Object.keys(report), ["schemaVersion", "manifestDigest", "publisher", "apps"]);

  for (const [index, appId] of ["alpha", "beta"].entries()) {
    const { executable, args, options } = calls[index];
    assert.equal(executable, "em++");
    assert.equal(options.cwd, fx.browserRoot);
    assert.equal(options.shell, false);
    assert.ok(args.some((arg) => arg.endsWith("cpp/BrowserRuntimeAbi.cpp")));
    assert.ok(args.some((arg) => arg.endsWith(`dist/generated/browser-apps/${appId}.cpp`)));
    assert.ok(args.includes("-sINITIAL_MEMORY=536870912"));
    assert.ok(args.includes("-sALLOW_MEMORY_GROWTH=1"));
    assert.ok(args.includes("-sMAXIMUM_MEMORY=2147483648"));
    assert.ok(args.includes("-sAUDIO_WORKLET=1"));
    assert.ok(args.includes("-sWASM_WORKERS=1"));
    assert.ok(args.includes("-sUSE_PTHREADS=1"));
    assert.ok(args.includes("-lidbfs.js"));
    assert.ok(args.some((arg) => arg.includes("emscriptenRegisterAudioObject")));
    assert.ok(args.some((arg) => arg.includes("_synth_browser_start_audio_worklet")));
    for (const inputExport of [
      "_synth_browser_audio_input_channels",
      "_synth_browser_set_audio_input_source",
      "_synth_browser_clear_audio_input_source",
      "_synth_browser_consume_pending_audio_request",
      "_synth_browser_submit_audio_devices",
    ]) {
      assert.ok(args.some((arg) => arg.includes(inputExport)), `missing browser audio input export ${inputExport}`);
    }
    assert.match(outputPath(args), new RegExp(
      `dist\\/wasm\\/\\.apps\\.stage-[^/]+\\/${appId}\\/${appId}\\.js$`,
    ));

    const emission = report.apps[index];
    assert.deepEqual(emission, {
      appId,
      artifacts: {
        entry: `apps/${appId}/${appId}.js`,
        wasm: `apps/${appId}/${appId}.wasm`,
        pthreadWorker: `apps/${appId}/${appId}.js`,
        wasmWorker: `apps/${appId}/${appId}.js`,
        audioWorklet: `apps/${appId}/${appId}.js`,
      },
    });
  }

  const policy = (args) => {
    const invariant = [];
    for (let index = 0; index < args.length; index += 1) {
      if (args[index] === "-I") {
        index += 1;
      } else if (!args[index].includes("dist/generated/browser-apps/") &&
                 !args[index].includes("dist/wasm/")) {
        invariant.push(args[index]);
      }
    }
    return invariant;
  };
  assert.deepEqual(policy(calls[0].args), policy(calls[1].args));

  assert.equal(
    await readFile(path.join(fx.browserRoot, "dist", "generated", "browser-apps", "alpha.cpp"), "utf8"),
    '#include "alpha.hpp"\n#include "synth/browser/BrowserAppEntry.hpp"\n\nSYNTH_BROWSER_APP(fixture::alpha)\n',
  );
  assert.deepEqual(
    JSON.parse(await readFile(path.join(fx.browserRoot, "dist", "wasm", "apps", "emissions.json"), "utf8")),
    report,
  );
  for (const appId of ["alpha", "beta"]) {
    assert.equal(
      await readFile(path.join(fx.browserRoot, "dist", "wasm", "apps", appId, `${appId}.js`), "utf8"),
      "export default function Module() {}\n",
    );
  }
});

test("does not replace the last complete emission report when a compile fails", async () => {
  const fx = await fixture();
  const reportPath = path.join(fx.browserRoot, "dist", "wasm", "apps", "emissions.json");
  await mkdir(path.dirname(reportPath), { recursive: true });
  for (const appId of ["alpha", "beta"]) {
    const appRoot = path.join(fx.browserRoot, "dist", "wasm", "apps", appId);
    await mkdir(appRoot, { recursive: true });
    await writeFile(path.join(appRoot, `${appId}.js`), `known-good ${appId} js\n`);
    await writeFile(path.join(appRoot, `${appId}.wasm`), `known-good ${appId} wasm\n`);
  }
  await writeFile(reportPath, "known-good\n");
  const before = await snapshotTree(path.dirname(reportPath));
  let calls = 0;

  await assert.rejects(buildBrowserApps({
    browserRoot: fx.browserRoot,
    manifestPath: fx.manifestPath,
    allowedSourceRoots: [fx.sourceRoot],
    runCommand: async (_executable, args) => {
      calls += 1;
      if (calls === 2) throw new Error("compiler failed");
      const output = outputPath(args);
      await mkdir(path.dirname(output), { recursive: true });
      await writeFile(output, "js");
      await writeFile(output.replace(/\.js$/, ".wasm"), "wasm");
    },
  }), /compiler failed/);

  assert.deepEqual(await snapshotTree(path.dirname(reportPath)), before);
});

async function snapshotTree(root) {
  const snapshot = [];
  async function visit(directory, prefix = "") {
    const entries = await readdir(directory, { withFileTypes: true });
    entries.sort((left, right) => left.name < right.name ? -1 : left.name > right.name ? 1 : 0);
    for (const entry of entries) {
      const relativePath = prefix ? `${prefix}/${entry.name}` : entry.name;
      const filename = path.join(directory, entry.name);
      if (entry.isDirectory()) await visit(filename, relativePath);
      else snapshot.push([
        relativePath,
        createHash("sha256").update(await readFile(filename)).digest("hex"),
      ]);
    }
  }
  await visit(root);
  return snapshot;
}

test("can isolate fixture emissions and their report from the production output root", async () => {
  const fx = await fixture();
  const outputRoot = path.join(fx.browserRoot, "dist", "wasm", "fixture-apps");
  const report = await buildBrowserApps({
    browserRoot: fx.browserRoot,
    manifestPath: fx.manifestPath,
    allowedSourceRoots: [fx.sourceRoot],
    outputRoot,
    runCommand: async (_executable, args) => {
      const output = outputPath(args);
      await mkdir(path.dirname(output), { recursive: true });
      await writeFile(output, "js");
      await writeFile(output.replace(/\.js$/, ".wasm"), "wasm");
    },
  });

  assert.deepEqual(report.apps.map(({ artifacts }) => artifacts.entry), [
    "fixture-apps/alpha/alpha.js",
    "fixture-apps/beta/beta.js",
  ]);
  assert.deepEqual(
    JSON.parse(await readFile(path.join(outputRoot, "emissions.json"), "utf8")),
    report,
  );
  await assert.rejects(
    readFile(path.join(fx.browserRoot, "dist", "wasm", "apps", "emissions.json")),
    { code: "ENOENT" },
  );
});

test("rejects a successful compiler invocation that omits required artifacts", async () => {
  const fx = await fixture();
  await assert.rejects(buildBrowserApps({
    browserRoot: fx.browserRoot,
    manifestPath: fx.manifestPath,
    allowedSourceRoots: [fx.sourceRoot],
    runCommand: async (_executable, args) => {
      const output = outputPath(args);
      await mkdir(path.dirname(output), { recursive: true });
      await writeFile(output, "js");
    },
  }), /alpha.*wasm.*missing/i);
});
