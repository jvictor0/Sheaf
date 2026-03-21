import test from "node:test";
import assert from "node:assert/strict";
import { ReplayQueue } from "../src/replayQueue.js";
test("replay queue continues after a thrown task error", async () => {
    const queue = new ReplayQueue();
    const events = [];
    let errorCount = 0;
    queue.enqueue(async () => {
        events.push("first");
        throw new Error("boom");
    }, async () => {
        errorCount += 1;
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
    queue.enqueue(async () => {
        events.push("second");
    }, async () => {
        errorCount += 1;
    });
    await new Promise((resolve) => setTimeout(resolve, 10));
    assert.deepEqual(events, ["first", "second"]);
    assert.equal(errorCount, 1);
});
