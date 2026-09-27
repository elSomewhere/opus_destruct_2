/**
 * Mock engine worker: serves the engine protocol (docs/API.md) with the pure-TypeScript
 * MockEngine. Selected with ?engine=mock (the default).
 */
import { postToMain, reportError, serveCommands } from './host.ts';
import { MockEngine } from './mock/engine.ts';

const TICK_MS = 1000 / 60;

const engine = new MockEngine(postToMain);
serveCommands((cmd) => engine.handle(cmd));

// The worker owns the simulation clock: a fixed-rate tick, independent of rendering.
let next = performance.now();
function loop(): void {
  const now = performance.now();
  try {
    engine.tick(now);
  } catch (err) {
    reportError(err);
  }
  next += TICK_MS;
  if (next < now) next = now + TICK_MS; // fell behind: skip, don't burst
  setTimeout(loop, Math.max(0, next - performance.now()));
}
loop();
