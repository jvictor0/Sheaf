// Playwright reuses an already-running static server rather than starting its
// own. That server streams files from disk, so app code is always fresh -- but
// its synthesized responses (the fixture catalog, the module stub) are built
// in-process at startup and stay frozen at whatever the tree looked like then.
// "The server is up" therefore says nothing about whether what it SYNTHESIZES
// matches the tree, and a run against a day-old process once produced a
// failure indistinguishable from a real defect until someone read the
// process's start time by hand.
//
// The fix is to make the stale case say so itself. The server reports when it
// started and how recently its own sources changed; a run refuses to trust a
// process that predates the code it is meant to be serving.

// The port the server binds and the check interrogates. One definition, because
// a check pointed at a port nothing is listening on fails its fetch, returns
// early, and reports current -- the dead instrument this file exists to catch.
export const STATIC_SERVER_PORT = 4173;
export const SERVER_IDENTITY_PATH = "/__server-identity";

// Sources whose contents are baked into the server's in-process responses.
// A file served from disk does not belong here -- editing one of those is
// already picked up without a restart, and listing it would make every ordinary
// edit look like staleness. This file is one of them: the server imports its
// port and its own source list from here.
export const SERVER_IDENTITY_SOURCES = Object.freeze([
  "src/static-server.mjs",
  "src/server-currency.mjs",
  "dist/src/protocol.js",
]);

// Pure so it can be tested without a server: the whole point is that the
// stale branch is exercised, and a check nothing ever fails is not a check.
export function serverCurrencyFailure(identity) {
  if (!identity || typeof identity !== "object") {
    return "the static server did not report an identity; it predates the currency check entirely, so restart it";
  }
  const { startedAt, newestSourceMtimeMs, sources } = identity;
  if (typeof startedAt !== "number" || typeof newestSourceMtimeMs !== "number") {
    return "the static server reported an identity missing startedAt or newestSourceMtimeMs; restart it";
  }
  if (startedAt >= newestSourceMtimeMs) return undefined;
  const stale = Math.round((newestSourceMtimeMs - startedAt) / 1000);
  const named = Array.isArray(sources) ? sources.join(", ") : SERVER_IDENTITY_SOURCES.join(", ");
  // Named as staleness rather than left to surface as a failing assertion
  // somewhere downstream, which is what cost the time last go round.
  return (
    `STALE STATIC SERVER: the process listening on the fixture ports started ${stale}s ` +
    `BEFORE its own sources were last changed (${named}). It is serving synthesized ` +
    `responses built from an older tree, so any failure below would be about the ` +
    `server, not the code. Kill it and re-run.`
  );
}

export function assertServerCurrent(identity) {
  const failure = serverCurrencyFailure(identity);
  if (failure) throw new Error(failure);
}
