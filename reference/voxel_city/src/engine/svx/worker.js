import { createSvxSource } from "./source.js";
import { roadNetwork } from "./roads.js";

/**
 * The structvox export in a worker (svx/source.js): what a browser host
 * runs to generate the city beside its engine (opus_destruct_2's web
 * client). Messages { id, type, ... }, replies { id, type, ... } with the
 * voxel arrays transferred:
 *
 *   init      { config, options }        -> { extent: [lo, hi], spawn, materials }
 *   generate  { cx, cy, cz }             -> { any, vox, look, flora, water, isolated }
 *   grids     { cx, cy, cz }             -> { grids }
 *   grid      { gid }                    -> { grid: its SourceGrid, voxels: { h, chunks } | null }
 *   region    { cx, cy }                 -> { region }
 *   coarse    { lo, n, factor }          -> { vox }
 *   roads     { lo, hi } (m)             -> { lanes, walks, parking } (svx/roads.js region)
 *
 * `handle` is the whole protocol (tests run it without a worker).
 */
export function handle(state, msg) {
  if (msg.type === "init") {
    state.src = createSvxSource(msg.config, msg.options ?? {});
    return { reply: { extent: [state.src.chunkLo(), state.src.chunkHi()], spawn: state.src.spawn(), materials: state.src.materials() } };
  }
  const src = state.src;
  if (!src) throw new Error("svx worker: init first");
  switch (msg.type) {
    case "generate": {
      const { cx, cy, cz } = msg;
      const g = src.generate(cx, cy, cz);
      // (copies: the source keeps its recent chunks)
      const layers = ["look", "flora", "water"].map((l) => src.generateLayer(cx, cy, cz, l)?.slice() ?? null);
      const iso = src.isolated(cx, cy, cz);
      const reply = { any: g.any, vox: g.vox.slice(), look: layers[0], flora: layers[1], water: layers[2], isolated: iso ? Int32Array.from(iso) : null };
      return { reply, transfer: [reply.vox, ...layers, reply.isolated].filter(Boolean).map((a) => a.buffer) };
    }
    case "grids":
      return { reply: { grids: src.grids(msg.cx, msg.cy, msg.cz) } };
    case "grid": {
      const voxels = src.generateGrid(msg.gid);
      const transfer = voxels ? voxels.chunks.flatMap((c) => [c.vox, c.look, c.flora].filter(Boolean).map((a) => a.buffer)) : [];
      return { reply: { grid: src.grid(msg.gid), voxels }, transfer };
    }
    case "region":
      return { reply: { region: src.region(msg.cx, msg.cy) } };
    case "roads": {
      state.roads ??= roadNetwork(src.world);
      return { reply: state.roads.region(msg.lo, msg.hi) };
    }
    case "coarse": {
      const vox = src.coarse(msg.lo, msg.n, msg.factor);
      return { reply: { vox }, transfer: [vox.buffer] };
    }
    default:
      throw new Error(`svx worker: unknown message ${msg.type}`);
  }
}

// (in a worker: answer messages; imported elsewhere: only `handle`)
if (typeof self !== "undefined" && typeof self.postMessage === "function" && typeof window === "undefined") {
  const state = {};
  self.onmessage = (e) => {
    const msg = e.data;
    try {
      const { reply, transfer } = handle(state, msg);
      self.postMessage({ id: msg.id, type: msg.type, ...reply }, transfer ?? []);
    } catch (err) {
      self.postMessage({ id: msg.id, type: "error", error: String(err?.message ?? err) });
    }
  };
}
