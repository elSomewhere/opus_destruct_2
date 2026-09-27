/**
 * The character assets of the game: voxel models (a few geometries per faction, many looks
 * through palettes), movement styles, loadouts (rifles, SMGs, machine guns, pistols, knives;
 * some civilians carry a pistol or a knife), furniture to sit on, their GPU meshes, and retro
 * frame sets (baked on demand, one per frame of the game loop so switching to retro never
 * stalls).
 */
import {
  bakeRetroSet,
  civilianPalette,
  makeBench,
  makeCivilian,
  makeKnife,
  makeLmg,
  makePistol,
  makeRifle,
  makeSmg,
  makeSoldier,
  meshPart,
  ModelMesher,
  randomStyle,
  Rng,
  soldierPalette,
  type Furniture,
  type GaitStyle,
  type Palette,
  type Prop,
  type PropKind,
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
  female: boolean;
  style: GaitStyle;
}

/**
 * Per weapon: rounds per magazine, damage per round, seconds between rounds, spread (x the
 * brain's), and the radius (m) of the voxel sphere a round carves out of the world (world voxels
 * are 0.125 m: a round takes out about one).
 */
export const WEAPON_STATS: Record<PropKind, { mag: number; damage: number; interval: number; spread: number; carve: number }> = {
  rifle: { mag: 30, damage: 34, interval: 0.105, spread: 1, carve: 0.13 },
  smg: { mag: 30, damage: 24, interval: 0.075, spread: 1.25, carve: 0.11 },
  lmg: { mag: 100, damage: 34, interval: 0.085, spread: 1.5, carve: 0.14 },
  pistol: { mag: 15, damage: 26, interval: 0.28, spread: 1.3, carve: 0.11 },
  knife: { mag: 0, damage: 30, interval: 1, spread: 1, carve: 0 },
};

const SOLDIER_GEOMETRIES = 4;
const CIVILIAN_GEOMETRIES = 10;

export class Cast {
  readonly renderer: CharacterRenderer;
  readonly props: Record<PropKind, Prop>;
  private readonly soldiers: VoxelModel[] = [];
  private readonly civilians: { model: VoxelModel; female: boolean }[] = [];
  private readonly meshes = new Map<VoxelModel, GpuCharacterMesh>();
  private readonly partMeshes = new Map<VoxelPart, GpuCharacterMesh>();
  private readonly retro = new Map<string, RetroSet>();
  private readonly retroQueue: { key: string; model: VoxelModel; weapon: Prop | null; voxelSize: number }[] = [];
  private readonly mesher = new ModelMesher();
  readonly bench: Furniture;
  /** Squad camouflage scheme (all soldiers of a game share it). */
  scheme = 0;

  constructor(renderer: CharacterRenderer) {
    this.renderer = renderer;
    this.props = { rifle: makeRifle(), smg: makeSmg(), lmg: makeLmg(), pistol: makePistol(), knife: makeKnife() };
    this.bench = makeBench();
  }

  /** A look for a new character (geometry built on first use). */
  look(faction: Faction, seed: number): Look {
    if (faction === 'soldier') {
      const g = seed % SOLDIER_GEOMETRIES;
      this.soldiers[g] ??= makeSoldier(g + 1).model;
      const palette = soldierPalette(seed, this.scheme);
      return { model: this.soldiers[g], palette, paletteId: this.renderer.palette(palette), faction, geometry: g, female: false, style: randomStyle(seed, 'soldier') };
    }
    const g = seed % CIVILIAN_GEOMETRIES;
    if (!this.civilians[g]) {
      const v = makeCivilian(g + 1);
      this.civilians[g] = { model: v.model, female: v.spec.female };
    }
    const c = this.civilians[g]!;
    const palette = civilianPalette(seed * 7 + 1);
    return { model: c.model, palette, paletteId: this.renderer.palette(palette), faction, geometry: g, female: c.female, style: randomStyle(seed, c.female ? 'civilianFemale' : 'civilian') };
  }

  /** What a new character carries. */
  loadout(faction: Faction, seed: number): Prop | null {
    const r = new Rng(seed * 2654435761 + 99).next();
    if (faction === 'soldier') return this.props[r < 0.55 ? 'rifle' : r < 0.72 ? 'smg' : r < 0.86 ? 'lmg' : 'pistol'];
    return r < 0.12 ? this.props.pistol : r < 0.15 ? this.props.smg : r < 0.2 ? this.props.knife : null;
  }

  /** GPU mesh of a shared model, a prop or a baked retro frame (cached by model). */
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
    if (m) {
      this.renderer.releaseMesh(m);
      this.partMeshes.delete(part);
    }
  }

  /** Whether a part is one of the shared props (their meshes are never released). */
  isProp(part: VoxelPart): boolean {
    return Object.values(this.props).some((p) => p.model.parts[0] === part);
  }

  /** The retro frames of a model (null while baking is pending; queued on first request). */
  retroSet(model: VoxelModel, weapon: Prop | null, voxelSize: number): RetroSet | null {
    const key = `${model.name}|${weapon?.kind ?? 'bare'}|${voxelSize}`;
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
