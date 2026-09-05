import { randomUUID } from "node:crypto";
import { mkdir, mkdtemp, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath, pathToFileURL } from "node:url";
import { spawn } from "node:child_process";

import { generateBrowserBinding, readAppBuildManifest } from "./app-build-manifest.mjs";

const CORE_SOURCES = [
  "ParameterModulation.cpp",
  "ButtonGrid.cpp",
  "MidiController.cpp",
  "PatchPersistence.cpp",
  "DspWavetable.cpp",
  "Modules.cpp",
  "MidiReconcile.cpp",
  "MidiDevicePoller.cpp",
  "MidiConfigViewModel.cpp",
  "MidiConfigBlocks.cpp",
  "MasterClock.cpp",
  "ControllerWizard.cpp",
];

const EXPORTED_FUNCTIONS = [
  "_malloc", "_free", "_synth_browser_abi_version", "_synth_browser_ui_protocol_version",
  "_synth_browser_runtime_config_version", "_synth_browser_create", "_synth_browser_initialize",
  "_synth_browser_audio_output_channels", "_synth_browser_prepare", "_synth_browser_process",
  "_synth_browser_start_audio_worklet", "_synth_browser_set_timestamp_epoch_offset",
  "_synth_browser_audio_input_channels", "_synth_browser_set_audio_input_source",
  "_synth_browser_clear_audio_input_source", "_synth_browser_consume_pending_audio_request",
  "_synth_browser_audio_worklet_block_count",
  "_synth_browser_audio_worklet_peak_microunits", "_synth_browser_audio_worklet_deadline_microunits",
  "_synth_browser_message_tick", "_synth_browser_build_ui_frame", "_synth_browser_dispatch_action",
  "_synth_browser_consume_persistence_dirty", "_synth_browser_submit_midi_endpoints",
  "_synth_browser_submit_audio_devices",
  "_synth_browser_dequeue_midi_action", "_synth_browser_deliver_midi",
  "_synth_browser_dequeue_midi_output", "_synth_browser_midi_diagnostics",
  "_synth_browser_destroy",
];

const EXPORTED_RUNTIME_METHODS = [
  "stringToUTF8", "lengthBytesUTF8", "FS", "IDBFS", "HEAPU8", "HEAPF32",
  "emscriptenRegisterAudioObject",
];

function commonCompilerArgs(browserRoot) {
  return [
    "-I", path.resolve(browserRoot, "..", "include"),
    "-I", path.join(browserRoot, "cpp"),
    "-std=c++20", "-O2",
    "-pthread",
    "-sUSE_PTHREADS=1",
    "-sPTHREAD_POOL_SIZE=1",
    "-sINITIAL_MEMORY=536870912",
    "-sALLOW_MEMORY_GROWTH=1",
    "-sMAXIMUM_MEMORY=2147483648",
    "-sSTACK_SIZE=16777216",
    "-sAUDIO_WORKLET=1",
    "-sWASM_WORKERS=1",
    "-lidbfs.js",
    "-sMODULARIZE=1",
    "-sEXPORT_ES6=1",
    "-sENVIRONMENT=web,worker",
    "-sWASM_BIGINT",
    `-sEXPORTED_FUNCTIONS=${JSON.stringify(EXPORTED_FUNCTIONS)}`,
    `-sEXPORTED_RUNTIME_METHODS=${JSON.stringify(EXPORTED_RUNTIME_METHODS)}`,
  ];
}

function compilerArgs(browserRoot, app, bindingPath, outputPath) {
  return [
    ...commonCompilerArgs(browserRoot),
    ...app.includeDirs.flatMap((directory) => ["-I", path.resolve(browserRoot, directory)]),
    ...CORE_SOURCES.map((source) => path.resolve(browserRoot, "..", "src", source)),
    path.join(browserRoot, "cpp", "BrowserRuntimeAbi.cpp"),
    bindingPath,
    "-o", outputPath,
  ];
}

function defaultRunCommand(executable, args, options) {
  return new Promise((resolve, reject) => {
    const child = spawn(executable, args, { ...options, stdio: "inherit", shell: false });
    child.once("error", reject);
    child.once("exit", (code, signal) => {
      if (code === 0) resolve();
      else reject(new Error(`${executable} exited with ${code ?? `signal ${signal}`}`));
    });
  });
}

async function requireArtifact(filename, appId, role) {
  let metadata;
  try {
    metadata = await stat(filename);
  } catch (error) {
    if (error?.code === "ENOENT") throw new Error(`${appId} ${role} artifact is missing: ${filename}`);
    throw error;
  }
  if (!metadata.isFile() || metadata.size === 0) {
    throw new Error(`${appId} ${role} artifact must be a non-empty file: ${filename}`);
  }
}

async function atomicWriteJson(filename, value) {
  const temporary = `${filename}.tmp-${process.pid}-${randomUUID()}`;
  try {
    await writeFile(temporary, `${JSON.stringify(value, null, 2)}\n`);
    await rename(temporary, filename);
  } finally {
    await rm(temporary, { force: true });
  }
}

async function replaceOutputRoot(stagingRoot, outputRoot) {
  const parent = path.dirname(outputRoot);
  const backupRoot = await mkdtemp(path.join(parent, `.${path.basename(outputRoot)}.previous-`));
  await rm(backupRoot, { recursive: true, force: true });
  let movedPrevious = false;
  try {
    try {
      await rename(outputRoot, backupRoot);
      movedPrevious = true;
    } catch (error) {
      if (error?.code !== "ENOENT") throw error;
    }
    await rename(stagingRoot, outputRoot);
    if (movedPrevious) await rm(backupRoot, { recursive: true, force: true });
  } catch (error) {
    if (movedPrevious) {
      await rm(outputRoot, { recursive: true, force: true });
      await rename(backupRoot, outputRoot);
    }
    throw error;
  }
}

export async function buildBrowserApps({
  browserRoot,
  manifestPath = path.join(browserRoot, "first-party-apps.json"),
  allowedSourceRoots,
  outputRoot = path.join(browserRoot, "dist", "wasm", "apps"),
  runCommand = defaultRunCommand,
}) {
  const manifest = await readAppBuildManifest({ browserRoot, manifestPath, allowedSourceRoots });
  const generatedRoot = path.join(browserRoot, "dist", "generated", "browser-apps");
  const wasmRoot = path.resolve(browserRoot, "dist", "wasm");
  const appsRoot = path.resolve(outputRoot);
  const outputPrefix = path.relative(wasmRoot, appsRoot);
  if (outputPrefix === "" || outputPrefix === ".." || outputPrefix.startsWith(`..${path.sep}`) || path.isAbsolute(outputPrefix)) {
    throw new Error("outputRoot must be a dedicated directory beneath dist/wasm");
  }
  const reportPrefix = outputPrefix.split(path.sep).join("/");
  await mkdir(generatedRoot, { recursive: true });
  await mkdir(path.dirname(appsRoot), { recursive: true });
  const stagingRoot = await mkdtemp(path.join(path.dirname(appsRoot), `.${path.basename(appsRoot)}.stage-`));
  let staged = true;
  try {
    const emissions = [];
    for (const app of manifest.apps) {
      const bindingPath = path.join(generatedRoot, `${app.appId}.cpp`);
      const outputDirectory = path.join(stagingRoot, app.appId);
      const outputPath = path.join(outputDirectory, `${app.appId}.js`);
      const wasmPath = path.join(outputDirectory, `${app.appId}.wasm`);
      await writeFile(bindingPath, generateBrowserBinding(app));
      await mkdir(outputDirectory, { recursive: true });
      await runCommand(process.env.EMXX || "em++", compilerArgs(browserRoot, app, bindingPath, outputPath), {
        cwd: browserRoot,
        env: process.env,
        shell: false,
      });
      await requireArtifact(outputPath, app.appId, "entry");
      await requireArtifact(wasmPath, app.appId, "wasm");

      const entry = `${reportPrefix}/${app.appId}/${app.appId}.js`;
      emissions.push({
        appId: app.appId,
        artifacts: {
          entry,
          wasm: `${reportPrefix}/${app.appId}/${app.appId}.wasm`,
          pthreadWorker: entry,
          wasmWorker: entry,
          audioWorklet: entry,
        },
      });
    }

    const report = {
      schemaVersion: 1,
      manifestDigest: manifest.digest,
      publisher: manifest.publisher,
      apps: emissions,
    };
    await atomicWriteJson(path.join(stagingRoot, "emissions.json"), report);
    await replaceOutputRoot(stagingRoot, appsRoot);
    staged = false;
    return report;
  } finally {
    if (staged) await rm(stagingRoot, { recursive: true, force: true });
  }
}

function parseCliArguments(argv, browserRoot) {
  let manifestPath = path.join(browserRoot, "first-party-apps.json");
  let outputRoot = path.join(browserRoot, "dist", "wasm", "apps");
  const allowedSourceRoots = [];
  for (let index = 0; index < argv.length; index += 1) {
    const option = argv[index];
    const value = argv[index + 1];
    if (["--manifest", "--allowed-source-root", "--output-root"].includes(option) && value === undefined) {
      throw new Error(`${option} requires a path`);
    }
    if (option === "--manifest") {
      manifestPath = path.resolve(browserRoot, value);
      index += 1;
    } else if (option === "--allowed-source-root") {
      allowedSourceRoots.push(path.resolve(browserRoot, value));
      index += 1;
    } else if (option === "--output-root") {
      outputRoot = path.resolve(browserRoot, value);
      index += 1;
    } else {
      throw new Error(`Unknown option: ${option}`);
    }
  }
  return {
    manifestPath,
    outputRoot,
    allowedSourceRoots: allowedSourceRoots.length > 0 ? allowedSourceRoots : undefined,
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  const browserRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
  const options = parseCliArguments(process.argv.slice(2), browserRoot);
  await buildBrowserApps({ browserRoot, ...options });
}
