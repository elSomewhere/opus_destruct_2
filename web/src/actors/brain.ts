/**
 * Behaviour of the actors.
 *
 * Soldiers (hostile to the player): patrol around their post; gunfire, explosions, screams and
 * bodies put them on alert and they go to look; once they see the player they fight: react
 * after a moment, keep a combat range (advance, back off, strafe, crouch behind what is there),
 * aim and fire bursts whose spread tightens while they hold their aim, and hunt the last
 * known position when they lose sight.
 *
 * Civilians: wander and stop to look around; fear builds from gunfire, impacts, explosions,
 * screams and bodies. Frightened, they run away from the danger (panicking, hands to the head,
 * when very scared), cower when it is right on them, and put their hands up when the player
 * aims at them from close by.
 *
 * Brains think at ~10 Hz and set the actor's goals (destination, speed, facing, stance, mood);
 * ActorWorld moves and animates.
 */
import { vdist, vnorm, type V3 } from 'svx-anim';
import type { Actor, ActorWorld, Noise } from './actors.ts';

export interface Brain {
  /** ~10 Hz decisions. */
  think(a: Actor, w: ActorWorld, dt: number): void;
  /** Every frame (weapon timing, reflexes). */
  tick(a: Actor, w: ActorWorld, dt: number): void;
  hear(a: Actor, n: Noise, w: ActorWorld): void;
  hurt(a: Actor, from: V3 | null, w: ActorWorld): void;
  readonly state: string;
}

const rnd = (a: number, b: number): number => a + Math.random() * (b - a);

type SoldierState = 'patrol' | 'alert' | 'combat' | 'search';

export class SoldierBrain implements Brain {
  state: SoldierState = 'patrol';
  private readonly home: V3;
  private wait = rnd(0, 3);
  private seen = false;
  private seenAt = -99;
  private lastSeen: V3 | null = null;
  private reaction = 0;
  private aimTime = 0;
  private reposition = 0;
  private burst = 0;
  private shotTimer = 0;
  private pause = rnd(0.3, 0.8);
  private lookTimer = 0;
  private alertPos: V3 | null = null;
  private alertTime = 0;
  private readonly crouchy = Math.random() < 0.55;
  private readonly aggressive = Math.random() < 0.4;

  constructor(home: V3) {
    this.home = [...home];
  }

  think(a: Actor, w: ActorWorld, dt: number): void {
    const pl = w.player;
    const chest = pl.chest();
    const dist = vdist(a.pos, pl.feet);
    // perception: in range, in view (any direction once alerted), and a clear line
    const facing = [Math.cos(a.yaw), Math.sin(a.yaw)];
    const to = [chest[0] - a.pos[0], chest[1] - a.pos[1]];
    const tl = Math.hypot(to[0]!, to[1]!) || 1;
    const inView = this.state !== 'patrol' || (facing[0]! * to[0]! + facing[1]! * to[1]!) / tl > -0.3 || dist < 4;
    this.seen = pl.alive && dist < 55 && inView && w.canSee(a, chest);
    if (this.seen) {
      this.seenAt = w.time;
      this.lastSeen = [pl.feet[0], pl.feet[1], pl.feet[2]];
      if (this.state !== 'combat') {
        this.state = 'combat';
        this.reaction = rnd(0.35, 0.8);
        this.reposition = rnd(0.5, 1.5);
        w.noise({ pos: a.pos, radius: 18, kind: 'shout', source: a });
      }
    }
    switch (this.state) {
      case 'patrol': {
        a.carry = 'relaxed';
        a.crouch = 0;
        a.face = null;
        if (w.arrived(a)) {
          this.wait -= dt;
          if (this.wait <= 0) {
            const p = w.nav.randomPoint(this.home, 3, 12);
            if (p) w.goTo(a, p, rnd(1.2, 1.6));
            this.wait = rnd(2, 6);
          }
        }
        break;
      }
      case 'alert': {
        a.carry = 'ready';
        a.crouch = 0;
        this.alertTime += dt;
        if (this.alertPos && !w.arrived(a)) {
          a.face = null;
        } else {
          // look around
          this.lookTimer -= dt;
          if (this.lookTimer <= 0) {
            const ang = a.yaw + rnd(-2, 2);
            a.face = [a.pos[0] + Math.cos(ang) * 5, a.pos[1] + Math.sin(ang) * 5, a.pos[2] + 1.5];
            this.lookTimer = rnd(1, 2.2);
          }
        }
        if (this.alertTime > 14) {
          this.state = 'patrol';
          a.face = null;
        }
        break;
      }
      case 'combat': {
        a.face = this.lastSeen ? [this.lastSeen[0], this.lastSeen[1], this.lastSeen[2] + 1.3] : null;
        if (!pl.alive) {
          this.state = 'alert';
          this.alertTime = 0;
          break;
        }
        if (w.time - this.seenAt > 2.5) {
          this.state = 'search';
          if (this.lastSeen) w.goTo(a, this.lastSeen, rnd(3, 4));
          break;
        }
        this.reposition -= dt;
        if (this.reposition <= 0 || (w.arrived(a) && a.speed > 0 && Math.random() < 0.3)) {
          this.reposition = rnd(1.8, 4);
          const far = this.aggressive ? 16 : 24;
          if (dist > far) {
            // close in (run when far)
            const p = w.nav.randomPoint(pl.feet, far * 0.55, far * 0.8) ?? pl.feet;
            w.goTo(a, p, dist > 32 ? rnd(3.8, 4.6) : rnd(1.8, 2.4));
            a.crouch = 0;
          } else if (dist < 6) {
            const p = w.nav.randomPoint(a.pos, 5, 9, pl.feet);
            if (p) w.goTo(a, p, 2);
            a.crouch = 0;
          } else if (Math.random() < 0.5) {
            w.stop(a);
            a.crouch = this.crouchy && Math.random() < 0.6 ? 1 : 0;
          } else {
            const p = w.nav.randomPoint(a.pos, 2, 5);
            if (p) w.goTo(a, p, rnd(1.3, 1.9));
            a.crouch = this.crouchy && Math.random() < 0.3 ? 1 : 0;
          }
        }
        // aim while holding or walking; lower the weapon to run
        a.carry = a.speed > 3 ? 'ready' : 'aim';
        break;
      }
      case 'search': {
        a.carry = 'ready';
        a.crouch = 0;
        a.face = null;
        if (w.arrived(a)) {
          this.state = 'alert';
          this.alertTime = 4;
          this.alertPos = null;
        }
        break;
      }
    }
  }

  tick(a: Actor, w: ActorWorld, dt: number): void {
    if (this.state !== 'combat') {
      this.aimTime = 0;
      return;
    }
    this.reaction -= dt;
    this.aimTime = a.carry === 'aim' ? this.aimTime + dt : 0;
    this.shotTimer -= dt;
    if (!this.seen || w.time - this.seenAt > 0.3 || this.reaction > 0 || this.aimTime < 0.35 || !w.player.alive) return;
    if (this.burst <= 0) {
      this.pause -= dt;
      if (this.pause > 0) return;
      this.burst = Math.floor(rnd(3, 7));
      this.pause = rnd(0.5, 1.4);
    }
    if (this.shotTimer > 0) return;
    const target = w.player.chest();
    const d = vdist(a.pos, target);
    const moving = Math.hypot(a.vel[0], a.vel[1]) > 0.4 ? 1 : 0;
    const steady = Math.min(1, this.aimTime / 1.5);
    const spread = Math.max(0.006, 0.04 + 0.03 * moving + 0.012 * (d / 20) - 0.028 * steady - 0.01 * a.crouch);
    if (w.fireAt(a, target, spread)) {
      this.burst--;
      this.shotTimer = rnd(0.09, 0.12);
    } else {
      this.burst = 0; // a friend in the way: wait
    }
  }

  hear(a: Actor, n: Noise, w: ActorWorld): void {
    if (n.source === a) return;
    if (this.state === 'combat') return;
    const d = vdist(a.pos, n.pos);
    if (d > n.radius) return;
    this.state = 'alert';
    this.alertTime = 0;
    this.alertPos = [...n.pos];
    const p = w.nav.randomPoint(n.pos, 1, 5) ?? n.pos;
    w.goTo(a, p, n.kind === 'explosion' || n.kind === 'shot' ? rnd(2.8, 3.6) : 2.2);
  }

  hurt(a: Actor, from: V3 | null, w: ActorWorld): void {
    if (from && this.state !== 'combat') {
      this.state = 'combat';
      this.lastSeen = [...from];
      this.seenAt = w.time;
      this.reaction = rnd(0.4, 0.9);
    }
    // being hit breaks the aim for a moment
    this.aimTime = Math.min(this.aimTime, 0.1);
    void a;
  }
}

type CivState = 'wander' | 'flee' | 'cower' | 'surrender';

export class CivilianBrain implements Brain {
  state: CivState = 'wander';
  fear = 0;
  private threat: V3 | null = null;
  private wait = rnd(0, 4);
  private timer = 0;
  private aimed = 0;
  private screamed = -99;
  private readonly bravery = rnd(0.7, 1.3);

  think(a: Actor, w: ActorWorld, dt: number): void {
    this.fear = Math.max(0, this.fear - dt * 0.06 * this.bravery);
    // the player aiming at me from close by
    const pl = w.player;
    const d = vdist(a.pos, pl.feet);
    if (pl.alive && d < 9 && this.state !== 'cower') {
      const eye = pl.eye();
      const dir = vnorm([a.pos[0] - eye[0], a.pos[1] - eye[1], a.pos[2] + 1.2 - eye[2]]);
      const f = pl.forward;
      const cos = dir[0] * f[0] + dir[1] * f[1] + dir[2] * f[2];
      this.aimed = cos > 0.985 ? this.aimed + dt : Math.max(0, this.aimed - dt * 0.5);
      if (this.aimed > 0.5) {
        this.state = 'surrender';
        this.timer = 2.5;
        w.stop(a);
      }
    }
    switch (this.state) {
      case 'wander':
        a.mood = 'normal';
        a.crouch = 0;
        a.face = null;
        if (this.fear > 0.45) {
          this.startFlee(a, w);
          break;
        }
        if (w.arrived(a)) {
          this.wait -= dt;
          if (this.wait <= 0) {
            const p = w.nav.randomPoint(a.pos, 5, 14);
            if (p) w.goTo(a, p, rnd(1.1, 1.5));
            this.wait = rnd(1.5, 6);
          }
        }
        break;
      case 'flee':
        a.mood = this.fear > 0.8 ? 'panic' : 'normal';
        a.face = null;
        if (this.fear > 0.9 && w.time - this.screamed > 4) {
          this.screamed = w.time;
          w.noise({ pos: a.pos, radius: 14, kind: 'scream', source: a });
        }
        if (w.arrived(a) || a.stuck > 1.2) {
          if (this.fear > 0.35) this.startFlee(a, w);
          else {
            this.state = 'wander';
            this.wait = rnd(2, 5);
          }
        }
        break;
      case 'cower':
        a.mood = 'cower';
        this.timer -= dt;
        if (this.timer <= 0) {
          a.mood = 'normal';
          this.startFlee(a, w);
        }
        break;
      case 'surrender':
        a.mood = 'surrender';
        a.face = [pl.feet[0], pl.feet[1], pl.feet[2] + 1.6];
        this.timer -= dt;
        if (this.aimed > 0.3) this.timer = Math.max(this.timer, 1.5);
        if (this.timer <= 0) {
          a.mood = 'normal';
          this.fear = Math.max(this.fear, 0.7);
          this.threat = [...pl.feet];
          this.startFlee(a, w);
        }
        break;
    }
  }

  private startFlee(a: Actor, w: ActorWorld): void {
    this.state = 'flee';
    const from = this.threat ?? w.player.feet;
    const p = w.nav.randomPoint(a.pos, 12, 28, from) ?? w.nav.randomPoint(a.pos, 6, 16);
    if (p) w.goTo(a, p, this.fear > 0.8 ? rnd(4.6, 5.6) : rnd(3.2, 4.2));
    else {
      this.state = 'cower';
      this.timer = rnd(3, 6);
      w.stop(a);
    }
  }

  tick(): void {}

  hear(a: Actor, n: Noise, w: ActorWorld): void {
    if (n.source === a) return;
    const d = vdist(a.pos, n.pos);
    if (d > n.radius) return;
    const near = 1 - d / n.radius;
    const gain = { shot: 0.5, impact: 0.45, explosion: 1.4, death: 0.9, scream: 0.35, shout: 0.25 }[n.kind] * (0.4 + near) / this.bravery;
    this.fear = Math.min(2, this.fear + gain);
    this.threat = [...n.pos];
    if (n.kind === 'explosion' && d < 9 && this.state !== 'surrender') {
      this.state = 'cower';
      this.timer = rnd(2.5, 5);
      w.stop(a);
    } else if (this.state === 'wander' && this.fear > 0.45) this.startFlee(a, w);
  }

  hurt(a: Actor, from: V3 | null, w: ActorWorld): void {
    this.fear = 2;
    if (from) this.threat = [...from];
    if (this.state !== 'flee') this.startFlee(a, w);
  }
}
