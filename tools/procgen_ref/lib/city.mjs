// Record helpers of the city stages (cellnet, streets, townplan): cell networks, their roads,
// blocks and sub-cells as lines (tests/city/city_records.hpp is the C++ twin).
import { f, line } from "./rec.mjs";

/** A rect {x0, y0, x1, y1}. */
export const frect = (r) => `${f(r.x0)},${f(r.y0)},${f(r.x1)},${f(r.y1)}`;
/** A road side {cls, hr, id} (blockPoly's NO_SIDE: -/0/-). */
export const fside = (s) => `${f(s.cls)}/${f(s.hr)}/${f(s.id)}`;
/** A block's or sub-cell's sides, N E S W. */
export const fsides = (s) => [s.N, s.E, s.S, s.W].map(fside).join(";");
/** Points {x, y}. */
export const fpts = (pts) => pts.map((p) => `${f(p.x)},${f(p.y)}`).join(";");
/** A polygon of {x, y, side}, or "-". */
export const fpoly = (p) => (p ? p.map((q) => `${f(q.x)},${f(q.y)},${fside(q.side)}`).join(";") : "-");
/** A block's cuts (blockPoly), or "-". */
export const fcuts = (cuts) => (cuts ? cuts.map((k) => [k.nx, k.ny, k.c, k.cls, k.hr, k.id].map(f).join(",")).join(";") : "-");

// The fields the port's records have (network/road.hpp, city/cellNetwork.hpp): a field the
// reference gives one of them that is not listed here would be one the port lacks.
const ROAD_KEYS = new Set(["id", "cell", "cls", "pts", "hc", "hr", "corner", "median", "parking", "lanes", "lane", "sidewalk", "shoulder", "home", "arterialEdge", "paving", "diagonal", "sub", "strip"]);
const BLOCK_KEYS = new Set(["id", "cell", "sub", "district", "rect", "sides", "prop", "u", "core", "poly", "cuts"]);
const SUB_KEYS = new Set(["id", "rect", "sides", "district", "u", "core", "settlement", "fringe"]);
const EDGE_KEYS = new Set(["axis", "line", "span", "fixed", "s0", "s1", "cls", "spec", "hr", "wob", "paving", "pts", "u"]);
const NET_KEYS = new Set(["id", "i", "j", "rect", "edges", "roads", "blocks", "subcells"]);
function checkKeys(what, rec, keys) {
  for (const k of Object.keys(rec)) if (!keys.has(k)) throw new Error(`${what} ${rec.id ?? ""}: a field the port's record lacks: ${k}`);
}

/** An edge (cellNetwork's edgeInfo). */
export function edgeLine(tag, e) {
  checkKeys("edge", e, EDGE_KEYS);
  return line(tag, e.axis, e.line, e.span, e.fixed, e.s0, e.s1, e.cls, e.spec ? e.spec.cls : "-", e.hr, e.wob, e.paving, e.u, fpts(e.pts));
}

/** A road record (makeRoad's, with everything a cell network gives it). */
export function roadLine(r) {
  checkKeys("road", r, ROAD_KEYS);
  return line("road", r.id, r.cell, r.cls, r.hc, r.hr, r.corner, r.median, r.parking, r.lanes, r.lane, r.sidewalk, r.shoulder, r.home ? `${f(r.home[0])},${f(r.home[1])}` : "-",
    r.arterialEdge, r.paving, r.diagonal ? `${f(r.diagonal.f)},${f(r.diagonal.k)}` : "-", r.sub, r.strip, fpts(r.pts));
}

/** A block record. */
export function blockLine(b) {
  checkKeys("block", b, BLOCK_KEYS);
  return line("block", b.id, b.cell, b.sub, b.district, frect(b.rect), fsides(b.sides), frect(b.prop), b.u, b.core, fpoly(b.poly), fcuts(b.cuts));
}

/** A sub-cell record. */
export function subLine(s) {
  checkKeys("sub", s, SUB_KEYS);
  return line("sub", s.id, frect(s.rect), fsides(s.sides), s.district, s.u, s.core, s.settlement, s.fringe ?? false);
}

/** A cell network: its header, edges, roads, blocks and sub-cells. */
export function* netLines(net) {
  checkKeys("net", net, NET_KEYS);
  yield line("cell", net.id, net.i, net.j, frect(net.rect), net.roads.length, net.blocks.length, net.subcells.length);
  for (const k of ["W", "E", "N", "S"]) yield edgeLine(`edge${k}`, net.edges[k]);
  for (const r of net.roads) yield roadLine(r);
  for (const b of net.blocks) yield blockLine(b);
  for (const s of net.subcells) yield subLine(s);
}
