// Stage "sites": the site layer (world/sites.js) over every world of lib/worlds.mjs, with the
// stand-in site kinds of lib/sitekinds.mjs (the reference's kinds come later): the sites of the
// lattice cells round the origin (and, on a cube face, round its edge) - kind, anchor, arterial
// cell, rects, pads, seed, plan bounds -, padDistance, the ground override at points round them
// (levelled pads, blend ramps, path and ramp pads, the kinds' materials), sitesNear,
// sitesInCell, nearest and mapData, and siteSource: the z ranges of rects over the sites, and
// their structures rasterized into ground-filled chunks at LODs 0 to 5 (custom volumes, coarse
// LODs leaving out what lies deep in the rock). The worlds are bare (World.js: no highways, no
// water - neither yet ported - to keep sites away from). Every base height a terrain sample here
// can read is made first (lib/worlds.mjs warmBasesIn; docs/CITY.md §6).
import { REF, line, samples } from "../lib/rec.mjs";
import { allWorlds, warmBasesIn } from "../lib/worlds.mjs";
import { complexPoints, digest, groundChunk, textDigest } from "../lib/complexes.mjs";
import { registerSiteKinds } from "../lib/sitekinds.mjs";

const { World } = await import(REF + "world/World.js");
const { SiteLayer, siteSource, padDistance } = await import(REF + "world/sites.js");

registerSiteKinds();

const R = (q) => [q.x0, q.y0, q.x1, q.y1];
const ids = (list) => list.map((s) => s.id).join(",") || "none";

function padLine(p) {
  const path = p.path ? `${p.path.length}:${p.path.map((q) => `${q.x},${q.y},${q.z}`).join(";")}` : "-";
  const ramp = p.ramp ? [p.ramp.axis, p.ramp.a0, p.ramp.a1, p.ramp.z0, p.ramp.z1] : "-";
  return line("pad", ...R(p.rect), p.z, p.margin, p.round, ramp, path, p.half, p.road);
}

function siteLines(s) {
  const b = s.plan.bounds;
  return [
    line("site", s.id, s.type, s.a, s.b, s.cell.i, s.cell.j, ...R(s.cellRect), ...R(s.rect), ...R(s.blend), s.margin, s.padZ, s.seed, s.center.x, s.center.y, b ? R(b) : "-",
      R(s.placed.footprint), s.pads.length),
    ...s.pads.map(padLine),
  ];
}

export default function* sites() {
  const r = samples(43);
  for (const [key, overrides] of allWorlds()) {
    const w = new World(overrides);
    const L = (w.sites = new SiteLayer(w));
    const cell = L.cell;
    // the lattice cells probed: round the origin, and on a cube face round its edge
    const cells = [];
    for (let b = -3; b <= 3; b += 1) for (let a = -3; a <= 3; a += 1) cells.push([a, b]);
    const edge = w.chart.half ? Math.floor((w.chart.half * 8) / cell) : null;
    if (edge !== null) for (let b = -1; b <= 1; b += 1) for (let a = edge - 2; a <= edge + 1; a += 1) cells.push([a, b]);
    warmBasesIn(w, { x0: -6 * cell, y0: -6 * cell, x1: 7 * cell, y1: 7 * cell });
    if (edge !== null) warmBasesIn(w, { x0: (edge - 3) * cell, y0: -3 * cell, x1: (edge + 4) * cell, y1: 4 * cell });
    yield line("world", key, cell, L.n, edge);
    const found = [];
    for (const [a, b] of cells) {
      const s = L.siteAt(a, b);
      if (!s) {
        yield line("none", a, b);
        continue;
      }
      found.push(s);
      yield* siteLines(s);
    }
    for (const s of found) {
      // padDistance and the ground at points round the site (its blend rect and a margin more)
      for (const p of s.pads) {
        const q = p.rect;
        const d = [];
        for (let k = 0; k < 4; k += 1) {
          const x = Math.round(q.x0 - p.margin + r() * (q.x1 - q.x0 + 2 * p.margin));
          const y = Math.round(q.y0 - p.margin + r() * (q.y1 - q.y0 + 2 * p.margin));
          d.push(`${x},${y}:${padDistance(p, x, y)}`);
        }
        yield line("pd", s.id, d.join(" "));
      }
      const B = s.blend;
      const m = s.margin;
      const out = {};
      for (let k = 0; k < 40; k += 1) {
        let x;
        let y;
        if (k < 30) {
          x = Math.round(B.x0 - m + r() * (B.x1 - B.x0 + 2 * m));
          y = Math.round(B.y0 - m + r() * (B.y1 - B.y0 + 2 * m));
        } else {
          // on a pad (a path pad's points too)
          const p = s.pads[Math.floor(r() * s.pads.length)];
          const pts = p.path;
          if (pts) {
            const q = pts[Math.floor(r() * pts.length)];
            x = q.x + Math.round((r() - 0.5) * 60);
            y = q.y + Math.round((r() - 0.5) * 60);
          } else {
            x = Math.round(p.rect.x0 + r() * (p.rect.x1 - p.rect.x0));
            y = Math.round(p.rect.y0 + r() * (p.rect.y1 - p.rect.y0));
          }
        }
        const nat = s.padZ + Math.round((r() - 0.5) * 160);
        const g = L.ground(x, y, nat, out);
        if (!g) {
          yield line("g", x, y, nat, "-");
          continue;
        }
        yield line("g", x, y, nat, g.z, g.mat, g.sub, g.site.id, g.site.pads.indexOf(g.pad), g.natural, g.inside, g.pad.path ? [g.pad._z, g.pad._s, g.pad._c] : "-");
      }
      yield line("cell", s.id, ids(L.sitesInCell(s.cell.i, s.cell.j)));
    }
    // sitesNear, nearest and mapData round the origin
    for (let k = 0; k < 20; k += 1) {
      const x0 = Math.round((r() - 0.5) * 6 * cell);
      const y0 = Math.round((r() - 0.5) * 6 * cell);
      const rect = { x0, y0, x1: x0 + Math.round(r() * cell), y1: y0 + Math.round(r() * cell) };
      yield line("near", ...R(rect), ids(L.sitesNear(rect)));
      const n = L.nearest(x0, y0, 1 + Math.floor(r() * 6), Math.floor(r() * 3));
      yield line("nearest", x0, y0, n.map((q) => `${q.id}/${q.type}/${q.x}/${q.y}/${q.d}/${q.z}`).join(",") || "none");
      yield line("map", L.mapData(rect).map((q) => `${q.id}/${q.type}/${R(q.rect).join(",")}/${q.center.x},${q.center.y}`).join(" ") || "none");
    }
    yield line("nearest4", L.nearest(0, 0).map((q) => q.id).join(",") || "none");
    // structures and siteSource round the sites of the cells next to the origin
    for (const s of found) {
      if (Math.abs(s.a) > 1 || Math.abs(s.b) > 1) continue;
      const st = s.def.structure(w, s);
      let h = 2166136261;
      for (const q of st.boxes) h = textDigest(line(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode) + "\n", h);
      const u = s.plan.under;
      yield line("st", s.id, st.boxes.length, h, st.bb ? [st.bb.x0, st.bb.y0, st.bb.z0, st.bb.x1, st.bb.y1, st.bb.z1] : "-", (st.custom ?? []).length,
        u ? `${u.sectors.length}/${u.levels.length}/${u.shafts.length}/${u.ladders.length}/${u.tram ? u.tram.stations.length : 0}` : "-");
      // z ranges at the corners of the site's bounds (beyond its structure's, some)
      const sb = s.plan.bounds ?? s.blend;
      for (const [x, y] of [[sb.x0, sb.y0], [sb.x1, sb.y0], [sb.x0, sb.y1], [sb.x1, sb.y1]]) {
        const rect = { x0: x - 40, y0: y - 40, x1: x + 40, y1: y + 40 };
        yield line("zc", s.id, ...R(rect), siteSource.zRange(w, rect, 0) ?? "-");
      }
      const pts = u ? complexPoints(u) : [];
      pts.push([Math.round(s.center.x), Math.round(s.center.y), s.padZ]);
      for (const lod of [0, 0, 0, 0, 1, 2, 2, 3, 4, 5]) {
        const [x, y, z] = pts[Math.floor(r() * pts.length)];
        const e = 32 << lod;
        const cx = Math.floor((x + Math.floor((r() - 0.5) * e * 0.5)) / e);
        const cy = Math.floor((y + Math.floor((r() - 0.5) * e * 0.5)) / e);
        const cz = Math.floor((z + Math.floor((r() - 0.5) * e * 0.5)) / e);
        const ch = groundChunk(lod, cx, cy, cz, s.padZ);
        const s0 = 1 << lod;
        const bx = (cx * 32 - 1) * s0;
        const by = (cy * 32 - 1) * s0;
        const rect = { x0: bx, y0: by, x1: bx + 34 * s0 - 1, y1: by + 34 * s0 - 1 };
        const zr = siteSource.zRange(w, rect, lod);
        siteSource.rasterize(w, ch, { zMin: s.padZ - 6 });
        yield line("ch", s.id, lod, cx, cy, cz, zr ?? "-", ch.countNonAir(), digest(ch.data));
      }
    }
  }
}
