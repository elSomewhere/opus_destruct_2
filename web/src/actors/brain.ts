/**
 * Behaviour of the actors.
 *
 * Soldiers (hostile to the player, and to civilians who shoot at them): patrol round their
 * post; gunfire, explosions, screams and bodies put them on alert and they go to look; once
 * they see their target they fight: react after a moment, keep a combat range (advance, back
 * off, strafe), take positions (kneeling, prone at range, crouched behind cover popping up or
 * leaning out to fire), fire bursts whose spread tightens while they hold their aim (worse on
 * the move), reload when the magazine runs dry, hit whoever gets close with the rifle or fists,
 * and hunt the last known position when they lose sight.
 *
 * Civilians live their day: they stroll at their own pace, some jog, they wait about (the
 * animator's idle postures and fidgets), pair up and talk, sit on benches, at café tables or on
 * the ground, and look at people and at what happens. Fear builds from gunfire, impacts, explosions, screams
 * and bodies: they run (panicking when very scared), cower when it is right on them, put
 * their hands up when the player aims at them close by; the armed ones shoot back. Now and
 * then a conversation turns into a fist fight (or a knife fight); bystanders stop and watch.
 *
 * Brains think at ~10 Hz and set the actor's goals (destination, speed, facing, stance,
 * activity); ActorWorld moves and animates.
 */
import { vdist, vnorm, type V3 } from 'svx-anim';
import type { Actor, ActorWorld, Noise, Seat } from './actors.ts';

export interface Brain {
  /** ~10 Hz decisions. */
  think(a: Actor, w: ActorWorld, dt: number): void;
  /** Every frame (weapon timing, reflexes). */
  tick(a: Actor, w: ActorWorld, dt: number): void;
  hear(a: Actor, n: Noise, w: ActorWorld): void;
  hurt(a: Actor, from: V3 | null, w: ActorWorld, by: Actor | null): void;
  readonly state: string;
}

const rnd = (a: number, b: number): number => a + Math.random() * (b - a);
const chance = (p: number): boolean => Math.random() < p;

/** A target: the player or another actor. */
export type Target = { kind: 'player' } | { kind: 'actor'; actor: Actor };

function targetAlive(w: ActorWorld, t: Target): boolean {
  return t.kind === 'player' ? w.player.alive : t.actor.char.alive && t.actor.opacity > 0.5;
}

function targetFeet(w: ActorWorld, t: Target): V3 {
  return t.kind === 'player' ? w.player.feet : t.actor.pos;
}

function targetChest(w: ActorWorld, t: Target): V3 {
  if (t.kind === 'player') return w.player.chest();
  const c = t.actor.char.pose.p[3]!;
  return [c[0], c[1], c[2] + 0.12];
}

// ---- soldiers ------------------------------------------------------------------------------

type SoldierState = 'patrol' | 'alert' | 'combat' | 'search';

export class SoldierBrain implements Brain {
  state: SoldierState = 'patrol';
  private readonly home: V3;
  private wait = rnd(0, 3);
  private seen = false;
  private seenAt = -99;
  private lastSeen: V3 | null = null;
  private target: Target = { kind: 'player' };
  private reaction = 0;
  private aimTime = 0;
  private reposition = 0;
  private burst = 0;
  private shotTimer = 0;
  private pause = rnd(0.3, 0.8);
  private lookTimer = 0;
  private alertTime = 0;
  private meleeCool = 0;
  /** Holding a position in this stance until the next reposition. */
  private hold: 'stand' | 'crouch' | 'kneel' | 'prone' = 'stand';
  private popUp = false;
  private readonly crouchy = Math.random() < 0.6;
  private readonly aggressive = Math.random() < 0.4;

  constructor(home: V3) {
    this.home = [...home];
  }

  /** The soldier's hostile target switches to this actor (it shot at soldiers). */
  engage(t: Target): void {
    this.target = t;
  }

  think(a: Actor, w: ActorWorld, dt: number): void {
    // a dead or gone target: back to the player
    if (!targetAlive(w, this.target)) this.target = { kind: 'player' };
    const chest = targetChest(w, this.target);
    const feet = targetFeet(w, this.target);
    const dist = vdist(a.pos, feet);
    const facing = [Math.cos(a.yaw), Math.sin(a.yaw)];
    const to = [chest[0] - a.pos[0], chest[1] - a.pos[1]];
    const tl = Math.hypot(to[0]!, to[1]!) || 1;
    const inView = this.state !== 'patrol' || (facing[0]! * to[0]! + facing[1]! * to[1]!) / tl > -0.3 || dist < 4;
    const alive = targetAlive(w, this.target);
    // line of sight from where the eyes are, and from standing / leaning (cover)
    const eyeNow = w.eyes(a);
    this.seen = alive && dist < 60 && inView && w.canSeeFrom(eyeNow, chest);
    let peek = 0;
    if (alive && !this.seen && inView && dist < 60 && this.state === 'combat') {
      const standEye: V3 = [a.pos[0], a.pos[1], a.pos[2] + 1.6];
      if (w.canSeeFrom(standEye, chest)) this.popUp = true;
      else {
        const side: V3 = [-facing[1]!, facing[0]!, 0];
        for (const s of [1, -1]) {
          const e: V3 = [standEye[0] + side[0] * s * 0.45, standEye[1] + side[1] * s * 0.45, standEye[2]];
          if (w.canSeeFrom(e, chest)) {
            peek = s;
            break;
          }
        }
      }
      if (this.popUp || peek !== 0) this.seen = true;
    }
    a.lean = this.state === 'combat' ? peek : 0;
    if (this.seen) {
      this.seenAt = w.time;
      this.lastSeen = [...feet];
      if (this.state !== 'combat') {
        this.state = 'combat';
        this.reaction = rnd(0.35, 0.8);
        this.reposition = rnd(0.5, 1.5);
        w.noise({ pos: a.pos, radius: 18, kind: 'shout', source: a });
      }
    }
    const gun = a.weapon && a.weapon.kind !== 'knife';
    const longGun = gun && a.weapon!.kind !== 'pistol';
    switch (this.state) {
      case 'patrol': {
        a.carry = 'relaxed';
        a.crouch = 0;
        a.stance = 'stand';
        a.face = null;
        a.lookAt = w.pointOfInterest(a);
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
        a.stance = 'stand';
        this.alertTime += dt;
        if (w.arrived(a)) {
          this.lookTimer -= dt;
          if (this.lookTimer <= 0) {
            const ang = a.yaw + rnd(-2, 2);
            a.face = [a.pos[0] + Math.cos(ang) * 5, a.pos[1] + Math.sin(ang) * 5, a.pos[2] + 1.5];
            this.lookTimer = rnd(1, 2.2);
          }
        } else a.face = null;
        if (this.alertTime > 14) {
          this.state = 'patrol';
          a.face = null;
        }
        break;
      }
      case 'combat': {
        a.face = this.lastSeen ? [this.lastSeen[0], this.lastSeen[1], this.lastSeen[2] + 1.3] : null;
        a.aimTarget = alive ? chest : null;
        if (!alive) {
          this.state = 'alert';
          this.alertTime = 0;
          a.stance = 'stand';
          break;
        }
        if (w.time - this.seenAt > 2.5) {
          this.state = 'search';
          a.stance = 'stand';
          if (this.lastSeen) w.goTo(a, this.lastSeen, rnd(3, 4));
          break;
        }
        // close in for a blow
        this.meleeCool -= dt;
        if (dist < 1.6 && this.meleeCool <= 0 && !a.char.animator.busy) {
          this.meleeCool = rnd(1.2, 2.2);
          a.stance = 'stand';
          w.melee(a, this.target, longGun ? 'riflePush' : chance(0.5) ? 'cross' : 'jab');
        }
        this.reposition -= dt;
        if (this.reposition <= 0 || (w.arrived(a) && a.speed > 0 && chance(0.3))) {
          this.reposition = rnd(2.2, 5);
          const far = this.aggressive ? 14 : 22;
          this.popUp = false;
          if (dist > far) {
            // close in (run when far; aimed walk otherwise)
            const p = w.nav.randomPoint(feet, far * 0.55, far * 0.8) ?? feet;
            w.goTo(a, p, dist > 32 ? rnd(3.8, 4.6) : rnd(1.6, 2.2));
            this.hold = 'stand';
          } else if (dist < 5) {
            const p = w.nav.randomPoint(a.pos, 5, 9, feet);
            if (p) w.goTo(a, p, 2);
            this.hold = 'stand';
          } else if (chance(0.55)) {
            w.stop(a);
            // hold a position: prone at range, kneeling, crouched, or standing
            const r = Math.random();
            this.hold = longGun && dist > 16 && r < 0.3 ? 'prone' : longGun && r < 0.6 ? 'kneel' : this.crouchy && r < 0.8 ? 'crouch' : 'stand';
          } else {
            const p = w.nav.randomPoint(a.pos, 2, 5);
            if (p) w.goTo(a, p, rnd(1.2, 1.8));
            this.hold = this.crouchy && chance(0.35) ? 'crouch' : 'stand';
          }
        }
        const moving = !w.arrived(a);
        const up = this.popUp && this.shotTimer > -0.6;
        a.stance = moving || up ? 'stand' : this.hold === 'kneel' ? 'kneel' : this.hold === 'prone' ? 'prone' : 'stand';
        a.crouch = !up && this.hold === 'crouch' ? 1 : moving && this.hold === 'crouch' ? 0.7 : 0;
        // aim while holding or walking; lower the weapon to run; machine guns fire from the hip on the move
        a.carry = a.speed > 3 ? 'ready' : a.weapon?.kind === 'lmg' && moving ? 'hip' : 'aim';
        if (this.popUp && this.burst <= 0 && this.pause > 0.3) this.popUp = false;
        break;
      }
      case 'search': {
        a.carry = 'ready';
        a.crouch = 0;
        a.stance = 'stand';
        a.face = null;
        a.aimTarget = null;
        if (w.arrived(a)) {
          this.state = 'alert';
          this.alertTime = 4;
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
    this.aimTime = a.carry === 'aim' || a.carry === 'hip' ? this.aimTime + dt : 0;
    this.shotTimer -= dt;
    const an = a.char.animator;
    if (!this.seen || w.time - this.seenAt > 0.3 || this.reaction > 0 || this.aimTime < 0.35 || !targetAlive(w, this.target)) return;
    if (an.transitioning || an.knockedDown || (an.busy && an.actionName !== null && !an.actionName.startsWith('reload'))) return;
    if (a.reloading) return;
    if (this.burst <= 0) {
      this.pause -= dt;
      if (this.pause > 0) return;
      this.burst = Math.floor(rnd(3, 7));
      this.pause = rnd(0.5, 1.4);
    }
    if (this.shotTimer > 0) return;
    const target = targetChest(w, this.target);
    const d = vdist(a.pos, target);
    const moving = Math.hypot(a.vel[0], a.vel[1]) > 0.4 ? 1 : 0;
    const steady = Math.min(1, this.aimTime / 1.5);
    const braced = a.char.animator.stance === 'prone' ? 0.02 : a.char.animator.stance === 'kneel' ? 0.012 : a.crouch * 0.01;
    const spread = Math.max(0.006, 0.04 + 0.035 * moving + 0.012 * (d / 20) - 0.028 * steady - braced) * w.spreadOf(a);
    if (w.fireAt(a, target, spread)) {
      this.burst--;
      this.shotTimer = w.intervalOf(a) * rnd(0.95, 1.25);
    } else this.burst = 0;
  }

  hear(a: Actor, n: Noise, w: ActorWorld): void {
    if (n.source === a || this.state === 'combat') return;
    const d = vdist(a.pos, n.pos);
    if (d > n.radius) return;
    this.state = 'alert';
    this.alertTime = 0;
    const p = w.nav.randomPoint(n.pos, 1, 5) ?? n.pos;
    w.goTo(a, p, n.kind === 'explosion' || n.kind === 'shot' ? rnd(2.8, 3.6) : 2.2);
  }

  hurt(a: Actor, from: V3 | null, w: ActorWorld, by: Actor | null): void {
    if (by && by.faction === 'civilian') this.target = { kind: 'actor', actor: by };
    if (from && this.state !== 'combat') {
      this.state = 'combat';
      this.lastSeen = [...from];
      this.seenAt = w.time;
      this.reaction = rnd(0.4, 0.9);
    }
    this.aimTime = Math.min(this.aimTime, 0.1);
    void a;
  }
}

// ---- civilians -----------------------------------------------------------------------------

type CivState = 'stroll' | 'wait' | 'talk' | 'sit' | 'ground' | 'jog' | 'watch' | 'flee' | 'cower' | 'surrender' | 'defend' | 'brawl';

export class CivilianBrain implements Brain {
  state: CivState = 'wait';
  fear = 0;
  private threat: V3 | null = null;
  private timer = rnd(1, 5);
  private aimed = 0;
  private screamed = -99;
  private readonly bravery = rnd(0.7, 1.3);
  /** Personal pace (m/s) and whether this one likes to jog. */
  private readonly pace = rnd(1.05, 1.6);
  private readonly jogger = chance(0.12);
  partner: Actor | null = null;
  /** Started the conversation (walks over; the other waits for it). */
  private lead = false;
  private speaking = false;
  private turnTimer = 0;
  private seat: Seat | null = null;
  private lookTimer = 0;
  private defendTarget: Actor | null = null;
  private opponent: Actor | null = null;
  /** Something that draws the eye for a while (a fight nearby). */
  private glanceAt: V3 | null = null;
  private glanceUntil = 0;
  private burst = 0;
  private shotTimer = 0;

  /** Frees whatever the civilian holds (seat, partner) when it changes activity. */
  private leave(a: Actor, w: ActorWorld): void {
    if (this.seat) {
      w.releaseSeat(this.seat, a);
      this.seat = null;
    }
    a.stance = 'stand';
    a.seat = null;
    a.talk = null;
    if (this.partner) {
      const p = this.partner.brain as CivilianBrain;
      if (p.partner === a) {
        p.partner = null;
        if (p.state === 'talk') p.timer = 0;
      }
      this.partner = null;
    }
  }

  /** Starts a conversation with a partner (called for both). */
  talkWith(a: Actor, partner: Actor, lead: boolean): void {
    this.state = 'talk';
    this.partner = partner;
    this.lead = lead;
    this.speaking = lead;
    this.turnTimer = rnd(2.5, 5);
    this.timer = rnd(12, 32);
    a.goal = null;
  }

  /** A brawl starts (the world's Brawler takes over movement). */
  brawl(a: Actor, opponent: Actor): void {
    this.state = 'brawl';
    this.opponent = opponent;
    this.timer = rnd(18, 28);
    a.talk = null;
  }

  private choose(a: Actor, w: ActorWorld): void {
    this.leave(a, w);
    const r = Math.random();
    if (this.jogger && r < 0.35) {
      this.state = 'jog';
      const p = w.nav.randomPoint(a.pos, 25, 45);
      if (p) w.goTo(a, p, rnd(2.6, 3.2));
      return;
    }
    if (r < 0.2) {
      // find someone free to talk to
      const other = w.nearestFree(a, 14);
      if (other) {
        w.meet(a, other);
        return;
      }
    }
    if (r < 0.32) {
      const s = w.freeSeat(a.pos, 25);
      if (s) {
        this.state = 'sit';
        this.seat = s;
        w.claimSeat(s, a);
        w.goTo(a, s.front, this.pace);
        this.timer = rnd(18, 45);
        return;
      }
    }
    if (r < 0.38) {
      this.state = 'ground';
      w.stop(a);
      a.groundVariant = (['cross', 'kneesUp', 'legsOut'] as const)[Math.floor(Math.random() * 3)]!;
      this.timer = rnd(15, 35);
      return;
    }
    if (r < 0.55) {
      this.state = 'wait';
      w.stop(a);
      this.timer = rnd(5, 18);
      return;
    }
    this.state = 'stroll';
    const p = w.nav.randomPoint(a.pos, 8, 25);
    if (p) w.goTo(a, p, this.pace);
    this.timer = 40;
  }

  think(a: Actor, w: ActorWorld, dt: number): void {
    this.fear = Math.max(0, this.fear - dt * 0.05 * this.bravery);
    this.timer -= dt;
    const pl = w.player;
    const d = vdist(a.pos, pl.feet);
    // the player aiming at me from close by
    if (pl.alive && pl.threat && d < 9 && this.state !== 'cower' && this.state !== 'defend' && this.state !== 'brawl') {
      const eye = pl.eye();
      const dir = vnorm([a.pos[0] - eye[0], a.pos[1] - eye[1], a.pos[2] + 1.2 - eye[2]]);
      const f = pl.forward;
      const cos = dir[0] * f[0] + dir[1] * f[1] + dir[2] * f[2];
      this.aimed = cos > 0.985 ? this.aimed + dt : Math.max(0, this.aimed - dt * 0.5);
      if (this.aimed > 0.5 && this.state !== 'surrender') {
        this.leave(a, w);
        this.state = 'surrender';
        this.timer = 2.5;
        w.stop(a);
      }
    }
    // looking at people and things while going about
    this.lookTimer -= dt;
    if (this.lookTimer <= 0 && this.state !== 'talk' && this.state !== 'brawl' && this.state !== 'defend') {
      this.lookTimer = rnd(1.5, 4);
      a.lookAt = chance(0.6) ? w.pointOfInterest(a) : null;
    }
    const armed = a.weapon !== null && a.weapon.kind !== 'knife';
    switch (this.state) {
      case 'stroll':
      case 'jog':
        a.mood = 'normal';
        if (this.fear > 0.45) return this.startFlee(a, w);
        if (w.arrived(a) || this.timer <= 0 || a.stuck > 1.5) this.choose(a, w);
        break;
      case 'wait':
        a.mood = 'normal';
        if (this.fear > 0.45) return this.startFlee(a, w);
        if (this.timer <= 0) this.choose(a, w);
        break;
      case 'ground':
        a.stance = 'ground';
        if (this.fear > 0.4) return this.startFlee(a, w);
        if (this.timer <= 0) {
          a.stance = 'stand';
          if (!a.char.animator.transitioning && a.char.animator.stance === 'stand') this.choose(a, w);
        }
        break;
      case 'sit': {
        const s = this.seat;
        if (!s || this.fear > 0.4) return this.startFlee(a, w);
        if (a.stance !== 'sit') {
          if (w.arrived(a) || vdist(a.pos, s.front) < 0.35) {
            // turn round in front of the seat, then sit down
            a.face = [a.pos[0] + Math.cos(s.yaw), a.pos[1] + Math.sin(s.yaw), a.pos[2] + 1.5];
            if (Math.abs(Math.atan2(Math.sin(s.yaw - a.yaw), Math.cos(s.yaw - a.yaw))) < 0.25) {
              a.stance = 'sit';
              // at a table: working at it; on a bench: sitting in one's own way
              a.seat =
                s.desk !== null
                  ? { pos: s.seat, backrest: true, deskHeight: s.desk, variant: 'desk' }
                  : { pos: s.seat, backrest: true, variant: (['upright', 'leanBack', 'crossLegs', 'elbowsOnKnees'] as const)[a.id % 4]! };
              a.face = null;
            }
          } else if (a.stuck > 2) this.choose(a, w);
        } else if (this.timer <= 0) {
          a.stance = 'stand';
          if (!a.char.animator.transitioning && a.char.animator.stance === 'stand') this.choose(a, w);
        }
        break;
      }
      case 'talk': {
        const p = this.partner;
        if (!p || !p.char.alive || this.fear > 0.4) return this.fear > 0.4 ? this.startFlee(a, w) : this.choose(a, w);
        const pd = vdist(a.pos, p.pos);
        // the one who started it walks up to the other (to conversational distance, on its own
        // side), the other waits and turns to it
        const spot = (): V3 => {
          const dir = vnorm([a.pos[0] - p.pos[0], a.pos[1] - p.pos[1], 0]);
          const gap = 0.95 + (a.id % 3) * 0.12;
          return [p.pos[0] + dir[0] * gap, p.pos[1] + dir[1] * gap, p.pos[2]];
        };
        if (this.lead && pd > 1.45) {
          if (w.arrived(a) || (a.goal && vdist(a.goal, p.pos) > 1.8)) w.goTo(a, spot(), pd > 4 ? this.pace : Math.min(this.pace, 1.2));
        } else if (!w.arrived(a)) w.stop(a);
        const close = pd < 1.6;
        a.face = pd < 4 ? p.char.animator.eyes() : null;
        a.lookAt = p.char.animator.eyes();
        this.turnTimer -= dt;
        if (this.turnTimer <= 0) {
          this.turnTimer = rnd(2.5, 6);
          this.speaking = !this.speaking;
          const pb = p.brain as CivilianBrain;
          pb.speaking = !this.speaking;
          pb.turnTimer = this.turnTimer;
        }
        a.talk = close ? (this.speaking ? 'speak' : 'listen') : null;
        if (this.timer <= 0) {
          // now and then a talk turns into a fight
          if (close && chance(w.brawlChance)) {
            w.startBrawl(a, p);
            return;
          }
          this.choose(a, w);
        }
        break;
      }
      case 'watch':
        a.mood = 'normal';
        a.face = this.glanceAt;
        if (this.fear > 0.6 || this.timer <= 0) {
          a.face = null;
          this.choose(a, w);
        }
        break;
      case 'brawl': {
        a.mood = 'normal';
        if (this.timer > 0 && w.brawling(a)) break;
        // over (someone out, or both spent): the winner walks off, the loser gets away
        w.endBrawl(a);
        const o = this.opponent;
        this.opponent = null;
        if (a.char.knockedOut || a.char.animator.knockedDown) {
          // still on the ground: flee once up
          this.fear = 0.6;
          this.startFlee(a, w);
        } else if (o && !o.char.alive) {
          // a killing: get away from the body, fast
          this.state = 'jog';
          this.fear = 0.3;
          const p = w.nav.randomPoint(a.pos, 25, 45, o.pos);
          if (p) w.goTo(a, p, rnd(3.4, 4.2));
          this.timer = 40;
        } else if (!o || o.char.knockedOut || a.char.health >= o.char.health) {
          this.state = 'stroll';
          this.fear = 0.2;
          const p = w.nav.randomPoint(a.pos, 10, 25, o ? o.pos : undefined);
          if (p) w.goTo(a, p, this.pace * 1.1);
          this.timer = 30;
        } else {
          this.fear = 0.6;
          this.startFlee(a, w);
        }
        break;
      }
      case 'defend': {
        const t = this.defendTarget;
        if (!t || !t.char.alive || this.timer <= 0 || !armed) {
          this.defendTarget = null;
          a.carry = 'relaxed';
          a.aimTarget = null;
          return this.startFlee(a, w);
        }
        a.carry = 'aim';
        a.face = t.char.animator.eyes();
        a.aimTarget = [t.pos[0], t.pos[1], t.pos[2] + 1.2];
        a.stance = 'stand';
        a.crouch = 0.4;
        if (w.arrived(a) && chance(0.1)) {
          const p = w.nav.randomPoint(a.pos, 2, 5, t.pos);
          if (p) w.goTo(a, p, 1.6);
        }
        break;
      }
      case 'flee':
        a.mood = this.fear > 0.8 ? 'panic' : 'normal';
        a.face = null;
        a.stance = 'stand';
        if (this.fear > 0.9 && w.time - this.screamed > 4) {
          this.screamed = w.time;
          w.noise({ pos: a.pos, radius: 14, kind: 'scream', source: a });
        }
        if (w.arrived(a) || a.stuck > 1.2) {
          if (this.fear > 0.35) this.startFlee(a, w);
          else this.choose(a, w);
        }
        break;
      case 'cower':
        a.mood = 'cower';
        if (this.timer <= 0) {
          a.mood = 'normal';
          this.startFlee(a, w);
        }
        break;
      case 'surrender':
        a.mood = 'surrender';
        a.face = [pl.feet[0], pl.feet[1], pl.feet[2] + 1.6];
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
    this.leave(a, w);
    a.talk = null;
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

  tick(a: Actor, w: ActorWorld, dt: number): void {
    // a fight draws the eye (also from a bench or mid-conversation)
    if (this.glanceAt && w.time < this.glanceUntil && this.state !== 'brawl' && this.state !== 'flee') a.lookAt = this.glanceAt;
    if (this.state !== 'defend' || !this.defendTarget) return;
    this.shotTimer -= dt;
    const an = a.char.animator;
    if (an.transitioning || an.knockedDown || a.reloading || this.shotTimer > 0) return;
    const t = this.defendTarget;
    const chest: V3 = [t.pos[0], t.pos[1], t.pos[2] + 1.25];
    if (!w.canSee(a, chest)) return;
    if (this.burst <= 0) {
      this.burst = Math.floor(rnd(2, 5));
      this.shotTimer = rnd(0.5, 1.2);
      return;
    }
    // untrained: wide spread
    if (w.fireAt(a, chest, 0.06 * w.spreadOf(a))) this.burst--;
    this.shotTimer = w.intervalOf(a) * rnd(1.2, 2);
  }

  hear(a: Actor, n: Noise, w: ActorWorld): void {
    if (n.source === a || this.state === 'brawl') return;
    const d = vdist(a.pos, n.pos);
    if (d > n.radius) return;
    const near = 1 - d / n.radius;
    const gain = { shot: 0.5, impact: 0.45, explosion: 1.4, death: 0.9, scream: 0.35, shout: 0.25, fight: 0.12 }[n.kind] * (0.4 + near) / this.bravery;
    if (n.kind === 'fight') {
      // a fight nearby: heads turn to it; people walking by stop and watch (from a few metres)
      this.glanceAt = [n.pos[0], n.pos[1], n.pos[2]];
      this.glanceUntil = w.time + rnd(3, 6);
      if (this.state === 'watch') this.timer = Math.max(this.timer, 4);
      if (this.state === 'stroll' || this.state === 'wait' || this.state === 'jog') {
        this.leave(a, w);
        this.state = 'watch';
        this.timer = rnd(6, 15);
        a.lookAt = [...n.pos];
        if (d < 3) {
          const p = w.nav.randomPoint(a.pos, 2, 4, n.pos);
          if (p) w.goTo(a, p, 1.2);
        } else w.stop(a);
      }
      this.fear = Math.min(2, this.fear + gain);
      return;
    }
    this.fear = Math.min(2, this.fear + gain);
    this.threat = [...n.pos];
    // armed civilians shoot back at soldiers firing nearby
    const armed = a.weapon !== null && a.weapon.kind !== 'knife';
    if (armed && n.source && n.source.faction === 'soldier' && (n.kind === 'shot' || n.kind === 'impact') && this.bravery > 0.85 && this.state !== 'surrender') {
      this.leave(a, w);
      this.state = 'defend';
      this.defendTarget = n.source;
      this.timer = rnd(15, 30);
      w.markHostile(a);
      return;
    }
    if (n.kind === 'explosion' && d < 9 && this.state !== 'surrender') {
      this.leave(a, w);
      this.state = 'cower';
      this.timer = rnd(2.5, 5);
      w.stop(a);
    } else if ((this.state === 'stroll' || this.state === 'wait' || this.state === 'talk' || this.state === 'sit' || this.state === 'ground' || this.state === 'jog' || this.state === 'watch') && this.fear > 0.45) this.startFlee(a, w);
  }

  hurt(a: Actor, from: V3 | null, w: ActorWorld, by: Actor | null): void {
    if (this.state === 'brawl') return;
    this.fear = 2;
    if (from) this.threat = [...from];
    const armed = a.weapon !== null && a.weapon.kind !== 'knife';
    if (armed && by && this.bravery > 0.8) {
      this.leave(a, w);
      this.state = 'defend';
      this.defendTarget = by;
      this.timer = rnd(15, 30);
      w.markHostile(a);
      return;
    }
    if (this.state !== 'flee') this.startFlee(a, w);
  }
}
