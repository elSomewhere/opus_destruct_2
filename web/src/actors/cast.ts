/**
 * The character assets of the game: voxel models (a few geometries per faction, many looks
 * through palettes), the rifle, their GPU meshes, and retro frame sets (baked on demand, one
 * per frame of the game loop so switching to retro never stalls).
 */
import {
  bakeRetroSet,
  civilianPalette,
  makeCivilian,
  makeRifle,
  makeSoldier,
  meshPart,
  ModelMesher,
  soldierPalette,
  type Palette,
  type Prop,
  type RetroSet,
  type VoxelModel,
  type VoxelPart,
} from 'svx-anim';
import type { CharacterRenderer, GpuCharacterMesh } from '../render/characters.ts';

export type Faction = 'civilian' | 'soldier';

export interface Look {
  model: VoxelModel;
  palette: Palette;
  paletteId: number;
  faction: Faction;
  geometry: number;
}

const SOLDIER_GEOMETRIES = 4;
const CIVILIAN_GEOMETRIES = 10;

export class Cast {
  readonly renderer: CharacterRenderer;
  readonly rifle: Prop;
  readonly rifleMesh: GpuCharacterMesh;
  private readonly soldiers: VoxelModel[] = [];
  private readonly civilians: VoxelModel[] = [];
  private readonly meshes = new Map<VoxelModel, GpuCharacterMesh>();
  private readonly partMeshes = new Map<VoxelPart, GpuCharacterMesh>();
  private readonly retro = new Map<string, RetroSet>();
  private readonly retroQueue: { key: string; model: VoxelModel; weapon: Prop | null; voxelSize: number }[] = [];
  private readonly mesher = new ModelMesher();
  /** Squad camouflage scheme (all soldiers of a game share it). */
  scheme = 0;

  constructor(renderer: CharacterRenderer) {
    this.renderer = renderer;
    this.rifle = makeRifle();
    this.rifleMesh = renderer.uploadMesh(this.mesher.mesh(this.rifle.model), 'rifle');
  }

  /** A look for a new character (geometry built on first use). */
  look(faction: Faction, seed: number): Look {
    if (faction === 'soldier') {
      const g = seed % SOLDIER_GEOMETRIES;
      this.soldiers[g] ??= makeSoldier(g + 1).model;
      const palette = soldierPalette(seed, this.scheme);
      return { model: this.soldiers[g], palette, paletteId: this.renderer.palette(palette), faction, geometry: g };
    }
    const g = seed % CIVILIAN_GEOMETRIES;
    this.civilians[g] ??= makeCivilian(g + 1).model;
    const palette = civilianPalette(seed * 7 + 1);
    return { model: this.civilians[g], palette, paletteId: this.renderer.palette(palette), faction, geometry: g };
  }

  /** GPU mesh of a shared model or a baked retro frame (cached by model). */
  mesh(model: VoxelModel): GpuCharacterMesh {
    let m = this.meshes.get(model);
    if (!m) {
      m = this.renderer.uploadMesh(this.mesher.mesh(model), model.name);
      this.meshes.set(model, m);
    }
    return m;
  }

  /** GPU mesh of one loose part (gibs); released with releasePart. */
  partMesh(part: VoxelPart, voxelSize: number): GpuCharacterMesh {
    let m = this.partMeshes.get(part);
    if (!m) {
      m = this.renderer.uploadMesh(meshPart(part, voxelSize), 'gib');
      this.partMeshes.set(part, m);
    }
    return m;
  }

  releasePart(part: VoxelPart): void {
    const m = this.partMeshes.get(part);
    if (m && part !== this.rifle.model.parts[0]) {
      this.renderer.releaseMesh(m);
      this.partMeshes.delete(part);
    }
  }

  /** The retro frames of a model (null while baking is pending; queued on first request). */
  retroSet(model: VoxelModel, weapon: Prop | null, voxelSize: number): RetroSet | null {
    const key = `${model.name}|${weapon ? 'armed' : 'bare'}|${voxelSize}`;
    const have = this.retro.get(key);
    if (have) return have;
    if (!this.retroQueue.some((q) => q.key === key)) this.retroQueue.push({ key, model, weapon, voxelSize });
    return null;
  }

  /** Bakes at most one queued retro set (call once per frame). */
  pump(): boolean {
    const q = this.retroQueue.shift();
    if (!q) return false;
    this.retro.set(q.key, bakeRetroSet(q.model, { weapon: q.weapon, voxelSize: q.voxelSize }));
    return true;
  }

  get pendingBakes(): number {
    return this.retroQueue.length;
  }
}
