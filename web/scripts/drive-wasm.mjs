#!/usr/bin/env node
/**
 * Browser drive through the endless city (?engine=wasm&world=drive; docs/VEHICLES.md): waits for
 * the city and its traffic, takes the wheel of the nearest car, drives it (scripted controls
 * through the debug handle), slides it with the handbrake, rams a van dropped in its way, gets
 * out. Screenshots of each; fails on console / page / WebGPU errors, or when the car does not
 * drive, crumple or let the player out. Usage (dev server running, e.g. `npm run dev -- --port 5190`):
 *   node scripts/drive-wasm.mjs [baseUrl] [outDir]
 * Env: CHROME_PATH (browser binary), SMOKE_HEADFUL=1 (show the window).
 */
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'drive-out');
mkdirSync(outDir, { recursive: true });
const chrome = process.env.CHROME_PATH ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const failures = [];
const check = (cond, what) => {
  console.log(cond ? `ok   ${what}` : `FAIL ${what}`);
  if (!cond) failures.push(what);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
/**
 * In a container (root, no GPU): Chrome runs only without its sandbox, and WebGPU on SwiftShader
 * over Vulkan (Dawn's default there loses its device at once). SMOKE_SWIFTSHADER=0 turns it off.
 */
function containerArgs() {
  if (process.platform !== 'linux' || process.env.SMOKE_SWIFTSHADER === '0') return [];
  const root = process.getuid?.() === 0;
  if (!root && process.env.SMOKE_SWIFTSHADER !== '1') return [];
  return [
    ...(root ? ['--no-sandbox'] : []),
    '--use-angle=swiftshader',
    '--enable-features=Vulkan,UseSkiaRenderer',
    '--use-vulkan=swiftshader',
    '--disable-vulkan-fallback-to-gl-for-testing',
  ];
}
const browser = await puppeteer.launch({
  executablePath: chrome,
  headless: process.env.SMOKE_HEADFUL ? false : true,
  args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--no-first-run', '--window-size=960,580', ...containerArgs()],
  defaultViewport: { width: 960, height: 540, deviceScaleFactor: 1 },
  protocolTimeout: 240000,
});
const errors = [];
try {
  const page = await browser.newPage();
  page.on('console', (m) => {
    const t = m.text();
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl/.test(t)) errors.push(t);
    if (process.env.SMOKE_VERBOSE) console.log(`  [page] ${t}`);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message}`));
  await page.goto(`${base}?engine=wasm&world=drive&seed=1`, { waitUntil: 'load' });
  // (software WebGPU: fewer pixels, or the page falls far behind the engine)
  if (containerArgs().length > 0) {
    while (!(await page.evaluate(() => !!window.__structvox))) await sleep(200);
    await page.evaluate(() => {
      window.__structvox.renderer.renderScale = 0.5;
    });
  }
  const t0 = Date.now();
  for (;;) {
    const ok = await page.evaluate(() => {
      const s = window.__structvox?.state();
      return !!s && s.ready && s.engine !== null && s.engine.residentChunks > 50 && s.render !== null && s.render.chunksDrawn > 0;
    });
    if (ok) break;
    if (Date.now() - t0 > 120000) throw new Error('the drive city did not load');
    await sleep(250);
  }
  check(true, `drive city loaded in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  const shot = async (name) => page.screenshot({ path: `${outDir}/${name}.png` });
  const state = () => page.evaluate(() => window.__structvox.state());
  const vehicles = () => page.evaluate(() => window.__structvox.vehicles());
  // traffic comes
  const t1 = Date.now();
  let vs = [];
  while (Date.now() - t1 < 60000) {
    vs = await vehicles();
    if (vs.filter((v) => v.chassis > 0).length >= 6) break;
    await sleep(500);
  }
  check(vs.length >= 6, `traffic: ${vs.length} vehicles (${vs.filter((v) => v.flags & 4).length} parked, ${vs.filter((v) => Math.abs(v.speed) > 2).length} moving)`);
  await page.evaluate(() => window.__structvox.look(0, -8));
  await sleep(1500);
  await shot('drive-01-street');

  // a sedan dropped on the road beside the player, taken
  const p0 = (await state()).player;
  // (the avenue's lanes are to the player's left as they spawn, looking along +x)
  await page.evaluate((p) => window.__structvox.spawnVehicle(1, 4, p[0] + 3, p[1] + 5, p[2], 0), p0);
  await sleep(1500);
  // (the car dropped in: the nearest, not one of the traffic's)
  const near = (await vehicles())
    .map((v) => ({ id: v.id, kind: v.kind, flags: v.flags, d: Math.hypot(v.pos[0] - p0[0] - 3, v.pos[1] - p0[1] - 5) }))
    .sort((a, b) => a.d - b.d)
    .slice(0, 3);
  console.log(`dropped at ${[p0[0] + 3, p0[1] + 5].map((x) => x.toFixed(1))}; nearest: ${near.map((v) => `${v.id} ${v.kind} f${v.flags} ${v.d.toFixed(1)} m`).join(', ')}`);
  const entered = await page.evaluate(() => window.__structvox.enterVehicle());
  check(entered, 'took the wheel of the nearest car');
  let s = await state();
  // (the engine's word comes with its next vehicles message: a slow page takes a while)
  for (let k = 0; k < 40 && s.engineDriving !== s.driving; k++) {
    await sleep(250);
    s = await state();
  }
  check(s.driving !== 0 && s.engineDriving === s.driving, `driving vehicle ${s.driving} (engine: ${s.engineDriving})`);
  const mine = () => vehicles().then((l) => l.find((v) => v.id === s.driving));
  let car = await mine();
  console.log(`car: ${JSON.stringify(car)}`);
  await shot('drive-02-chase');

  // full throttle down the road (a slow page - software WebGPU - answers late: many engine
  // seconds may pass between two looks, and the car may well have met something by the last;
  // what counts is that it drove)
  await page.evaluate(() => window.__structvox.drive(1, 0));
  let top = 0;
  for (let k = 0; k < 9; k++) {
    await sleep(500);
    car = await mine();
    if (car) top = Math.max(top, car.speed);
    const e = (await state()).engine;
    console.log(`  ${(k + 1) * 0.5} s: ${car ? `${car.speed.toFixed(1)} m/s, gear ${car.gear}, ${car.rpm.toFixed(0)} rpm, x ${car.pos[0].toFixed(1)}, wheels ${car.wheels}, damage ${car.damage.toFixed(2)}` : 'gone'} (tick ${e?.ticks})`);
  }
  check(car !== undefined && top > 3, `the car drives (up to ${top.toFixed(1)} m/s, now ${car?.speed.toFixed(1)})`);
  console.log(`camera: ${JSON.stringify((await state()).cameraDistance)}`);
  await shot('drive-03-speed');
  // a slide: steer and the handbrake - from a town speed (at full speed a spin may well end in
  // the parked cars, which is a crash, not a slide)
  await page.evaluate(() => window.__structvox.drive(0, 1, true));
  for (let k = 0; k < 40; k++) {
    await sleep(100);
    car = await mine();
    if (!car || car.speed < 15) break;
  }
  await page.evaluate(() => window.__structvox.drive(0.4, 1, true));
  for (let k = 0; k < 7; k++) {
    await sleep(200);
    const w = (await state()).wheels;
    console.log(`  sliding: ${w.map((x) => `${x.contact ? 'c' : '-'} ${x.slip.toFixed(1)} m/s on ${x.material}`).join(' | ')}`);
  }
  await shot('drive-04-slide');
  await page.evaluate(() => window.__structvox.drive(0, 0, true, 1));
  await sleep(2500);
  s = await state();
  console.log(`render: ${JSON.stringify({ wheels: s.render.wheels, skids: s.render.skidMarks, particles: s.render.particles, fps: s.fps.toFixed(0) })}`);
  check(s.render.wheels >= 4, `wheels drawn (${s.render.wheels})`);
  check(s.render.skidMarks > 0, `skid marks laid (${s.render.skidMarks})`);
  await page.evaluate(() => window.__structvox.camera('far'));
  await sleep(600);
  await shot('drive-05-marks');
  await page.evaluate(() => window.__structvox.camera('chase'));

  // a van dropped 30 m ahead, rammed
  car = await mine();
  if (!car) throw new Error('the car is gone');
  const yaw = (car.yaw * Math.PI) / 180;
  const at = [car.pos[0] + Math.cos(yaw) * 30, car.pos[1] + Math.sin(yaw) * 30, car.pos[2] - 0.6];
  await page.evaluate((a, y) => window.__structvox.spawnVehicle(2, 1, a[0], a[1], a[2], y + 90), at, car.yaw);
  await sleep(1200);
  const damage0 = car.damage;
  // (the van: the one nearest where it was dropped)
  const van = (await vehicles())
    .filter((v) => v.kind === 'van')
    .map((v) => ({ id: v.id, d: Math.hypot(v.pos[0] - at[0], v.pos[1] - at[1]) }))
    .sort((a, b) => a.d - b.d)[0]?.id;
  const vanDamage = async () => (await vehicles()).find((v) => v.id === van)?.damage ?? 0;
  await page.evaluate(() => window.__structvox.drive(1, 0));
  let hit = false;
  for (let k = 0; k < 60 && !hit; k++) {
    await sleep(100);
    car = await mine();
    if (car && (car.damage > damage0 + 0.02 || (await vanDamage()) > 0.02)) hit = true;
    if (k === 30) await shot('drive-06-approach');
  }
  await sleep(300);
  await shot('drive-07-crash');
  await page.evaluate(() => window.__structvox.drive(0, 0, true, 1));
  await sleep(2000);
  car = await mine();
  // (a car wrecked before - damage 1 - crumples still: the van shows it)
  const vd = await vanDamage();
  check(car !== undefined && (car.damage > damage0 || vd > 0), `the crash crumpled it (damage ${damage0.toFixed(2)} -> ${car?.damage.toFixed(2)}, the van ${vd.toFixed(2)})`);
  await page.evaluate(() => window.__structvox.camera('far'));
  await sleep(500);
  await shot('drive-08-wreck');

  // out
  await page.evaluate(() => window.__structvox.exitVehicle());
  s = await state();
  for (let k = 0; k < 40 && (s.driving !== 0 || s.engineDriving !== 0); k++) {
    await sleep(250);
    s = await state();
  }
  check(s.driving === 0 && s.engineDriving === 0, 'got out');
  await page.evaluate(() => window.__structvox.look(-160, -10));
  await sleep(800);
  await shot('drive-09-onfoot');
  const e = s.engine;
  console.log(`engine: tick ${e.tickMs.toFixed(2)} ms, rigid ${e.rigidMs} ms, stream ${e.streamMs} ms, ${e.pieces} pieces, ${e.residentChunks} chunks resident, fps ${s.fps.toFixed(0)}`);
  check(errors.length === 0, `no console errors${errors.length ? `: ${errors.slice(0, 3).join(' | ')}` : ''}`);
} catch (e) {
  check(false, String(e));
} finally {
  await browser.close();
}
process.exit(failures.length ? 1 : 0);
