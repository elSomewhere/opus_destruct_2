// Stage "parts": oriented parts (world/parts.js) - partId, partPriority, residentD, homeChunk and
// reachOf on samples, makePart on random placements (yaw, pitch, roll) and extents, PartBudget on
// random angles configurations with and without a lattice of cells round it (a regular one,
// defined the same in tests/city/test_parts.cpp) granting sequences of candidates, and
// roadPieces of random polylines.
import { REF, line, samples } from "../lib/rec.mjs";

const T = await import(REF + "world/parts.js");
const P = await import(REF + "core/placement.js");

const lattice = {
  indexAt: (axis, v) => Math.floor((v - (axis ? 64 : 0)) / 2048),
  cellRect: (i, j) => ({ x0: i * 2048, y0: j * 2048 + 64, x1: (i + 1) * 2048, y1: (j + 1) * 2048 + 64 }),
};

export default function* parts() {
  const r = samples(19);
  const big = (n) => Math.floor((r() - 0.5) * n);
  yield line("reach", T.PART_REACH, T.residentD({}), T.residentD({ world: { angles: { residentRadius: 64 } } }), T.residentD(null), T.residentD({ world: {} }));
  for (let i = 0; i < 2000; i += 1) {
    const ci = big(1e7);
    const cj = big(2 ** 40);
    const k = Math.floor(r() * 256);
    yield line("id", T.partId(ci, cj, k), T.partId(cj, ci, 255 - k), T.partId(-ci, 3, 0), T.partId(ci + 0.5, -cj, k));
  }
  for (const kind of ["road", "ramp", "wing", "building", "tree", ""])
    for (let i = 0; i < 50; i += 1) {
      const id = Math.floor(r() * 2 ** 30);
      yield line("prio", kind, T.partPriority(kind, id), T.partPriority(kind, id, { yieldsToGrid: true }), T.partPriority(kind, id, {}));
    }
  for (let i = 0; i < 1000; i += 1) {
    const x0 = big(1e5);
    const y0 = big(1e5);
    const w = 200 + Math.floor(r() * 4000);
    const h = 200 + Math.floor(r() * 4000);
    const rect = { x0, y0, x1: x0 + w, y1: y0 + h };
    const x = x0 + big(6000) + 1000 + (i % 3 ? 0 : r());
    const y = y0 + big(6000) + 1000;
    const z = big(2000);
    const home = T.homeChunk(x, y, z, rect);
    const ax0 = x - Math.floor(r() * 300);
    const ay0 = y - Math.floor(r() * 300);
    const ax1 = x + Math.floor(r() * 300);
    const ay1 = y + Math.floor(r() * 300);
    yield line("home", home.cx, home.cy, home.cz, T.reachOf({ x0: ax0, y0: ay0, x1: ax1, y1: ay1 }, home));
  }
  for (let i = 0; i < 300; i += 1) {
    const yaw = Math.floor(r() * 132);
    const pitch = i % 3 ? 0 : Math.floor(r() * 13) - 6;
    const roll = i % 7 ? 0 : Math.floor(r() * 132);
    const ox = big(8000);
    const oy = big(8000);
    const oz = Math.floor(r() * 200);
    const priority = Math.floor(r() * 100);
    const pl = new P.Placement({ origin: { x: ox, y: oy, z: oz }, yaw, pitch, roll, priority });
    const u0 = -Math.floor(r() * 20);
    const v0 = -Math.floor(r() * 20);
    const w0 = -Math.floor(r() * 10);
    const u1 = Math.floor(r() * 120);
    const v1 = Math.floor(r() * 80);
    const w1 = Math.floor(r() * 90);
    const extent = i % 4 === 1 ? { u0, v0, u1, v1 } : { u0, v0, w0, u1, v1, w1 };
    const ci = big(100);
    const cj = big(100);
    const cci = big(5000);
    const ccj = big(5000);
    const rx0 = ox - Math.floor(r() * 3000);
    const ry0 = oy - Math.floor(r() * 3000);
    const rx1 = ox + 1 + Math.floor(r() * 3000);
    const ry1 = oy + 1 + Math.floor(r() * 3000);
    const cell = { i: ci, j: cj, ci: cci, cj: ccj, rect: { x0: rx0, y0: ry0, x1: rx1, y1: ry1 } };
    const index = Math.floor(r() * 256);
    const p = T.makePart({ cell, index, key: `k${i}`, kind: ["road", "building", "wing", "ramp"][i % 4], placement: pl, extent, ...(i % 5 === 0 ? { anchored: true } : {}) });
    const a = p.aabb;
    yield line("part", p.id, p.cell, p.key, p.kind, a.x0, a.y0, a.z0, a.x1, a.y1, a.z1, p.home.cx, p.home.cy, p.home.cz, p.base.x, p.base.y, p.base.z, p.reach, p.anchored, p.priority,
      p.placement.yaw, p.placement.pitch, p.placement.roll);
  }
  for (let c = 0; c < 60; c += 1) {
    const angles = { enabled: r() < 0.9 };
    if (r() < 0.7) angles.partArea = [900, 1800, 3600, 6400][Math.floor(r() * 4)];
    if (r() < 0.5) angles.partCluster = 1 + Math.floor(r() * 6);
    if (r() < 0.5) angles.maxPartsPerChunk = 1 + Math.floor(r() * 3);
    if (r() < 0.5) angles.maxResident = 2 + Math.floor(r() * 12);
    if (r() < 0.5) angles.residentRadius = 32 + Math.floor(r() * 96);
    const config = { world: { angles } };
    const cellRect = lattice.cellRect(big(20), big(20));
    const b = new T.PartBudget(config, cellRect, { lattice: c % 4 === 3 ? null : lattice });
    yield line("budget", b.enabled, b.maxPerChunk, b.cluster, b.nx, b.ny, b.limit, b.maxResident, b.R, b.used.length);
    const out = [];
    for (let k = 0; k < 120; k += 1) {
      const x = cellRect.x0 + Math.floor(r() * 2048);
      const y = cellRect.y0 + Math.floor(r() * 2048);
      const home = T.homeChunk(x, y, Math.floor(r() * 10), cellRect);
      const fit = b.fits(x, y, home);
      out.push(`${fit ? 1 : 0}:${b.grant(x, y, home)}`);
    }
    yield line("grants", b.count, ...out);
    yield line("used", b.used, b.homes.flatMap((q) => [q.x, q.y]));
  }
  for (let i = 0; i < 100; i += 1) {
    const n = 2 + Math.floor(r() * 5);
    const pts = [];
    for (let k = 0; k < n; k += 1) {
      const x = big(3000);
      const y = big(3000) + (k % 2 ? r() : 0);
      pts.push({ x, y });
    }
    if (i % 10 === 0) pts.push({ ...pts[pts.length - 1] });
    const road = { id: `R${i}`, pts, hr: 8 + Math.floor(r() * 40) / 2 };
    for (const pc of T.roadPieces(road, { x0: -1500, y0: -1500, x1: 1500, y1: 1500 }))
      yield line("piece", pc.key, pc.seg, pc.k, pc.s0, pc.s1, pc.a.x, pc.a.y, pc.b.x, pc.b.y, pc.aabb.x0, pc.aabb.y0, pc.aabb.x1, pc.aabb.y1, pc.home.cx, pc.home.cy, pc.home.cz, pc.reach);
  }
}
