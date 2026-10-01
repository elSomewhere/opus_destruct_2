// Stage "sitelinks": the site links (sites/links.js) over every world of lib/worlds.mjs, between
// the sites of the stand-in kinds of lib/sitekinds.mjs (the reference's kinds come later; their
// ports are the same: a base's deepest anteroom, the tram station nearest the other site): every
// link of the lattice cells round the origin - its sites, ports, legs, length, profile (cover
// under the terrain, grade-limited) and bounds -, zAt along it, near and mapData over rects, and
// siteLinkSource: its z ranges over tile rects along the links and the tunnels rasterized into
// ground-filled chunks at LODs 0 to 2. Every base height a terrain sample here can read is made
// first (lib/worlds.mjs warmBasesIn; docs/CITY.md §6).
import { REF, line, samples } from "../lib/rec.mjs";
import { allWorlds, warmBasesIn } from "../lib/worlds.mjs";
import { digest, groundChunk } from "../lib/complexes.mjs";
import { registerSiteKinds } from "../lib/sitekinds.mjs";

const { World } = await import(REF + "world/World.js");
const { SiteLayer } = await import(REF + "world/sites.js");
const { SiteLinks, siteLinkSource } = await import(REF + "sites/links.js");

registerSiteKinds();

const R = (q) => [q.x0, q.y0, q.x1, q.y1];
const ids = (list) => list.map((L) => L.id).join(",") || "none";

function* linkLines(L) {
  yield line("link", L.id, L.a.id, L.b.id, L.A.x, L.A.y, L.A.z, L.A.d, L.B.x, L.B.y, L.B.z, L.B.d, L.len, L.zlo, L.zhi, R(L.bb), L.prof.length);
  for (const g of L.segs) yield line("seg", g.p.x, g.p.y, g.q.x, g.q.y, g.alongX, g.s0, g.len, g.dirSign, R(g.rect));
  yield line("prof", L.prof);
}

/** A point of a link drawn from r: on one of its legs, with its arc length. */
function pointOn(r, L) {
  const g = L.segs[Math.floor(r() * L.segs.length)];
  const t = Math.floor(r() * g.len);
  return { x: g.alongX ? g.p.x + g.dirSign * t : g.p.x, y: g.alongX ? g.p.y : g.p.y + g.dirSign * t, s: g.s0 + t };
}

export default function* sitelinks() {
  const r = samples(47);
  yield line("source", siteLinkSource.id, siteLinkSource.order, siteLinkSource.maxLod);
  for (const [key, overrides] of allWorlds()) {
    const w = new World(overrides);
    w.sites = new SiteLayer(w);
    const SL = (w.siteLinks = new SiteLinks(w));
    const cell = w.sites.cell;
    warmBasesIn(w, { x0: -6 * cell, y0: -6 * cell, x1: 7 * cell, y1: 7 * cell });
    yield line("world", key);
    const links = [];
    for (let b = -3; b <= 2; b += 1)
      for (let a = -3; a <= 2; a += 1)
        for (let dir = 0; dir < 2; dir += 1) {
          const L = SL.link(a, b, dir);
          if (!L) {
            yield line("none", a, b, dir);
            continue;
          }
          links.push(L);
          yield* linkLines(L);
        }
    for (const L of links) {
      const z = [];
      for (let k = 0; k < 12; k += 1) {
        const s = Math.round(-300 + r() * (L.len + 600)) + (k % 3 === 0 ? 0.5 : 0);
        z.push(`${s}:${SiteLinks.zAt(L, s)}`);
      }
      yield line("z", L.id, z.join(" "));
      // the source's z ranges over tile rects along the link, and its chunks
      for (const lod of [0, 1, 2]) {
        const p = pointOn(r, L);
        const s0 = 1 << lod;
        const cx = Math.floor((p.x + Math.floor((r() - 0.5) * 64 * s0)) / (32 * s0));
        const cy = Math.floor((p.y + Math.floor((r() - 0.5) * 64 * s0)) / (32 * s0));
        const bx = (cx * 32 - 1) * s0;
        const by = (cy * 32 - 1) * s0;
        const rect = { x0: bx, y0: by, x1: bx + 34 * s0 - 1, y1: by + 34 * s0 - 1 };
        yield line("zr", L.id, lod, ...R(rect), siteLinkSource.zRange(w, rect, lod) ?? "-");
      }
      // round the corner of an L route: which leg owns which column
      if (L.segs.length > 1) {
        const c = L.segs[1].p;
        const zl = SiteLinks.zAt(L, L.segs[1].s0);
        for (let k = 0; k < 2; k += 1) {
          const cx = Math.floor((c.x + Math.floor((r() - 0.5) * 48)) / 32);
          const cy = Math.floor((c.y + Math.floor((r() - 0.5) * 48)) / 32);
          const cz = Math.floor((zl + 20) / 32);
          const ch = groundChunk(0, cx, cy, cz, zl + 400);
          siteLinkSource.rasterize(w, ch);
          yield line("corner", L.id, cx, cy, cz, ch.countNonAir(), digest(ch.data));
        }
      }
      for (const lod of [0, 0, 0, 1, 2]) {
        const p = pointOn(r, L);
        const zl = SiteLinks.zAt(L, p.s);
        const e = 32 << lod;
        const cx = Math.floor((p.x + Math.floor((r() - 0.5) * e)) / e);
        const cy = Math.floor((p.y + Math.floor((r() - 0.5) * e)) / e);
        const cz = Math.floor((zl + 20 + Math.floor((r() - 0.5) * e)) / e);
        const ch = groundChunk(lod, cx, cy, cz, zl + 400);
        siteLinkSource.rasterize(w, ch);
        yield line("ch", L.id, lod, cx, cy, cz, ch.countNonAir(), digest(ch.data));
      }
    }
    for (let k = 0; k < 12; k += 1) {
      const x0 = Math.round((r() - 0.5) * 5 * cell);
      const y0 = Math.round((r() - 0.5) * 5 * cell);
      const rect = { x0, y0, x1: x0 + Math.round(r() * cell * 0.5), y1: y0 + Math.round(r() * cell * 0.5) };
      yield line("near", ...R(rect), ids(SL.near(rect)));
      if (k % 3 === 0) yield line("map", SL.mapData(rect).map((m) => `${m.id}:${m.pts.map((p) => `${p.x},${p.y}`).join(";")}`).join(" ") || "none");
    }
  }
}
