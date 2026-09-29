/**
 * What vehicles do to their surroundings, seen (docs/VEHICLES.md): skid marks and smoke from
 * sliding tyres (dust on soil), and sparks where a car hits something hard - its speed changing
 * suddenly between two engine ticks, the sparks on the side the blow came from.
 */
import type { Vec3 } from '../engine/protocol.ts';
import { Material } from '../engine/protocol.ts';
import type { SkidMarks } from '../render/skids.ts';
import type { Effects } from './effects.ts';
import { rotate, rotateInv, VehicleTracker } from './vehicles.ts';

/** What tyres leave marks and smoke on. */
const HARD = new Set<number>([
  Material.Rc,
  Material.Concrete,
  Material.Steel,
  Material.Masonry,
  Material.Rock,
  Material.Bedrock,
  Material.Wood,
  Material.Stone,
  Material.SteelSection,
  Material.Asphalt,
  Material.Paint,
]);
/** m/s of tyre slip where marks start, and smoke. */
const MARK_SLIP = 2.5;
const SMOKE_SLIP = 4.5;
/** Smoke puffs per second and m/s of slip beyond SMOKE_SLIP, per wheel; per frame at most. */
const SMOKE_RATE = 5;
const SMOKE_PER_FRAME = 36;
/** A change of speed (m/s) between two samples that throws sparks. */
const CRASH_DV = 3;

export class VehicleEffects {
  private readonly carry = new Map<number, number>();
  private readonly last = new Map<number, { t: number; vel: Vec3 }>();

  clear(): void {
    this.carry.clear();
    this.last.clear();
  }

  update(dt: number, nowS: number, tracker: VehicleTracker, skids: SkidMarks, fx: Effects): void {
    let smokeBudget = SMOKE_PER_FRAME;
    for (const w of tracker.wheels.values()) {
      const v = tracker.vehicles.get(w.vehicle);
      if (!v) continue;
      const pose = tracker.pose(v, nowS);
      const up = rotate(pose.rot, [0, 0, 1]);
      const wp = tracker.wheelPose(w, nowS);
      const contact: Vec3 = [wp.centre[0] - up[0] * w.radius, wp.centre[1] - up[1] * w.radius, wp.centre[2] - up[2] * w.radius];
      const hard = HARD.has(w.material);
      const marks = w.contact && hard ? Math.min(1, Math.max(0, (w.slip - MARK_SLIP) / 7)) : 0;
      skids.track(w.id, contact, up, w.width, marks, nowS);
      if (!w.contact) continue;
      if (hard && w.slip > SMOKE_SLIP) {
        const c = (this.carry.get(w.id) ?? 0) + (w.slip - SMOKE_SLIP) * SMOKE_RATE * dt;
        let n = Math.floor(c);
        this.carry.set(w.id, Math.min(c - n, 2));
        const strength = Math.min(1, (w.slip - SMOKE_SLIP) / 10);
        for (; n > 0 && smokeBudget > 0; n--, smokeBudget--) fx.tyreSmoke(contact, v.vel, strength);
      } else if (w.material === Material.Soil && (w.slip > 1.5 || Math.hypot(v.vel[0], v.vel[1]) > 8)) {
        const c = (this.carry.get(w.id) ?? 0) + (2 + w.slip * 3) * dt;
        let n = Math.floor(c);
        this.carry.set(w.id, Math.min(c - n, 2));
        for (; n > 0 && smokeBudget > 0; n--, smokeBudget--) fx.wheelDust(contact, v.vel, Math.min(1, w.slip / 6));
      }
    }
    skids.prune(nowS);
    for (const id of this.carry.keys()) if (!tracker.wheels.has(id)) this.carry.delete(id);
    // crashes: a sudden change of velocity between two engine samples
    for (const v of tracker.vehicles.values()) {
      const prev = this.last.get(v.id);
      const t = v.cur.t;
      if (prev && prev.t === t) continue;
      this.last.set(v.id, { t, vel: [...v.vel] });
      if (!prev || t - prev.t > 0.1) continue;
      const dv: Vec3 = [v.vel[0] - prev.vel[0], v.vel[1] - prev.vel[1], v.vel[2] - prev.vel[2]];
      const m = Math.hypot(dv[0], dv[1], dv[2]);
      if (m < CRASH_DV) continue;
      // (the blow came from the side the change points away from: sparks on that face of its box)
      const pose = tracker.pose(v, nowS);
      const d = rotateInv(pose.rot, [-dv[0] / m, -dv[1] / m, -dv[2] / m]);
      const he = v.halfExtent;
      const k = Math.min(he[0] / Math.max(1e-3, Math.abs(d[0])), he[1] / Math.max(1e-3, Math.abs(d[1])));
      const local: Vec3 = [d[0] * k, d[1] * k, 0.45 * he[2]];
      fx.sparks(VehicleTracker.toWorld(pose, local), v.vel, Math.min(48, 8 + (m - CRASH_DV) * 5));
    }
    for (const id of this.last.keys()) if (!tracker.vehicles.has(id)) this.last.delete(id);
  }
}
