// Complex specs and records shared by the stages of the site complexes and the sites
// (tests/city/complexes.hpp is the C++ twin): specs drawn from a sample stream in the shapes the
// reference's site kinds give planComplex (a base's one sector under a compound, a campus's 2 x 2
// sectors with a tram, a stronghold's two sectors), and random ones; a complex's plan as record
// lines; the chunks a structure is rasterized into (ground-filled first) and their digests.
import { REF, line } from "./rec.mjs";

const { stairDims } = await import(REF + "buildings/interior/stairs.js");
const { LEVEL_GAP } = await import(REF + "sites/complex.js");
const { ChunkBuffer, P, P2 } = await import(REF + "voxel/chunk.js");
const { MAT } = await import(REF + "voxel/materials.js");
const { vx } = await import(REF + "core/units.js");

export const THEMES = ["military", "lab", "power", "barracks", "hangar", "containment"];
const SD = stairDims(30);

/** A stair shaft's rect (stairDims(30)) centred on x, its near end at y0. */
export function shaftRect(cx, y0) {
  return { x0: cx - Math.floor(SD.W / 2), x1: cx - Math.floor(SD.W / 2) + SD.W - 1, y0, y1: y0 + SD.L - 1 };
}

const R = (q) => [q.x0, q.y0, q.x1, q.y1];
const rects = (list) => list.map((q) => `${q.x0},${q.y0},${q.x1},${q.y1}`).join(";") || "none";

/**
 * A complex spec drawn from r: { spec, top } (top: the walking level of the surface). kind 0: a
 * base (one sector, a bunker shaft), 1: a campus (2 x 2 sectors, two portals, a tram), 2: a
 * stronghold (two sectors, a portal, a tram), 3: random (1-5 sectors, 0-3 entries, maybe a tram),
 * 4: tiny sectors (rooms rarely fit), 5: a sector crowded with entry shafts (corridors blocked,
 * rooms no route reaches dropped).
 */
export function drawSpec(r, kind) {
  const ox = Math.round((r() - 0.5) * 400000);
  const oy = Math.round((r() - 0.5) * 400000);
  const top = Math.round((r() - 0.4) * 3000);
  const theme = () => (r() < 0.1 ? undefined : THEMES[Math.floor(r() * THEMES.length)]);
  const levels = () => {
    const t = r();
    return t < 0.1 ? 1 : t < 0.55 ? 2 : t < 0.9 ? 3 : 4;
  };
  if (kind === 0) {
    const W = vx(170 + r() * 120);
    const H = vx(140 + r() * 100);
    const rect = { x0: ox, y0: oy, x1: ox + W - 1, y1: oy + H - 1 };
    const bounds = { x0: rect.x0 + vx(8), y0: rect.y0 + vx(8), x1: rect.x1 - vx(8), y1: rect.y1 - vx(8) };
    const bx = Math.round(rect.x0 + W * (0.72 + r() * 0.2));
    const by = Math.round(rect.y0 + H * (0.05 + r() * 0.25));
    const rooms = r() < 0.5 ? [9, 14] : undefined;
    const sector = { bounds, z0: top - vx(14), levels: levels(), theme: theme(), rooms };
    const openTop = r() < 0.8 ? true : r() < 0.5 ? false : undefined;
    return { top, spec: { sectors: [sector], entries: [{ rect: shaftRect(bx, by + 12), zTop: top, sector: 0, dir: r() < 0.8 ? -1 : 1, openTop }], tram: null } };
  }
  if (kind === 1) {
    const W = vx(300 + r() * 80);
    const H = vx(240 + r() * 80);
    const r0 = { x0: ox, y0: oy, x1: ox + W - 1, y1: oy + H - 1 };
    const pad = vx(10);
    const mx = Math.round((r0.x0 + r0.x1) / 2);
    const my = Math.round((r0.y0 + r0.y1) / 2);
    const quads = [
      { x0: r0.x0 + pad, y0: r0.y0 + pad, x1: mx - pad / 2, y1: my - pad / 2 },
      { x0: mx + pad / 2, y0: r0.y0 + pad, x1: r0.x1 - pad, y1: my - pad / 2 },
      { x0: r0.x0 + pad, y0: my + pad / 2, x1: mx - pad / 2, y1: r0.y1 - pad },
      { x0: mx + pad / 2, y0: my + pad / 2, x1: r0.x1 - pad, y1: r0.y1 - pad },
    ];
    const themes = ["lab", "containment", "power", "barracks"];
    for (let i = 3; i > 0; i -= 1) {
      const j = Math.floor(r() * (i + 1));
      const t = themes[i];
      themes[i] = themes[j];
      themes[j] = t;
    }
    const sectors = quads.map((b, k) => ({ bounds: b, z0: top - vx(16) - k * vx(9), levels: 2 + Math.floor(r() * 2), theme: themes[k], rooms: [8, 12] }));
    let deepest = Infinity;
    for (const s of sectors) deepest = Math.min(deepest, s.z0 - (s.levels - 1) * LEVEL_GAP);
    const entries = [];
    const seen = new Set();
    for (const [fx, fy] of [[0.18 + r() * 0.1, 0.38], [0.6 + r() * 0.1, 0.4]]) {
      const cx = Math.round(r0.x0 + W * fx);
      const qy = Math.round(r0.y0 + H * fy);
      let sector = quads.findIndex((b) => cx >= b.x0 && cx <= b.x1 && qy >= b.y0 && qy <= b.y1);
      if (sector < 0) sector = 0;
      if (seen.has(sector)) continue;
      seen.add(sector);
      entries.push({ rect: shaftRect(cx, qy + 12), zTop: top, sector, dir: -1, openTop: true });
    }
    return { top, spec: { sectors, entries, tram: { z: deepest - LEVEL_GAP } } };
  }
  if (kind === 2) {
    const L = vx(70 + r() * 20);
    const Wd = vx(55 + r() * 15);
    const halfL = Math.round(L * 1.05);
    const halfW = Math.round(Wd * 1.05);
    const alongX = r() < 0.5;
    const q = (u0, v0, u1, v1) => (alongX ? { x0: ox + u0, y0: oy + v0, x1: ox + u1, y1: oy + v1 } : { x0: ox + v0, y0: oy + u0, x1: ox + v1, y1: oy + u1 });
    const quads = [q(vx(6), -halfW, halfL, halfW), q(-halfL, -halfW, -vx(6), halfW)];
    const themes = r() < 0.5 ? ["military", "power"] : ["lab", "containment"];
    const sectors = quads.map((b, k) => ({ bounds: b, z0: top - vx(18) - k * vx(8), levels: 2 + Math.floor(r() * 2), theme: themes[k], rooms: [8, 12] }));
    let deepest = Infinity;
    for (const s of sectors) deepest = Math.min(deepest, s.z0 - (s.levels - 1) * vx(15));
    const px = ox + Math.round((r() - 0.5) * L);
    const py = oy + Math.round((r() - 0.5) * Wd);
    let sector = quads.findIndex((b) => px >= b.x0 && px <= b.x1 && py >= b.y0 && py <= b.y1);
    if (sector < 0) sector = 0;
    return { top, spec: { sectors, entries: [{ rect: shaftRect(px, py + 12), zTop: top + 1, sector, dir: -1, openTop: true }], tram: { z: deepest - vx(15) } } };
  }
  if (kind === 5) {
    const W = vx(150 + r() * 100);
    const bounds = { x0: ox, y0: oy, x1: ox + W, y1: oy + W };
    const ne = 10 + Math.floor(r() * 20);
    const entries = [];
    for (let k = 0; k < ne; k += 1) {
      const cx = Math.round(ox + r() * W);
      const cy = Math.round(oy + r() * W);
      entries.push({ rect: shaftRect(cx, cy), zTop: top, sector: 0, dir: r() < 0.5 ? -1 : 1, openTop: true });
    }
    return { top, spec: { sectors: [{ bounds, z0: top - vx(14), levels: 1 + Math.floor(r() * 2), theme: theme(), rooms: [ne + 10, ne + 30] }], entries, tram: null } };
  }
  const n = kind === 4 ? 1 + Math.floor(r() * 2) : 1 + Math.floor(r() * 5);
  const sectors = [];
  let x = ox;
  for (let k = 0; k < n; k += 1) {
    const w = kind === 4 ? vx(20 + r() * 40) : vx(60 + r() * 340);
    const h = kind === 4 ? vx(20 + r() * 40) : vx(60 + r() * 340);
    const y = oy + Math.round((r() - 0.5) * vx(200));
    const t = r();
    const rooms = t < 0.3 ? undefined : t < 0.6 ? [8, 12] : [Math.floor(r() * 14), 0];
    if (rooms && rooms[1] === 0) rooms[1] = rooms[0] + Math.floor(r() * 8);
    sectors.push({ bounds: { x0: x, y0: y, x1: x + w - 1, y1: y + h - 1 }, z0: top - vx(12 + r() * 30), levels: levels(), theme: theme(), rooms });
    x += w + vx(10 + r() * 60);
  }
  const entries = [];
  const ne = Math.floor(r() * 4);
  for (let k = 0; k < ne; k += 1) {
    const s = Math.floor(r() * (n + 1)) - (r() < 0.1 ? 1 : 0);
    const b = sectors[Math.max(0, Math.min(n - 1, s))].bounds;
    const cx = Math.round(b.x0 + (b.x1 - b.x0) * r());
    const cy = Math.round(b.y0 + (b.y1 - b.y0) * r());
    const openTop = r() < 0.6 ? true : r() < 0.5 ? false : undefined;
    entries.push({ rect: shaftRect(cx, cy), zTop: top + Math.floor(r() * 3), sector: s, dir: r() < 0.5 ? -1 : 1, openTop });
  }
  let tram = null;
  if (r() < 0.5) {
    let deepest = Infinity;
    for (const s of sectors) deepest = Math.min(deepest, s.z0 - (s.levels - 1) * LEVEL_GAP);
    tram = { z: deepest - LEVEL_GAP - Math.floor(r() * vx(10)) };
  }
  return { top, spec: { sectors, entries, tram } };
}

/** A complex's plan as record lines. */
export function* complexLines(cx) {
  for (const s of cx.sectors)
    yield line("sec", s.id, s.theme, ...R(s.bounds), s.center.x, s.center.y, ...R(s.shaftRect), s.levels.map((l) => l.k).join(","), s.themeDef.id);
  for (const lv of cx.levels) {
    yield line("lv", lv.sector, lv.k, lv.zf, lv.theme, rects(lv.keep), lv.keepClear ? rects(lv.keepClear) : "-", lv.blocked.map((p) => p.join(":")).join(",") || "none");
    for (const q of lv.rooms) yield line("rm", ...R(q), q.type, q.shape, q.fixed, q.lower);
    yield line("co", rects(lv.corridors));
  }
  for (const sh of cx.shafts) {
    const st = sh.st;
    yield line("sh", ...R(sh.rect), sh.dir, sh.openTop, sh.zLow, sh.zHigh, sh.levels.join(","), ...R(st.rect), st.axis, st.dir, st.laneLow, st.lane, st.landing, st.f0, st.f1, st.L, st.W, st.open,
      st.flights.map((fl) => `${fl.f}/${fl.z0}/${fl.H}`).join(","));
  }
  for (const L of cx.ladders) yield line("la", ...R(L.rect), L.zTop, L.zBot, L.upper, L.lower, L.sector);
  if (cx.tram) {
    const t = cx.tram;
    yield line("tram", t.z, rects(t.segs), t.routes.map((pts) => pts.map((p) => `${p.x},${p.y}`).join(" ")).join("|"));
    for (const s of t.stations) yield line("st", s.sector, ...R(s.hall), s.hall.type, s.hall.fixed, s.track.x, s.track.y);
  }
  yield line("bounds", cx.bounds ? R(cx.bounds) : "-");
}

/** FNV-1a over a string's characters (ASCII). */
export function textDigest(s, h = 2166136261) {
  for (let i = 0; i < s.length; i += 1) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}

/**
 * Box lines: a list's tag and length, then each box (full) or, for long lists, a digest of every
 * 32 boxes' lines (the block that differs narrows a difference down; full lines show it).
 */
export function* boxLines(tag, list, full = true) {
  yield line(tag, list.length);
  if (full) {
    for (const b of list) yield line(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1, b.m, b.mode);
    return;
  }
  for (let k = 0; k < list.length; k += 32) {
    let h = 2166136261;
    for (const b of list.slice(k, k + 32)) h = textDigest(line(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1, b.m, b.mode) + "\n", h);
    yield line(k, h);
  }
}

/** FNV-1a over a typed array's values (stages/chunk.mjs digest). */
export function digest(a) {
  let h = 2166136261;
  for (let i = 0; i < a.length; i += 1) h = Math.imul(h ^ a[i], 16777619);
  return h >>> 0;
}

/**
 * A chunk filled as the ground pass would leave it under a surface at z = surface: rock below
 * (with a sparse lattice of air cells, existing voids) and air above.
 */
export function groundChunk(lod, cx, cy, cz, surface) {
  const ch = new ChunkBuffer(lod, cx, cy, cz);
  const d = ch.data;
  for (let k = 0; k < P; k += 1) {
    if (ch.wz(k) > surface) continue;
    for (let j = 0; j < P; j += 1)
      for (let i = 0; i < P; i += 1) d[i + j * P + k * P2] = (i * 7 + j * 13 + k * 5) % 11 === 0 ? 0 : MAT.ROCK;
  }
  return ch;
}

/** Points of a complex worth a chunk: rooms and corridors at their levels, shafts, ladders, the tram. */
export function complexPoints(cx) {
  const pts = [];
  const mid = (q) => [Math.round((q.x0 + q.x1) / 2), Math.round((q.y0 + q.y1) / 2)];
  for (const lv of cx.levels) {
    for (const q of lv.rooms) pts.push([...mid(q), lv.zf + 4]);
    for (const c of lv.corridors) pts.push([...mid(c), lv.zf]);
  }
  for (const sh of cx.shafts) pts.push([...mid(sh.rect), sh.zLow], [...mid(sh.rect), sh.zHigh]);
  for (const L of cx.ladders) pts.push([...mid(L.rect), L.zTop]);
  if (cx.tram) {
    for (const s of cx.tram.stations) pts.push([s.track.x, s.track.y, cx.tram.z]);
    for (const c of cx.tram.segs) pts.push([...mid(c), cx.tram.z]);
  }
  return pts;
}

/** Rasterizes a structure into a chunk as siteSource does (custom volumes, then the boxes by index). */
export function rasterizeStructure(st, chunk, deep) {
  const box = chunk.worldBox;
  for (const c of st.custom ?? []) {
    const b = c.bb;
    if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1 || b.z1 < box.z0 || b.z0 > box.z1) continue;
    c.rasterize(chunk);
  }
  const qs = st.grid.query(box);
  qs.sort((a, b) => a.i - b.i);
  for (const q of qs) {
    if (q.z1 < box.z0 || q.z0 > box.z1 || q.z1 < deep) continue;
    chunk.fillBox(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode ?? 0);
  }
}
