// The stale-server check has to be exercised, not merely installed. A guard
// that has never rejected anything is indistinguishable from one that cannot,
// and this particular guard only earns its keep in the case nobody hits on
// purpose.
import assert from "node:assert/strict";
import test from "node:test";

import { serverCurrencyFailure } from "../src/server-currency.mjs";

test("a server started after its sources is current", () => {
  assert.equal(
    serverCurrencyFailure({ startedAt: 2_000, newestSourceMtimeMs: 1_000, sources: ["src/static-server.mjs"] }),
    undefined,
  );
});

test("a server started at the same moment as its sources is current", () => {
  // The boundary is deliberately inclusive: a server started in the same
  // millisecond as the last write is serving that write.
  assert.equal(
    serverCurrencyFailure({ startedAt: 1_000, newestSourceMtimeMs: 1_000, sources: [] }),
    undefined,
  );
});

test("a server older than its sources is rejected, and says why", () => {
  const failure = serverCurrencyFailure({
    startedAt: 1_000,
    newestSourceMtimeMs: 61_000,
    sources: ["src/static-server.mjs"],
  });
  assert.ok(failure, "a server predating its own sources must be rejected");
  // The message has to name staleness. The whole cost of the original
  // incident was that the symptom looked like an ordinary failing assertion.
  assert.match(failure, /STALE STATIC SERVER/);
  assert.match(failure, /60s/, "the message reports how far behind the process is");
  assert.match(failure, /src\/static-server\.mjs/, "the message names the sources it dated against");
});

test("an identity the server could not produce is rejected rather than assumed current", () => {
  for (const identity of [undefined, null, {}, { startedAt: 1 }, { newestSourceMtimeMs: 1 }]) {
    assert.ok(
      serverCurrencyFailure(identity),
      `a malformed identity (${JSON.stringify(identity)}) must not read as current`,
    );
  }
});
