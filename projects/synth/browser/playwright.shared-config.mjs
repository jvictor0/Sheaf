import { STATIC_SERVER_PORT } from "./src/server-currency.mjs";

export function synthBrowserPlaywrightConfig({ testDir, staticServerCommand }) {
  return {
    testDir,
    testMatch: "**/*.spec.ts",
    // AudioWorklet and native-deadline assertions share fixed loopback ports and
    // browser audio resources; keep the whole browser suite serialized.
    workers: 1,
    // A reused server is proven current before the run trusts it. Reuse itself
    // is kept: starting a fresh process per run costs every run, while the
    // failure it avoids is narrow and now detected.
    globalSetup: new URL("./playwright.global-setup.mjs", import.meta.url).pathname,
    webServer: {
      command: staticServerCommand,
      port: STATIC_SERVER_PORT,
      reuseExistingServer: true,
    },
  };
}
