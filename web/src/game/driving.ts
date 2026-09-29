/**
 * Driving (docs/VEHICLES.md): the player's vehicle - getting in and out, its controls (keyboard or
 * gamepad; sent to the engine only as they change: the engine logs them for replays), and the
 * cameras: a chase camera that lags the car's heading (and shows some of a slide), kept out of
 * walls; a farther one; one on the roof. The mouse (or the right stick) looks around; the view
 * swings back behind the car after a moment.
 */
import type { EngineClient } from '../engine/client.ts';
import type { Vec3 } from '../engine/protocol.ts';
import type { Input } from './input.ts';
import type { OccupancyStore } from './occupancy.ts';
import { rotate, VehicleTracker, type Pose, type VehicleState } from './vehicles.ts';

export type CameraMode = 'chase' | 'far' | 'roof';
export const CAMERA_MODES: readonly CameraMode[] = ['chase', 'far', 'roof'];

export interface DriveControls {
  throttle: number;
  brake: number;
  steer: number;
  handbrake: boolean;
}

export interface DriveCamera {
  eye: Vec3;
  forward: Vec3;
  fovY: number;
}

const BASE_FOV = (70 * Math.PI) / 180;
const SPEED_FOV = (12 * Math.PI) / 180;
const MOUSE = 0.0022;
/** Seconds without looking around before the view swings back behind the car. */
const RECENTRE_S = 1.2;
/** Steeper pitches (added, radians) the chase camera tries when the way back from the car is short. */
const RISES = [0.25, 0.5, 0.8, 1.1];

function wrap(a: number): number {
  return a - 2 * Math.PI * Math.floor((a + Math.PI) / (2 * Math.PI));
}

const pressedPads = new Set<string>();

export class Driving {
  /** The vehicle the player drives (0: on foot). */
  vehicle = 0;
  mode: CameraMode = 'chase';
  /** Scripted controls (the debug API): used instead of the keys while set. */
  scripted: DriveControls | null = null;
  /** The controls last sent. */
  controls: DriveControls = { throttle: 0, brake: 0, steer: 0, handbrake: false };
  /** Whether the gamepad drove this frame (the HUD shows its hints). */
  gamepad = false;
  private confirmed = false;
  private camYaw = 0;
  private orbitYaw = 0;
  private orbitPitch = 0;
  private lastLook = -Infinity;
  private camDist = 0;
  /** How far the chase camera has risen over what is behind the car (radians of pitch). */
  private camRise = 0;
  private fov = BASE_FOV;
  private lastSpeed = 0;
  /** The last pose of the driven vehicle (where the player gets out if it is gone). */
  lastPose: Pose | null = null;
  lastSeat: Vec3 | null = null;
  /** (debugging) the chase camera's distances: wanted, free of walls, and now. */
  camInfo = { want: 0, free: 0, now: 0 };

  get driving(): boolean {
    return this.vehicle !== 0;
  }

  enter(v: VehicleState, pose: Pose, engine: EngineClient): void {
    this.vehicle = v.id;
    this.confirmed = false;
    this.controls = { throttle: 0, brake: 0, steer: 0, handbrake: false };
    const f = rotate(pose.rot, [1, 0, 0]);
    this.camYaw = Math.atan2(f[1], f[0]);
    this.orbitYaw = 0;
    this.orbitPitch = 0;
    this.camDist = 0;
    this.camRise = 0;
    this.lastSpeed = v.speed;
    this.lastPose = pose;
    engine.enterVehicle(v.id);
  }

  /** Leaves the vehicle (the engine puts its handbrake on). */
  exit(engine: EngineClient): void {
    if (this.vehicle === 0) return;
    this.vehicle = 0;
    this.scripted = null;
    engine.exitVehicle();
  }

  cycleCamera(): void {
    this.mode = CAMERA_MODES[(CAMERA_MODES.indexOf(this.mode) + 1) % CAMERA_MODES.length]!;
    this.camDist = 0;
    this.camRise = 0;
  }

  /**
   * The engine's word on the player's vehicle (after a `vehicles` message): false once the car is
   * gone (all its wheels torn off, or removed) or the engine, having confirmed it, no longer has
   * the player in it. Until it confirms, the command is on its way (messages sent before it
   * arrived may still be queued: a busy page can be seconds behind).
   */
  confirm(tracker: VehicleTracker): boolean {
    if (this.vehicle === 0) return true;
    if (!tracker.vehicles.has(this.vehicle)) return false;
    if (tracker.player === this.vehicle) {
      this.confirmed = true;
      return true;
    }
    return !this.confirmed;
  }

  /** Reads the controls (keys while the pointer is locked, or a gamepad) and sends them if they changed. */
  steer(input: Input, engine: EngineClient): void {
    let c: DriveControls = { throttle: 0, brake: 0, steer: 0, handbrake: false };
    const pad = gamepad();
    this.gamepad = false;
    if (this.scripted) {
      c = { ...this.scripted };
    } else {
      if (input.locked) {
        const fwd = input.isDown('KeyW') || input.isDown('ArrowUp');
        const back = input.isDown('KeyS') || input.isDown('ArrowDown');
        c.throttle = fwd && back ? 0 : fwd ? 1 : back ? -1 : 0;
        c.brake = fwd && back ? 1 : 0;
        c.steer = (input.isDown('KeyA') || input.isDown('ArrowLeft') ? 1 : 0) - (input.isDown('KeyD') || input.isDown('ArrowRight') ? 1 : 0);
        c.handbrake = input.isDown('Space');
      }
      if (pad) {
        const rt = pad.buttons[7]?.value ?? 0;
        const lt = pad.buttons[6]?.value ?? 0;
        const sx = deadzone(pad.axes[0] ?? 0);
        const hb = pad.buttons[0]?.pressed === true || pad.buttons[5]?.pressed === true;
        if (rt > 0.02 || lt > 0.02 || sx !== 0 || hb) {
          this.gamepad = true;
          c.throttle = quantize(rt - lt);
          c.steer = quantize(-sx);
          c.handbrake = hb || c.handbrake;
        }
      }
    }
    const o = this.controls;
    if (c.throttle !== o.throttle || c.brake !== o.brake || c.steer !== o.steer || c.handbrake !== o.handbrake) {
      this.controls = c;
      engine.drive(c.throttle, c.brake, c.steer, c.handbrake);
    }
  }

  /** Mouse (or right stick) look while driving. */
  look(dx: number, dy: number, nowS: number): void {
    const pad = gamepad();
    if (pad) {
      const rx = deadzone(pad.axes[2] ?? 0);
      const ry = deadzone(pad.axes[3] ?? 0);
      dx += rx * 18;
      dy += ry * 12;
    }
    if (dx === 0 && dy === 0) return;
    this.orbitYaw = wrap(this.orbitYaw - dx * MOUSE);
    this.orbitPitch = Math.max(-0.5, Math.min(1.1, this.orbitPitch - dy * MOUSE));
    this.lastLook = nowS;
  }

  /**
   * The camera this frame, for the vehicle at `pose` (interpolated); `trauma` returns crash jolts
   * (a sudden change of speed) for the camera shake.
   */
  camera(v: VehicleState, pose: Pose, dt: number, nowS: number, occupancy: OccupancyStore | null, voxelSize: number): { cam: DriveCamera; jolt: number } {
    this.lastPose = pose;
    this.lastSeat = VehicleTracker.toWorld(pose, v.seat);
    const he = v.halfExtent;
    const fwd = rotate(pose.rot, [1, 0, 0]);
    const heading = Math.atan2(fwd[1], fwd[0]);
    // (the heading followed: the car's, turned a little towards where it goes when it slides)
    let target = heading;
    const vxy = Math.hypot(v.vel[0], v.vel[1]);
    if (vxy > 6 && v.speed > 2) target = heading + wrap(Math.atan2(v.vel[1], v.vel[0]) - heading) * 0.35;
    this.camYaw = wrap(this.camYaw + wrap(target - this.camYaw) * (1 - Math.exp(-dt * 4.5)));
    if (nowS - this.lastLook > RECENTRE_S) {
      const k = Math.exp(-dt * 2.5);
      this.orbitYaw *= k;
      this.orbitPitch *= k;
    }
    const speed = Math.abs(v.speed);
    const fovTarget = BASE_FOV + Math.min(1, speed / 40) * SPEED_FOV;
    this.fov += (fovTarget - this.fov) * (1 - Math.exp(-dt * 2));
    // a sudden change of speed shakes the camera (a crash)
    const dv = Math.abs(v.speed - this.lastSpeed);
    this.lastSpeed = v.speed;
    const jolt = dv > 1.5 ? Math.min(0.9, (dv - 1.5) * 0.08) : 0;

    if (this.mode === 'roof') {
      const eye = VehicleTracker.toWorld(pose, [v.seat[0] - 0.2, 0, he[2] + 0.4]);
      const pitch = Math.asin(Math.max(-1, Math.min(1, fwd[2]))) - 0.08 + this.orbitPitch;
      const yaw = heading + this.orbitYaw;
      const cp = Math.cos(pitch);
      return { cam: { eye, forward: [cp * Math.cos(yaw), cp * Math.sin(yaw), Math.sin(pitch)], fovY: this.fov }, jolt };
    }
    const far = this.mode === 'far';
    const dist = (far ? 1.55 : 1) * (2.0 * he[0] + 1.8);
    const pitch0 = (far ? 0.3 : 0.2) + this.orbitPitch;
    const yaw = this.camYaw + this.orbitYaw;
    const look: Vec3 = [pose.pos[0], pose.pos[1], pose.pos[2] + 0.45 * he[2]];
    const dirAt = (pitch: number): Vec3 => {
      const cp = Math.cos(pitch);
      return [cp * Math.cos(yaw), cp * Math.sin(yaw), -Math.sin(pitch)];
    };
    // Kept out of walls: the way from the car back to the camera, through the world's voxels.
    // Where it is short (a wall, a parked car behind), the camera rises over it - the steeper
    // pitch with the most room - rather than closing in on the car's roof.
    const room = (pitch: number): number => {
      const d = dirAt(pitch);
      return occupancy?.ready ? Math.min(dist, clearance(occupancy, look, [-d[0], -d[1], -d[2]], dist, voxelSize) - 0.3) : dist;
    };
    let rise = 0;
    let best = room(pitch0);
    if (best < 0.75 * dist)
      for (const r of RISES) {
        const f = room(pitch0 + r);
        if (f > best + 0.25) {
          best = f;
          rise = r;
        }
        if (best >= 0.75 * dist) break;
      }
    // (rising at once, settling back slowly)
    this.camRise = rise > this.camRise ? this.camRise + (rise - this.camRise) * (1 - Math.exp(-dt * 8)) : this.camRise + (rise - this.camRise) * (1 - Math.exp(-dt * 1.5));
    const dir = dirAt(pitch0 + this.camRise);
    // (never nearer than just behind the car itself: from inside it, it would not be seen)
    const free = Math.max(Math.min(dist, he[0] + 0.6), room(pitch0 + this.camRise));
    if (this.camDist === 0 || free < this.camDist) this.camDist = free;
    else this.camDist += (free - this.camDist) * (1 - Math.exp(-dt * 2.5));
    this.camInfo = { want: dist, free, now: this.camDist };
    const eye: Vec3 = [look[0] - dir[0] * this.camDist, look[1] - dir[1] * this.camDist, look[2] - dir[2] * this.camDist];
    return { cam: { eye, forward: dir, fovY: this.fov }, jolt };
  }

  /**
   * Where the player gets out: beside the driver's door, else the other side, behind, in front, on
   * the roof - the first place their box is free.
   */
  exitSpot(v: VehicleState | undefined, occupancy: OccupancyStore | null, width: number, height: number): { pos: Vec3; yaw: number } | null {
    const pose = this.lastPose;
    if (!pose) return null;
    const f = rotate(pose.rot, [1, 0, 0]);
    const yaw = Math.atan2(f[1], f[0]);
    const he = v?.halfExtent ?? [2.3, 0.95, 1.5];
    const seat = v?.seat ?? [0, 0.4, 0.9];
    const side = seat[1] >= 0 ? 1 : -1;
    const tries: Vec3[] = [
      [seat[0], side * (he[1] + 0.45), 0.15],
      [seat[0], -side * (he[1] + 0.45), 0.15],
      [-(he[0] + 0.5), 0, 0.15],
      [he[0] + 0.5, 0, 0.15],
      [0, 0, he[2] + 0.1],
    ];
    const hw = width / 2;
    for (const t of tries) {
      const p = VehicleTracker.toWorld(pose, t);
      // (upright in the world, whatever way up the car lies)
      p[2] = Math.max(p[2], pose.origin[2] + 0.1);
      if (!occupancy?.ready || !occupancy.overlaps([p[0] - hw, p[1] - hw, p[2]], [p[0] + hw, p[1] + hw, p[2] + height])) return { pos: p, yaw };
    }
    const top = VehicleTracker.toWorld(pose, [0, 0, he[2] + 0.2]);
    return { pos: [top[0], top[1], Math.max(top[2], pose.origin[2] + he[2] + 0.2)], yaw };
  }
}

/** Metres free along a ray through the world grid's voxels (up to `max`). */
function clearance(occ: OccupancyStore, from: Vec3, dir: Vec3, max: number, h: number): number {
  const step = h * 0.5;
  const n = Math.ceil(max / step);
  for (let k = 1; k <= n; k++) {
    const t = Math.min(max, k * step);
    const x = from[0] + dir[0] * t;
    const y = from[1] + dir[1] * t;
    const z = from[2] + dir[2] * t;
    if (occ.solid(Math.floor(x / h + 0.5), Math.floor(y / h + 0.5), Math.floor(z / h + 0.5))) return t;
  }
  return max;
}

function deadzone(v: number): number {
  const a = Math.abs(v);
  return a < 0.12 ? 0 : Math.sign(v) * ((a - 0.12) / 0.88);
}

function quantize(v: number): number {
  return Math.round(Math.max(-1, Math.min(1, v)) * 32) / 32;
}

/** The first connected gamepad (standard mapping), or null. */
function gamepad(): Gamepad | null {
  if (typeof navigator === 'undefined' || !navigator.getGamepads) return null;
  for (const p of navigator.getGamepads()) if (p && p.connected) return p;
  return null;
}

/** A gamepad button's press this frame (edge), by its index. */
export function padPressed(index: number): boolean {
  const p = gamepad();
  const key = `${index}`;
  const down = p?.buttons[index]?.pressed === true;
  const was = pressedPads.has(key);
  if (down) pressedPads.add(key);
  else pressedPads.delete(key);
  return down && !was;
}
