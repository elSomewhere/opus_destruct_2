/**
 * Thugs: street fighters who go for civilians, soldiers and the player with their fists or,
 * mostly, a knife.
 *
 * A thug loiters with a swagger, eyeing people. It picks a victim (the player when close and in
 * sight, civilians, now and then a soldier), walks up, draws its knife and shouts (which scares
 * civilians off), then charges. Against a character it fights through the world's brawls (the
 * Brawler: guard, footwork, strikes, knife attacks, blocks); against the player it keeps close,
 * circling, and strikes and cuts. Badly hurt, or shot at by soldiers, it runs.
 */
import { KNIFE_ATTACKS, vdist, vnorm, type V3 } from 'svx-anim';
import type { Actor, ActorWorld, Noise } from './actors.ts';
import type { Brain, Target } from './brain.ts';

type ThugState = 'loiter' | 'stalk' | 'brawl' | 'player' | 'flee';

const rnd = (a: number, b: number): number => a + Math.random() * (b - a);
const chance = (p: number): boolean => Math.random() < p;
const FISTS = ['jab', 'jab', 'cross', 'hook', 'uppercut', 'frontKick'] as const;

export class ThugBrain implements Brain {
  state: ThugState = 'loiter';
  private target: Target | null = null;
  private timer = rnd(2, 5);
  private strikeCool = 0;
  private fear = 0;
  private shouted = -99;
  private readonly boldness = rnd(0.7, 1.3);

  /** A brawl starts (the world's Brawler takes over the footwork). */
  brawl(a: Actor, opponent: Actor): void {
    this.state = 'brawl';
    this.target = { kind: 'actor', actor: opponent };
    a.talk = null;
  }

  private victims(a: Actor, w: ActorWorld): Target | null {
    const pl = w.player;
    const eye = w.eyes(a);
    // the player, close and in sight, is the favourite
    if (pl.alive && pl.threat && vdist(a.pos, pl.feet) < 16 && w.canSeeFrom(eye, pl.chest()) && chance(0.6)) return { kind: 'player' };
    let best: Actor | null = null;
    let bd = Infinity;
    for (const b of w.actors) {
      if (b === a || !b.char.alive || b.opacity < 0.5 || b.faction === 'thug' || b.brawler) continue;
      const d = vdist(a.pos, b.pos);
      const reach = b.faction === 'soldier' ? (chance(0.3 * this.boldness) ? 10 : 0) : 14;
      if (d > reach || d >= bd) continue;
      if (!w.canSeeFrom(eye, w.eyes(b))) continue;
      best = b;
      bd = d;
    }
    return best ? { kind: 'actor', actor: best } : null;
  }

  private targetPos(w: ActorWorld): V3 | null {
    const t = this.target;
    if (!t) return null;
    if (t.kind === 'player') return w.player.alive ? w.player.feet : null;
    return t.actor.char.alive && t.actor.opacity > 0.5 ? t.actor.pos : null;
  }

  private loiter(a: Actor, w: ActorWorld): void {
    this.state = 'loiter';
    this.target = null;
    this.timer = rnd(2.5, 6);
    a.face = null;
    a.aimTarget = null;
    a.char.motion.input.guard = false;
    const p = w.nav.randomPoint(a.pos, 4, 14);
    if (p) w.goTo(a, p, rnd(1.0, 1.3));
  }

  private flee(a: Actor, w: ActorWorld, from: V3): void {
    this.state = 'flee';
    this.timer = rnd(8, 14);
    a.face = null;
    a.char.motion.input.guard = false;
    w.holster(a);
    const p = w.nav.randomPoint(a.pos, 14, 30, from) ?? w.nav.randomPoint(a.pos, 8, 18);
    if (p) w.goTo(a, p, rnd(4.2, 5.2));
  }

  think(a: Actor, w: ActorWorld, dt: number): void {
    this.timer -= dt;
    this.strikeCool -= dt;
    this.fear = Math.max(0, this.fear - dt * 0.05);
    a.stance = 'stand';
    a.crouch = 0;
    a.carry = 'relaxed';
    // badly hurt, or scared off: run
    const hurt = a.char.health < a.char.maxHealth * 0.35;
    if (this.state !== 'flee' && this.state !== 'brawl' && (hurt || this.fear > this.boldness)) return this.flee(a, w, this.threatPos(a, w));
    switch (this.state) {
      case 'loiter': {
        a.mood = 'normal';
        if (w.arrived(a) && chance(0.3)) {
          const p = w.nav.randomPoint(a.pos, 4, 14);
          if (p) w.goTo(a, p, rnd(1.0, 1.3));
        }
        a.lookAt = w.pointOfInterest(a);
        if (this.timer <= 0) {
          this.timer = rnd(2, 4);
          const t = this.victims(a, w);
          if (t) {
            this.target = t;
            this.state = 'stalk';
            this.timer = 25;
          }
        }
        break;
      }
      case 'stalk': {
        const tp = this.targetPos(w);
        if (!tp || this.timer <= 0) return this.loiter(a, w);
        const d = vdist(a.pos, tp);
        const t = this.target!;
        a.lookAt = t.kind === 'player' ? w.player.eye() : t.actor.char.eyes();
        // close enough to mean it: the knife comes out, a shout, then a charge
        if (d < 8) {
          w.drawWeapon(a);
          if (w.time - this.shouted > 6) {
            this.shouted = w.time;
            w.noise({ pos: a.pos, radius: 14, kind: 'shout', source: a });
          }
        }
        if (t.kind === 'player' && d < 1.5) {
          this.state = 'player';
          w.stop(a);
          break;
        }
        if (t.kind === 'actor' && d < 1.7) {
          if (!t.actor.brawler) w.startBrawl(a, t.actor);
          else this.loiter(a, w);
          break;
        }
        if (w.arrived(a) || (a.goal && vdist(a.goal, tp) > 1.2)) {
          const dir = vnorm([a.pos[0] - tp[0], a.pos[1] - tp[1], 0]);
          w.goTo(a, [tp[0] + dir[0] * 0.9, tp[1] + dir[1] * 0.9, tp[2]], d > 8 ? rnd(1.2, 1.5) : rnd(3.4, 4.2));
        }
        break;
      }
      case 'brawl': {
        if (!w.brawling(a)) {
          // over: another victim soon, or a walk
          this.loiter(a, w);
          this.timer = rnd(1, 3);
        }
        break;
      }
      case 'player': {
        const pl = w.player;
        if (!pl.alive) return this.loiter(a, w);
        const d = vdist(a.pos, pl.feet);
        if (d > 5) {
          this.state = 'stalk';
          this.timer = 20;
          break;
        }
        // face up, guard up, keep in reach, circle a little, strike
        w.drawWeapon(a);
        a.face = pl.eye();
        a.lookAt = pl.eye();
        a.char.motion.input.guard = true;
        if (d > 1.25 || d < 0.7 || (w.arrived(a) && chance(0.15))) {
          const dir = vnorm([a.pos[0] - pl.feet[0], a.pos[1] - pl.feet[1], 0]);
          const side = chance(0.5) ? 0.5 : -0.5;
          const p: V3 = [pl.feet[0] + (dir[0] - dir[1] * side) * 0.95, pl.feet[1] + (dir[1] + dir[0] * side) * 0.95, pl.feet[2]];
          w.goTo(a, p, 2.2);
        }
        const an = a.char.motion;
        if (d < 1.45 && this.strikeCool <= 0 && !an.busy && !a.char.controlled) {
          const knife = a.char.weapon?.kind === 'knife';
          const pick = knife ? KNIFE_ATTACKS[Math.floor(Math.random() * KNIFE_ATTACKS.length)]! : FISTS[Math.floor(Math.random() * FISTS.length)]!;
          w.melee(a, { kind: 'player' }, pick);
          this.strikeCool = knife ? rnd(0.9, 1.7) : rnd(0.6, 1.3);
        }
        break;
      }
      case 'flee': {
        a.mood = 'normal';
        if (w.arrived(a) || this.timer <= 0) {
          if (!hurt && this.fear < 0.5) this.loiter(a, w);
          else {
            const p = w.nav.randomPoint(a.pos, 8, 20);
            if (p) w.goTo(a, p, rnd(1.6, 2.4));
            this.timer = rnd(6, 10);
          }
        }
        break;
      }
    }
  }

  private threatPos(a: Actor, w: ActorWorld): V3 {
    let best: V3 = w.player.feet;
    let bd = vdist(a.pos, best);
    for (const b of w.actors) {
      if (b.faction !== 'soldier' || !b.char.alive) continue;
      const d = vdist(a.pos, b.pos);
      if (d < bd) {
        bd = d;
        best = b.pos;
      }
    }
    return best;
  }

  tick(a: Actor, w: ActorWorld, dt: number): void {
    void a;
    void w;
    void dt;
  }

  hear(a: Actor, n: Noise, w: ActorWorld): void {
    if (n.source === a) return;
    const d = vdist(a.pos, n.pos);
    if (d > n.radius) return;
    // rounds landing close and blasts put a thug off; a fight nearby draws it in
    if (n.kind === 'explosion') this.fear += 0.8 * (1 - d / n.radius);
    else if ((n.kind === 'shot' || n.kind === 'impact') && n.source?.faction === 'soldier' && d < 10) this.fear += 0.12;
    if (n.kind === 'fight' && this.state === 'loiter' && n.source && n.source !== a && chance(0.3)) {
      this.timer = 0;
      void w;
    }
  }

  hurt(a: Actor, from: V3 | null, w: ActorWorld, by: Actor | null): void {
    if (this.state === 'brawl') return;
    this.fear += by?.faction === 'soldier' || !by ? 0.35 : 0.1;
    // someone hit back: go for them
    if (by && by.char.alive && this.state === 'loiter') {
      this.target = { kind: 'actor', actor: by };
      this.state = 'stalk';
      this.timer = 20;
    } else if (!by && from && this.state === 'loiter' && w.player.alive) {
      this.target = { kind: 'player' };
      this.state = 'stalk';
      this.timer = 20;
    }
    void a;
  }
}
