export const SCHEDULER_STALE_AFTER_SECONDS = 125;

function defaultNowMilliseconds() {
  return globalThis.performance?.now?.() ?? 0;
}

export class SchedulerHeartbeatTracker {
  constructor({
    nowMilliseconds = defaultNowMilliseconds,
    staleAfterSeconds = SCHEDULER_STALE_AFTER_SECONDS,
  } = {}) {
    if (typeof nowMilliseconds !== "function") {
      throw new TypeError("Heartbeat clock must be a function");
    }
    if (!Number.isFinite(staleAfterSeconds) || staleAfterSeconds <= 0) {
      throw new RangeError("Heartbeat stale timeout must be positive");
    }
    this.nowMilliseconds = nowMilliseconds;
    this.staleAfterSeconds = staleAfterSeconds;
    this.connected = false;
    this.lastLiveHeartbeatMilliseconds = null;
  }

  setConnected(connected) {
    const nextConnected = Boolean(connected);
    if (!nextConnected || !this.connected) {
      this.lastLiveHeartbeatMilliseconds = null;
    }
    this.connected = nextConnected;
  }

  record(metadata = {}) {
    if (!this.connected || metadata?.retained !== false) {
      return false;
    }
    this.lastLiveHeartbeatMilliseconds = this.nowMilliseconds();
    return true;
  }

  hasLiveHeartbeat() {
    return Number.isFinite(this.lastLiveHeartbeatMilliseconds);
  }

  ageSeconds() {
    if (!this.hasLiveHeartbeat()) {
      return Number.POSITIVE_INFINITY;
    }
    const elapsed = this.nowMilliseconds() - this.lastLiveHeartbeatMilliseconds;
    return Math.max(0, Math.floor(elapsed / 1000));
  }

  isFresh() {
    return this.connected && this.hasLiveHeartbeat() &&
      this.ageSeconds() <= this.staleAfterSeconds;
  }
}
