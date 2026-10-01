// Shared inputs and records of the building stages (archetypes, houses, facade, sample; the C++
// twin is tests/city/building_records.hpp): the districts an envelope may be planned in, the
// configs of every world, scripted lots (plain and turned) and the envelope records.
//
// The registering modules are imported in the reference's order (docs/CITY.md §2.2), so the
// registries hold what the port registers (world/register_all.cpp).
import { REF, f } from "./rec.mjs";
import { allWorlds } from "./worlds.mjs";

const { DISTRICTS } = await import(REF + "world/registry.js");
await import(REF + "city/districts.js");
const FL = await import(REF + "city/flavors.js");
await import(REF + "buildings/styles.js");
await import(REF + "buildings/civic.js");
await import(REF + "buildings/archetypes.js");
const F = await import(REF + "buildings/frame.js");
const { makeConfig } = await import(REF + "config/defaults.js");

/** Every district, then each one as every flavor sees it (village flavors too): flavoredDistrict(d, fl). */
export function districtList() {
  const flavors = FL.FLAVORS.all();
  const villages = flavors.map((fl) => FL.flavorOf({ village: true, flavor: fl.id }));
  const out = [...DISTRICTS.all()];
  for (const d of DISTRICTS.all()) for (const fl of [...flavors, ...villages]) out.push(FL.flavoredDistrict(d, fl));
  return out;
}

/** The merged config of every world of lib/worlds.mjs (allWorlds), in its order. */
export function configList() {
  return allWorlds().map(([, o]) => makeConfig(o));
}

const FRONTS = ["N", "E", "S", "W"];

/**
 * A scripted lot whose frame is U x V cells (draws from r): plain, a world rect facing one of the
 * four fronts, or (one in four) turned, a turn of a table yaw about an origin with its nominal
 * front, its rect the turned frame's world bounds; corner and micro flags; an id of cell, block and
 * lot numbers from k; the district id given.
 */
export function scriptedLot(r, U, V, k, districtId) {
  const turned = r() < 0.25;
  const fi = Math.floor(r() * 4);
  const corner = r() < 0.3;
  const micro = r() < 0.12;
  const x0 = Math.floor((r() - 0.5) * 400000);
  const y0 = Math.floor((r() - 0.5) * 400000);
  const lot = { id: `C${(k % 9) - 4}_${(k % 7) - 3}/b${k % 13}/l${k}`, district: districtId, corner, micro };
  if (turned) {
    const yaw = Math.floor(r() * 132);
    const ou = Math.floor(r() * 41) - 20;
    const ov = Math.floor(r() * 41) - 20;
    lot.front = F.nominalFront(yaw);
    lot.turn = { yaw, origin: { x: x0, y: y0 }, ou, ov, U, V };
    lot.rect = { ...F.lotFrameOf(lot).R };
  } else {
    lot.front = FRONTS[fi];
    const ns = lot.front === "N" || lot.front === "S";
    lot.rect = { x0, y0, x1: x0 + (ns ? U : V) - 1, y1: y0 + (ns ? V : U) - 1 };
  }
  return lot;
}

export const rectStr = (q) => `${f(q.x0)},${f(q.y0)},${f(q.x1)},${f(q.y1)}`;
const join = (list, sep, fmt) => (list && list.length ? list.map(fmt).join(sep) : "-");
const tiersStr = (tiers) => join(tiers, "|", (t) => `${f(t.f0)}/${f(t.f1)}/${t.rects.map(rectStr).join(";")}`);
const roofStr = (o) => [o.type, o.pitch, o.ridge, o.slope, o.overhang, o.crown, o.chimney].map(f).join("/");
const programStr = (p) => [p.ground, p.podium, p.upper].map(f).join("/");
const steepleStr = (s) => (s ? [s.rect, s.shaft, s.spire, s.dome, s.tent].map(f).join("/") : "-");
const domesStr = (d) => join(d, ";", (q) => [q.rect, q.fv, q.r, q.drum, q.h, q.m].map(f).join("/"));
const civicStr = (x) => (x ? [x.civic, x.storefront, x.sign, x.signColor, x.portico, x.parking, x.setF].map(f).join("/") : "-");
const turnStr = (t) => (t ? [t.yaw, t.yaw2, t.origin.x, t.origin.y, t.ou, t.ov].map(f).join("/") : "-");

/** An archetype's raw envelope (what envelope(ctx) returns) as a record. */
export function specLine(env) {
  if (!env) return "spec -";
  const annexes = join(env.annexes, "|", (a) => `${a.kind}/${rectStr(a.rect)}/${f(a.height)}`);
  return [
    "spec",
    tiersStr(env.tiers),
    annexes,
    f(env.floors),
    f(env.storyH),
    f(env.basements),
    roofStr(env.roof),
    programStr(env.program),
    f(env.podiumFloors),
    f(env.entranceSide),
    f(!!env.stoop),
    f(!!env.yard),
    f(!!env.plinth),
    f(!!env.porch),
    steepleStr(env.steeple),
    domesStr(env.domes),
    f(env.forceStyle),
    civicStr(env.extra),
  ].join(" ");
}

/** A building's envelope (finalizeEnvelope's record) as a record: every field, in its order. */
export function envLine(e) {
  if (!e) return "env -";
  const annexes = join(e.annexes, "|", (a) => `${a.kind}/${rectStr(a.rect)}/${f(a.height)}/${rectStr(a.world)}/${a.canon ? rectStr(a.canon) : "-"}`);
  const civic = e.civic === undefined ? "-" : [e.civic, e.storefront, e.sign, e.signColor, e.portico, e.parking, e.setF].map(f).join("/");
  return [
    "env",
    f(e.mirror),
    f(e.entranceU),
    e.id,
    e.lot,
    e.archetype,
    e.style,
    f(e.district),
    e.front,
    rectStr(e.R),
    f(e.U),
    f(e.V),
    tiersStr(e.tiers),
    annexes,
    f(e.floors),
    f(e.storyH),
    f(e.basements),
    f(e.basementH),
    f(e.baseZ),
    f(e.groundZ),
    roofStr(e.roof),
    programStr(e.program),
    f(e.podiumFloors),
    f(e.stoop),
    f(e.yard),
    f(e.plinth),
    f(e.porch),
    steepleStr(e.steeple),
    domesStr(e.domes),
    civic,
    f(e.topZ),
    f(e.bottomZ),
    rectStr(e.bounds),
    turnStr(e.turn),
  ].join(" ");
}

/** Run lengths of a label grid: value:count,... */
export function rle(a) {
  const out = [];
  let i = 0;
  while (i < a.length) {
    let j = i;
    while (j < a.length && a[j] === a[i]) j += 1;
    out.push(`${a[i]}:${j - i}`);
    i = j;
  }
  return out.join(",");
}
const fdoor = (d) => [d.id, d.u0, d.u1, d.v0, d.v1, d.orient, d.a, d.b, d.kind, d.sideA, d.width, d.leaf, d.height].map(f).join(",");
const froom = (m) => [m.id, m.type, m.rects.map(rectStr).join("|"), m.paint, m.floorMat, m.stair, m.unit, m.template].map(f).join(",");

/** A floor grid's records (as stage floorgrid has them): its cells, rooms, doors with their leaves, the door graph. */
export function* gridRecords(tag, g) {
  yield [`${tag}c`, f(g.U), f(g.V), f(g.doorExtra), rle(g.cells) || "-"].join(" ");
  for (const m of g.rooms) yield [`${tag}r`, froom(m), f(g.area(m))].join(" ");
  for (const d of g.doors) {
    const leaf = g.doorLeafRect(d);
    yield [`${tag}d`, fdoor(d), leaf ? rectStr(leaf) : "-"].join(" ");
  }
  const adj = g.doorGraph();
  yield [`${tag}g`, [...adj].map(([k, s]) => `${k}>${[...s].join("/")}`).join(" ") || "-"].join(" ");
}
