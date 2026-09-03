import { createHash } from "node:crypto";
import { cp, mkdir, mkdtemp, readFile, readdir, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { buildFirstPartyCatalog, CANONICAL_CATALOG_PATH } from "./build-first-party-catalog.mjs";
import { parseCatalog, parseCatalogSources } from "./catalog.js";
import { COMMAND_BUFFER_VERSION } from "./protocol.js";
import { readExportedI32Constant } from "./wasm-exports.mjs";

export const browserRuntimeModules = Object.freeze([
  "activation.js",
  "audio-input-limits.js",
  "audio.js",
  "catalog-client.js",
  "catalog.js",
  "launcher.js",
  "main.js",
  "midi.js",
  "package-loader.js",
  "persistence.js",
  // Sorts before protocol.js and is imported by it: the versions live in a
  // module Node can load from source, so a .js needs nothing built first. The extension is load-bearing: the site
// packager copies dist/src/*.js into the browser runtime and drops .mjs as
// Node-only tooling, so a module the browser fetches cannot be one.
  "protocol-versions.js",
  "protocol.js",
  "ui.js",
  "worker.js",
]);

export const cloudflareHeaders = `/*
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
  Permissions-Policy: midi=(self), microphone=(self)

/catalogs/sheaf/packages/*/*/*.wasm
  Content-Type: application/wasm

/catalogs/sheaf/packages/*/*/*.js
  Content-Type: text/javascript
`;

const requiredCloudflarePermissionsPolicy = "Permissions-Policy: midi=(self), microphone=(self)";

export const publishedCatalogSource = "https://jvictor0.github.io/Sheaf/catalogs/sheaf/catalog.json";

function defaultBrowserRoot() {
  const directory = path.dirname(fileURLToPath(import.meta.url));
  return path.basename(path.dirname(directory)) === "dist"
    ? path.resolve(directory, "..", "..")
    : path.resolve(directory, "..");
}

async function assertExists(filename, relativePath, { nonempty = false } = {}) {
  let metadata;
  try {
    metadata = await stat(filename);
  } catch (error) {
    if (error?.code === "ENOENT") throw new Error(`Missing required browser publish artifact: ${relativePath}`);
    throw error;
  }
  if (nonempty && metadata.isFile() && metadata.size === 0)
    throw new Error(`Required browser publish artifact is empty: ${relativePath}`);
  return metadata;
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function compareCodeUnits(left, right) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function expectedCatalogVersion(apps) {
  const identities = apps.map(({ appId, buildId }) => ({ appId, buildId }));
  return `first-party-${sha256(JSON.stringify(identities))}`;
}

function pathFromDeploymentUrl(url, deploymentOrigin) {
  const parsed = new URL(url);
  if (parsed.origin !== deploymentOrigin) throw new Error(`Published reference leaves deployment origin: ${url}`);
  return parsed.pathname.replace(/^\/+/, "");
}

async function inventoryFiles(root, prefix = "") {
  const files = [];
  const entries = await readdir(root, { withFileTypes: true });
  entries.sort((left, right) => compareCodeUnits(left.name, right.name));
  for (const entry of entries) {
    const relativePath = prefix ? `${prefix}/${entry.name}` : entry.name;
    const filename = path.join(root, entry.name);
    if (entry.isDirectory()) files.push(...await inventoryFiles(filename, relativePath));
    else if (entry.isFile()) files.push(relativePath);
    else throw new Error(`Published artifact is not a regular file: ${relativePath}`);
  }
  return files;
}

function rollbackHtml(catalog, app) {
  const prefix = `packages/${app.appId}/${app.buildId}/`;
  const packagePaths = {};
  for (const file of app.browser.files) {
    const relativePath = file.path.slice(prefix.length);
    const deploymentPath = `../../../catalogs/${catalog.publisher.id}/${file.path}`;
    packagePaths[relativePath] = deploymentPath;
    packagePaths[path.posix.basename(relativePath)] = deploymentPath;
  }
  const entryRelativePath = app.browser.entry.slice(prefix.length);
  const runtimeIdentity = {
    publisherId: catalog.publisher.id,
    appId: app.appId,
    runtimeConfigVersion: app.browser.runtimeConfigVersion,
  };
  return `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>${app.displayName} rollback</title>
    <link rel="stylesheet" href="../../../synth-browser.css">
  </head>
  <body>
    <main id="synth-root"></main>
    <script type="module">
      import { installSynthBrowserApp } from "../../../dist/src/main.js";

      const root = document.querySelector("#synth-root");
      const packagePaths = ${JSON.stringify(packagePaths, null, 6)};
      const locateFile = Object.fromEntries(Object.entries(packagePaths).map(
        ([requestedPath, deploymentPath]) => [requestedPath, new URL(deploymentPath, import.meta.url).href],
      ));
      const entryUrl = locateFile[${JSON.stringify(entryRelativePath)}];
      installSynthBrowserApp(root, {
        module: { entryUrl, locateFile, mainScriptUrlOrBlob: entryUrl },
        runtimeIdentity: ${JSON.stringify(runtimeIdentity)},
      }).catch((error) => {
        root.dataset.synthStatus = error instanceof Error ? error.message : "rollback startup failed";
      });
    </script>
  </body>
</html>
`;
}

// tasks.md 7.5a. A whole-catalog publication ships apps the shipped runtime has
// to be able to decode, and the only thing that decides that is the version
// compiled into each module -- not the catalog metadata, which is written from
// the manifest and would happily describe a stale build as current. Version 2
// made this concrete: a stale package's encoder met the amended decoder and
// threw `invalid presence flag`, and nothing upstream of the browser noticed.
//
// So every published `.wasm` is read. At least one module per app must export
// the accessor (an app whose packages all stopped exporting it would otherwise
// satisfy an "every exported value is 2" check by exporting none), and every
// value found must equal both the catalog's declared version and the version
// this runtime's own decoder implements.
async function assertPackageUiProtocolVersion(publishRoot, catalog, app) {
  const wasmFiles = app.browser.files.filter(({ mediaType }) => mediaType === "application/wasm");
  let found = 0;
  for (const file of wasmFiles) {
    const relativePath = `catalogs/${catalog.publisher.id}/${file.path}`;
    const bytes = new Uint8Array(await readFile(path.join(publishRoot, relativePath)));
    let version;
    try {
      version = readExportedI32Constant(bytes, "synth_browser_ui_protocol_version");
    } catch (error) {
      throw new Error(
        `Package ${app.appId} module ${file.path} does not expose a readable UI protocol version: ` +
        `${error instanceof Error ? error.message : String(error)}`,
      );
    }
    if (version === undefined) continue;
    ++found;
    if (version !== app.browser.uiProtocolVersion)
      throw new Error(
        `Package ${app.appId} module ${file.path} was compiled against UI protocol version ${version}, ` +
        `but its catalog record declares ${app.browser.uiProtocolVersion}. Rebuild the app packages ` +
        `(make -C projects/synth/browser browser-apps) before publishing.`,
      );
    if (version !== COMMAND_BUFFER_VERSION)
      throw new Error(
        `Package ${app.appId} module ${file.path} was compiled against UI protocol version ${version}, ` +
        `but this runtime speaks version ${COMMAND_BUFFER_VERSION}. Publishing it would ship an app the ` +
        `shell rejects with no rendered frame.`,
      );
  }
  if (found === 0)
    throw new Error(
      `Package ${app.appId} exports no synth_browser_ui_protocol_version from any of its ` +
      `${wasmFiles.length} WebAssembly module(s); its UI protocol version cannot be verified.`,
    );
}

export async function validatePublishedSite({ publishRoot, catalogSource } = {}) {
  for (const relativePath of [
    "index.html",
    "synth-browser.css",
    "catalog-sources.json",
    CANONICAL_CATALOG_PATH,
    "dist/src/main.js",
    "dist/src/worker.js",
    "dist/src/package-loader.js",
    "_headers",
  ]) await assertExists(path.join(publishRoot, relativePath), relativePath, { nonempty: true });

  const rootHtml = await readFile(path.join(publishRoot, "index.html"), "utf8");
  if (!/data-synth-launcher="true"/.test(rootHtml)) throw new Error("Published root index.html is not a catalog launcher");
  if (/data-synth-auto|rollback\/apps\//i.test(rootHtml))
    throw new Error("Published root index.html contains a direct application dependency");

  const deploymentBase = "https://deployment.invalid/catalog-sources.json";
  const sources = parseCatalogSources(
    JSON.parse(await readFile(path.join(publishRoot, "catalog-sources.json"), "utf8")),
    deploymentBase,
  );
  const localCatalogUrl = new URL(CANONICAL_CATALOG_PATH, deploymentBase).href;
  const expectedCatalogSource = catalogSource === undefined
    ? localCatalogUrl
    : parseCatalogSources([catalogSource], deploymentBase)[0].catalogUrl;
  if (sources[0]?.catalogUrl !== expectedCatalogSource)
    throw new Error(`First catalog reference must resolve to ${expectedCatalogSource}; received ${String(sources[0]?.catalogUrl)}`);
  const catalog = parseCatalog(
    JSON.parse(await readFile(path.join(publishRoot, CANONICAL_CATALOG_PATH), "utf8")),
    localCatalogUrl,
  );
  if (catalog.publisher.id !== "sheaf") throw new Error("First-party catalog publisher must be sheaf");
  const orderedAppIds = catalog.apps.map(({ appId }) => appId);
  if (orderedAppIds.some((appId, index) => index > 0 && compareCodeUnits(orderedAppIds[index - 1], appId) >= 0))
    throw new Error("First-party catalog apps must be in deterministic appId order");
  const expectedVersion = expectedCatalogVersion(catalog.apps);
  if (catalog.catalogVersion !== expectedVersion)
    throw new Error(`First-party catalog version mismatch: expected ${expectedVersion}, received ${catalog.catalogVersion}`);

  const expectedPackageFiles = [];
  for (const app of catalog.apps) {
    const packagePaths = new Set(app.browser.files.map(({ path: filePath }) => filePath));
    if (!packagePaths.has(app.browser.entry)) throw new Error(`Catalog entry ${app.browser.entry} is not a package file`);
    for (const file of app.browser.files) {
      const relativePath = pathFromDeploymentUrl(file.url, new URL(deploymentBase).origin);
      const expectedPrefix = `catalogs/${catalog.publisher.id}/packages/${app.appId}/${app.buildId}/`;
      if (!relativePath.startsWith(expectedPrefix))
        throw new Error(`Catalog package reference ${relativePath} is outside immutable first-party package ${expectedPrefix}`);
      expectedPackageFiles.push(file.path);
      const filename = path.join(publishRoot, relativePath);
      const metadata = await assertExists(filename, relativePath, { nonempty: true });
      if (metadata.size !== file.size)
        throw new Error(`Package file ${file.path} size mismatch: expected ${file.size}, received ${metadata.size}`);
      const actualDigest = sha256(await readFile(filename));
      if (actualDigest !== file.sha256)
        throw new Error(`Package file ${file.path} SHA-256 mismatch: expected ${file.sha256}, received ${actualDigest}`);
      if (file.mediaType === "application/wasm" && !relativePath.endsWith(".wasm"))
        throw new Error(`Package file ${file.path} has inconsistent WASM media type`);
      if (file.mediaType === "text/javascript" && !/\.(?:m?js)$/.test(relativePath))
        throw new Error(`Package file ${file.path} has inconsistent JavaScript media type`);
    }
    await assertPackageUiProtocolVersion(publishRoot, catalog, app);
  }
  const packageRoot = path.join(publishRoot, "catalogs", catalog.publisher.id, "packages");
  const actualPackageFiles = (await inventoryFiles(packageRoot)).map((relativePath) => `packages/${relativePath}`);
  expectedPackageFiles.sort(compareCodeUnits);
  if (JSON.stringify(actualPackageFiles) !== JSON.stringify(expectedPackageFiles))
    throw new Error("Published package tree contains missing or undeclared files");

  const rollbackRoot = path.join(publishRoot, "rollback", "apps");
  const rollbackEntries = await readdir(rollbackRoot, { withFileTypes: true });
  const rollbackIds = rollbackEntries.map(({ name }) => name).sort(compareCodeUnits);
  if (rollbackEntries.some((entry) => !entry.isDirectory()) ||
      JSON.stringify(rollbackIds) !== JSON.stringify([...orderedAppIds].sort(compareCodeUnits))) {
    throw new Error("Rollback app pages do not match the complete first-party catalog");
  }
  for (const app of catalog.apps) {
    const relativePath = `rollback/apps/${app.appId}/index.html`;
    await assertExists(path.join(publishRoot, relativePath), relativePath, { nonempty: true });
    const rollbackFiles = await inventoryFiles(path.join(rollbackRoot, app.appId));
    if (rollbackFiles.length !== 1 || rollbackFiles[0] !== "index.html")
      throw new Error(`Rollback page ${app.appId} contains undeclared files`);
  }

  const headers = await readFile(path.join(publishRoot, "_headers"), "utf8");
  if (!headers.split(/\r?\n/u).some((line) => line.trim() === requiredCloudflarePermissionsPolicy))
    throw new Error(`Published _headers is missing ${requiredCloudflarePermissionsPolicy}`);
  if (headers !== cloudflareHeaders) throw new Error("Published _headers does not match the Cloudflare runtime policy");
  return Object.freeze({ catalog, apps: catalog.apps });
}

async function replaceDestination(stagingRoot, publishRoot) {
  const parent = path.dirname(publishRoot);
  const backupRoot = await mkdtemp(path.join(parent, `.${path.basename(publishRoot)}.previous-`));
  await rm(backupRoot, { recursive: true, force: true });
  let movedPrevious = false;
  try {
    try {
      await rename(publishRoot, backupRoot);
      movedPrevious = true;
    } catch (error) {
      if (error?.code !== "ENOENT") throw error;
    }
    await rename(stagingRoot, publishRoot);
    if (movedPrevious) await rm(backupRoot, { recursive: true, force: true });
  } catch (error) {
    if (movedPrevious) {
      await rm(publishRoot, { recursive: true, force: true });
      await rename(backupRoot, publishRoot);
    }
    throw error;
  }
}

export async function publishSite({
  browserRoot = defaultBrowserRoot(),
  publishRoot = path.join(browserRoot, "dist", "site"),
  catalogBuilder = buildFirstPartyCatalog,
  catalogSource,
} = {}) {
  for (const relativePath of [
    "public/index.html",
    "public/synth-browser.css",
    "catalog-sources.json",
    "first-party-apps.json",
    "dist/wasm/apps/emissions.json",
  ]) await assertExists(path.join(browserRoot, relativePath), relativePath, { nonempty: true });
  for (const moduleName of browserRuntimeModules)
    await assertExists(path.join(browserRoot, "dist", "src", moduleName), `dist/src/${moduleName}`, { nonempty: true });

  await mkdir(path.dirname(publishRoot), { recursive: true });
  const stagingRoot = await mkdtemp(path.join(path.dirname(publishRoot), `.${path.basename(publishRoot)}.stage-`));
  let staged = true;
  try {
    await cp(path.join(browserRoot, "public"), stagingRoot, { recursive: true });
    await mkdir(path.join(stagingRoot, "dist", "src"), { recursive: true });
    await Promise.all(browserRuntimeModules.map((moduleName) => cp(
      path.join(browserRoot, "dist", "src", moduleName),
      path.join(stagingRoot, "dist", "src", moduleName),
    )));
    const { catalog } = await catalogBuilder({ browserRoot, outputRoot: stagingRoot });
    if (catalogSource !== undefined) {
      await writeFile(
        path.join(stagingRoot, "catalog-sources.json"),
        `${JSON.stringify([catalogSource], null, 2)}\n`,
      );
    }

    for (const app of catalog.apps) {
      const rollbackRoot = path.join(stagingRoot, "rollback", "apps", app.appId);
      await mkdir(rollbackRoot, { recursive: true });
      await writeFile(path.join(rollbackRoot, "index.html"), rollbackHtml(catalog, app));
    }
    await writeFile(path.join(stagingRoot, "_headers"), cloudflareHeaders);
    await validatePublishedSite({ publishRoot: stagingRoot, catalogSource });
    await replaceDestination(stagingRoot, publishRoot);
    staged = false;
    return Object.freeze({ publishRoot });
  } finally {
    if (staged) await rm(stagingRoot, { recursive: true, force: true });
  }
}

export async function publishPublisherArtifact({
  browserRoot = defaultBrowserRoot(),
  publishRoot = path.join(browserRoot, "dist", "site"),
  pagesRoot = path.join(browserRoot, "dist", "pages"),
} = {}) {
  const { catalog } = await validatePublishedSite({ publishRoot });
  await mkdir(path.dirname(pagesRoot), { recursive: true });
  const stagingRoot = await mkdtemp(path.join(path.dirname(pagesRoot), `.${path.basename(pagesRoot)}.stage-`));
  let staged = true;
  try {
    await cp(path.join(publishRoot, "catalogs"), path.join(stagingRoot, "catalogs"), { recursive: true });
    const roots = await readdir(stagingRoot);
    if (roots.length !== 1 || roots[0] !== "catalogs")
      throw new Error(`Pages publisher artifact must contain only catalogs; received ${roots.join(", ")}`);
    for (const app of catalog.apps) {
      const prefix = `catalogs/${catalog.publisher.id}/packages/${app.appId}/${app.buildId}/`;
      for (const file of app.browser.files) {
        const relativePath = `catalogs/${catalog.publisher.id}/${file.path}`;
        if (!relativePath.startsWith(prefix))
          throw new Error(`Pages package reference ${relativePath} is outside immutable package ${prefix}`);
        const metadata = await assertExists(path.join(stagingRoot, relativePath), relativePath, { nonempty: true });
        if (metadata.size !== file.size)
          throw new Error(`Pages package file ${file.path} size mismatch: expected ${file.size}, received ${metadata.size}`);
        const digest = sha256(await readFile(path.join(stagingRoot, relativePath)));
        if (digest !== file.sha256)
          throw new Error(`Pages package file ${file.path} SHA-256 mismatch: expected ${file.sha256}, received ${digest}`);
      }
    }
    await replaceDestination(stagingRoot, pagesRoot);
    staged = false;
    return Object.freeze({ pagesRoot });
  } finally {
    if (staged) await rm(stagingRoot, { recursive: true, force: true });
  }
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [command, value] = process.argv.slice(2);
  if (command === "--publisher-only" && value === undefined) {
    const { pagesRoot } = await publishPublisherArtifact();
    console.log(`Published browser catalogs to ${path.relative(process.cwd(), pagesRoot)}`);
  } else if (command === "--catalog-source" && value !== undefined) {
    const { publishRoot } = await publishSite({ catalogSource: value });
    console.log(`Published browser site to ${path.relative(process.cwd(), publishRoot)}`);
  } else if (command === undefined) {
    const { publishRoot } = await publishSite();
    console.log(`Published browser site to ${path.relative(process.cwd(), publishRoot)}`);
  } else {
    throw new Error("Usage: publish-site.mjs [--publisher-only | --catalog-source <catalog-url>]");
  }
}
