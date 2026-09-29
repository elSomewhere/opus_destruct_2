/**
 * First-person controller (z up). Collision uses the engine's `collide` rules (plus client-side
 * step-up, see stepmove.ts):
 * - locally, every frame, when the engine streams chunk occupancy (occupancy.ts): movement
 *   never waits for a busy worker;
 * - otherwise by worker round trips: at most one move is in flight; time that passes meanwhile
 *   is folded into the next move, so a busy worker delays the player's response but never
 *   lets them clip through geometry.
 */
import type { EngineClient } from '../engine/client.ts';
import type { Vec3 } from '../engine/protocol.ts';
import type { Input } from './input.ts';
import type { OccupancyStore } from './occupancy.ts';
import { moveWithStep, moveWithStepSync, type StepMoveResult } from './stepmove.ts';

export const PLAYER = {
  width: 0.6,
  height: 1.75,
  eye: 1.6,
  walkSpeed: 5,
  runSpeed: 8.5,
  jumpSpeed: 6.2,
  gravity: 20,
  /** Ledges up to this height are climbed without jumping (Doom allows 24 units = 0.75 m). */
  stepHeight: 0.55,
  flySpeed: 12,
  mouseSensitivity: 0.0022,
} as const;

/** Longest time step folded into one collide request (seconds). */
const MAX_MOVE_DT = 0.1;

export class Player {
  /** Feet position (centre of the box bottom). */
  pos: Vec3 = [0, 0, 0];
  vel: Vec3 = [0, 0, 0];
  yaw = 0;
  pitch = 0;
  onGround = false;
  noclip = false;
  active = false;
  private spawnPos: Vec3 = [0, 0, 0];
  private spawnYaw = 0;
  private spawnPitch = 0;
  private inFlight = false;
  private unsentDt = 0;
  /** Visual eye offset that smooths out step-ups. */
  private stepSmooth = 0;
  /** Generation counter: results of requests sent before a respawn are ignored. */
  private generation = 0;
  /**
   * What the player stands on moves them (a lift's car, a turntable: a piece on a driven joint):
   * its velocity under their feet and its turn, from the last sweep.
   */
  private groundVel: Vec3 = [0, 0, 0];
  private groundSpin = 0;

  spawn(pos: Vec3, dir: Vec3): void {
    this.spawnPos = [...pos];
    this.spawnYaw = Math.atan2(dir[1], dir[0]);
    this.spawnPitch = Math.atan2(dir[2], Math.hypot(dir[0], dir[1]));
    this.respawn();
    this.active = true;
  }

  respawn(): void {
    this.pos = [this.spawnPos[0], this.spawnPos[1], this.spawnPos[2] + 0.05];
    this.vel = [0, 0, 0];
    this.yaw = this.spawnYaw;
    this.pitch = this.spawnPitch;
    this.onGround = false;
    this.stepSmooth = 0;
    this.unsentDt = 0;
    this.groundVel = [0, 0, 0];
    this.groundSpin = 0;
    this.generation++;
  }

  look(dx: number, dy: number): void {
    this.yaw -= dx * PLAYER.mouseSensitivity;
    this.pitch = Math.max(-1.55, Math.min(1.55, this.pitch - dy * PLAYER.mouseSensitivity));
  }

  forward(): Vec3 {
    const cp = Math.cos(this.pitch);
    return [cp * Math.cos(this.yaw), cp * Math.sin(this.yaw), Math.sin(this.pitch)];
  }

  eye(): Vec3 {
    return [this.pos[0], this.pos[1], this.pos[2] + PLAYER.eye + this.stepSmooth];
  }

  update(dt: number, input: Input, engine: EngineClient, local?: OccupancyStore): void {
    if (!this.active) return;
    this.stepSmooth *= Math.exp(-dt * 12);
    const f: Vec3 = [Math.cos(this.yaw), Math.sin(this.yaw), 0];
    const r: Vec3 = [Math.sin(this.yaw), -Math.cos(this.yaw), 0];
    const fwd = (input.isDown('KeyW') ? 1 : 0) - (input.isDown('KeyS') ? 1 : 0);
    const side = (input.isDown('KeyD') ? 1 : 0) - (input.isDown('KeyA') ? 1 : 0);
    let wx = f[0] * fwd + r[0] * side;
    let wy = f[1] * fwd + r[1] * side;
    const wl = Math.hypot(wx, wy);
    if (wl > 0) {
      wx /= wl;
      wy /= wl;
    }

    if (this.noclip) {
      const look = this.forward();
      const speed = PLAYER.flySpeed * (input.isDown('ShiftLeft') ? 3 : 1);
      const up = (input.isDown('Space') ? 1 : 0) - (input.isDown('ControlLeft') || input.isDown('KeyC') ? 1 : 0);
      this.pos = [
        this.pos[0] + (look[0] * fwd + r[0] * side) * speed * dt,
        this.pos[1] + (look[1] * fwd + r[1] * side) * speed * dt,
        this.pos[2] + (look[2] * fwd + up) * speed * dt,
      ];
      this.vel = [0, 0, 0];
      return;
    }

    const speed = input.isDown('ShiftLeft') || input.isDown('ShiftRight') ? PLAYER.runSpeed : PLAYER.walkSpeed;
    const control = this.onGround ? 14 : 2.5;
    const k = 1 - Math.exp(-control * dt);
    this.vel[0] += (wx * speed - this.vel[0]) * k;
    this.vel[1] += (wy * speed - this.vel[1]) * k;
    if (this.onGround && input.wasPressed('Space')) {
      this.vel[2] = PLAYER.jumpSpeed;
      this.onGround = false;
    }
    this.vel[2] = Math.max(-50, this.vel[2] - PLAYER.gravity * dt);

    this.unsentDt = Math.min(MAX_MOVE_DT, this.unsentDt + dt);
    if (this.inFlight || !engine.alive) return;
    const t = this.unsentDt;
    this.unsentDt = 0;
    // (riding: carried by the ground's motion, and turned with it)
    const ride: Vec3 = this.onGround ? [this.groundVel[0] * t, this.groundVel[1] * t, this.groundVel[2] * t] : [0, 0, 0];
    if (this.onGround && this.groundSpin !== 0) this.yaw += this.groundSpin * t;
    const move: Vec3 = [this.vel[0] * t + ride[0], this.vel[1] * t + ride[1], this.vel[2] * t + ride[2]];
    const hw = PLAYER.width / 2;
    const min: Vec3 = [this.pos[0] - hw, this.pos[1] - hw, this.pos[2]];
    const max: Vec3 = [this.pos[0] + hw, this.pos[1] + hw, this.pos[2] + PLAYER.height];
    if (local?.ready) {
      // pushed up by a rising lift's car rather than stuck inside it
      const lift = local.depenetrate(min, max, 1.25);
      if (lift !== null && lift > 0) {
        this.pos = [this.pos[0], this.pos[1], this.pos[2] + lift];
        min[2] += lift;
        max[2] += lift;
        this.stepSmooth -= lift;
        if (this.vel[2] < 0) this.vel[2] = 0;
      }
      this.apply(move, moveWithStepSync((a, b, m) => local.collide(a, b, m), min, max, move, PLAYER.stepHeight, this.onGround));
      return;
    }
    const gen = this.generation;
    this.inFlight = true;
    moveWithStep((a, b, m) => engine.collide(a, b, m), min, max, move, PLAYER.stepHeight, this.onGround)
      .then((res) => {
        if (gen === this.generation) this.apply(move, res);
      })
      .catch(() => undefined)
      .finally(() => {
        this.inFlight = false;
      });
  }

  private apply(requested: Vec3, res: StepMoveResult): void {
    const m = res.move;
    this.pos = [this.pos[0] + m[0], this.pos[1] + m[1], this.pos[2] + m[2]];
    const eps = 1e-5;
    if (res.stepped > 0) {
      // Climbed a ledge: keep the eye where it was and ease it up.
      this.stepSmooth -= res.stepped;
      this.vel[2] = 0;
    } else {
      if (Math.abs(m[0] - requested[0]) > eps) this.vel[0] = 0;
      if (Math.abs(m[1] - requested[1]) > eps) this.vel[1] = 0;
      if (Math.abs(m[2] - requested[2]) > eps) this.vel[2] = 0;
    }
    this.onGround = res.onGround;
    if (this.onGround && this.vel[2] < 0) this.vel[2] = 0;
    this.groundVel = res.onGround && res.groundVelocity ? [...res.groundVelocity] : [0, 0, 0];
    this.groundSpin = res.onGround && res.groundAngular ? res.groundAngular[2] : 0;
  }
}
