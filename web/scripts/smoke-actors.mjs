#!/usr/bin/env node
/**
 * Browser smoke test of the characters (svx_anim actors) in the real engine (?engine=wasm):
 * population, soldiers firing (their rounds carve the world), the player killing a soldier,
 * a rocket gibbing a group, the retro presentation, city life (conversations, benches, a brawl
 * ending in a knockout), a thug going for a civilian, the player's knife, the animation lab, and characters on a Freedoom map (when
 * data/freedoom/freedoom2.wad exists). Fails on console / page / WebGPU
 * errors. Usage (dev server running): node scripts/smoke-actors.mjs [baseUrl] [outDir]
 */
import { existsSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'smoke-actors-out');
mkdirSync(outDir, { recursive: true });
const chrome = process.env.CHROME_PATH ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const failures = [];
const check = (cond, what) => {
  console.log(cond ? `ok   ${what}` : `FAIL ${what}`);
  if (!cond) failures.push(what);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await puppeteer.launch({
  executablePath: chrome,
  headless: process.env.SMOKE_HEADFUL ? false : true,
  args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--no-first-run', '--window-size=1280,760'],
  defaultViewport: { width: 1280, height: 720, deviceScaleFactor: 1 },
});
const errors = [];
async function waitFor(page, fn, arg, ms, what) {
  const t0 = Date.now();
  for (;;) {
    let v = null;
    try {
      v = await page.evaluate(fn, arg);
    } catch (e) {
      if (!/Execution context was destroyed|Cannot find context/.test(String(e))) throw e;
    }
    if (v) return v;
    if (Date.now() - t0 > ms) {
      check(false, `${what} (timed out after ${ms} ms)`);
      return null;
    }
    await sleep(150);
  }
}
const watch = (page) => {
  page.on('console', (m) => {
    const t = m.text();
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl/.test(t)) errors.push(t);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message} @ ${(e.stack ?? '').split('\n').slice(1, 4).map((l) => l.trim()).join(' < ')}`));
};
/** Aims the player at an actor's chest (standing, kneeling or prone) and fires the current weapon. */
const fireAt = (page, id) =>
  page.evaluate(
    ({ id }) => {
      const sv = window.__structvox;
      const a = sv.actorWorld().actors.find((x) => x.id === id);
      if (!a) return false;
      const e = sv.actorWorld().player.eye();
      const t = a.char.pose.p[3]; // the chest
      sv.look((Math.atan2(t[1] - e[1], t[0] - e[0]) * 180) / Math.PI, (Math.atan2(t[2] - e[2], Math.hypot(t[0] - e[0], t[1] - e[1])) * 180) / Math.PI);
      sv.fire();
      return true;
    },
    { id },
  );

try {
  const page = await browser.newPage();
  watch(page);
  // 1. the city with soldiers and civilians; god mode so the player survives the fight
  await page.goto(`${base}?engine=wasm&world=city&seed=1&god=1&civilians=10&soldiers=6`, { waitUntil: 'load' });
  const pop = await waitFor(page, () => {
    const s = window.__structvox?.actorWorld?.().stats();
    return s && s.civilians + s.soldiers >= 10 ? s : null;
  }, null, 60000, 'characters populate the city');
  if (pop) check(pop.civilians >= 6 && pop.soldiers >= 4, `city: ${pop.civilians} civilians, ${pop.soldiers} soldiers`);
  await page.screenshot({ path: `${outDir}/01-city.png` });
  // 2. soldiers find the player and fire; their rounds carve the world
  const fight = await waitFor(page, () => {
    const w = window.__structvox.actorWorld();
    const e = window.__structvox.state().engine;
    return w.shotsFired >= 20 && w.worldHits >= 5 ? { rounds: w.shotsFired, carved: w.worldHits, bonds: e?.bondsBroken ?? 0, fleeing: w.actors.filter((a) => a.brain.state === 'flee').length } : null;
  }, null, 45000, 'soldiers fire at the player');
  if (!fight)
    console.log(
      '     ',
      JSON.stringify(
        await page.evaluate(() => {
          const w = window.__structvox.actorWorld();
          const e = window.__structvox.state().engine;
          return { rounds: w.shotsFired, bonds: e?.bondsBroken, detached: e?.detachedVoxels, soldiers: w.actors.filter((a) => a.faction === 'soldier').map((a) => `${a.brain.state}/${a.char.weapon?.kind}/${a.mag}`) };
        }),
      ),
    );
  if (fight) {
    check(fight.rounds >= 20, `soldiers fired ${fight.rounds} rounds, ${fight.carved} carved the world (${fight.bonds} bonds broken so far)`);
    check(fight.fleeing > 0, `${fight.fleeing} civilians flee the gunfire`);
  }
  // 3. the player kills a soldier (voxel-exact hits, a wound, then a ragdoll)
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.characters({ ai: false });
    sv.noclip(true);
    const w = sv.actorWorld();
    const a = w.actors.find((x) => x.faction === 'soldier' && x.char.alive);
    // a spot 5 m away with a clear line to the soldier's chest
    const chest = a.char.pose.p[3];
    let spot = null;
    for (let k = 0; k < 16 && !spot; k++) {
      const ang = (k / 16) * Math.PI * 2;
      const p = [a.pos[0] + Math.cos(ang) * 5, a.pos[1] + Math.sin(ang) * 5, a.pos[2] + 0.1];
      if (w.env.lineOfSight([p[0], p[1], p[2] + 1.6], chest)) spot = p;
    }
    spot ??= [a.pos[0] - 5, a.pos[1], a.pos[2] + 0.1];
    sv.teleport(spot[0], spot[1], spot[2]);
    window.__victim = a.id;
  });
  await sleep(400);
  const victim = await page.evaluate(() => window.__victim);
  for (let k = 0; k < 8; k++) {
    await fireAt(page, victim);
    await sleep(220);
  }
  const dead = await waitFor(page, (id) => {
    const a = window.__structvox.actorWorld().actors.find((x) => x.id === id);
    return a && !a.char.alive ? { own: a.char.ownsModel, voxels: a.char.model.voxelCount } : null;
  }, victim, 8000, 'player rounds kill the soldier');
  if (dead) check(dead.own, `the soldier is wounded (own model, ${dead.voxels} voxels left) and dead (ragdoll)`);
  await sleep(1200);
  await page.screenshot({ path: `${outDir}/02-kill.png` });
  // 4. a rocket into a group: gibs and blood
  const gibbed = await page.evaluate(() => {
    const sv = window.__structvox;
    const w = sv.actorWorld();
    const p = sv.state().player;
    const ids = [];
    for (let i = 0; i < 3; i++) ids.push(sv.spawnAt('civilian', p[0] + 6 + (i - 1) * 0.6, p[1], p[2], Math.PI));
    return ids;
  });
  await sleep(600);
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.select('rocket');
    sv.look(0, -8);
    sv.fire();
  });
  const gore = await waitFor(page, () => {
    const s = window.__structvox.actorWorld().stats();
    return s.gibs >= 6 && s.stains > 0 ? s : null;
  }, null, 6000, 'a rocket gibs the group');
  if (gore) check(gore.gibs >= 6, `rocket: ${gore.gibs} gibs, ${gore.stains} blood stains (${gibbed.length} targets)`);
  await sleep(800);
  await page.screenshot({ path: `${outDir}/03-rocket.png` });
  // 5. the retro presentation (baked frames)
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.characters({ ai: true, style: 'retro' });
    sv.spawn('soldier', 3, 4, 10);
  });
  const retro = await waitFor(page, () => {
    const w = window.__structvox.actorWorld();
    const n = w.actors.filter((a) => a.char.retroFrame).length;
    return n > 0 && w.stats().bakes === 0 ? n : null;
  }, null, 20000, 'retro frames are baked and drawn');
  if (retro) check(retro > 0, `retro: ${retro} characters drawn from baked frames`);
  await page.screenshot({ path: `${outDir}/04-retro.png` });
  const fps = await page.evaluate(() => window.__structvox.state().fps);
  check(fps > 20, `frame rate ${fps.toFixed(0)} fps`);

  // 6. city life without soldiers: conversations, benches, a brawl that ends in a knockout
  await page.goto(`${base}?engine=wasm&world=city&seed=4&god=1&civilians=30&soldiers=0&thugs=0`, { waitUntil: 'load' });
  await waitFor(page, () => (window.__structvox?.actorWorld?.().stats().civilians ?? 0) >= 20, null, 60000, 'civilians populate the city');
  await page.evaluate(() => window.__structvox.noclip(true)); // a spectator frightens nobody
  const life = await waitFor(page, () => {
    const w = window.__structvox.actorWorld();
    const s = w.stats();
    return s.talking >= 2 && s.sitting >= 1 ? { talking: s.talking, sitting: s.sitting, ground: w.actors.filter((a) => a.char.animator.stance === 'ground').length } : null;
  }, null, 60000, 'civilians talk and sit on benches');
  if (life) check(true, `city life: ${life.talking} talking, ${life.sitting} on benches, ${life.ground} on the ground`);
  await page.screenshot({ path: `${outDir}/05-city-life.png` });
  const pair = await page.evaluate(() => {
    const sv = window.__structvox;
    const p = sv.state().player;
    const a = sv.spawnAt('civilian', p[0] + 3, p[1], p[2], Math.PI);
    const b = sv.spawnAt('civilian', p[0] + 4, p[1], p[2], 0);
    // bare fists (a spawned civilian may carry a knife)
    for (const x of sv.actorWorld().actors) if (x.id === a || x.id === b) x.weapon = null;
    return sv.brawl(a, b) ? [a, b] : null;
  });
  check(!!pair, 'a brawl starts');
  const blows = pair && await waitFor(page, (ids) => {
    const w = window.__structvox.actorWorld();
    const [a, b] = ids.map((id) => w.actors.find((x) => x.id === id));
    return a && b && a.char.health < a.char.maxHealth && b.char.health < b.char.maxHealth ? { a: a.char.health, b: b.char.health } : null;
  }, pair, 20000, 'brawl blows land on both');
  if (blows) check(true, `brawl: both hurt (${blows.a.toFixed(0)} / ${blows.b.toFixed(0)} health)`);
  const over = pair && await waitFor(page, (ids) => {
    const w = window.__structvox.actorWorld();
    const [a, b] = ids.map((id) => w.actors.find((x) => x.id === id));
    return w.stats().fights === 0 ? { a: a.brain.state, b: b.brain.state, ko: a.char.knockedOut || b.char.knockedOut, dead: !a.char.alive || !b.char.alive } : null;
  }, pair, 45000, 'the brawl ends');
  if (over) check(!over.dead, `the brawl ends (${over.a} / ${over.b}${over.ko ? ', knockout' : ''}), nobody beaten to death`);

  // 7. thugs and knives: a thug goes for a civilian; the player's knife cuts
  const thugFight = await page.evaluate(() => {
    const sv = window.__structvox;
    const p = sv.state().player;
    const w = sv.actorWorld();
    const victim = sv.spawnAt('civilian', p[0] + 4, p[1] + 2, p[2], 0);
    const thug = sv.spawnAt('thug', p[0] + 9, p[1] + 2, p[2], Math.PI);
    w.actors.find((x) => x.id === thug).weapon = w.cast.props.knife;
    return { victim, thug };
  });
  const attacked = await waitFor(page, (ids) => {
    const w = window.__structvox.actorWorld();
    const v = w.actors.find((x) => x.id === ids.victim);
    const t = w.actors.find((x) => x.id === ids.thug);
    return v && t && (v.char.health < v.char.maxHealth || w.stats().fights > 0) ? { state: t.brain.state, knife: t.char.weapon?.kind ?? null } : null;
  }, thugFight, 30000, 'a thug attacks a civilian');
  if (attacked) check(true, `a thug attacks a civilian (${attacked.state}${attacked.knife ? ', knife out' : ''})`);
  const knifed = await page.evaluate(async () => {
    const sv = window.__structvox;
    const w = sv.actorWorld();
    sv.characters({ ai: false });
    const p = sv.state().player;
    const id = sv.spawnAt('civilian', p[0] + 1.2, p[1], p[2], Math.PI);
    await new Promise((r) => setTimeout(r, 400));
    const a = w.actors.find((x) => x.id === id);
    const e = w.player.eye();
    const c = a.char.pose.p[3];
    sv.look((Math.atan2(c[1] - e[1], c[0] - e[0]) * 180) / Math.PI, (Math.atan2(c[2] - e[2], Math.hypot(c[0] - e[0], c[1] - e[1])) * 180) / Math.PI);
    sv.select('knife');
    const hp0 = a.char.health;
    sv.fire();
    await new Promise((r) => setTimeout(r, 300));
    sv.select('pistol');
    sv.characters({ ai: true });
    return { hp0, hp: a.char.health };
  });
  check(knifed.hp < knifed.hp0, `the player's knife cuts (${knifed.hp0} -> ${Math.round(knifed.hp)} health)`);

  // 8. Freedoom MAP01 with characters
  const wad = resolve('../data/freedoom/freedoom2.wad');
  if (existsSync(wad)) {
    await page.goto(`${base}?engine=wasm&world=rooms&god=1`, { waitUntil: 'load' });
    await waitFor(page, () => window.__structvox?.state().ready, null, 60000, 'rooms ready');
    const input = await page.$('input[type=file]');
    await input.uploadFile(wad);
    await page.evaluate(() => [...document.querySelectorAll('button')].find((b) => b.textContent === 'Load WAD').click());
    await waitFor(page, () => window.__structvox.state().ready && window.__structvox.state().engine?.voxels > 1e7, null, 120000, 'MAP01 loads');
    const doom = await waitFor(page, () => {
      const s = window.__structvox.actorWorld().stats();
      return s.civilians + s.soldiers > 0 ? s : null;
    }, null, 30000, 'characters populate MAP01');
    if (doom) check(doom.civilians + doom.soldiers >= 4, `MAP01: ${doom.civilians} civilians, ${doom.soldiers} soldiers`);
    await sleep(2000);
    await page.screenshot({ path: `${outDir}/07-map01.png` });
  } else console.log('skip Freedoom (run scripts/fetch_freedoom.sh)');

  // 9. the animation lab (no engine)
  const lab = await browser.newPage();
  watch(lab);
  await lab.goto(`${base}lab.html`, { waitUntil: 'load' });
  const labOk = await waitFor(lab, () => (window.__lab && window.__lab.actors.length > 0 ? window.__lab.actors.length : null), null, 30000, 'lab loads');
  if (labOk) {
    await lab.evaluate(() => {
      const l = window.__lab;
      l.shoot([0, -3, 1.3], [0, 1, 0]);
      l.rocket([-8, -3, 0]);
    });
    await sleep(1500);
    const s = await lab.evaluate(() => ({ dead: window.__lab.actors.filter((a) => !a.char.alive).length, gibs: window.__lab.gibs.gibs.length }));
    check(s.dead > 0 || s.gibs > 0, `lab: ${labOk} characters, shooting and rockets (${s.dead} dead, ${s.gibs} gibs)`);
    await lab.screenshot({ path: `${outDir}/08-lab.png` });
  }
  check(errors.length === 0, `no console/page/WebGPU errors${errors.length ? `: ${errors.slice(0, 3).join(' | ')}` : ''}`);
} catch (e) {
  check(false, `exception: ${e?.stack ?? e}`);
} finally {
  await browser.close();
}
console.log(failures.length ? `SMOKE FAILED (${failures.length})` : 'SMOKE PASSED');
process.exitCode = failures.length ? 1 : 0;
