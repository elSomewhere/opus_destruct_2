/**
 * Displacement fields of running physics bubbles (protocol `ChunkMeshesMessage.fields`): one
 * rgba16float 3D texture per bubble, sampled trilinearly in the world vertex shader, so chunks
 * inside a bubble move every tick without being re-meshed. Up to MAX_FIELDS at once (the engine
 * runs at most 4 bubbles); unused slots bind a 1x1x1 empty texture.
 */
import type { DisplacementField, Vec3 } from '../engine/protocol.ts';

export const MAX_FIELDS = 4;
/** Floats in the field uniform: per field (origin.xyz, 1/h) and (size.xyz, active). */
export const FIELD_UNIFORM_FLOATS = MAX_FIELDS * 8;

interface Slot {
  id: number;
  texture: GPUTexture;
  size: [number, number, number];
  origin: Vec3;
  voxelSize: number;
  maxDisp: number;
}

export class FieldStore {
  private readonly device: GPUDevice;
  private readonly empty: GPUTexture;
  private slots: Slot[] = [];
  /** Bumped whenever a texture object changes (the bind group must be rebuilt). */
  version = 0;
  readonly uniform = new Float32Array(FIELD_UNIFORM_FLOATS);

  constructor(device: GPUDevice) {
    this.device = device;
    this.empty = device.createTexture({
      label: 'empty displacement field',
      size: [1, 1, 1],
      dimension: '3d',
      format: 'rgba16float',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
  }

  get count(): number {
    return this.slots.length;
  }

  /** Largest displacement bound of the current fields (m; 0 without fields). */
  get maxDisp(): number {
    return this.slots.reduce((m, s) => Math.max(m, s.maxDisp), 0);
  }

  /** Texture views for the MAX_FIELDS bindings. */
  views(): GPUTextureView[] {
    const out: GPUTextureView[] = [];
    for (let k = 0; k < MAX_FIELDS; k++) out.push((this.slots[k]?.texture ?? this.empty).createView({ dimension: '3d' }));
    return out;
  }

  /** Replaces the set of fields (the complete current set from the engine). */
  set(fields: readonly DisplacementField[]): void {
    const next: Slot[] = [];
    const old = new Map<number, Slot>();
    for (const s of this.slots) old.set(s.id, s);
    for (const f of fields.slice(0, MAX_FIELDS)) {
      const [nx, ny, nz] = f.size;
      if (nx <= 0 || ny <= 0 || nz <= 0 || f.data.byteLength < nx * ny * nz * 8) continue;
      let slot = old.get(f.id);
      if (slot && (slot.size[0] !== nx || slot.size[1] !== ny || slot.size[2] !== nz)) {
        slot.texture.destroy();
        this.version++;
        slot = undefined;
      }
      if (!slot) {
        slot = {
          id: f.id,
          texture: this.device.createTexture({
            label: `displacement field ${f.id}`,
            size: [nx, ny, nz],
            dimension: '3d',
            format: 'rgba16float',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
          }),
          size: [nx, ny, nz],
          origin: f.origin,
          voxelSize: f.voxelSize,
          maxDisp: f.maxDisp,
        };
        this.version++;
      }
      old.delete(f.id);
      this.device.queue.writeTexture({ texture: slot.texture }, f.data, { bytesPerRow: nx * 8, rowsPerImage: ny }, [nx, ny, nz]);
      slot.origin = f.origin;
      slot.voxelSize = f.voxelSize;
      slot.maxDisp = f.maxDisp;
      next.push(slot);
    }
    for (const s of old.values()) {
      s.texture.destroy();
      this.version++;
    }
    // slot order must match the bindings: keep the new order and re-bind when it changes
    if (next.length !== this.slots.length || next.some((s, i) => s !== this.slots[i])) this.version++;
    this.slots = next;
    const u = this.uniform;
    u.fill(0);
    next.forEach((s, k) => {
      u.set([s.origin[0], s.origin[1], s.origin[2], 1 / s.voxelSize], 8 * k);
      u.set([s.size[0], s.size[1], s.size[2], 1], 8 * k + 4);
    });
  }

  clear(): void {
    this.set([]);
  }

  /** How far geometry inside the box [min, max] may move (0 outside every field). */
  inflation(min: Vec3, max: Vec3): number {
    let r = 0;
    for (const s of this.slots) {
      const h = s.voxelSize;
      let overlaps = true;
      for (let a = 0; a < 3 && overlaps; a++) {
        const lo = s.origin[a]! - 0.5 * h;
        const hi = s.origin[a]! + (s.size[a]! - 0.5) * h;
        if (max[a]! < lo || min[a]! > hi) overlaps = false;
      }
      if (overlaps) r = Math.max(r, s.maxDisp);
    }
    return r;
  }
}
