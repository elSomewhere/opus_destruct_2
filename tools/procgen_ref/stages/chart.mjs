// Stage "chart": the charts (world/chart.js: flat, torus, the six cube faces) at sample points,
// and the wrapping world's lattice helpers (world/wrap.js) of every world of lib/worlds.mjs.
import { REF, line, samples } from "../lib/rec.mjs";
import { WORLDS } from "../lib/worlds.mjs";

const { makeChart } = await import(REF + "world/chart.js");
const { Wrap } = await import(REF + "world/wrap.js");
const { makeConfig } = await import(REF + "config/defaults.js");
const { presetConfig } = await import(REF + "config/presets.js");

/** The charts: world configs as makeChart reads them. */
export const CHARTS = [
  {},
  { chart: "flat" },
  { chart: "torus", size: 48000 },
  { chart: "torus", size: 96000 },
  { chart: "torus", size: 192000 },
  { chart: "torus", size: 30000, latitude: false },
  { chart: "torus" },
  { chart: "cube", planet: { radius: 240000, face: 0 } },
  { chart: "cube", planet: { radius: 240000, face: 1 } },
  { chart: "cube", planet: { radius: 240000, face: 2 } },
  { chart: "cube", planet: { radius: 240000, face: 3 } },
  { chart: "cube", planet: { radius: 240000, face: 4 } },
  { chart: "cube", planet: { radius: 240000, face: 5 } },
  { chart: "cube", planet: { radius: 50000, face: 3 } },
  { chart: "cube" },
];

export default function* chart() {
  const r = samples(7);
  for (const cfg of CHARTS) {
    const c = makeChart(cfg);
    yield line("chart", c.id, c.R ?? "-", c.half ?? "-", c.radius ?? "-", c.face ?? "-", typeof c.latitude === "function");
    for (let k = 0; k < 400; k += 1) {
      const scale = [10, 1000, 100000, 1e7][k % 4];
      const x = (r() - 0.5) * 2 * scale;
      const y = (r() - 0.5) * 2 * scale;
      const f = c.toField(x, y);
      const lat = c.latitude ? c.latitude(f[0], f[1], f[2], f[3]) : "-";
      yield line("p", x, y, f[0], f[1], f[2], f[3], c.contains(x, y), c.edgeDistance(x, y), lat);
    }
    // whole laps of a torus: the same field point
    for (let k = 0; k < 20; k += 1) {
      const x = Math.round((r() - 0.5) * 2e6);
      const y = Math.round((r() - 0.5) * 2e6);
      const f = c.toField(x + 3 * (c.size ?? 0), y - 2 * (c.size ?? 0));
      yield line("lap", x, y, f[0], f[1], f[2], f[3]);
    }
  }
  for (const [key, id, size] of WORLDS) {
    const cfg = makeConfig(presetConfig(id, { size }));
    const W = new Wrap(cfg);
    yield line("wrap", key, makeChart(cfg.world).id, W.on, W.size, W.sizeV, ...[500, 620, 3600, 9000, 1e5, 7, 0].map((s) => W.count(s)));
    for (let k = 0; k < 60; k += 1) {
      const n = [0, 1, 7, W.count(620), W.count(9000)][k % 5];
      const i = Math.floor((r() - 0.5) * 400);
      const x = (r() - 0.5) * 4e6;
      yield line("w", n, i, W.canon(i, n), W.lap(i, n), x, W.v(x), W.vi(x), W.v(Math.round(x)), W.vi(Math.round(x)));
    }
  }
}
