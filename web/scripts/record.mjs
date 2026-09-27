#!/usr/bin/env node
/**
 * Records a short video of a page (headless Chrome, WebGPU): opens `url`, waits until the
 * `ready` expression is truthy, runs the `action` script in the page, captures frames with the
 * DevTools screencast for `seconds`, and encodes them with ffmpeg (H.264, `fps` frames/s).
 *
 * usage: node scripts/record.mjs --url URL --ready EXPR --action FILE.js --seconds S --out OUT.mp4
 *        [--fps 30] [--width 960] [--height 540] [--settle MS]
 * The action file is evaluated in the page (an async function body: `await` works).
 */
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const puppeteer = (await import('puppeteer-core')).default;
const arg = (k, d) => {
  const i = process.argv.indexOf(`--${k}`);
  return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : d;
};
const url = arg('url');
const ready = arg('ready', 'true');
const actionFile = arg('action');
const seconds = Number(arg('seconds', '4'));
const out = arg('out', 'out.mp4');
const fps = Number(arg('fps', '30'));
const width = Number(arg('width', '960'));
const height = Number(arg('height', '540'));
const settle = Number(arg('settle', '1500'));
if (!url || !actionFile) {
  console.error('usage: record.mjs --url URL --ready EXPR --action FILE.js --seconds S --out OUT.mp4');
  process.exit(2);
}
const action = readFileSync(actionFile, 'utf8');
const chrome = process.env.CHROME_PATH ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const browser = await puppeteer.launch({
  executablePath: chrome,
  headless: true,
  args: ['--enable-unsafe-webgpu', '--enable-features=WebGPU', '--ignore-gpu-blocklist', '--no-first-run',
         `--window-size=${width},${height + 40}`],
  defaultViewport: { width, height, deviceScaleFactor: 1 },
});
const dir = mkdtempSync(join(tmpdir(), 'svx-record-'));
try {
  const page = await browser.newPage();
  page.on('pageerror', (e) => console.error(`pageerror ${e.message}`));
  await page.goto(url, { waitUntil: 'load' });
  const t0 = Date.now();
  while (!(await page.evaluate(`!!(${ready})`).catch(() => false))) {
    if (Date.now() - t0 > 120000) throw new Error(`not ready: ${ready}`);
    await new Promise((r) => setTimeout(r, 200));
  }
  await new Promise((r) => setTimeout(r, settle));
  // screencast frames arrive when the page repaints; keep their timestamps and resample to
  // a constant frame rate (the last frame repeats until the next one)
  const cdp = await page.createCDPSession();
  const frames = [];
  cdp.on('Page.screencastFrame', async (f) => {
    frames.push({ t: f.metadata.timestamp, data: f.data });
    await cdp.send('Page.screencastFrameAck', { sessionId: f.sessionId }).catch(() => {});
  });
  await cdp.send('Page.startScreencast', { format: 'jpeg', quality: 85, maxWidth: width, maxHeight: height, everyNthFrame: 1 });
  await new Promise((r) => setTimeout(r, 300));
  const actionDone = page.evaluate(`(async () => { ${action} })()`);
  await new Promise((r) => setTimeout(r, seconds * 1000));
  await actionDone.catch((e) => console.error(`action: ${e}`));
  await cdp.send('Page.stopScreencast');
  if (frames.length === 0) throw new Error('no frames captured');
  const tStart = frames[0].t, n = Math.round(seconds * fps);
  let j = 0;
  for (let i = 0; i < n; i++) {
    const t = tStart + i / fps;
    while (j + 1 < frames.length && frames[j + 1].t <= t) j++;
    writeFileSync(join(dir, `f${String(i).padStart(5, '0')}.jpg`), Buffer.from(frames[j].data, 'base64'));
  }
  const r = spawnSync('ffmpeg', ['-y', '-loglevel', 'error', '-framerate', String(fps), '-i', join(dir, 'f%05d.jpg'),
                                 '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '23', out], { stdio: 'inherit' });
  if (r.status !== 0) throw new Error('ffmpeg failed');
  console.log(`${out}: ${n} frames at ${fps} fps from ${frames.length} screencast frames`);
} finally {
  await browser.close();
  rmSync(dir, { recursive: true, force: true });
}
