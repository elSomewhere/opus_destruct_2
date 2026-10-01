/**
 * Generic named registries. The framework's extension points are all
 * registries: districts (land-use zones), building archetypes, style
 * profiles, prop prefabs and feature sources. A new city style, a military
 * zone or a biome is added by registering entries — no core edits needed.
 */
export class Registry {
  constructor(kind) {
    this.kind = kind;
    this.entries = new Map();
  }

  register(def) {
    if (!def?.id) throw new Error(`${this.kind}: entry needs an id`);
    this.entries.set(def.id, Object.freeze({ ...def }));
    return def;
  }

  get(id) {
    const e = this.entries.get(id);
    if (!e) throw new Error(`${this.kind}: unknown "${id}"`);
    return e;
  }

  maybe(id) {
    return this.entries.get(id) ?? null;
  }

  has(id) {
    return this.entries.has(id);
  }

  all() {
    return [...this.entries.values()];
  }
}

export const DISTRICTS = new Registry("district");
export const ARCHETYPES = new Registry("archetype");
export const STYLES = new Registry("style");
export const PREFABS = new Registry("prefab");
export const BIOMES = new Registry("biome");
