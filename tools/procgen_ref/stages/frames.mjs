// Stage "frames": canonical building frames (buildings/frame.js) - plain frames of every front
// and turned frames of random placements (with a second yaw now and then): sides, the maps both
// ways for cells, points and rects (fractional ones too), directions, distances, shifted frames
// and turns; nominalFront for every yaw; frames from recorded turns (turnedFrame, lotFrameOf,
// frameOf).
import { REF, line, samples } from "../lib/rec.mjs";

const F = await import(REF + "buildings/frame.js");
const P = await import(REF + "core/placement.js");

const SIDES = ["F", "B", "L", "R"];
const WSIDES = ["N", "E", "S", "W"];

export default function* frames() {
  const r = samples(13);
  const big = (n) => Math.floor((r() - 0.5) * n);
  yield line("cside", ...SIDES.flatMap((s) => [...F.CSIDE_DIR[s], F.COPP[s]]));
  for (let yaw = 0; yaw < 132; yaw += 1) yield line("nf", yaw, F.nominalFront(yaw), F.nominalFront(yaw, (yaw * 7) % 132), F.nominalFront(yaw, 1 + (yaw % 5)));
  const frameLine = (tag, f) => line(tag, f.R.x0, f.R.y0, f.R.x1, f.R.y1, f.front, f.U, f.V, !!f.turned, f.ou ?? 0, f.ov ?? 0, ...SIDES.map((s) => f.worldSide(s)), ...WSIDES.map((s) => f.canonSide(s)));
  // plain frames
  for (let i = 0; i < 400; i += 1) {
    const x0 = big(4000);
    const y0 = big(4000);
    const R = { x0, y0, x1: x0 + Math.floor(r() * 60), y1: y0 + Math.floor(r() * 60) };
    const front = WSIDES[Math.floor(r() * 4)];
    const f = new F.Frame(R, front);
    yield frameLine("f", f);
    const out = [f.placement.px, f.placement.py, f.placement.q, f.placement.origin.x, f.placement.origin.y];
    for (let k = 0; k < 4; k += 1) {
      const u = big(80);
      const v = big(80);
      out.push(...f.toWorld(u, v), ...f.fromWorld(R.x0 + u, R.y0 + v), ...f.dirToWorld(u, v));
    }
    const cr = { x0: Math.floor(r() * 10), y0: Math.floor(r() * 10), x1: 10 + Math.floor(r() * 30), y1: 10 + Math.floor(r() * 30) };
    const wr = f.rectToWorld(cr);
    const br = f.rectFromWorld(wr);
    out.push(wr.x0, wr.y0, wr.x1, wr.y1, br.x0, br.y0, br.x1, br.y1);
    yield line("fm", ...out);
  }
  // turned frames
  for (let i = 0; i < 400; i += 1) {
    const yaw = Math.floor(r() * 132);
    const yaw2 = r() < 0.3 ? Math.floor(r() * 132) : 0;
    const ox = big(4000);
    const oy = big(4000);
    const p = new P.Placement({ origin: { x: ox, y: oy, z: 0 }, yaw, yaw2 });
    const ou = big(40);
    const ov = big(40);
    const U = 8 + Math.floor(r() * 60);
    const V = 8 + Math.floor(r() * 60);
    const f = new F.TurnedFrame(p, ou, ov, U, V, F.nominalFront(p.yaw, p.yaw2));
    yield frameLine("t", f);
    const out = [];
    for (let k = 0; k < 4; k += 1) {
      const u = big(80);
      const v = big(80);
      const [wx, wy] = f.toWorld(u, v);
      out.push(wx, wy, ...f.fromWorld(wx, wy), ...f.fromWorld(wx + big(9), wy + big(9)), ...f.dirToWorld(u, v));
      const fu = u + r() * 3;
      const fv = v - r() * 3;
      const [px, py] = f.pointToWorld(fu, fv);
      out.push(px, py, ...f.pointFromWorld(px, py), ...f.pointFromWorld(px + 0.25, py - 7.5));
    }
    const cr = { x0: Math.floor(r() * 10), y0: Math.floor(r() * 10), x1: 10 + Math.floor(r() * 30), y1: 10 + Math.floor(r() * 30) };
    const fr = { x0: cr.x0 + 0.5, y0: cr.y0 - 0.25, x1: cr.x1 - 0.75, y1: cr.y1 + 0.5 };
    const wr = f.rectToWorld(cr);
    const wf = f.rectToWorld(fr);
    const br = f.rectFromWorld(wr);
    out.push(wr.x0, wr.y0, wr.x1, wr.y1, wf.x0, wf.y0, wf.x1, wf.y1, br.x0, br.y0, br.x1, br.y1);
    for (let k = 0; k < 3; k += 1) out.push(f.distance(cr, wr.x0 + big(100), wr.y0 + big(100)));
    yield line("tm", ...out);
    const s = f.shifted(big(10), big(10), 5 + Math.floor(r() * 20), 5 + Math.floor(r() * 20));
    yield frameLine("ts", s);
    const t = s.turn;
    yield line("turn", t.yaw, t.yaw2, t.origin.x, t.origin.y, t.ou, t.ov, ...s.toWorld(1, 2), ...s.fromWorld(ox, oy));
    // the frame of a recorded turn, with its own front and the nominal one
    const turn = { yaw: t.yaw, ...(t.yaw2 ? { yaw2: t.yaw2 } : {}), origin: { ...t.origin }, ou: t.ou, ov: t.ov, U: s.U + 3, V: s.V + 1 };
    const g = F.turnedFrame(turn, s.U, s.V);
    yield frameLine("tf", g);
    const lf = F.lotFrameOf({ turn, rect: { x0: 0, y0: 0, x1: 9, y1: 9 }, front: WSIDES[i % 4] });
    yield frameLine("lf", lf);
    const ef = F.frameOf({ turn: i % 3 ? turn : undefined, U: 12, V: 20, front: WSIDES[(i + 1) % 4], R: { x0: ox, y0: oy, x1: ox + 11, y1: oy + 19 } });
    yield frameLine("ef", ef);
    yield line("efm", ...ef.toWorld(3, 4), ...ef.fromWorld(ox + 5, oy + 6));
  }
  // a plain lot frame
  const lp = F.lotFrameOf({ rect: { x0: 10, y0: 20, x1: 40, y1: 35 }, front: "E" });
  yield frameLine("lp", lp);
}
