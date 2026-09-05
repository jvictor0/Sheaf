import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import { createServer } from "node:http";
import { once } from "node:events";
import path from "node:path";
import test from "node:test";
import { promisify } from "node:util";
import { fileURLToPath } from "node:url";

import { parse } from "yaml";

const repositoryRoot = fileURLToPath(new URL("../../../../../", import.meta.url));
const workflowPath = path.join(repositoryRoot, ".github", "workflows", "synth-browser-pages.yml");
const browserReadmePath = path.join(repositoryRoot, "projects", "synth", "browser", "README.md");
const expectedCatalogVersion = "first-party-catalog-1";
const execFileAsync = promisify(execFile);

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

async function loadDeployedValidator() {
  try {
    return await import("../src/validate-deployed-catalog.mjs");
  } catch (error) {
    if (error?.code === "ERR_MODULE_NOT_FOUND") assert.fail("missing deployed catalog validator");
    throw error;
  }
}

async function deployedFixture(t, options = {}) {
  const appFixtures = [
    { appId: "miniapp", displayName: "Mini App", buildId: "miniapp-build-1", wasmMarker: 1 },
    { appId: "braid-4", displayName: "Braid 4", buildId: "braid-build-1", wasmMarker: 2 },
  ].map(({ appId, displayName, buildId, wasmMarker }) => {
    const entryBytes = Buffer.from(`export default async function ${appId.replace("-", "")}Entry() {}\n`);
    const wasmBytes = Buffer.from([0, 97, 115, 109, 1, 0, 0, wasmMarker]);
    const packagePrefix = `packages/${appId}/${buildId}`;
    const files = [
      {
        path: `${packagePrefix}/${appId}.js`,
        mediaType: "text/javascript",
        size: entryBytes.byteLength,
        sha256: sha256(entryBytes),
      },
      {
        path: `${packagePrefix}/${appId}.wasm`,
        mediaType: "application/wasm",
        size: wasmBytes.byteLength,
        sha256: sha256(wasmBytes),
      },
    ];
    return { appId, displayName, buildId, entryBytes, wasmBytes, packagePrefix, files };
  });
  const catalog = {
    schemaVersion: 1,
    catalogVersion: expectedCatalogVersion,
    publisher: { id: "sheaf", name: "Sheaf" },
    apps: appFixtures.map(({ appId, displayName, buildId, files }) => ({
      appId,
      displayName,
      author: "Sheaf",
      category: "Instrument",
      buildId,
      browser: {
        abiVersion: 6,
        uiProtocolVersion: 2,
        runtimeConfigVersion: 1,
        entry: files[0].path,
        files,
      },
    })),
  };
  options.mutateCatalog?.(catalog);
  const routes = new Map([["/catalog.json", {
    bytes: Buffer.from(`${JSON.stringify(catalog)}\n`),
    mediaType: "application/json",
  }]]);
  for (const fixture of appFixtures) {
    routes.set(`/${fixture.packagePrefix}/${fixture.appId}.js`, {
      bytes: fixture.entryBytes,
      mediaType: options.javascriptMediaType ?? "text/javascript",
    });
    routes.set(`/${fixture.packagePrefix}/${fixture.appId}.wasm`, {
      bytes: fixture.appId === "miniapp" && options.wasmBytes ? options.wasmBytes : fixture.wasmBytes,
      mediaType: fixture.appId === "miniapp" && options.wasmMediaType ? options.wasmMediaType : "application/wasm",
    });
  }
  const server = createServer((request, response) => {
    const pathname = new URL(request.url ?? "/", "http://localhost").pathname;
    const route = routes.get(pathname);
    if (!route) {
      response.writeHead(404).end();
      return;
    }
    const headers = {
      "Content-Type": route.mediaType,
      "Content-Length": route.bytes.byteLength,
    };
    if (pathname !== options.missingCorsPath) headers["Access-Control-Allow-Origin"] = "*";
    response.writeHead(200, headers).end(route.bytes);
  });
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  t.after(() => new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve())));
  const address = server.address();
  return `http://127.0.0.1:${address.port}/catalog.json`;
}

async function readWorkflow() {
  let source;
  try {
    source = await readFile(workflowPath, "utf8");
  } catch (error) {
    if (error?.code === "ENOENT") assert.fail(`missing workflow ${workflowPath}`);
    throw error;
  }
  return parse(source);
}

function uses(job) {
  return job.steps.filter((step) => step.uses).map((step) => step.uses);
}

function commands(job) {
  return job.steps.filter((step) => step.run).map((step) => step.run).join("\n");
}

test("workflow separates build, deploy, post-deploy validation, and cross-origin smoke responsibilities", async () => {
  const workflow = await readWorkflow();

  assert.deepEqual(Object.keys(workflow.jobs), ["build", "deploy", "validate", "smoke"]);
});

test("workflow is manually dispatchable and serializes default-branch publication", async () => {
  const workflow = await readWorkflow();

  assert.deepEqual(workflow.on.push.branches, ["main"]);
  assert.deepEqual(workflow.on.workflow_dispatch, {});
  assert.deepEqual(workflow.concurrency, {
    group: "synth-browser-pages",
    "cancel-in-progress": false,
  });
  assert.equal(
    workflow.jobs.deploy.if,
    "github.ref == format('refs/heads/{0}', github.event.repository.default_branch)",
  );
  assert.deepEqual(workflow.jobs.deploy.environment, {
    name: "github-pages",
    url: "${{ steps.deployment.outputs.page_url }}",
  });
});

test("every external action is an expected official action pinned to its verified full SHA", async () => {
  const workflow = await readWorkflow();
  const allUses = Object.values(workflow.jobs).flatMap(uses);

  assert.deepEqual([...new Set(allUses)].sort(), [
    "actions/checkout@9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0", // v7.0.0
    "actions/configure-pages@45bfe0192ca1faeb007ade9deae92b16b8254a0d", // v6.0.0
    "actions/deploy-pages@cd2ce8fcbc39b97be8ca5fce6e763baed58fa128", // v5.0.0
    "actions/setup-node@820762786026740c76f36085b0efc47a31fe5020", // v7.0.0
    "actions/upload-pages-artifact@fc324d3547104276b827a68afc52ff2a11cc49c9", // v5.0.0
    "emscripten-core/setup-emsdk@4528d102f7230f0e7b276855c01ea1159be0e984", // v16
  ].sort());
  for (const reference of allUses) assert.match(reference, /^[\w-]+\/[\w-]+@[0-9a-f]{40}$/);
});

test("jobs declare only their required token permissions", async () => {
  const { jobs } = await readWorkflow();

  assert.deepEqual(jobs.build.permissions, { contents: "read", pages: "read" });
  assert.deepEqual(jobs.deploy.permissions, { pages: "write", "id-token": "write" });
  assert.deepEqual(jobs.validate.permissions, { contents: "read" });
  assert.deepEqual(jobs.smoke.permissions, { contents: "read" });
});

test("build installs pinned toolchains, runs local gates, derives the catalog version, and uploads only the publisher artifact", async () => {
  const { build } = (await readWorkflow()).jobs;
  const buildUses = uses(build);
  const buildCommands = commands(build);
  const setupNode = build.steps.find(({ uses: reference }) => reference?.startsWith("actions/setup-node@"));
  const setupEmsdk = build.steps.find(({ uses: reference }) => reference?.startsWith("emscripten-core/setup-emsdk@"));
  const upload = build.steps.find(({ uses: reference }) => reference?.startsWith("actions/upload-pages-artifact@"));
  const localBrowserGate = build.steps.find(({ name }) => name === "Run local cross-origin publication gates");

  assert.deepEqual(setupNode.with, { "node-version": "22.17.1" });
  assert.deepEqual(setupEmsdk.with, { version: "6.0.3" });
  assert.equal(buildUses.some((reference) => reference.startsWith("actions/configure-pages@")), true);
  assert.match(buildCommands, /npm --prefix projects\/synth\/browser install --no-package-lock/);
  assert.match(buildCommands, /npm --prefix projects\/synth\/browser run build/);
  assert.match(buildCommands, /npm --prefix projects\/synth\/browser run check:generic-runtime/);
  assert.match(buildCommands, /node --test projects\/synth\/browser\/dist\/tests\/\*\.test\.mjs/);
  assert.match(buildCommands, /make -C projects\/synth\/browser browser-apps/);
  assert.doesNotMatch(buildCommands, /browser-fixture-app/);
  assert.match(buildCommands, /npm --prefix projects\/synth\/browser run publish:site/);
  assert.match(buildCommands, /npm --prefix projects\/synth\/browser run publish:pages/);
  assert.match(buildCommands, /tests\/two-origin-package\.spec\.ts/);
  assert.match(buildCommands, /tests\/deployed-origin\.spec\.ts/);
  assert.equal(localBrowserGate["working-directory"], "projects/synth/browser");
  assert.doesNotMatch(localBrowserGate.run, /--prefix/);
  assert.match(buildCommands, /catalogs\/sheaf\/catalog\.json/);
  assert.equal(build.outputs["catalog-version"], "${{ steps.publisher.outputs.catalog-version }}");
  assert.deepEqual(Object.keys(build.outputs), ["catalog-version"]);
  assert.deepEqual(upload.with, { path: "projects/synth/browser/dist/pages" });
});

test("deployment, validation, and smoke form a strict post-upload readiness chain", async () => {
  const { deploy, validate, smoke } = (await readWorkflow()).jobs;
  const deployment = deploy.steps.find(({ uses: reference }) => reference?.startsWith("actions/deploy-pages@"));
  const validateCommands = commands(validate);
  const smokeCommands = commands(smoke);
  const deployedBrowserGate = smoke.steps.find(({ name }) => name === "Run deployed-origin readiness smoke");

  assert.equal(deploy.needs, "build");
  assert.equal(deployment.id, "deployment");
  assert.equal(deploy.outputs["page-url"], "${{ steps.deployment.outputs.page_url }}");
  assert.deepEqual(validate.needs, ["build", "deploy"]);
  assert.match(validateCommands, /validate-deployed-catalog\.mjs/);
  assert.match(validateCommands, /--catalog-url "\$SYNTH_BROWSER_REMOTE_CATALOG_URL"/);
  assert.match(validateCommands, /--expected-catalog-version "\$SYNTH_BROWSER_EXPECTED_CATALOG_VERSION"/);
  assert.equal(validate.env.SYNTH_BROWSER_EXPECTED_CATALOG_VERSION, "${{ needs.build.outputs.catalog-version }}");
  assert.match(validate.env.SYNTH_BROWSER_REMOTE_CATALOG_URL, /needs\.deploy\.outputs\.page-url/);
  assert.equal(smoke.needs, "validate");
  assert.equal(smoke.env.SYNTH_BROWSER_EXPECTED_CATALOG_VERSION, "${{ needs.validate.outputs.catalog-version }}");
  assert.equal(smoke.env.SYNTH_BROWSER_REMOTE_CATALOG_URL, "${{ needs.validate.outputs.catalog-url }}");
  assert.match(smokeCommands, /playwright test tests\/deployed-origin\.spec\.ts/);
  assert.equal(deployedBrowserGate["working-directory"], "projects/synth/browser");
  assert.doesNotMatch(deployedBrowserGate.run, /--prefix/);
});

test("deployed validator accepts every file in a complete CORS-readable catalog", async (t) => {
  const catalogUrl = await deployedFixture(t);
  const { validateDeployedCatalog } = await loadDeployedValidator();

  const result = await validateDeployedCatalog({ catalogUrl, expectedCatalogVersion });

  assert.deepEqual(result, { catalogUrl, expectedCatalogVersion, appCount: 2, fileCount: 4 });
});

test("deployed validator accepts GitHub Pages application/javascript responses", async (t) => {
  const catalogUrl = await deployedFixture(t, { javascriptMediaType: "application/javascript" });
  const { validateDeployedCatalog } = await loadDeployedValidator();

  const result = await validateDeployedCatalog({ catalogUrl, expectedCatalogVersion });

  assert.deepEqual(result, { catalogUrl, expectedCatalogVersion, appCount: 2, fileCount: 4 });
});

test("deployed validator rejects a package response without CORS", async (t) => {
  const catalogUrl = await deployedFixture(t, {
    missingCorsPath: "/packages/miniapp/miniapp-build-1/miniapp.js",
  });
  const { validateDeployedCatalog } = await loadDeployedValidator();

  await assert.rejects(
    validateDeployedCatalog({ catalogUrl, expectedCatalogVersion }),
    /miniapp\.js.*Access-Control-Allow-Origin/i,
  );
});

test("deployed validator checks the second app's package responses too", async (t) => {
  const catalogUrl = await deployedFixture(t, {
    missingCorsPath: "/packages/braid-4/braid-build-1/braid-4.wasm",
  });
  const { validateDeployedCatalog } = await loadDeployedValidator();

  await assert.rejects(
    validateDeployedCatalog({ catalogUrl, expectedCatalogVersion }),
    /braid-4\.wasm.*Access-Control-Allow-Origin/i,
  );
});

test("deployed validator rejects incorrect live WASM MIME delivery", async (t) => {
  const catalogUrl = await deployedFixture(t, { wasmMediaType: "application/octet-stream" });
  const { validateDeployedCatalog } = await loadDeployedValidator();

  await assert.rejects(
    validateDeployedCatalog({ catalogUrl, expectedCatalogVersion }),
    /miniapp\.wasm.*application\/octet-stream.*application\/wasm/i,
  );
});

test("deployed validator rejects an unexpected whole catalog version", async (t) => {
  const catalogUrl = await deployedFixture(t);
  const { validateDeployedCatalog } = await loadDeployedValidator();

  await assert.rejects(
    validateDeployedCatalog({ catalogUrl, expectedCatalogVersion: "other-catalog" }),
    /catalog version.*other-catalog.*first-party-catalog-1/i,
  );
});

test("deployed validator rejects package references outside the app build root", async (t) => {
  const catalogUrl = await deployedFixture(t, {
    mutateCatalog(catalog) {
      catalog.apps[0].browser.files[1].path = "packages/miniapp/other-build/miniapp.wasm";
    },
  });
  const { validateDeployedCatalog } = await loadDeployedValidator();

  await assert.rejects(
    validateDeployedCatalog({ catalogUrl, expectedCatalogVersion }),
    /other-build.*outside immutable package root/i,
  );
});

test("deployed validator rejects a declared package size mismatch", async (t) => {
  const catalogUrl = await deployedFixture(t, {
    mutateCatalog(catalog) { catalog.apps[0].browser.files[0].size += 1; },
  });
  const { validateDeployedCatalog } = await loadDeployedValidator();

  await assert.rejects(
    validateDeployedCatalog({ catalogUrl, expectedCatalogVersion }),
    /miniapp\.js.*size.*expected/i,
  );
});

test("deployed validator rejects changed package bytes by exact SHA-256", async (t) => {
  const catalogUrl = await deployedFixture(t, { wasmBytes: Buffer.from([0, 97, 115, 109, 2, 0, 0, 0]) });
  const { validateDeployedCatalog } = await loadDeployedValidator();

  await assert.rejects(
    validateDeployedCatalog({ catalogUrl, expectedCatalogVersion }),
    /miniapp\.wasm.*SHA-256 mismatch/i,
  );
});

test("publisher documentation distinguishes one-time Pages setup, stable artifacts, and live-only evidence", async () => {
  const readme = await readFile(browserReadmePath, "utf8");

  assert.match(readme, /Settings.*Pages.*Source.*GitHub\s+Actions/is);
  assert.match(readme, /https:\/\/jvictor0\.github\.io\/Sheaf\/catalogs\/sheaf\/catalog\.json/);
  assert.match(readme, /catalogs and immutable packages only/i);
  assert.match(readme, /Access-Control-Allow-Origin:\s*\*/i);
  assert.match(readme, /application\/wasm/);
  assert.match(readme, /Cloudflare.*(?:COOP|Cross-Origin-Opener-Policy).*(?:COEP|Cross-Origin-Embedder-Policy).*Permissions-Policy/is);
  assert.match(
    readme,
    /\[current GitHub Pages limits\]\(https:\/\/docs\.github\.com\/en\/pages\/getting-started-with-github-pages\/github-pages-limits\)/i,
  );
  assert.match(readme, /published-site size.*deployment duration.*bandwidth.*rate limits/is);
  assert.match(readme, /local.*(?:cannot|do not).*prove.*live.*(?:CORS|MIME)|live.*(?:CORS|MIME).*only.*CI/is);
  assert.match(readme, /does not claim.*live.*Cloudflare/i);
});

test("compiled workflow contract passes when invoked from the repository root", async () => {
  if (process.env.SYNTH_BROWSER_ROOT_CWD_PROBE === "1") return;
  const childEnvironment = { ...process.env, SYNTH_BROWSER_ROOT_CWD_PROBE: "1" };
  delete childEnvironment.NODE_TEST_CONTEXT;

  await execFileAsync(process.execPath, ["--test", fileURLToPath(import.meta.url)], {
    cwd: repositoryRoot,
    env: childEnvironment,
  });
});
