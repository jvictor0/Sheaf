import { expect, test } from "@playwright/test";

async function openTestPage(page: import("@playwright/test").Page) {
  await page.route("**/dist/src/main.js", (route) => route.fulfill({
    status: 200,
    contentType: "application/javascript",
    body: "",
  }));
  await page.goto("http://127.0.0.1:4173/public/index.html");
}

test("fetches and verifies every declared package file with CORS before creating object URLs", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const bytesByName = {
      "app.js": new TextEncoder().encode("export default async () => ({});\n"),
      "app.wasm": Uint8Array.from([0, 97, 115, 109, 1, 0, 0, 0]),
      "pthread.js": new TextEncoder().encode("postMessage('ready');\n"),
      "audio.js": new TextEncoder().encode("registerProcessor('audio', class {});\n"),
    };
    const hex = async (bytes: Uint8Array) => Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(bytes).buffer)))
      .map((value) => value.toString(16).padStart(2, "0")).join("");
    const files = await Promise.all(Object.entries(bytesByName).map(async ([name, bytes]) => ({
      path: `packages/test-app/build-1/${name}`,
      url: `https://publisher.example/packages/test-app/build-1/${name}`,
      mediaType: name.endsWith(".wasm") ? "application/wasm" : "text/javascript",
      size: bytes.byteLength,
      sha256: await hex(bytes),
    })));
    const requests: Array<{ url: string; mode?: RequestMode; credentials?: RequestCredentials }> = [];
    const created: Array<{ url: string; type: string; size: number }> = [];
    const packageLease = await materializePackage({
      appId: "test-app",
      buildId: "build-1",
      browser: {
        entry: "packages/test-app/build-1/app.js",
        entryUrl: "https://publisher.example/packages/test-app/build-1/app.js",
        files,
      },
    } as any, async (url: string, init?: RequestInit) => {
      requests.push({ url, mode: init?.mode, credentials: init?.credentials });
      const name = new URL(url).pathname.split("/").at(-1)! as keyof typeof bytesByName;
      const bytes = bytesByName[name];
      return new Response(bytes, {
        status: 200,
        headers: {
          "Content-Type": name.endsWith(".wasm") ? "application/wasm" : "application/javascript",
          "Content-Length": String(bytes.byteLength),
        },
      });
    }, {
      createObjectURL(blob: Blob) {
        const url = `blob:verified/${created.length}`;
        created.push({ url, type: blob.type, size: blob.size });
        return url;
      },
      revokeObjectURL() {},
    });
    return { requests, created, packageLease: {
      entryUrl: packageLease.entryUrl,
      locateFile: packageLease.locateFile,
      mainScriptUrlOrBlob: packageLease.mainScriptUrlOrBlob,
    } };
  });

  expect(result.requests).toHaveLength(4);
  expect(result.requests.every(({ mode, credentials }) => mode === "cors" && credentials === "omit")).toBe(true);
  expect(result.created).toEqual([
    { url: "blob:verified/0", type: "text/javascript", size: 33 },
    { url: "blob:verified/1", type: "application/wasm", size: 8 },
    { url: "blob:verified/2", type: "text/javascript", size: 22 },
    { url: "blob:verified/3", type: "text/javascript", size: 38 },
  ]);
  expect(result.packageLease).toEqual({
    entryUrl: "blob:verified/0",
    locateFile: {
      "app.js": "blob:verified/0",
      "app.wasm": "blob:verified/1",
      "pthread.js": "blob:verified/2",
      "audio.js": "blob:verified/3",
    },
    mainScriptUrlOrBlob: "blob:verified/0",
  });
});

test("rejects missing worker files before creating any object URL", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const bytes = new TextEncoder().encode("entry");
    const digest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)))
      .map((value) => value.toString(16).padStart(2, "0")).join("");
    let created = 0;
    try {
      await materializePackage({
        appId: "test-app",
        buildId: "build-1",
        browser: {
          entry: "packages/test-app/build-1/app.js",
          entryUrl: "https://publisher.example/packages/test-app/build-1/app.js",
          files: [
            { path: "packages/test-app/build-1/app.js", url: "https://publisher.example/packages/test-app/build-1/app.js", mediaType: "text/javascript", size: bytes.byteLength, sha256: digest },
            { path: "packages/test-app/build-1/pthread.worker.js", url: "https://publisher.example/packages/test-app/build-1/pthread.worker.js", mediaType: "text/javascript", size: bytes.byteLength, sha256: digest },
          ],
        },
      } as any, async (url: string) => url.endsWith("pthread.worker.js")
        ? new Response("missing", { status: 404, headers: { "Content-Type": "text/javascript" } })
        : new Response(bytes, { status: 200, headers: { "Content-Type": "text/javascript" } }), {
        createObjectURL() { created += 1; return `blob:${created}`; },
        revokeObjectURL() {},
      });
      return { error: "", created };
    } catch (error) {
      return { error: error instanceof Error ? error.message : String(error), created };
    }
  });

  expect(result.error).toMatch(/pthread\.worker\.js.*404|404.*pthread\.worker\.js/i);
  expect(result.created).toBe(0);
});

test("rejects wrong response media types", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const bytes = new TextEncoder().encode("export default async () => ({});\n");
    const digest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)))
      .map((value) => value.toString(16).padStart(2, "0")).join("");
    const app = {
      appId: "test-app",
      buildId: "build-1",
      browser: {
        entry: "packages/test-app/build-1/app.js",
        entryUrl: "https://publisher.example/packages/test-app/build-1/app.js",
        files: [{ path: "packages/test-app/build-1/app.js", url: "https://publisher.example/packages/test-app/build-1/app.js", mediaType: "text/javascript", size: bytes.byteLength, sha256: digest }],
      },
    };
    try {
      await materializePackage(app as any, async () => new Response(bytes, {
        status: 200,
        headers: { "Content-Type": "application/octet-stream", "Content-Length": String(bytes.byteLength) },
      }));
      return "accepted";
    } catch (error) {
      return error instanceof Error ? error.message : String(error);
    }
  });

  expect(result).toMatch(/media type.*application\/octet-stream|expected.*text\/javascript/i);
});

test("accepts advisory transfer lengths when decoded bytes match the digest", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const bytes = new TextEncoder().encode("export default async () => ({});\n");
    const digest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)))
      .map((value) => value.toString(16).padStart(2, "0")).join("");
    const app = {
      appId: "test-app",
      buildId: "build-1",
      browser: {
        entry: "packages/test-app/build-1/app.js",
        entryUrl: "https://publisher.example/packages/test-app/build-1/app.js",
        files: [{ path: "packages/test-app/build-1/app.js", url: "https://publisher.example/packages/test-app/build-1/app.js", mediaType: "text/javascript", size: bytes.byteLength, sha256: digest }],
      },
    };
    const materialized = await materializePackage(app as any, async () => new Response(bytes, {
      status: 200,
      headers: { "Content-Type": "text/javascript", "Content-Length": "17" },
    }));
    materialized.dispose();
    return "accepted";
  });

  expect(result).toBe("accepted");
});

test("rejects decoded bytes that do not match the catalog's declared package size", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const bytes = new TextEncoder().encode("export default async () => ({});\n");
    const digest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)))
      .map((value) => value.toString(16).padStart(2, "0")).join("");
    const app = {
      appId: "test-app",
      buildId: "build-1",
      browser: {
        entry: "packages/test-app/build-1/app.js",
        entryUrl: "https://publisher.example/packages/test-app/build-1/app.js",
        files: [{
          path: "packages/test-app/build-1/app.js",
          url: "https://publisher.example/packages/test-app/build-1/app.js",
          mediaType: "text/javascript",
          size: bytes.byteLength + 1,
          sha256: digest,
        }],
      },
    };
    try {
      await materializePackage(app as any, async () => new Response(bytes, {
        status: 200,
        headers: { "Content-Type": "text/javascript", "Content-Length": String(bytes.byteLength) },
      }));
      return "accepted";
    } catch (error) {
      return error instanceof Error ? error.message : String(error);
    }
  });

  expect(result).toMatch(/app\.js.*size.*expected/i);
});

test("rejects stale or hash-mismatched WASM before importing the verified entry", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const { loadEmscriptenRuntime } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const entryBytes = new TextEncoder().encode("export default async () => ({});\n");
    const expectedWasm = Uint8Array.from([0, 97, 115, 109, 1, 0, 0, 0]);
    const staleWasm = Uint8Array.from([0, 97, 115, 109, 2, 0, 0, 0]);
    const digest = async (bytes: Uint8Array) => Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(bytes).buffer)))
      .map((value) => value.toString(16).padStart(2, "0")).join("");
    const files = [
      { name: "app.js", mediaType: "text/javascript", bytes: entryBytes },
      { name: "app.wasm", mediaType: "application/wasm", bytes: expectedWasm },
    ];
    let imports = 0;
    let created = 0;
    try {
      const materialized = await materializePackage({
        appId: "test-app",
        buildId: "build-1",
        browser: {
          entry: "packages/test-app/build-1/app.js",
          entryUrl: "https://publisher.example/packages/test-app/build-1/app.js",
          files: await Promise.all(files.map(async (file) => ({
            path: `packages/test-app/build-1/${file.name}`,
            url: `https://publisher.example/packages/test-app/build-1/${file.name}`,
            mediaType: file.mediaType,
            size: file.bytes.byteLength,
            sha256: await digest(file.bytes),
          }))),
        },
      } as any, async (url: string) => {
        const wasm = url.endsWith(".wasm");
        return new Response(wasm ? staleWasm : entryBytes, {
          status: 200,
          headers: { "Content-Type": wasm ? "application/wasm" : "text/javascript" },
        });
      }, {
        createObjectURL() { created += 1; return `blob:${created}`; },
        revokeObjectURL() {},
      });
      await loadEmscriptenRuntime(materialized, async () => { imports += 1; return {}; });
      return { error: "", imports, created };
    } catch (error) {
      return { error: error instanceof Error ? error.message : String(error), imports, created };
    }
  });

  expect(result.error).toMatch(/app\.wasm.*sha-?256|sha-?256.*app\.wasm|integrity/i);
  expect(result.imports).toBe(0);
  expect(result.created).toBe(0);
});

test("passes explicit immutable mappings to the Emscripten factory and refuses document-relative fallback", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { loadEmscriptenRuntime } = await (new Function("return import('/dist/src/worker.js')")() as Promise<any>);
    const calls: unknown[] = [];
    let fallbackError = "";
    await loadEmscriptenRuntime({
      entryUrl: "blob:verified-entry",
      locateFile: {
        "app.js": "blob:verified-worker",
        "app.wasm": "blob:verified-wasm",
        "worklets/audio.js": "blob:verified-audio",
        "audio.js": "blob:verified-audio",
      },
      mainScriptUrlOrBlob: "blob:verified-worker",
    }, async (url: string) => {
      calls.push(["import", url]);
      return { default: async (options: any) => {
        calls.push(["factory", options.locateFile("app.wasm", "https://launcher.example/dist/"), options.mainScriptUrlOrBlob]);
        calls.push(["nested", options.locateFile("./worklets/audio.js", "https://launcher.example/dist/")]);
        try { options.locateFile("missing.worker.js", "https://launcher.example/dist/"); }
        catch (error) { fallbackError = error instanceof Error ? error.message : String(error); }
        return {
          FS: { filesystems: { IDBFS: {} } },
          IDBFS: {},
          HEAPU8: new Uint8Array(16),
          HEAPF32: new Float32Array(4),
          _synth_browser_abi_version: () => 6,
          _synth_browser_ui_protocol_version: () => 2,
          _synth_browser_runtime_config_version: () => 1,
          emscriptenRegisterAudioObject: () => 1,
          _synth_browser_start_audio_worklet: () => 0,
          _synth_browser_audio_input_channels: () => 0,
          _synth_browser_set_audio_input_source: () => 0,
          _synth_browser_clear_audio_input_source: () => 0,
          _synth_browser_consume_pending_audio_request: () => -1,
        };
      } };
    });
    return { calls, fallbackError };
  });

  expect(result.calls).toEqual([
    ["import", "blob:verified-entry"],
    ["factory", "blob:verified-wasm", "blob:verified-worker"],
    ["nested", "blob:verified-audio"],
  ]);
  expect(result.fallbackError).toMatch(/missing\.worker\.js.*not materialized|unmapped/i);
});

test("normalizes every materialized sidecar path with one strict package grammar", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { normalizeMaterializedPath } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const accepted = ["worker.js", "./worklets/audio.js", "assets/runtime_data-1.bin"]
      .map((value) => normalizeMaterializedPath(value, "test path"));
    const rejected = [
      "", "/worker.js", "../worker.js", "worklets//audio.js", "././worker.js",
      "worker%2ejs", "https:worker.js", "worker.js?cache", "worker.js#fragment",
      "worker\\windows.js", "worker name.js",
    ].map((value) => {
      try {
        normalizeMaterializedPath(value, "test path");
        return "accepted";
      } catch (error) {
        return error instanceof Error ? error.message : String(error);
      }
    });
    return { accepted, rejected };
  });

  expect(result.accepted).toEqual(["worker.js", "worklets/audio.js", "assets/runtime_data-1.bin"]);
  expect(result.rejected).toHaveLength(11);
  expect(result.rejected.every((message) => /test path.*normalized package-relative path/i.test(message))).toBe(true);
});

test("rewrites verified Emscripten import-meta worker bootstraps to typed object URLs", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const source = 'const worker = new Worker(new URL("pthread.js",import.meta.url), { type: "module" });\nexport default async () => worker;\n';
    const files = [
      { name: "app.js", mediaType: "text/javascript", bytes: new TextEncoder().encode(source) },
      { name: "pthread.js", mediaType: "text/javascript", bytes: new TextEncoder().encode("postMessage('ready');\n") },
    ];
    const digest = async (bytes: Uint8Array) => Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(bytes).buffer)))
      .map((value) => value.toString(16).padStart(2, "0")).join("");
    const blobs: Array<{ url: string; blob: Blob }> = [];
    const revoked: string[] = [];
    const materialized = await materializePackage({
      appId: "test-app",
      buildId: "build-1",
      browser: {
        entry: "packages/test-app/build-1/app.js",
        entryUrl: "https://publisher.example/packages/test-app/build-1/app.js",
        files: await Promise.all(files.map(async (file) => ({
          path: `packages/test-app/build-1/${file.name}`,
          url: `https://publisher.example/packages/test-app/build-1/${file.name}`,
          mediaType: file.mediaType,
          size: file.bytes.byteLength,
          sha256: await digest(file.bytes),
        }))),
      },
    } as any, async (url: string) => {
      const file = files.find(({ name }) => url.endsWith(name))!;
      return new Response(file.bytes, { status: 200, headers: { "Content-Type": file.mediaType } });
    }, {
      createObjectURL(blob: Blob) {
        const url = `blob:verified/${blobs.length + 1}`;
        blobs.push({ url, blob });
        return url;
      },
      revokeObjectURL(url: string) { revoked.push(url); },
    });
    const snapshot = {
      entryUrl: materialized.entryUrl,
      locateFile: materialized.locateFile,
      mainScriptUrlOrBlob: materialized.mainScriptUrlOrBlob,
      blobs: await Promise.all(blobs.map(async ({ url, blob }) => ({ url, text: await blob.text(), type: blob.type }))),
    };
    materialized.dispose();
    materialized.dispose();
    return { ...snapshot, revoked };
  });

  expect(result.entryUrl).toBe("blob:verified/3");
  expect(result.mainScriptUrlOrBlob).toBe("blob:verified/1");
  expect(result.locateFile["pthread.js"]).toBe("blob:verified/2");
  expect(result.blobs[2].type).toBe("text/javascript");
  expect(result.blobs[2].text).toContain('new URL("blob:verified/2")');
  expect(result.blobs[2].text).not.toContain('new URL("pthread.js",import.meta.url)');
  expect(result.revoked).toEqual(["blob:verified/1", "blob:verified/2", "blob:verified/3"]);
});

test("rejects ambiguous filename mappings instead of guessing", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const bytes = new TextEncoder().encode("worker");
    const digest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)))
      .map((value) => value.toString(16).padStart(2, "0")).join("");
    try {
      await materializePackage({
        appId: "test-app",
        buildId: "build-1",
        browser: {
          entry: "packages/test-app/build-1/a/worker.js",
          entryUrl: "https://publisher.example/packages/test-app/build-1/a/worker.js",
          files: ["a/worker.js", "b/worker.js"].map((name) => ({
            path: `packages/test-app/build-1/${name}`,
            url: `https://publisher.example/packages/test-app/build-1/${name}`,
            mediaType: "text/javascript",
            size: bytes.byteLength,
            sha256: digest,
          })),
        },
      } as any, async () => new Response(bytes, { status: 200, headers: { "Content-Type": "text/javascript" } }));
      return "";
    } catch (error) {
      return error instanceof Error ? error.message : String(error);
    }
  });

  expect(result).toMatch(/ambiguous.*worker\.js|worker\.js.*ambiguous/i);
});

test("revokes every materialized object URL exactly once when dispose is repeated", async ({ page }) => {
  await openTestPage(page);
  const result = await page.evaluate(async () => {
    const { materializePackage } = await (new Function("return import('/dist/src/package-loader.js')")() as Promise<any>);
    const bytes = new TextEncoder().encode("entry");
    const digest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)))
      .map((value) => value.toString(16).padStart(2, "0")).join("");
    const revoked: string[] = [];
    let next = 0;
    const materialized = await materializePackage({
      appId: "test-app",
      buildId: "build-1",
      browser: {
        entry: "packages/test-app/build-1/app.js",
        entryUrl: "https://publisher.example/packages/test-app/build-1/app.js",
        files: [{ path: "packages/test-app/build-1/app.js", url: "https://publisher.example/packages/test-app/build-1/app.js", mediaType: "text/javascript", size: bytes.byteLength, sha256: digest }],
      },
    } as any, async () => new Response(bytes, { status: 200, headers: { "Content-Type": "text/javascript" } }), {
      createObjectURL() { next += 1; return `blob:verified/${next}`; },
      revokeObjectURL(url: string) { revoked.push(url); },
    });
    materialized.dispose();
    materialized.dispose();
    return revoked;
  });

  expect(result).toEqual(["blob:verified/1"]);
});
