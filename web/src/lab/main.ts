/**
 * svx_anim lab (lab.html): the animation engine on its own, without the physics engine. A test
 * course, an orbit / follow camera and a cast of scripted characters showing locomotion
 * (walk, run, crouch, strafe, stairs, turning on the spot), weapon handling, moods and
 * reactions; click a character to shoot it (wounds, severed limbs, ragdoll), shift-click the
 * ground for a rocket (gibs); smooth or retro presentation. `window.__lab` drives it from scripts.
 */
import '../styles.css';
import {
  bakeRetroSet,
  Character,
  GibSystem,
  makeCivilian,
  makeRifle,
  makeSoldier,
  meshPart,
  ModelMesher,
  VoxelCollision,
  type Gib,
  type GibSpec,
  type HumanVariant,
  type RetroSet,
  type V3,
  type VoxelModel,
} from 'svx-anim';
import { DebugView, type Vec3 } from '../engine/protocol.ts';
import type { GpuCharacterMesh } from '../render/characters.ts';
import { cross, normalize } from '../render/math.ts';
import { Renderer } from '../render/renderer.ts';
import { buildLabLevel, LAB_H } from './level.ts';

interface Actor {
  name: string;
  variant: HumanVariant;
  armed: boolean;
  char: Character;
  palette: number;
  script: (a: Actor, t: number, dt: number) => void;
  pos: V3;
  yaw: number;
  fireCooldown: number;
  own: { mesher: ModelMesher; mesh: GpuCharacterMesh | null; version: number } | null;
  propSkin: Float32Array;
  stepSkin: Float32Array;
}

type Style = 'smooth' | 'retro' | 'retro-chunky';
const FOV = (50 * Math.PI) / 180;
const BLOOD: [number, number, number] = [0.3, 0.012, 0.01];

async function main(): Promise<void> {
  const canvas = document.querySelector<HTMLCanvasElement>('#view')!;
  const renderer = await Renderer.create(canvas, (r) => console.error('device lost', r));
  const level = buildLabLevel();
  renderer.setTextures(level.textures);
  for (const m of level.meshes) renderer.chunks.upsert(m);
  const collision = new VoxelCollision(LAB_H, level.solidAt);
  const groundAt = (x: number, y: number, z: number): number => collision.groundHeight(x, y, z + 0.7, z - 1.5) ?? z;
  const cc = renderer.characters;
  const gibs = new GibSystem(collision);

  const mesher = new ModelMesher();
  const meshes = new Map<VoxelModel, GpuCharacterMesh>();
  const meshOf = (m: VoxelModel): GpuCharacterMesh => {
    let g = meshes.get(m);
    if (!g) {
      g = cc.uploadMesh(mesher.mesh(m), m.name);
      meshes.set(m, g);
    }
    return g;
  };
  const rifle = makeRifle();
  const rifleMesh = meshOf(rifle.model);
  const retroSets = new Map<string, RetroSet>();
  let style: Style = (new URLSearchParams(location.search).get('anim') as Style) ?? 'smooth';
  if (!['smooth', 'retro', 'retro-chunky'].includes(style)) style = 'smooth';
  const retroFor = (a: Actor): RetroSet | null => {
    if (style === 'smooth') return null;
    const vs = style === 'retro-chunky' ? 1 / 16 : 1 / 32;
    const key = `${a.variant.spec.name}|${a.armed}|${vs}`;
    let set = retroSets.get(key);
    if (!set) {
      set = bakeRetroSet(a.variant.model, { weapon: a.armed ? rifle : null, voxelSize: vs });
      retroSets.set(key, set);
    }
    return set;
  };

  const actors: Actor[] = [];
  const specs: { name: string; variant: () => HumanVariant; armed: boolean; pos: V3; yaw: number; script: Actor['script'] }[] = [];
  const spawn = (s: (typeof specs)[number]): Actor => {
    const variant = s.variant();
    const char = new Character({ model: variant.model, palette: variant.palette, collision, weapon: s.armed ? rifle : null, seed: actors.length + 1 });
    const pos: V3 = [s.pos[0], s.pos[1], groundAt(s.pos[0], s.pos[1], 1)];
    char.place(pos, s.yaw);
    const a: Actor = { name: s.name, variant, armed: s.armed, char, palette: cc.palette(variant.palette), script: s.script, pos, yaw: s.yaw, fireCooldown: 0, own: null, propSkin: new Float32Array(16), stepSkin: new Float32Array(char.skin.length) };
    a.char.retro = retroFor(a);
    return a;
  };
  const add = (name: string, variant: () => HumanVariant, armed: boolean, pos: V3, yaw: number, script: Actor['script']): void => {
    specs.push({ name, variant, armed, pos, yaw, script });
  };

  // movement helpers: steer the root along a path at a speed, facing the motion
  const moveTo = (a: Actor, x: number, y: number, speed: number, dt: number, face = true): boolean => {
    const dx = x - a.pos[0], dy = y - a.pos[1];
    const d = Math.hypot(dx, dy);
    if (d < 0.05) return true;
    const step = Math.min(d, speed * dt);
    a.pos[0] += (dx / d) * step;
    a.pos[1] += (dy / d) * step;
    a.pos[2] = groundAt(a.pos[0], a.pos[1], a.pos[2]);
    if (face) a.yaw = turnTowards(a.yaw, Math.atan2(dy, dx), 6 * dt);
    return false;
  };
  const circle = (cx: number, cy: number, r: number, speed: number, dir = 1) => (a: Actor, t: number): void => {
    const th = (speed / r) * dir * t;
    const tx = cx + r * Math.cos(th), ty = cy + r * Math.sin(th);
    a.pos[0] = tx;
    a.pos[1] = ty;
    a.pos[2] = groundAt(tx, ty, a.pos[2]);
    a.yaw = th + (dir * Math.PI) / 2;
  };
  const waypoints = (pts: [number, number][], speed: number) => {
    let i = 0;
    return (a: Actor, _t: number, dt: number): void => {
      const p = pts[i % pts.length]!;
      if (moveTo(a, p[0], p[1], speed, dt)) i++;
    };
  };
  const input = (a: Actor) => a.char.animator.input;

  const target: V3 = [0, 12, 1.4];
  add('patrol', () => makeSoldier(1), true, [-8, -3, 0], 0, waypoints([[-2, -3], [-2, 0], [-8, 0], [-8, -3]], 1.5));
  add('sprint', () => makeSoldier(2), true, [-6, -10, 0], 0, circle(-10, -10, 4, 5));
  add('strafe+fire', () => makeSoldier(5), true, [-3, 4, 0], Math.PI / 2, (a, t, dt) => {
    input(a).carry = 'aim';
    input(a).aimAt = target;
    moveTo(a, -3 + 2.5 * Math.sin(t * 0.45), 4 + Math.sin(t * 0.3) * 0.6, 1.3, dt, false);
    a.yaw = turnTowards(a.yaw, Math.atan2(target[1] - a.pos[1], target[0] - a.pos[0]), 5 * dt);
    a.fireCooldown -= dt;
    if (a.fireCooldown <= 0 && Math.sin(t * 1.3) > 0.3) {
      a.char.fire();
      a.fireCooldown = 0.11;
    }
  });
  add('crouch', () => makeSoldier(9), true, [2, -12, 0], 0, (a, t, dt) => {
    input(a).crouch = 1;
    input(a).carry = 'ready';
    waypoints([[6, -12], [6, -9], [2, -9], [2, -12]], 1.0)(a, t, dt);
  });
  add('stairs', () => makeCivilian(3), false, [2, 4.5, 0], 0, waypoints([[15, 4.5], [2, 4.5]], 1.4));
  add('panic', () => makeCivilian(6), false, [-10, 6, 0], 0, (a, t) => {
    input(a).mood = 'panic';
    circle(-10, 3, 3, 5.2, -1)(a, t);
  });
  add('cower', () => makeCivilian(4), false, [1.5, 1, 0], -Math.PI / 2, (a) => {
    input(a).mood = 'cower';
  });
  add('surrender', () => makeCivilian(2), false, [0, 1, 0], -Math.PI / 2, (a) => {
    input(a).mood = 'surrender';
  });
  add('idle', () => makeCivilian(1), false, [-1.5, 1, 0], -Math.PI / 2, (a, t) => {
    a.yaw = -Math.PI / 2 + (Math.floor(t / 4) % 2 === 0 ? 0 : 1.6);
  });
  add('aim-turn', () => makeSoldier(11), true, [3.5, 1, 0], -Math.PI / 2, (a, t, dt) => {
    const tt: V3 = [3.5 + 6 * Math.cos(t * 0.35), 1 + 6 * Math.sin(t * 0.35), 1.2];
    input(a).carry = 'aim';
    input(a).aimAt = tt;
    a.yaw = turnTowards(a.yaw, Math.atan2(tt[1] - a.pos[1], tt[0] - a.pos[0]), 3 * dt);
  });
  add('walker', () => makeCivilian(8), false, [-6, 10, 0], 0, waypoints([[-1, 10], [-1, 12], [-6, 12], [-6, 10]], 1.3));
  const reset = (): void => {
    for (const a of actors) if (a.own?.mesh) cc.releaseMesh(a.own.mesh);
    actors.length = 0;
    for (const s of specs) actors.push(spawn(s));
    for (const g of gibs.gibs) releaseGib(g);
    gibs.clear();
  };

  // gibs: meshes per loose part
  const gibMeshes = new Map<Gib, { mesh: GpuCharacterMesh; palette: number; skin: Float32Array; own: boolean }>();
  const spawnGib = (spec: GibSpec, a: Actor, bleed: number): void => {
    const own = !spec.prop;
    const mesh = own ? cc.uploadMesh(meshPart(spec.part, spec.voxelSize), 'gib') : rifleMesh;
    const g = gibs.spawn(spec.part, spec.voxelSize, spec.bonePos, spec.boneRot, spec.boneRestHead, spec.vel, spec.ang);
    g.bleed = bleed;
    gibMeshes.set(g, { mesh, palette: a.palette, skin: new Float32Array(16), own });
  };
  const releaseGib = (g: Gib): void => {
    const m = gibMeshes.get(g);
    if (m?.own) cc.releaseMesh(m.mesh);
    gibMeshes.delete(g);
  };
  const killed = (a: Actor): void => {
    const w = a.char.dropWeapon();
    if (w) spawnGib(w, a, 0);
  };

  // camera
  const cam = { target: [0, 0, 1.0] as Vec3, dist: 7, yaw: Math.PI / 2 + 0.3, pitch: 0.18, follow: -1 };
  const view = (): { eye: Vec3; fwd: Vec3 } => {
    const cp = Math.cos(cam.pitch);
    const fwd: Vec3 = [-cp * Math.cos(cam.yaw), -cp * Math.sin(cam.yaw), -Math.sin(cam.pitch)];
    return { eye: [cam.target[0] - fwd[0] * cam.dist, cam.target[1] - fwd[1] * cam.dist, cam.target[2] - fwd[2] * cam.dist], fwd };
  };
  /** World ray through a canvas pixel. */
  const pixelRay = (px: number, py: number): { o: V3; d: V3 } => {
    const { eye, fwd } = view();
    const right = normalize(cross(fwd, [0, 0, 1]));
    const up = cross(right, fwd);
    const w = canvas.clientWidth, h = canvas.clientHeight;
    const t = Math.tan(FOV / 2);
    const x = ((px / w) * 2 - 1) * t * (w / h);
    const y = (1 - (py / h) * 2) * t;
    return { o: eye, d: normalize([fwd[0] + right[0] * x + up[0] * y, fwd[1] + right[1] * x + up[1] * y, fwd[2] + right[2] * x + up[2] * y]) };
  };
  const shoot = (o: V3, d: V3): boolean => {
    let best: { a: Actor; hit: NonNullable<ReturnType<Character['raycast']>> } | null = null;
    for (const a of actors) {
      const hit = a.char.raycast(o, d, best ? best.hit.distance : 100);
      if (hit && (!best || hit.distance < best.hit.distance)) best = { a, hit };
    }
    if (!best) return false;
    const { a, hit } = best;
    const wasAlive = a.char.alive;
    const r = a.char.wound(hit, d, 34);
    gibs.spray(hit.point, d, 8 + Math.min(16, r.removed.length >> 1), 3.2, 0.5);
    gibs.spray(hit.point, [-d[0], -d[1], -d[2]], 3, 1.5, 0.9);
    for (let i = 0; i < r.removed.length; i += 4) {
      const c = a.variant.palette[r.removed[i]!.slot] ?? BLOOD;
      gibs.spray(hit.point, d, 1, 2.8, 0.7, [c[0], c[1], c[2]]);
    }
    for (const g of r.gibs) spawnGib(g, a, 30);
    if (wasAlive && !a.char.alive) killed(a);
    return true;
  };
  const rocket = (p: V3): void => {
    for (const a of actors) {
      const wasAlive = a.char.alive;
      const r = a.char.blast(p, 1, 1);
      if (r.gibbed) {
        gibs.spray(a.char.bounds().center, [0, 0, 1], 60, 5, 1.2);
        for (const g of r.gibs) spawnGib(g, a, 40);
      }
      if (wasAlive && !a.char.alive) killed(a);
    }
    gibs.impulse(p, 4, 11);
    renderer.particles.spawn({ pos: p, vel: [0, 0, 0], life: 0.3, size: 1.2, grow: 3, color: [4, 1.8, 0.45, 0.9], additive: true });
  };

  let drag: { x: number; y: number; moved: boolean } | null = null;
  canvas.addEventListener('pointerdown', (e) => {
    drag = { x: e.clientX, y: e.clientY, moved: false };
    canvas.setPointerCapture(e.pointerId);
  });
  canvas.addEventListener('pointerup', (e) => {
    if (drag && !drag.moved) {
      const r = pixelRay(e.offsetX, e.offsetY);
      if (e.shiftKey) {
        const t = collision.raycast(r.o, r.d, 200);
        if (t > 0) rocket([r.o[0] + r.d[0] * t, r.o[1] + r.d[1] * t, r.o[2] + r.d[2] * t]);
      } else shoot(r.o, r.d);
    }
    drag = null;
  });
  canvas.addEventListener('pointermove', (e) => {
    if (!drag) return;
    if (Math.abs(e.clientX - drag.x) + Math.abs(e.clientY - drag.y) > 3) drag.moved = true;
    if (!drag.moved) return;
    cam.yaw -= (e.clientX - drag.x) * 0.006;
    cam.pitch = Math.max(-0.2, Math.min(1.4, cam.pitch + (e.clientY - drag.y) * 0.006));
    drag.x = e.clientX;
    drag.y = e.clientY;
  });
  canvas.addEventListener('wheel', (e) => {
    cam.dist = Math.max(1, Math.min(40, cam.dist * Math.exp(e.deltaY * 0.001)));
  });

  // panel
  const ui = document.querySelector<HTMLElement>('#ui')!;
  const panel = document.createElement('div');
  panel.style.cssText = 'position:absolute;top:8px;left:8px;pointer-events:auto;background:rgba(0,0,0,.55);padding:8px 10px;border-radius:6px;font:12px system-ui;color:#e6e8eb;max-width:280px;line-height:1.5';
  panel.innerHTML = `<b>svx_anim lab</b><br>drag: orbit · wheel: zoom · click: shoot · shift-click: rocket<br>
    <label>time <input id="ts" type="range" min="0" max="1.5" step="0.05" value="1"></label><br>
    <label>animation <select id="style"><option value="smooth">smooth</option><option value="retro">retro (Voxel Doom)</option><option value="retro-chunky">retro, chunky</option></select></label><br>
    <button id="reset">reset cast</button> <button id="kill">kill all</button> <button id="gib">rocket all</button><div id="cast"></div>`;
  ui.append(panel);
  const cast = panel.querySelector<HTMLDivElement>('#cast')!;
  const follow = (i: number): void => {
    cam.follow = i;
    if (i < 0) cam.target = [0, 0, 1];
  };
  const b0 = document.createElement('button');
  b0.textContent = 'overview';
  b0.onclick = () => follow(-1);
  cast.append(b0);
  specs.forEach((s, i) => {
    const b = document.createElement('button');
    b.textContent = s.name;
    b.onclick = () => follow(i);
    cast.append(b);
  });
  let timeScale = 1;
  panel.querySelector<HTMLInputElement>('#ts')!.oninput = (e) => (timeScale = Number((e.target as HTMLInputElement).value));
  const styleSel = panel.querySelector<HTMLSelectElement>('#style')!;
  styleSel.value = style;
  const setStyle = (s: Style): void => {
    style = s;
    styleSel.value = s;
    for (const a of actors) a.char.retro = a.char.weapon || !a.armed ? retroFor(a) : null;
  };
  styleSel.onchange = () => setStyle(styleSel.value as Style);
  panel.querySelector<HTMLButtonElement>('#reset')!.onclick = reset;
  panel.querySelector<HTMLButtonElement>('#kill')!.onclick = () => {
    for (const a of actors)
      if (a.char.alive) {
        a.char.die(a.char.bounds().center, [(Math.random() - 0.5) * 4, (Math.random() - 0.5) * 4, 1]);
        killed(a);
      }
  };
  panel.querySelector<HTMLButtonElement>('#gib')!.onclick = () => {
    for (const a of actors) rocket(a.char.bounds().center);
  };

  reset();
  let simT = 0;
  let last = 0;
  let retroClock = 0;
  const step = (dt: number): void => {
    simT += dt;
    retroClock += dt;
    const retroTick = retroClock >= 4 / 35;
    if (retroTick) retroClock %= 4 / 35;
    for (const a of actors) {
      if (a.char.alive) {
        a.script(a, simT, dt);
        a.char.setRoot(a.pos, a.yaw);
      }
      a.char.update(dt);
      if (style === 'smooth' || a.char.alive || retroTick) a.stepSkin.set(a.char.skin);
    }
    gibs.update(dt);
    const live = new Set(gibs.gibs);
    for (const [g, m] of gibMeshes) {
      if (!live.has(g)) releaseGib(g);
      else if (style === 'smooth' || retroTick) gibs.writeSkin(g, m.skin);
    }
  };
  const labApi = {
    cam,
    actors,
    follow,
    step,
    shoot,
    rocket,
    reset,
    setStyle,
    setTimeScale: (s: number) => (timeScale = s),
    names: () => specs.map((s) => s.name),
    gibs,
  };
  (window as unknown as { __lab: typeof labApi }).__lab = labApi;

  const frame = (tMs: number): void => {
    const dt = last === 0 ? 1 / 60 : Math.min(0.05, (tMs - last) / 1000);
    last = tMs;
    if (timeScale > 0) step(dt * timeScale);
    cc.begin();
    for (const a of actors) {
      const ch = a.char;
      const b = ch.bounds();
      const tint: [number, number, number, number] = [1, 0.15, 0.1, ch.flash * 0.45];
      if (ch.retroFrame && ch.alive) {
        ch.writeRetroSkin(a.propSkin);
        cc.add(meshOf(ch.retroFrame), a.propSkin, 1, a.palette, { center: b.center, radius: 1.3, tint });
      } else {
        let mesh: GpuCharacterMesh;
        if (ch.ownsModel) {
          a.own ??= { mesher: new ModelMesher(), mesh: null, version: -1 };
          if (a.own.version !== ch.geometryVersion || !a.own.mesh) {
            if (a.own.mesh) cc.releaseMesh(a.own.mesh);
            a.own.mesh = cc.uploadMesh(a.own.mesher.mesh(ch.model), a.name);
            a.own.version = ch.geometryVersion;
          }
          mesh = a.own.mesh;
        } else mesh = meshOf(ch.model);
        cc.add(mesh, a.stepSkin, ch.model.skeleton.count, a.palette, { center: b.center, radius: b.radius + 0.3, tint });
        if (ch.weapon && ch.alive) {
          ch.animator.writePropSkin(a.propSkin);
          cc.add(rifleMesh, a.propSkin, 1, a.palette, { center: ch.animator.weaponPos, radius: 0.8 });
        }
      }
      if (ch.alive) cc.decal([a.pos[0], a.pos[1], ch.animator.rootPos[2] + 0.004], [0, 0, 1], 0.42, [0, 0, 0, 0.5], 0);
    }
    for (const [g, m] of gibMeshes) cc.add(m.mesh, m.skin, 1, m.palette, { center: g.pos, radius: g.radius + 0.05 });
    gibs.forEachDrop((p, size, c) => cc.bit(p, size, [0, 0, 0, 1], c));
    gibs.forEachStain((p, n, size, age, c) => cc.decal(p, n, Math.max(0.045, size * 2.4), [c[0] * 0.8, c[1] * 0.8, c[2] * 0.8, Math.min(0.92, 0.5 + age)], 1));
    if (cam.follow >= 0 && actors[cam.follow]) {
      const p = actors[cam.follow]!.char.pose.p[1]!;
      cam.target = [p[0], p[1], p[2] + 0.1];
    }
    const { eye, fwd } = view();
    renderer.render({ camera: { eye, forward: fwd, fovY: FOV, near: 0.05 }, timeS: tMs / 1000, debugView: DebugView.None, flashPos: [0, 0, 0], flashIntensity: 0, voxelSize: LAB_H });
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  (window as unknown as { __structvox: unknown }).__structvox = { state: () => ({ ready: true, render: { chunksDrawn: 1 } }) };
}

function turnTowards(cur: number, want: number, maxStep: number): number {
  let d = want - cur;
  while (d > Math.PI) d -= 2 * Math.PI;
  while (d < -Math.PI) d += 2 * Math.PI;
  return cur + Math.max(-maxStep, Math.min(maxStep, d));
}

main().catch((e: unknown) => {
  console.error(e);
  document.body.append(Object.assign(document.createElement('pre'), { className: 'boot-error', textContent: `lab failed: ${String(e)}` }));
});
