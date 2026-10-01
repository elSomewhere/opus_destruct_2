/**
 * The browser checks' shared launch (scripts/*-wasm.mjs): Chrome with WebGPU at CHROME_PATH.
 *
 * In a container (root, no GPU) - or anywhere with SMOKE_SWIFTSHADER=1, as on a GPU-less CI
 * runner - Chrome runs without its sandbox (root only) and WebGPU in software: SwiftShader over
 * Vulkan (Dawn's default there loses its device at once). SMOKE_SWIFTSHADER=0 turns that off. A
 * software page is slow - the GPU process takes most of a small machine's cores - so the engine
 * gets two threads (beforeLoad) and the renderer fewer pixels and a frame in six (afterLoad), or
 * the page falls minutes behind.
 */
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export function chromePath() {
  return process.env.CHROME_PATH ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
}

/** Chrome's extra arguments for software WebGPU (none with a GPU). */
export function containerArgs() {
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

/** Is WebGPU in software (the page slow)? */
export function softwareWebGPU() {
  return containerArgs().length > 0;
}

/** Chrome, headless unless SMOKE_HEADFUL, with a page of width x height. */
export function launch(puppeteer, { width = 1280, height = 720 } = {}) {
  return puppeteer.launch({
    executablePath: chromePath(),
    headless: process.env.SMOKE_HEADFUL ? false : true,
    args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--no-first-run', `--window-size=${width},${height + 40}`, ...containerArgs()],
    defaultViewport: { width, height, deviceScaleFactor: 1 },
    protocolTimeout: 240000,
  });
}

/** Before the page loads: in software, the engine on two threads. */
export async function beforeLoad(page) {
  if (softwareWebGPU()) await page.evaluateOnNewDocument(() => Object.defineProperty(navigator, 'hardwareConcurrency', { get: () => 2 }));
}

/** Once it has: in software, the renderer at a third of the pixels, drawing a frame in six. */
export async function afterLoad(page) {
  if (!softwareWebGPU()) return;
  while (!(await page.evaluate(() => !!window.__structvox))) await sleep(200);
  await page.evaluate(() => {
    window.__structvox.renderer.renderScale = 0.35;
    window.__structvox.renderer.drawEvery = 6;
  });
}

/**
 * The world a check loads, as URL parameters: its own (`world=drive&seed=1`), or SMOKE_WORLD's -
 * e.g. `SMOKE_WORLD=preset=city/angledInfiniteCity` runs a check on the city preset.
 */
export function worldQuery(own) {
  return process.env.SMOKE_WORLD ?? own;
}
