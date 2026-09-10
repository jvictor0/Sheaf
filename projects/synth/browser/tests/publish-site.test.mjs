import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtemp, mkdir, readFile, readdir, rm, stat, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { pathToFileURL } from "node:url";
import { findBrowserRoot } from "./helpers/browser-root.mjs";

const browserRoot = await findBrowserRoot();
const [
  { readAppBuildManifest },
  { COMMAND_BUFFER_VERSION },
  { readExportedI32Constant },
  { buildFirstPartyCatalog },
  {
    browserRuntimeModules,
    cloudflareHeaders,
    publishedCatalogSource,
    publishPublisherArtifact,
    publishSite,
    validatePublishedSite,
  },
] = await Promise.all([
  "app-build-manifest.mjs",
  "protocol.js",
  "wasm-exports.mjs",
  "build-first-party-catalog.mjs",
  "publish-site.mjs",
].map((moduleName) => import(pathToFileURL(path.join(browserRoot, "dist", "src", moduleName)).href)));

const artifactRoles = Object.freeze({
  alpha: Object.freeze({
    entry: "apps/alpha/entry.mjs",
    wasm: "apps/alpha/engine.wasm",
    pthreadWorker: "apps/alpha/entry.mjs",
    wasmWorker: "apps/alpha/entry.mjs",
    audioWorklet: "apps/alpha/entry.mjs",
  }),
  beta: Object.freeze({
    entry: "apps/beta/loader.js",
    wasm: "apps/beta/runtime.wasm",
    pthreadWorker: "apps/beta/pthread.js",
    wasmWorker: "apps/beta/wasm-worker.mjs",
    audioWorklet: "apps/beta/worklet.js",
  }),
});


// A real (tiny) WebAssembly module exporting `synth_browser_ui_protocol_version`
// as a constant-returning function, so the publisher's per-package version
// assertion has something to read. The fixtures used to be eight arbitrary
// bytes, which meant nothing exercised that assertion at all -- and it is the
// one check standing between a stale Wasm build and a published catalog of apps
// the shell cannot decode (tasks.md 7.5a).
function leb128(value) {
  const bytes = [];
  let more = true;
  let current = value;
  while (more) {
    let byte = current & 0x7f;
    current >>= 7;
    if ((current === 0 && (byte & 0x40) === 0) || (current === -1 && (byte & 0x40) !== 0)) more = false;
    else byte |= 0x80;
    bytes.push(byte);
  }
  return bytes;
}

function uleb128(value) {
  const bytes = [];
  let current = value;
  do {
    let byte = current & 0x7f;
    current >>>= 7;
    if (current !== 0) byte |= 0x80;
    bytes.push(byte);
  } while (current !== 0);
  return bytes;
}

function wasmSection(id, payload) {
  return [id, ...uleb128(payload.length), ...payload];
}

function wasmString(text) {
  const bytes = [...Buffer.from(text, "utf8")];
  return [...uleb128(bytes.length), ...bytes];
}

function fixtureWasm({ uiProtocolVersion = 2, salt = "" } = {}) {
  const exportName = "synth_browser_ui_protocol_version";
  const typeSection = wasmSection(1, [0x01, 0x60, 0x00, 0x01, 0x7f]);
  const functionSection = wasmSection(3, [0x01, 0x00]);
  const exportSection = wasmSection(7, [0x01, ...wasmString(exportName), 0x00, 0x00]);
  const body = [0x00, 0x41, ...leb128(uiProtocolVersion), 0x0b];
  const codeSection = wasmSection(10, [0x01, ...uleb128(body.length), ...body]);
  const customSection = salt === "" ? [] : wasmSection(0, [...wasmString("salt"), ...Buffer.from(salt, "utf8")]);
  return Buffer.from([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...typeSection, ...functionSection, ...exportSection, ...codeSection, ...customSection,
  ]);
}

const emittedFiles = Object.freeze({
  "apps/alpha/engine.wasm": fixtureWasm({ salt: "alpha" }),
  "apps/alpha/entry.mjs": "export default async options => ({ app: 'alpha', options });\n",
  "apps/beta/loader.js": "export default async options => ({ app: 'beta', options });\n",
  "apps/beta/pthread.js": "postMessage('pthread');\n",
  "apps/beta/runtime.wasm": fixtureWasm({ salt: "beta" }),
  "apps/beta/wasm-worker.mjs": "postMessage('wasm-worker');\n",
  "apps/beta/worklet.js": "registerProcessor('fixture', class {});\n",
});

test("assembles a deterministic complete multi-app catalog from the exact matching emission report", async () => {
  const { browserRoot, root } = await createPublishFixture("catalog");
  const firstRoot = path.join(root, "catalog-one");
  const secondRoot = path.join(root, "catalog-two");

  const first = await buildFirstPartyCatalog({ browserRoot, outputRoot: firstRoot });
  const second = await buildFirstPartyCatalog({ browserRoot, outputRoot: secondRoot });

  assert.deepEqual(Object.keys(first), ["catalog", "packageRecords"]);
  assert.deepEqual(first.catalog.apps.map(({ appId }) => appId), ["alpha", "beta"]);
  assert.deepEqual(first.packageRecords.map(({ appId }) => appId), ["alpha", "beta"]);
  assert.notEqual(first.packageRecords[0].buildId, first.packageRecords[1].buildId);
  assert.equal(first.catalog.catalogVersion, catalogVersion(first.packageRecords));
  assert.deepEqual(first, second);
  assert.deepEqual(await snapshotTree(firstRoot), await snapshotTree(secondRoot));

  for (const app of first.catalog.apps) {
    assert.deepEqual({
      abiVersion: app.browser.abiVersion,
      uiProtocolVersion: app.browser.uiProtocolVersion,
      runtimeConfigVersion: app.browser.runtimeConfigVersion,
    }, { abiVersion: 6, uiProtocolVersion: 2, runtimeConfigVersion: 1 });
    for (const file of app.browser.files) {
      const emittedRelativePath = file.path.slice(`packages/${app.appId}/${app.buildId}/`.length);
      const sourceBytes = Buffer.from(emittedFiles[`apps/${app.appId}/${emittedRelativePath}`]);
      assert.equal(file.size, sourceBytes.byteLength, file.path);
      assert.equal(file.sha256, sha256(sourceBytes), file.path);
      assert.equal(file.mediaType,
        emittedRelativePath.endsWith(".wasm") ? "application/wasm" : "text/javascript");
      assert.deepEqual(
        await readFile(path.join(firstRoot, "catalogs", "sheaf", file.path)),
        sourceBytes,
      );
    }
  }

  await writeFile(path.join(browserRoot, "dist", "wasm", "apps", "beta", "runtime.wasm"),
    fixtureWasm({ salt: "beta-rebuilt" }));
  const changed = await buildFirstPartyCatalog({ browserRoot, outputRoot: path.join(root, "catalog-changed") });
  assert.equal(changed.packageRecords[0].buildId, first.packageRecords[0].buildId);
  assert.notEqual(changed.packageRecords[1].buildId, first.packageRecords[1].buildId);
  assert.notEqual(changed.catalog.catalogVersion, first.catalog.catalogVersion);
  assert.equal(changed.catalog.catalogVersion, catalogVersion(changed.packageRecords));
});

test("rejects a stale unrelated fixture report instead of packaging its output", async () => {
  const { browserRoot, root } = await createPublishFixture("stale-report");
  const outputRoot = path.join(root, "output");
  await writeFixture(outputRoot, { "known-good.txt": "preserve me\n" });
  await writeFixture(browserRoot, {
    "dist/wasm/fixture-apps/fake/fake.js": "throw new Error('fixture');\n",
    "dist/wasm/fixture-apps/fake/fake.wasm": "fixture wasm\n",
  });
  const reportPath = path.join(browserRoot, "dist", "wasm", "apps", "emissions.json");
  const report = JSON.parse(await readFile(reportPath, "utf8"));
  report.manifestDigest = "0".repeat(64);
  report.apps = [{ appId: "fake", artifacts: {
    entry: "fixture-apps/fake/fake.js",
    wasm: "fixture-apps/fake/fake.wasm",
    pthreadWorker: "fixture-apps/fake/fake.js",
    wasmWorker: "fixture-apps/fake/fake.js",
    audioWorklet: "fixture-apps/fake/fake.js",
  } }];
  await writeFile(reportPath, `${JSON.stringify(report, null, 2)}\n`);
  const before = await snapshotTree(outputRoot);

  await assert.rejects(
    buildFirstPartyCatalog({ browserRoot, outputRoot }),
    /manifest digest.*does not match|stale.*emission/i,
  );
  assert.deepEqual(await snapshotTree(outputRoot), before);
});

test("rejects an emission record that aliases another app's dedicated output directory", async () => {
  const { browserRoot, root } = await createPublishFixture("aliased-emission");
  const reportPath = path.join(browserRoot, "dist", "wasm", "apps", "emissions.json");
  const report = JSON.parse(await readFile(reportPath, "utf8"));
  report.apps[1].artifacts = { ...report.apps[0].artifacts };

  await writeFile(reportPath, `${JSON.stringify(report, null, 2)}\n`);
  await assert.rejects(
    buildFirstPartyCatalog({ browserRoot, outputRoot: path.join(root, "output") }),
    /beta.*dedicated emission directory.*apps\/beta|apps\/alpha.*beta/i,
  );
});

test("rejects missing and undeclared files without replacing the complete site", async () => {
  for (const [name, mutate, pattern] of [
    ["missing", async (browserRoot) => {
      await rm(path.join(browserRoot, "dist", "wasm", "apps", "beta", "runtime.wasm"));
    }, /runtime\.wasm.*(?:missing|not.*inventoried)|artifacts\.wasm.*not.*inventoried/i],
    ["extra", async (browserRoot) => {
      await writeFile(path.join(browserRoot, "dist", "wasm", "apps", "beta", "stale.js"), "stale\n");
    }, /unexpected.*stale\.js|stale\.js.*not.*role/i],
  ]) {
    const { browserRoot, publishRoot } = await createPublishFixture(`atomic-${name}`);
    await writeFixture(publishRoot, {
      "known-good/catalog.json": "previous catalog\n",
      "known-good/package.wasm": "previous package\n",
    });
    const before = await snapshotTree(publishRoot);
    await mutate(browserRoot);

    await assert.rejects(publishSite({ browserRoot, publishRoot }), pattern);
    assert.deepEqual(await snapshotTree(publishRoot), before, name);
  }
});

test("publishes launcher assets, both packages, and one generic rollback page per app", async () => {
  const { browserRoot, publishRoot } = await createPublishFixture("complete-site");

  const result = await publishSite({ browserRoot, publishRoot });

  assert.equal(result.publishRoot, publishRoot);
  for (const relativePath of [
    "index.html",
    "synth-browser.css",
    "catalog-sources.json",
    "catalogs/sheaf/catalog.json",
    "dist/src/main.js",
    "dist/src/worker.js",
    "dist/src/package-loader.js",
    "rollback/apps/alpha/index.html",
    "rollback/apps/beta/index.html",
    "_headers",
  ]) await assertFile(path.join(publishRoot, relativePath));
  await assert.rejects(stat(path.join(publishRoot, "rollback", "direct-miniapp")), { code: "ENOENT" });

  const sourceList = JSON.parse(await readFile(path.join(publishRoot, "catalog-sources.json"), "utf8"));
  assert.deepEqual(sourceList, ["catalogs/sheaf/catalog.json"]);
  assert.deepEqual(
    JSON.parse(await readFile(path.join(browserRoot, "catalog-sources.json"), "utf8")),
    ["catalogs/sheaf/catalog.json"],
    "the checked-in source list remains suitable for localhost development",
  );
  const catalog = JSON.parse(await readFile(path.join(publishRoot, "catalogs", "sheaf", "catalog.json"), "utf8"));
  assert.deepEqual(catalog.apps.map(({ appId }) => appId), ["alpha", "beta"]);
  assert.equal(catalog.catalogVersion, catalogVersion(catalog.apps));

  for (const app of catalog.apps) {
    for (const file of app.browser.files) {
      const filename = path.join(publishRoot, "catalogs", "sheaf", file.path);
      await assertFile(filename);
      assert.equal(file.sha256, sha256(await readFile(filename)), file.path);
    }
    const rollbackHtml = await readFile(path.join(publishRoot, "rollback", "apps", app.appId, "index.html"), "utf8");
    assert.match(rollbackHtml, /installSynthBrowserApp/);
    assert.match(rollbackHtml, /\.\.\/\.\.\/\.\.\/dist\/src\/main\.js/);
    assert.match(rollbackHtml, new RegExp(JSON.stringify(app.appId).replace(/[.*+?^${}()|[\]\\]/g, "\\$&")));
    assert.match(rollbackHtml, new RegExp(app.browser.entry.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")));
  }

  assert.deepEqual((await readdir(path.join(publishRoot, "dist", "src"))).sort(), [...browserRuntimeModules].sort());
  const headers = await readFile(path.join(publishRoot, "_headers"), "utf8");
  assert.equal(headers, cloudflareHeaders);
  assert.match(headers, /^  Permissions-Policy: midi=\(self\), microphone=\(self\)$/m);
  assert.match(headers, /packages\/\*\/\*\/\*\.wasm\n\s+Content-Type: application\/wasm/);
  assert.match(headers, /packages\/\*\/\*\/\*\.js\n\s+Content-Type: text\/javascript/);
  assert.doesNotMatch(headers, /direct-miniapp|rollback\/apps\/[a-z0-9-]+/);

  const validated = await validatePublishedSite({ publishRoot });
  assert.deepEqual(validated.catalog.apps.map(({ appId }) => appId), ["alpha", "beta"]);
});

test("publishes a Cloudflare launcher with an explicitly configured remote catalog source", async () => {
  const { browserRoot, publishRoot } = await createPublishFixture("remote-source");

  await publishSite({ browserRoot, publishRoot, catalogSource: publishedCatalogSource });

  assert.deepEqual(
    JSON.parse(await readFile(path.join(publishRoot, "catalog-sources.json"), "utf8")),
    [publishedCatalogSource],
  );
  assert.deepEqual(
    JSON.parse(await readFile(path.join(browserRoot, "catalog-sources.json"), "utf8")),
    ["catalogs/sheaf/catalog.json"],
    "the checked-in source list remains suitable for localhost development",
  );
  const validated = await validatePublishedSite({ publishRoot, catalogSource: publishedCatalogSource });
  assert.deepEqual(validated.catalog.apps.map(({ appId }) => appId), ["alpha", "beta"]);
});

test("site validation independently requires the production microphone permissions policy", async () => {
  const { browserRoot, publishRoot } = await createPublishFixture("missing-microphone-policy");
  await publishSite({ browserRoot, publishRoot });
  const headersPath = path.join(publishRoot, "_headers");
  const headers = await readFile(headersPath, "utf8");
  await writeFile(headersPath, headers.replace("microphone=(self)", "microphone=()"));

  await assert.rejects(
    validatePublishedSite({ publishRoot }),
    /Permissions-Policy: midi=\(self\), microphone=\(self\)/,
  );
});

test("publishing identical inputs twice produces byte-identical complete trees", async () => {
  const { browserRoot, root } = await createPublishFixture("deterministic-site");
  const first = path.join(root, "site-one");
  const second = path.join(root, "site-two");

  await publishSite({ browserRoot, publishRoot: first });
  await publishSite({ browserRoot, publishRoot: second });

  assert.deepEqual(await snapshotTree(first), await snapshotTree(second));
});

test("publishes only the complete two-app catalog tree to Pages and replaces it atomically", async () => {
  const { browserRoot, publishRoot, root } = await createPublishFixture("pages");
  const pagesRoot = path.join(root, "pages");
  await publishSite({ browserRoot, publishRoot });
  await publishPublisherArtifact({ publishRoot, pagesRoot });

  const catalog = JSON.parse(await readFile(path.join(pagesRoot, "catalogs", "sheaf", "catalog.json"), "utf8"));
  const expectedPaths = ["catalogs/sheaf/catalog.json"];
  for (const app of catalog.apps) {
    for (const file of app.browser.files) expectedPaths.push(`catalogs/sheaf/${file.path}`);
  }
  expectedPaths.sort();
  assert.deepEqual((await snapshotTree(pagesRoot)).map(([relativePath]) => relativePath), expectedPaths);
  for (const forbidden of ["index.html", "catalog-sources.json", "_headers", "rollback", "dist"])
    await assert.rejects(stat(path.join(pagesRoot, forbidden)), { code: "ENOENT" });

  const before = await snapshotTree(pagesRoot);
  const secondApp = catalog.apps[1];
  const secondWasm = secondApp.browser.files.find(({ mediaType }) => mediaType === "application/wasm");
  await writeFile(path.join(publishRoot, "catalogs", "sheaf", secondWasm.path), "changed second app bytes\n");
  await assert.rejects(
    publishPublisherArtifact({ publishRoot, pagesRoot }),
    /beta.*(?:size|SHA-256)|(?:size|SHA-256).*beta/i,
  );
  assert.deepEqual(await snapshotTree(pagesRoot), before);
});

test("site validation rejects changed bytes in either immutable package", async () => {
  const { browserRoot, publishRoot } = await createPublishFixture("invalid-package");
  await publishSite({ browserRoot, publishRoot });
  const catalog = JSON.parse(await readFile(path.join(publishRoot, "catalogs", "sheaf", "catalog.json"), "utf8"));
  const file = catalog.apps[1].browser.files.find(({ mediaType }) => mediaType === "application/wasm");
  await writeFile(path.join(publishRoot, "catalogs", "sheaf", file.path), "changed bytes\n");

  await assert.rejects(validatePublishedSite({ publishRoot }), /beta.*(?:size|SHA-256)|(?:size|SHA-256).*beta/i);
});

// tasks.md 7.5 and 7.5a. 7.5 asks the whole-catalog publication to confirm a
// stale-version package fails with the explicit version error, and 7.5a asks the
// assertion to read the artifact rather than trust build order. These three
// cases are that pair, one direction each.
test("refuses to publish a package compiled against a stale UI protocol version", async () => {
  const { browserRoot, publishRoot } = await createPublishFixture("stale-ui-protocol");
  await writeFile(path.join(browserRoot, "dist", "wasm", "apps", "beta", "runtime.wasm"),
    fixtureWasm({ uiProtocolVersion: 1, salt: "beta" }));

  await assert.rejects(
    publishSite({ browserRoot, publishRoot }),
    /beta.*UI protocol version 1|UI protocol version 1.*beta/i,
  );
  await assert.rejects(stat(path.join(publishRoot, "catalogs")), { code: "ENOENT" });
});

test("refuses to publish a package whose module advertises no UI protocol version", async () => {
  const { browserRoot, publishRoot } = await createPublishFixture("absent-ui-protocol");
  await writeFile(path.join(browserRoot, "dist", "wasm", "apps", "alpha", "engine.wasm"),
    Buffer.from([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]));

  await assert.rejects(
    publishSite({ browserRoot, publishRoot }),
    /alpha.*exports no synth_browser_ui_protocol_version/i,
  );
});

test("publishes when every package module reports the runtime's own UI protocol version", async () => {
  const { browserRoot, publishRoot } = await createPublishFixture("matching-ui-protocol");
  const { catalog } = await publishSite({ browserRoot, publishRoot })
    .then(() => validatePublishedSite({ publishRoot }));

  // Anti-vacuity: the assertion above passes trivially if the catalog has no
  // apps, or if their declared version were something other than the version
  // this runtime speaks.
  assert.equal(catalog.apps.length, 2);
  for (const app of catalog.apps) {
    assert.equal(app.browser.uiProtocolVersion, COMMAND_BUFFER_VERSION);
    const wasmFiles = app.browser.files.filter(({ mediaType }) => mediaType === "application/wasm");
    assert.equal(wasmFiles.length, 1, app.appId);
    const bytes = new Uint8Array(await readFile(
      path.join(publishRoot, "catalogs", catalog.publisher.id, wasmFiles[0].path)));
    assert.equal(
      readExportedI32Constant(bytes, "synth_browser_ui_protocol_version"),
      COMMAND_BUFFER_VERSION,
      app.appId,
    );
  }
});

async function createPublishFixture(name) {
  const root = await mkdtemp(path.join(os.tmpdir(), `synth-browser-publish-${name}-`));
  const browserRoot = path.join(root, "browser");
  const publishRoot = path.join(root, "site");
  await writeFixture(root, {
    "apps/alpha/Alpha.hpp": "#pragma once\n",
    "apps/beta/Beta.hpp": "#pragma once\n",
  });
  await writeFixture(browserRoot, {
    "catalog-sources.json": `${JSON.stringify(["catalogs/sheaf/catalog.json"])}\n`,
    "first-party-apps.json": `${JSON.stringify({
      schemaVersion: 1,
      publisher: { id: "sheaf", name: "Sheaf" },
      apps: [
        {
          appId: "beta",
          displayName: "Beta",
          author: "Fixture",
          category: "Test",
          header: "Beta.hpp",
          cppType: "fixture::Beta",
          includeDirs: ["../apps/beta"],
        },
        {
          appId: "alpha",
          displayName: "Alpha",
          author: "Fixture",
          category: "Test",
          header: "Alpha.hpp",
          cppType: "fixture::Alpha",
          includeDirs: ["../apps/alpha"],
        },
      ],
    }, null, 2)}\n`,
    "public/index.html": "<!doctype html><main id=\"synth-root\" data-synth-launcher=\"true\" data-synth-catalog-sources=\"/catalog-sources.json\"></main><script type=\"module\" src=\"./dist/src/main.js\"></script>\n",
    "public/synth-browser.css": "body { margin: 0; }\n",
    ...Object.fromEntries(browserRuntimeModules.map((moduleName) =>
      [`dist/src/${moduleName}`, `export const fixture = ${JSON.stringify(moduleName)};\n`])),
    ...Object.fromEntries(Object.entries(emittedFiles).map(([relativePath, contents]) =>
      [`dist/wasm/${relativePath}`, contents])),
  });
  const manifest = await readAppBuildManifest({ browserRoot });
  const report = {
    schemaVersion: 1,
    manifestDigest: manifest.digest,
    publisher: manifest.publisher,
    apps: manifest.apps.map(({ appId }) => ({ appId, artifacts: artifactRoles[appId] })),
  };
  await writeFixture(browserRoot, {
    "dist/wasm/apps/emissions.json": `${JSON.stringify(report, null, 2)}\n`,
  });
  return { root, browserRoot, publishRoot };
}

async function writeFixture(root, files) {
  for (const [relativePath, contents] of Object.entries(files)) {
    const filename = path.join(root, relativePath);
    await mkdir(path.dirname(filename), { recursive: true });
    await writeFile(filename, contents);
  }
}

async function assertFile(filename) {
  assert.equal((await stat(filename)).isFile(), true, filename);
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function catalogVersion(records) {
  const identities = records.map(({ appId, buildId }) => ({ appId, buildId }));
  return `first-party-${sha256(JSON.stringify(identities))}`;
}

async function snapshotTree(root) {
  const snapshot = [];
  async function visit(directory, prefix) {
    const entries = await readdir(directory, { withFileTypes: true });
    entries.sort((left, right) => left.name < right.name ? -1 : left.name > right.name ? 1 : 0);
    for (const entry of entries) {
      const relativePath = prefix ? `${prefix}/${entry.name}` : entry.name;
      const filename = path.join(directory, entry.name);
      if (entry.isDirectory()) await visit(filename, relativePath);
      else snapshot.push([relativePath, sha256(await readFile(filename))]);
    }
  }
  await visit(root, "");
  return snapshot;
}
