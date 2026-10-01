/** Minimal LRU cache on top of Map insertion order. */
export class LRU {
  constructor(capacity = 64) {
    this.capacity = capacity;
    this.map = new Map();
  }

  get(key) {
    const v = this.map.get(key);
    if (v === undefined) return undefined;
    this.map.delete(key);
    this.map.set(key, v);
    return v;
  }

  has(key) {
    return this.map.has(key);
  }

  set(key, value) {
    if (this.map.has(key)) this.map.delete(key);
    this.map.set(key, value);
    while (this.map.size > this.capacity) {
      this.map.delete(this.map.keys().next().value);
    }
    return value;
  }

  getOrCreate(key, factory) {
    const v = this.get(key);
    if (v !== undefined) return v;
    return this.set(key, factory());
  }

  clear() {
    this.map.clear();
  }

  get size() {
    return this.map.size;
  }
}
