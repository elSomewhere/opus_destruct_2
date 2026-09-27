/**
 * Typed plumbing for engine worker entry points (mock-worker.ts, wasm-worker.ts).
 *
 * - `postToMain` posts a protocol message and transfers its ArrayBuffers (zero copy).
 * - `serveCommands` validates incoming messages and reports handler failures to the main
 *   thread as `error` messages instead of letting them vanish in the worker console.
 */
import type { EngineCommand, EngineCommandType, WorkerMessage } from '../engine/protocol.ts';
import { isEngineCommand, workerMessageTransferables } from '../engine/protocol.ts';

export function postToMain(msg: WorkerMessage): void {
  postMessage(msg, workerMessageTransferables(msg));
}

export function reportError(err: unknown, command?: EngineCommandType, fatal = false): void {
  const message = err instanceof Error ? err.message : String(err);
  console.error(`[engine] ${command ?? 'worker'} failed:`, err);
  postToMain(command === undefined ? { type: 'error', message, fatal } : { type: 'error', message, fatal, command });
}

export function serveCommands(handler: (cmd: EngineCommand) => void | Promise<void>): void {
  addEventListener('message', (ev: MessageEvent<unknown>) => {
    const data = ev.data;
    if (!isEngineCommand(data)) {
      reportError(new Error(`unknown command: ${JSON.stringify(data)?.slice(0, 80)}`));
      return;
    }
    try {
      const r = handler(data);
      if (r instanceof Promise) r.catch((err: unknown) => reportError(err, data.type));
    } catch (err) {
      reportError(err, data.type);
    }
  });
}
