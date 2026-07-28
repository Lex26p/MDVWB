import assert from "node:assert/strict";
import fs from "node:fs/promises";

const source = await fs.readFile(
  new URL("../../www/fancoils/scheduler-status-health.js", import.meta.url),
  "utf8",
);
const moduleUrl = `data:text/javascript;base64,${Buffer.from(source).toString("base64")}`;
const { SchedulerHeartbeatTracker } = await import(moduleUrl);

let monotonicNow = 10_000;
const tracker = new SchedulerHeartbeatTracker({
  nowMilliseconds: () => monotonicNow,
  staleAfterSeconds: 125,
});

assert.equal(tracker.isFresh(), false);
tracker.setConnected(true);

// The broker's retained replay is useful for display, but it cannot prove that
// the scheduler process is alive after this browser connection was established.
assert.equal(tracker.record({ retained: true }), false);
assert.equal(tracker.hasLiveHeartbeat(), false);
assert.equal(tracker.isFresh(), false);

// Missing metadata is treated conservatively and never upgrades a retained or
// otherwise ambiguous message into a live heartbeat.
assert.equal(tracker.record(), false);
assert.equal(tracker.isFresh(), false);

assert.equal(tracker.record({ retained: false }), true);
assert.equal(tracker.isFresh(), true);
assert.equal(tracker.ageSeconds(), 0);

// Freshness uses the tab's monotonic elapsed time. Controller epoch and the
// browser's wall clock are deliberately absent from this calculation.
const originalDateNow = Date.now;
Date.now = () => 9_999_999_999_999;
try {
  monotonicNow += 124_000;
  assert.equal(tracker.ageSeconds(), 124);
  assert.equal(tracker.isFresh(), true);

  monotonicNow += 2_000;
  assert.equal(tracker.ageSeconds(), 126);
  assert.equal(tracker.isFresh(), false);
} finally {
  Date.now = originalDateNow;
}

// Reconnect invalidates the old live heartbeat. A retained replay remains
// preliminary until the scheduler publishes a new live status message.
tracker.setConnected(false);
assert.equal(tracker.isFresh(), false);
tracker.setConnected(true);
assert.equal(tracker.hasLiveHeartbeat(), false);
assert.equal(tracker.record({ retained: true }), false);
assert.equal(tracker.isFresh(), false);
assert.equal(tracker.record({ retained: false }), true);
assert.equal(tracker.isFresh(), true);

console.log("Scheduler heartbeat health tests: OK");
