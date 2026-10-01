/**
 * Pool of world workers. Tile jobs are dispatched only when a worker is free
 * (max `inflight` per worker) so the main thread can reprioritize the queue
 * every frame; request/response queries (map, building, probe, overview) go
 * to a separate query worker so the UI never waits behind tile jobs.
 */
export class WorkerPool {
  constructor(config, size) {
    this.size = size;
    this.config = config;
    this.workers = [];
    this.inflight = [];
    this.pending = new Map();
    this.nextId = 1;
    this.onTile = null;
    this.maxInflight = 2;
    for (let k = 0; k < size; k += 1) {
      const w = new Worker(new URL("../engine/stream/worker.js", import.meta.url), { type: "module" });
      w.onmessage = (e) => this.handle(k, e.data);
      w.onerror = (e) => console.error("worker error", e.message ?? e);
      w.postMessage({ type: "init", config, id: 0 });
      this.workers.push(w);
      this.inflight.push(0);
    }
    this.query = new Worker(new URL("../engine/stream/worker.js", import.meta.url), { type: "module" });
    this.query.onmessage = (e) => this.handle(-1, e.data);
    this.query.onerror = (e) => console.error("query worker error", e.message ?? e);
    this.query.postMessage({ type: "init", config, id: 0 });
  }

  handle(k, msg) {
    if (msg.type === "tile") {
      this.inflight[k] -= 1;
      this.pending.delete(msg.id);
      this.onTile?.(msg);
      return;
    }
    if (msg.type === "error") {
      console.error("[world worker]", msg.error);
      const p = this.pending.get(msg.id);
      if (p) {
        this.pending.delete(msg.id);
        if (p.tile && k >= 0) this.inflight[k] -= 1;
        p.reject?.(new Error(msg.error));
        if (p.tile) this.onTile?.({ type: "tile", id: msg.id, failed: true, error: String(msg.error).split("\n")[0], ...p.tile });
      }
      return;
    }
    const p = this.pending.get(msg.id);
    if (p) {
      this.pending.delete(msg.id);
      p.resolve(msg.data);
    }
  }

  freeSlots() {
    let n = 0;
    for (const f of this.inflight) n += Math.max(0, this.maxInflight - f);
    return n;
  }

  /** Dispatch a tile job to the least loaded worker. Returns false if all busy. */
  dispatchTile(job) {
    let best = -1;
    for (let k = 0; k < this.size; k += 1) {
      if (this.inflight[k] >= this.maxInflight) continue;
      if (best < 0 || this.inflight[k] < this.inflight[best]) best = k;
    }
    if (best < 0) return false;
    this.inflight[best] += 1;
    const id = this.nextId++;
    this.pending.set(id, { tile: job, resolve: () => {}, reject: () => {} });
    this.workers[best].postMessage({ type: "tile", id, ...job });
    return true;
  }

  request(type, payload) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.query.postMessage({ type, id, ...payload });
    });
  }

  dispose() {
    for (const w of this.workers) w.terminate();
    this.query?.terminate();
    this.workers = [];
  }
}
