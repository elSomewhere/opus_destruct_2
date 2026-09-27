// Types for the generated Emscripten module (web/src/wasm/svx_web.js). The factory resolves to
// the module object; the worker narrows it to its own SvxModule interface.
declare function createSvxModule(options?: Record<string, unknown>): Promise<unknown>;
export default createSvxModule;
