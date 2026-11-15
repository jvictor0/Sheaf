// Runs before any spec. Playwright reuses an already-running static server, so
// the first thing a run establishes is that the process it is about to trust
// is not older than the code under test. See src/server-currency.mjs for why
// "the server is up" is not the same question.
import { SERVER_IDENTITY_PATH, STATIC_SERVER_PORT, assertServerCurrent } from "./src/server-currency.mjs";

export default async function globalSetup() {
  let identity;
  try {
    const response = await fetch(`http://127.0.0.1:${STATIC_SERVER_PORT}${SERVER_IDENTITY_PATH}`);
    identity = response.ok ? await response.json() : undefined;
  } catch {
    // Playwright starts the server itself when none is listening, and this
    // hook can run before that server is accepting connections. A run with no
    // server to interrogate is not a stale run, so it is not this check's
    // business to fail it -- the webServer readiness gate covers that.
    return;
  }
  assertServerCurrent(identity);
}
