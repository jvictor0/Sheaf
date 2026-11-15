import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { pathToFileURL } from "node:url";
import { findBrowserRoot } from "./helpers/browser-root.mjs";

test("browser scaffold test runner is active", () => {
  assert.equal(typeof globalThis, "object");
});

test("playwright configs discover only browser specs through the shared definition", async () => {
  const browserRoot = await findBrowserRoot();
  const config = await readFile(path.join(browserRoot, "playwright.config.mjs"), "utf8");
  const sharedConfig = await readFile(path.join(browserRoot, "playwright.shared-config.mjs"), "utf8");
  const rootConfig = await readFile(path.resolve(browserRoot, "..", "..", "..", "playwright.config.mjs"), "utf8");

  assert.match(sharedConfig, /testMatch:\s*"\*\*\/\*\.spec\.ts"/);
  assert.match(config, /synthBrowserPlaywrightConfig/);
  assert.match(config, /staticServerCommand:\s*"node src\/static-server\.mjs"/);
  assert.match(rootConfig, /synthBrowserPlaywrightConfig/);
  assert.match(rootConfig, /staticServerCommand:\s*"node projects\/synth\/browser\/src\/static-server\.mjs"/);
  assert.match(rootConfig, /bare object.*root-level Playwright config/s);
});

test("node browser tests share one browser-root discovery helper", async () => {
  const browserRoot = await findBrowserRoot();
  const forbiddenRootFunction = new RegExp(["async", "function", "findBrowserRoot"].join("\\s+"));
  for (const relativePath of [
    "tests/package-contract.test.mjs",
    "tests/publish-site.test.mjs",
    "tests/scaffold.test.mjs",
  ]) {
    const source = await readFile(path.join(browserRoot, relativePath), "utf8");
    assert.match(source, /helpers\/browser-root\.mjs/);
    assert.doesNotMatch(source, forbiddenRootFunction);
  }
});

test("native processor creation keeps the live worklet stack after deferred input attach rollback", async () => {
  const browserRoot = await findBrowserRoot();
  const runtime = await readFile(path.resolve(browserRoot, "..", "include", "synth", "browser", "BrowserRuntime.hpp"), "utf8");
  const processorCreated = runtime.match(/static void AudioWorkletProcessorCreated[\s\S]*?static bool ProcessAudioWorklet/)?.[0] ?? "";
  assert.match(processorCreated, /ResolveDeferredAudioInputConnection/);
  assert.doesNotMatch(processorCreated, /emscripten_destroy_web_audio_node[\s\S]*ResolveDeferredAudioInputConnection|ResolveDeferredAudioInputConnection[\s\S]*emscripten_destroy_web_audio_node/);
  assert.doesNotMatch(processorCreated, /audioWorkletStarted_\.store\(false/);
  assert.match(runtime, /RetainAfterStopForAudioWorklet\(\) const[\s\S]*audioWorkletStarted_\.load/);
});

test("browser apps and the fixture use the same generic builder with no rollback alias", async () => {
  const makefile = await readBrowserMakefile();
  for (const target of ["browser-apps", "browser-fixture-app", "browser-apps-run", "browser-apps-smoke"]) {
    assert.match(makefile, new RegExp(`^${target}:`, "m"));
  }
  assert.equal((makefile.match(/dist\/src\/build-browser-apps\.mjs/g) ?? []).length, 2);
  assert.doesNotMatch(makefile, /browser-miniapp|browser-fake-app|dist\/wasm\/app\.js|miniapp_entry|fake_app_entry/);
  assert.match(makefile, /browser-fixture-app:[\s\S]*--output-root dist\/wasm\/fixture-apps/);
  assert.match(makefile, /browser-apps-smoke: browser-fixture-app\n\t\$\(MAKE\) browser-apps/);

  const browserRoot = await findBrowserRoot();
  await assert.rejects(access(path.join(browserRoot, "cpp", "miniapp_entry.cpp")));
  await assert.rejects(access(path.join(browserRoot, "cpp", "fake_app_entry.cpp")));
});

test("npm owns the full self-rebuilding browser suite without make recursion", async () => {
  const browserRoot = await findBrowserRoot();
  const packageJson = JSON.parse(await readFile(path.join(browserRoot, "package.json"), "utf8"));
  const testScript = packageJson.scripts.test;
  for (const required of [
    "npm run build",
    "npm run check:generic-runtime",
    "node --test dist/tests/*.test.mjs",
    "make browser-fixture-app",
    "make browser-apps",
    "npm run publish:site",
    "playwright test",
  ]) {
    assert.ok(testScript.includes(required), `npm test must include ${required}`);
  }
  assert.ok(testScript.indexOf("make browser-fixture-app") < testScript.indexOf("make browser-apps"));
  assert.ok(testScript.indexOf("make browser-apps") < testScript.indexOf("npm run publish:site"));
  assert.ok(testScript.indexOf("npm run publish:site") < testScript.indexOf("playwright test"));
  assert.doesNotMatch(testScript, /\bmake\s+(?:-C\s+\S+\s+)?test\b/);

  const makefile = await readBrowserMakefile();
  assert.match(makefile, /^test:\n\tnpm test$/m);
  const testRecipe = makefile.match(/^test:[\s\S]*?(?=^\S|\z)/m)?.[0] ?? "";
  assert.doesNotMatch(testRecipe, /\$\(MAKE\)|npx playwright|browser-fixture-app|browser-apps|publish:site/);
});

test("first-party smoke fails closed when mandatory audio-input ABI interception is unavailable", async () => {
  const browserRoot = await findBrowserRoot();
  const smokeSpec = await readFile(path.join(browserRoot, "tests", "first-party-apps-smoke.spec.ts"), "utf8");
  assert.match(smokeSpec, /missing mandatory ABI-v4 audio input source export/);
  assert.match(smokeSpec, /typeof module\._synth_browser_set_audio_input_source !== "function"/);
  assert.doesNotMatch(smokeSpec, /_synth_browser_set_audio_input_source\?\./);
});

test("emscripten runtime facade exports string and persistence helpers", async () => {
  const builder = await readBuilder();
  assert.match(builder, /"stringToUTF8", "lengthBytesUTF8", "FS", "IDBFS", "HEAPU8", "HEAPF32"/);
  assert.match(builder, /"emscriptenRegisterAudioObject"/);
  assert.match(builder, /"-lidbfs\.js"/);
});

test("emscripten exports pre-creation browser contract version functions", async () => {
  const makefile = await readBuilder();
  for (const name of [
    "_synth_browser_abi_version",
    "_synth_browser_ui_protocol_version",
    "_synth_browser_runtime_config_version",
  ]) {
    assert.match(makefile, new RegExp(`\\"${name}\\"`));
  }
});

test("emscripten exports browser ABI v6 audio input functions", async () => {
  const makefile = await readBuilder();
  for (const name of [
    "_synth_browser_audio_input_channels",
    "_synth_browser_set_audio_input_source",
    "_synth_browser_clear_audio_input_source",
    "_synth_browser_consume_pending_audio_request",
  ]) {
    assert.match(makefile, new RegExp(`\\"${name}\\"`));
  }
});

test("emscripten browser builds enable pthreads for engine midi sender", async () => {
  const builder = await readBuilder();
  for (const flag of [
    "-pthread",
    "-sUSE_PTHREADS=1",
    "-sPTHREAD_POOL_SIZE=1",
    "-sINITIAL_MEMORY=536870912",
    "-sALLOW_MEMORY_GROWTH=1",
    "-sMAXIMUM_MEMORY=2147483648",
    "-sSTACK_SIZE=16777216",
    "-sAUDIO_WORKLET=1",
    "-sWASM_WORKERS=1",
  ]) {
    assert.ok(builder.includes(`"${flag}"`), `missing common compiler flag ${flag}`);
  }
});

test("cloudflare pages build script bootstraps emscripten before publishing", async () => {
  const browserRoot = await findBrowserRoot();
  const packageJson = JSON.parse(await readFile(path.join(browserRoot, "package.json"), "utf8"));
  assert.equal(packageJson.scripts["build:cloudflare-pages"], "bash scripts/cloudflare-pages-build.sh");

  const script = await readFile(path.join(browserRoot, "scripts/cloudflare-pages-build.sh"), "utf8");
  assert.match(script, /emsdk" install latest/);
  assert.match(script, /emsdk" activate latest/);
  assert.match(script, /npm .*run publish:site/);
  assert.match(script, /--catalog-source https:\/\/jvictor0\.github\.io\/Sheaf\/catalogs\/sheaf\/catalog\.json/);

  const makefile = await readBrowserMakefile();
  assert.match(makefile, /browser-apps-smoke:/);
  assert.equal(packageJson.scripts["publish:site"], "npm run build && node dist/src/publish-site.mjs");
  assert.equal(packageJson.scripts["compile:browser-apps"], "node dist/src/build-browser-apps.mjs");
  assert.equal(packageJson.scripts["compile:browser-fixture"],
    "node dist/src/build-browser-apps.mjs --manifest tests/fixtures/fake-browser-apps.json --allowed-source-root tests/fixtures/cpp --output-root dist/wasm/fixture-apps");
});

async function readBuilder() {
  return await readFile(path.join(await findBrowserRoot(), "src", "build-browser-apps.mjs"), "utf8");
}

async function readBrowserMakefile() {
  return await readFile(path.join(await findBrowserRoot(), "Makefile"), "utf8");
}

// static-server.mjs is loaded from two depths: `src/`, where Playwright
// launches it, and `dist/src/`, the compiled copy frogg3rs's serve-site.mjs
// imports for contentTypeForPath. A path resolved from one depth is wrong at
// the other, and the wrong one reached for dist/dist/src/protocol.js. That
// resolved on a machine carrying a stale nested dist and 404'd on a clean
// checkout, so it passed every local run and took the published site down.
// Both copies are loaded here, against the same tree the site is built from.
test("the compiled static server loads the way the site's own tooling loads it", async () => {
  const browserRoot = await findBrowserRoot();
  for (const relativePath of ["src/static-server.mjs", "dist/src/static-server.mjs"]) {
    const module = await import(pathToFileURL(path.join(browserRoot, relativePath)).href);
    assert.equal(
      typeof module.contentTypeForPath,
      "function",
      `${relativePath} did not load; a path it resolves is wrong at that depth`,
    );
  }
});

// tsc compiling its own output produces dist/dist, which is gitignored, so it
// exists only where someone has built before and never on a fresh checkout.
// While it is there it satisfies exactly the wrong lookups, which is what let
// the failure above stay invisible locally.
test("the build has not compiled its own output into a nested dist", async () => {
  const browserRoot = await findBrowserRoot();
  await assert.rejects(
    access(path.join(browserRoot, "dist", "dist")),
    "dist/dist exists: tsc has taken its own output as input, and it will answer lookups no clean checkout can",
  );
});

// The site packager copies dist/src/*.js into the browser runtime and drops
// .mjs, which it treats as Node-only tooling. That invariant is sound and
// invisible: a browser-fetched module importing a .mjs still typechecks,
// still runs under Node, and only fails once the page tries to load it --
// as 48 e2e timeouts rather than an error naming the file. So the import
// graph the browser actually fetches is walked here instead.
test("no browser-fetched module imports a Node-only .mjs", async () => {
  const browserRoot = await findBrowserRoot();
  const { browserRuntimeModules } = await import(
    pathToFileURL(path.join(browserRoot, "dist", "src", "publish-site.mjs")).href
  );
  const offenders = [];
  for (const moduleName of browserRuntimeModules) {
    if (!moduleName.endsWith(".js")) continue;
    const source = await readFile(path.join(browserRoot, "dist", "src", moduleName), "utf8");
    for (const [, specifier] of source.matchAll(/from\s+"([^"]+)"/g)) {
      if (specifier.endsWith(".mjs")) offenders.push(`${moduleName} -> ${specifier}`);
    }
  }
  assert.deepEqual(
    offenders,
    [],
    "these ship to the browser but import a module the packager does not copy",
  );
});
