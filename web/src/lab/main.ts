/**
 * svx_anim lab (lab.html): the animation engine on its own, without the physics engine. A test
 * course, an orbit / follow camera and a cast of scripted characters showing locomotion
 * (walk, run, crouch, strafe, stairs, turning on the spot), weapon handling, moods and
 * reactions. `window.__lab` drives it from scripts.
 */
import '../styles.css';
import {
  HumanoidAnimator,
  makeCivilian,
  makeRifle,
  makeSoldier,
  ModelMesher,
  VoxelCollision,
  type HumanVariant,
  type Prop,
  type V3,
} from 'svx-anim';
import { DebugView, type Vec3 } from '../engine/protocol.ts';
import type { GpuCharacterMesh } from '../render/characters.ts';
import { Renderer } from '../render/renderer.ts';
import { buildLabLevel, LAB_H } from './level.ts';

interface Actor {
  name: string;
  variant: HumanVariant;
  mesh: GpuCharacterMesh;
  palette: number;
  anim: HumanoidAnimator;
  skin: Float32Array;
  prop: { prop: Prop; mesh: GpuCharacterMesh; skin: Float32Array } | null;
  /** Scripted behaviour: sets root and inputs for time t. */
  script: (a: Actor, t: number, dt: number) => void;
  pos: V3;
  yaw: number;
  fireCooldown: number;
}

async function main(): Promise<void> {
  const canvas = document.querySelector<HTMLCanvasElement>('#view')!;
  const renderer = await Renderer.create(canvas, (r) => console.error('device lost', r));
  const level = buildLabLevel();
  renderer.setTextures(level.textures);
  for (const m of level.meshes) renderer.chunks.upsert(m);
  const collision = new VoxelCollision(LAB_H, level.solidAt);
  const groundAt = (x: number, y: number, z: number): number => collision.groundHeight(x, y, z + 0.7, z - 1.5) ?? z;

  const mesher = new ModelMesher();
  const rifle = makeRifle();
  const rifleMesh = renderer.characters.uploadMesh(mesher.mesh(rifle.model), 'rifle');
  const actors: Actor[] = [];
  const spawn = (name: string, variant: HumanVariant, armed: boolean, pos: V3, yaw: number, script: Actor['script']): Actor => {
    const anim = new HumanoidAnimator(variant.model.skeleton, collision, actors.length + 1);
    if (armed) anim.weapon = rifle;
    pos[2] = groundAt(pos[0], pos[1], 1);
    anim.place(pos, yaw);
    const a: Actor = {
      name,
      variant,
      mesh: renderer.characters.uploadMesh(mesher.mesh(variant.model), variant.spec.name),
      palette: renderer.characters.palette(variant.palette),
      anim,
      skin: new Float32Array(variant.model.skeleton.count * 16),
      prop: armed ? { prop: rifle, mesh: rifleMesh, skin: new Float32Array(16) } : null,
      script,
      pos,
      yaw,
      fireCooldown: 0,
    };
    actors.push(a);
    return a;
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
  const circle = (cx: number, cy: number, r: number, speed: number, dir = 1) => (a: Actor, t: number, dt: number): void => {
    const w = (speed / r) * dir;
    const th = w * t;
    const tx = cx + r * Math.cos(th), ty = cy + r * Math.sin(th);
    a.pos[0] = tx;
    a.pos[1] = ty;
    a.pos[2] = groundAt(tx, ty, a.pos[2]);
    a.yaw = th + (dir * Math.PI) / 2;
    void dt;
  };
  const waypoints = (pts: [number, number][], speed: number) => {
    let i = 0;
    return (a: Actor, _t: number, dt: number): void => {
      const p = pts[i % pts.length]!;
      if (moveTo(a, p[0], p[1], speed, dt)) i++;
    };
  };

  const target: V3 = [0, 12, 1.4];
  spawn('patrol', makeSoldier(1), true, [-8, -3, 0], 0, waypoints([[-2, -3], [-2, 0], [-8, 0], [-8, -3]], 1.5));
  spawn('sprint', makeSoldier(2), true, [-6, -10, 0], 0, circle(-10, -10, 4, 5));
  const strafer = spawn('strafe+fire', makeSoldier(5), true, [-3, 4, 0], Math.PI / 2, (a, t, dt) => {
    a.anim.input.carry = 'aim';
    a.anim.input.aimAt = target;
    const x = -3 + 2.5 * Math.sin(t * 0.45);
    moveTo(a, x, 4 + Math.sin(t * 0.3) * 0.6, 1.3, dt, false);
    a.yaw = turnTowards(a.yaw, Math.atan2(target[1] - a.pos[1], target[0] - a.pos[0]), 5 * dt);
    a.fireCooldown -= dt;
    if (a.fireCooldown <= 0 && Math.sin(t * 1.3) > 0.3) {
      a.anim.fire();
      a.fireCooldown = 0.11;
    }
  });
  void strafer;
  spawn('crouch', makeSoldier(9), true, [2, -12, 0], 0, (a, t, dt) => {
    a.anim.input.crouch = 1;
    a.anim.input.carry = 'ready';
    waypoints([[6, -12], [6, -9], [2, -9], [2, -12]], 1.0)(a, t, dt);
  });
  spawn('stairs', makeCivilian(3), false, [2, 4.5, 0], 0, waypoints([[15, 4.5], [2, 4.5]], 1.4));
  spawn('panic', makeCivilian(6), false, [-10, 6, 0], 0, (a, t, dt) => {
    a.anim.input.mood = 'panic';
    circle(-10, 3, 3, 5.2, -1)(a, t, dt);
  });
  spawn('cower', makeCivilian(4), false, [1.5, 1, 0], -Math.PI / 2, (a) => {
    a.anim.input.mood = 'cower';
  });
  spawn('surrender', makeCivilian(2), false, [0, 1, 0], -Math.PI / 2, (a) => {
    a.anim.input.mood = 'surrender';
  });
  spawn('idle', makeCivilian(1), false, [-1.5, 1, 0], -Math.PI / 2, (a, t) => {
    // turns on the spot now and then
    a.yaw = -Math.PI / 2 + (Math.floor(t / 4) % 2 === 0 ? 0 : 1.6);
  });
  spawn('aim-turn', makeSoldier(11), true, [3.5, 1, 0], -Math.PI / 2, (a, t, dt) => {
    const tt: V3 = [3.5 + 6 * Math.cos(t * 0.35), 1 + 6 * Math.sin(t * 0.35), 1.2];
    a.anim.input.carry = 'aim';
    a.anim.input.aimAt = tt;
    a.yaw = turnTowards(a.yaw, Math.atan2(tt[1] - a.pos[1], tt[0] - a.pos[0]), 3 * dt);
  });
  spawn('walker', makeCivilian(8), false, [-6, 10, 0], 0, waypoints([[-1, 10], [-1, 12], [-6, 12], [-6, 10]], 1.3));

  // orbit / follow camera
  const cam = { target: [0, 0, 1.0] as Vec3, dist: 7, yaw: Math.PI / 2 + 0.3, pitch: 0.18, follow: -1 };
  let drag: { x: number; y: number } | null = null;
  canvas.addEventListener('pointerdown', (e) => {
    drag = { x: e.clientX, y: e.clientY };
    canvas.setPointerCapture(e.pointerId);
  });
  canvas.addEventListener('pointerup', () => (drag = null));
  canvas.addEventListener('pointermove', (e) => {
    if (!drag) return;
    cam.yaw -= (e.clientX - drag.x) * 0.006;
    cam.pitch = Math.max(-0.2, Math.min(1.4, cam.pitch + (e.clientY - drag.y) * 0.006));
    drag = { x: e.clientX, y: e.clientY };
  });
  canvas.addEventListener('wheel', (e) => {
    cam.dist = Math.max(1, Math.min(40, cam.dist * Math.exp(e.deltaY * 0.001)));
  });

  // panel
  const ui = document.querySelector<HTMLElement>('#ui')!;
  const panel = document.createElement('div');
  panel.className = 'lab-panel';
  panel.style.cssText = 'position:absolute;top:8px;left:8px;pointer-events:auto;background:rgba(0,0,0,.55);padding:8px 10px;border-radius:6px;font:12px system-ui;color:#e6e8eb;max-width:260px';
  panel.innerHTML = `<b>svx_anim lab</b><br>drag: orbit, wheel: zoom<br><label>time scale <input id="ts" type="range" min="0" max="1.5" step="0.05" value="1"></label><div id="cast"></div>`;
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
  actors.forEach((a, i) => {
    const b = document.createElement('button');
    b.textContent = a.name;
    b.onclick = () => follow(i);
    cast.append(b);
  });
  let timeScale = 1;
  panel.querySelector<HTMLInputElement>('#ts')!.oninput = (e) => (timeScale = Number((e.target as HTMLInputElement).value));

  let simT = 0;
  let last = 0;
  const step = (dt: number): void => {
    simT += dt;
    for (const a of actors) {
      a.script(a, simT, dt);
      a.anim.setRoot(a.pos, a.yaw);
      a.anim.update(dt);
    }
  };
  const labApi = {
    cam,
    actors,
    follow,
    step,
    setTimeScale: (s: number) => (timeScale = s),
    names: () => actors.map((a) => a.name),
  };
  (window as unknown as { __lab: typeof labApi }).__lab = labApi;

  const frame = (tMs: number): void => {
    const dt = last === 0 ? 1 / 60 : Math.min(0.05, (tMs - last) / 1000);
    last = tMs;
    if (timeScale > 0) step(dt * timeScale);
    const cc = renderer.characters;
    cc.begin();
    for (const a of actors) {
      a.anim.world.writeSkin(a.skin);
      const p = a.anim.world.p[1]!;
      cc.add(a.mesh, a.skin, a.variant.model.skeleton.count, a.palette, { center: [p[0], p[1], p[2]], radius: 1.3 });
      if (a.prop) {
        a.anim.writePropSkin(a.prop.skin);
        cc.add(a.prop.mesh, a.prop.skin, 1, a.palette, { center: a.anim.weaponPos, radius: 0.8 });
      }
      cc.decal([p[0], p[1], a.anim.rootPos[2] + 0.001], [0, 0, 1], 0.42, [0, 0, 0, 0.5], 0);
    }
    if (cam.follow >= 0) {
      const a = actors[cam.follow]!;
      const p = a.anim.world.p[1]!;
      cam.target = [p[0], p[1], p[2] + 0.1];
    }
    const cp = Math.cos(cam.pitch);
    const fwd: Vec3 = [-cp * Math.cos(cam.yaw), -cp * Math.sin(cam.yaw), -Math.sin(cam.pitch)];
    const eye: Vec3 = [cam.target[0] - fwd[0] * cam.dist, cam.target[1] - fwd[1] * cam.dist, cam.target[2] - fwd[2] * cam.dist];
    renderer.render({ camera: { eye, forward: fwd, fovY: (50 * Math.PI) / 180, near: 0.05 }, timeS: tMs / 1000, debugView: DebugView.None, flashPos: [0, 0, 0], flashIntensity: 0, voxelSize: LAB_H });
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
