#!/usr/bin/env node
/**
 * Browser check of the drive city's people (?engine=wasm&world=drive): waits for the city and its
 * pedestrians, checks they are where the engine says (their roots in their bounding spheres, their
 * shadows on the ground under them), looks at them walking the sidewalks (the street from the
 * spawn, one close up, a group from above), shoots one with the pistol through the debug handle (a
 * shot's raycast finds its body; a round into it) until the `characters` message says it is dead,
 * and looks at the body; with an engine that has gibs and blood, a rocket into the body then
 * tears it apart (fired from above it: the rocket goes off on the body in its way). Screenshots of
 * each (without the HUD and panels); fails on console / page /
 * WebGPU errors, or when no one comes, no one is drawn, the shot one does not die (or bleed, or
 * the blast make no gibs). Usage (dev server running, e.g. `npm run dev -- --port 5190`):
 *   node scripts/people-wasm.mjs [baseUrl] [outDir]
 * Env: CHROME_PATH (browser binary), SMOKE_HEADFUL=1 (show the window), SMOKE_WORLD (another world:
 *   preset=city/angledInfiniteCity), PEOPLE_BODIES
 * (deep|shallow|hybrid: ?bodies=).
 */
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { beforeLoad, launch, softwareWebGPU, worldQuery } from './browser.mjs';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'people-out');
mkdirSync(outDir, { recursive: true });
const failures = [];
const started = Date.now();
const since = () => `${((Date.now() - started) / 1000).toFixed(0)} s`;
const check = (cond, what) => {
  console.log(`${cond ? 'ok  ' : 'FAIL'} ${what} (at ${since()})`);
  if (!cond) failures.push(what);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
/** CharacterFlag bits (protocol.ts) the check looks at. */
const ALIVE = 1;
const ASLEEP = 8;
const DOWN = 16;
const GIB = 32;
const flagNames = (f) => ['alive', 'deep', 'physical', 'asleep', 'down', 'gib'].filter((_, i) => f & (1 << i)).join('+') || 'none';
const browser = await launch(puppeteer, { width: 960, height: 540 });
const errors = [];
try {
  const page = await browser.newPage();
  page.on('console', (m) => {
    const t = m.text();
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl/.test(t)) errors.push(t);
    if (process.env.SMOKE_VERBOSE) console.log(`  [page] ${t}`);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message}`));
  await beforeLoad(page);  // (in software WebGPU: the engine on two threads)
  const bodies = process.env.PEOPLE_BODIES ? `&bodies=${process.env.PEOPLE_BODIES}` : '';
  await page.goto(`${base}?engine=wasm&${worldQuery('world=drive&seed=1')}${bodies}`, { waitUntil: 'load' });
  // (software WebGPU: fewer pixels, and a frame drawn in six but for the screenshots - or the GPU
  // process takes the cores the page and the engine need, and the page falls minutes behind)
  const slow = softwareWebGPU();
  while (!(await page.evaluate(() => !!window.__structvox))) await sleep(200);
  if (slow) {
    await page.evaluate(() => {
      window.__structvox.renderer.renderScale = 0.35;
      window.__structvox.renderer.drawEvery = 6;
    });
  }
  const t0 = Date.now();
  for (;;) {
    const ok = await page.evaluate(() => {
      const s = window.__structvox?.state();
      return !!s && s.ready && s.engine !== null && s.engine.residentChunks > 50 && s.render !== null && s.render.chunksDrawn > 0;
    });
    if (ok) break;
    if (Date.now() - t0 > 240000) throw new Error('the drive city did not load');
    await sleep(250);
  }
  check(true, `drive city loaded in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  // (the view alone: no HUD, no panels; a little sharper than the software GPU's usual, briefly)
  const shot = async (name) => {
    await page.evaluate((s) => {
      document.getElementById('ui').style.visibility = 'hidden';
      window.__structvox.renderer.drawEvery = 1;
      window.__structvox.renderer.renderScale = s ? 0.6 : 1;
    }, slow);
    await sleep(slow ? 1500 : 600);
    await page.screenshot({ path: `${outDir}/${name}.png` });
    await page.evaluate((s) => {
      document.getElementById('ui').style.visibility = '';
      window.__structvox.renderer.drawEvery = s ? 6 : 1;
      window.__structvox.renderer.renderScale = s ? 0.35 : 1;
    }, slow);
    console.log(`  screenshot ${outDir}/${name}.png`);
  };
  const state = () => page.evaluate(() => window.__structvox.state());
  const people = () => page.evaluate(() => window.__structvox.characters());
  /** Keeps the view on a character (its middle; `dz` above it) from inside the page: they walk on while a slow page answers. */
  const follow = (id, dz) =>
    page.evaluate(
      (i, z) => {
        clearInterval(window.__follow);
        window.__follow = setInterval(() => {
          const c = window.__structvox.characters().find((o) => o.id === i);
          if (c) window.__structvox.aimAt(c.centre[0], c.centre[1], c.centre[2] + z);
        }, 30);
      },
      id,
      dz,
    );
  const unfollow = () => page.evaluate(() => clearInterval(window.__follow));
  const statsLine = (e) => `${e.characters} characters: ${e.charactersDeep} deep, ${e.charactersShallow} shallow, ${e.charactersPlanOnly} plan only, ${e.charactersAtRest} at rest, ${e.charactersMs} ms`;
  const flat = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);
  /** A point `d` m ahead of a character on the way it faces (its root's +y). */
  const ahead = (c, d) => {
    const l = Math.hypot(c.forward[0], c.forward[1]);
    const f = l > 1e-3 ? [c.forward[0] / l, c.forward[1] / l] : [1, 0];
    return [c.root[0] + f[0] * d, c.root[1] + f[1] * d, c.root[2] + 0.05];
  };

  // people come (two every half second, beyond 30 m)
  const t1 = Date.now();
  let list = [];
  let s = await state();
  while (Date.now() - t1 < 120000) {
    list = await people();
    s = await state();
    if (list.length >= 8 && s.engine?.characters >= 8) break;
    await sleep(1000);
  }
  console.log(`  engine: ${statsLine(s.engine)}; the page has ${list.length}`);
  check(list.length >= 4 && s.engine.characters >= 4, `people came: ${list.length} (engine: ${s.engine.characters})`);
  // where the engine says they are: the root (bone 0) on the ground, in the bounding sphere
  const p0 = s.player;
  let placed = 0;
  for (const c of list) {
    const d = Math.hypot(c.root[0] - c.centre[0], c.root[1] - c.centre[1], c.root[2] - c.centre[2]);
    if (d <= c.radius + 0.05 && Math.abs(c.root[2] - p0[2]) < 3 && c.radius > 0.3 && c.radius < 2.5) placed++;
    else console.log(`  odd: ${JSON.stringify(c)}`);
  }
  check(placed === list.length, `every one's root is on the street's ground and in its bounding sphere (${placed} of ${list.length})`);
  // their shadows on the ground under them (a physical body's root rides with its pelvis: not there)
  await sleep(500);
  const shadows = await page.evaluate(() => {
    const sv = window.__structvox;
    return sv
      .characters()
      .filter((c) => c.shadow)
      .map((c) => {
        const [x, y, z] = c.shadow;
        const r = sv.collideLocal([x - 0.1, y - 0.1, z + 1], [x + 0.1, y + 0.1, z + 1.05], [0, 0, -2]);
        return r ? z - (z + 1 + r.move[2]) : null;
      })
      .filter((d) => d !== null);
  });
  const onGround = shadows.filter((d) => Math.abs(d) < 0.03).length;
  console.log(`  shadows above the ground: ${shadows.map((d) => d.toFixed(3)).join(' ')}`);
  check(shadows.length > 0 && onGround >= Math.ceil(shadows.length * 0.7), `shadows lie on the ground under the people on their feet (${onGround} of ${shadows.length} within 3 cm)`);
  console.log(`  flags: ${[...new Set(list.map((c) => flagNames(c.flags)))].join(', ')}; meshes ${new Set(list.map((c) => c.mesh)).size}, palettes ${new Set(list.map((c) => c.palette)).size}`);

  // the street from the spawn, looking at the nearest
  list.sort((a, b) => flat(a.root, p0) - flat(b.root, p0));
  const first = list[0];
  console.log(`  nearest: ${first.id} at ${flat(first.root, p0).toFixed(1)} m`);
  await follow(first.id, 0);
  await sleep(500);
  await shot('people-01-street');
  s = await state();
  console.log(`  render: ${s.render.charactersDrawn}/${s.render.characters} drawn, ${s.render.triangles} tris`);
  check(s.render.charactersDrawn > 0, `people drawn looking along the street (${s.render.charactersDrawn} of ${s.render.characters})`);

  // one close up: 3.5 m ahead of it on its sidewalk, the view kept on it as it comes
  let near = (await people()).find((c) => c.id === first.id) ?? first;
  let spot = ahead(near, 3.5);
  await page.evaluate((a) => window.__structvox.teleport(a[0], a[1], a[2]), spot);
  await sleep(2500); // (the streets about the spot streamed in, the page caught up)
  near = (await people()).find((c) => c.id === first.id) ?? near;
  console.log(`  close up: ${near.id} ${flat(near.root, (await state()).player).toFixed(1)} m away, ${flagNames(near.flags)}`);
  await shot('people-02-close');
  s = await state();
  check(s.render.charactersDrawn > 0, `the one close up is drawn (${s.render.charactersDrawn} drawn)`);
  await unfollow();

  // a group from above: where most are within 15 m of one another
  list = await people();
  let best = list[0];
  let most = -1;
  for (const c of list) {
    const n = list.filter((o) => flat(o.root, c.root) < 15).length;
    if (n > most) {
      most = n;
      best = c;
    }
  }
  await page.evaluate(() => window.__structvox.noclip(true));
  await page.evaluate((c) => window.__structvox.teleport(c.root[0] - 6, c.root[1] - 6, c.root[2] + 4), best);
  await sleep(2500);
  await follow(best.id, -0.6);
  await sleep(300);
  await shot('people-03-group');
  await unfollow();
  s = await state();
  console.log(`  group of ${most} about ${best.id}: ${s.render.charactersDrawn}/${s.render.characters} drawn`);
  await page.evaluate(() => window.__structvox.noclip(false));

  // one shot: the stillest of those about (waiting at a kerb, stopped a while - a walker walks on
  // while a slow page aims), from 5 m ahead of it, rounds into its head (its chest after a few)
  // until it is dead (the loop in the page: they run once hit)
  const s0 = await people();
  await sleep(1000);
  list = await people();
  const pl = (await state()).player;
  const moved = (c) => {
    const o = s0.find((p) => p.id === c.id);
    return o ? flat(o.root, c.root) : 99;
  };
  const alive = list.filter((c) => c.flags & ALIVE && !(c.flags & GIB) && flat(c.root, pl) < 60);
  alive.sort((a, b) => Math.round(moved(a) * 2) - Math.round(moved(b) * 2) || flat(a.root, pl) - flat(b.root, pl));
  const target = alive[0];
  if (!target) throw new Error('no one alive to shoot');
  console.log(`  target ${target.id}: moved ${moved(target).toFixed(2)} m in a second, ${flat(target.root, pl).toFixed(1)} m away`);
  // 5 m from it with a clear line from the eye to its head: ahead of it, or behind, or to a side
  // (someone standing facing a wall has the wall ahead)
  spot = ahead(target, 5);
  for (const [fx, fy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
    const l = Math.hypot(target.forward[0], target.forward[1]) || 1;
    const f = [target.forward[0] / l, target.forward[1] / l];
    const at = [target.root[0] + (f[0] * fx - f[1] * fy) * 5, target.root[1] + (f[1] * fx + f[0] * fy) * 5, target.root[2] + 0.05];
    const clear = await page.evaluate(async ({ at, id, head }) => {
      const eye = [at[0], at[1], at[2] + 1.6];
      const d = [head[0] - eye[0], head[1] - eye[1], head[2] - eye[2]];
      const n = Math.hypot(d[0], d[1], d[2]) || 1;
      const hit = await window.__structvox.raycast(eye, [d[0] / n, d[1] / n, d[2] / n], n + 1, true);
      return hit?.character === id;
    }, { at, id: target.id, head: target.head });
    if (clear) {
      spot = at;
      break;
    }
  }
  await page.evaluate((a) => window.__structvox.teleport(a[0], a[1], a[2]), spot);
  await page.evaluate(() => window.__structvox.select('pistol'));
  await sleep(1500);
  const r = await page.evaluate(async (id) => {
    const sv = window.__structvox;
    const find = () => sv.characters().find((c) => c.id === id);
    const wait = (ms) => new Promise((res) => setTimeout(res, ms));
    let rounds = 0;
    let flash = 0;
    const health = find()?.health ?? 0;
    let hurt = 1;
    for (let k = 0; k < 16; k++) {
      const c = find();
      if (!c) return { rounds, flash, hurt, flags: -1 };
      if (!(c.flags & 1)) return { rounds, flash, hurt, flags: c.flags };
      const at = k < 3 || k % 2 === 0 ? c.head : [c.centre[0], c.centre[1], c.centre[2] + 0.25];
      sv.aimAt(at[0], at[1], at[2]);
      sv.fire();
      rounds++;
      for (let w = 0; w < 12; w++) {
        await wait(50);
        const d = find();
        if (d) {
          flash = Math.max(flash, d.flash);
          if (health > 0) hurt = Math.min(hurt, d.health / health);
        }
        if (d && !(d.flags & 1)) return { rounds, flash, hurt, flags: d.flags };
      }
    }
    return { rounds, flash, hurt, flags: find()?.flags ?? -1 };
  }, target.id);
  console.log(`  target ${target.id}: ${r.rounds} rounds, hit flash ${r.flash.toFixed(2)} (a flash lasts some ten ticks: a slow page may not see it), health x${r.hurt.toFixed(2)}, now ${r.flags < 0 ? 'gone' : flagNames(r.flags)}`);
  check(r.flash > 0 || r.hurt < 1 || (r.flags >= 0 && !(r.flags & ALIVE)), `a round hit it (flash ${r.flash.toFixed(2)}, health x${r.hurt.toFixed(2)})`);
  check(r.flags >= 0 && !(r.flags & ALIVE), `it died (after ${r.rounds} rounds: ${r.flags < 0 ? 'gone' : flagNames(r.flags)})`);
  await follow(target.id, -0.3);
  await shot('people-04-shot');
  // the body falls: down, at rest, or its middle well below a standing one's (slumped on a wall)
  const t2 = Date.now();
  const fell = (c) => (c.flags & (DOWN | ASLEEP)) !== 0 || c.centre[2] < target.centre[2] - 0.3;
  let body;
  while (Date.now() - t2 < 20000) {
    body = (await people()).find((c) => c.id === target.id);
    if (!body || fell(body)) break;
    await sleep(250);
  }
  check(body !== undefined && fell(body), `the body went down (${body ? `${flagNames(body.flags)}, its middle ${(target.centre[2] - body.centre[2]).toFixed(2)} m lower` : 'gone'})`);
  await sleep(2000);
  await unfollow();
  body = (await people()).find((c) => c.id === target.id) ?? body;
  if (body) await page.evaluate((c) => window.__structvox.aimAt(c.root[0], c.root[1], c.root[2] + 0.2), body);
  await shot('people-05-down');
  s = await state();
  console.log(`  body: ${body ? `${flagNames(body.flags)}, root at ${body.root.map((v) => v.toFixed(2)).join(' ')}` : 'gone'}; engine: ${statsLine(s.engine)}; ${s.render.charactersDrawn}/${s.render.characters} drawn; fps ${s.fps.toFixed(0)}`);
  // the engine's blood (an engine with gibs and blood reports the people's health too)
  const bloody = list.some((c) => c.health > 0);
  if (!bloody) console.log('  (no health from this engine: a module from before the gibs and blood)');
  else check(s.render.bloodDrops + s.render.bloodStains > 0, `the rounds drew blood (${s.render.bloodDrops} drops, ${s.render.bloodStains} stains)`);

  // a rocket into the body (it lies still; the living walk on while a rocket flies), from above it
  // - the rocket goes off on it: the blast tears it apart (gibs, blood)
  if (bloody) {
    const old = new Set((await people()).filter((c) => c.flags & GIB).map((c) => c.id));
    const b0 = (await people()).find((c) => c.id === target.id);
    if (!b0) throw new Error('the body is gone');
    await page.evaluate(() => {
      window.__structvox.noclip(true);
      window.__structvox.select('rocket');
    });
    await page.evaluate((c) => window.__structvox.teleport(c.root[0] - 0.8, c.root[1] - 0.8, c.root[2] + 1.6), b0);
    await sleep(800);
    const at = await page.evaluate((id) => {
      const sv = window.__structvox;
      const c = sv.characters().find((o) => o.id === id);
      if (!c) return null;
      sv.aimAt(c.centre[0], c.centre[1], c.centre[2]);
      sv.fire();
      return c.centre;
    }, target.id);
    if (!at) throw new Error('the body is gone');
    // (watched from 7 m back the way the pistol was fired, 3 m up)
    const bx = spot[0] - at[0];
    const by = spot[1] - at[1];
    const bl = Math.hypot(bx, by) || 1;
    const view = [at[0] + (bx / bl) * 7, at[1] + (by / bl) * 7, at[2] + 3];
    await page.evaluate((v) => window.__structvox.teleport(v[0], v[1], v[2] - 1.6), view);
    await page.evaluate((a) => window.__structvox.aimAt(a[0], a[1], a[2]), at);
    await sleep(300);
    await shot('people-06-blast');
    const t3 = Date.now();
    let gibs = [];
    while (Date.now() - t3 < 15000) {
      gibs = (await people()).filter((c) => c.flags & GIB && !old.has(c.id));
      if (gibs.length > 0) break;
      await sleep(200);
    }
    s = await state();
    const left = (await people()).find((c) => c.id === target.id);
    console.log(`  rocket into ${target.id}: ${gibs.length} new gibs (${old.size} before), the body ${left ? flagNames(left.flags) : 'gone'}; blood ${s.render.bloodDrops} drops, ${s.render.bloodStains} stains`);
    check(gibs.length > 0, `the blast tore the body apart (${gibs.length} new gibs)`);
    await sleep(2500);
    await page.evaluate((a) => window.__structvox.aimAt(a[0], a[1], a[2]), at);
    await shot('people-07-aftermath');
    s = await state();
    const now = (await people()).filter((c) => c.flags & GIB).length;
    console.log(`  after: ${now} gibs, ${s.render.charactersDrawn}/${s.render.characters} drawn, blood ${s.render.bloodDrops} drops, ${s.render.bloodStains} stains`);
    check(s.render.bloodStains > 0, `blood stains the street (${s.render.bloodStains} stains)`);
    // (the gib nearest the blast, close: a piece of one drawn with its one matrix)
    const piece = (await people()).filter((c) => c.flags & GIB).sort((p, q) => flat(p.centre, at) - flat(q.centre, at))[0];
    if (piece) {
      await page.evaluate((c) => window.__structvox.teleport(c.centre[0] - 1.2, c.centre[1] - 1.2, c.centre[2] - 0.4), piece);
      await sleep(1500);
      await page.evaluate((c) => window.__structvox.aimAt(c.centre[0], c.centre[1], c.centre[2]), piece);
      await shot('people-08-gib');
      s = await state();
      console.log(`  gib ${piece.id >>> 0}: ${flagNames(piece.flags)}, radius ${piece.radius.toFixed(2)} m, ${flat(piece.centre, at).toFixed(1)} m from the blast; ${s.render.charactersDrawn} drawn`);
    }
    await page.evaluate(() => window.__structvox.noclip(false));
  }
  check(errors.length === 0, `no console errors${errors.length ? `: ${errors.slice(0, 3).join(' | ')}` : ''}`);
} catch (e) {
  check(false, String(e));
} finally {
  await browser.close();
}
process.exit(failures.length ? 1 : 0);
