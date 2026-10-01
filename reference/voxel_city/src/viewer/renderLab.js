/** Fixed cameras used to compare renderer changes. Values are metres/radians. */
export const RENDER_LAB_VIEWS = [
  { id: "dense-street", label: "Dense street", note: "Eye-level downtown facades", preset: "angledInfiniteCity", seed: 1337, view: { x: 85.75, y: -19.5, z: null, yaw: -1.45, pitch: 0.04, mode: "walk" } },
  { id: "pitched-street", label: "Pitched street", note: "Planar road part against stepped ground", preset: "angledOldHarbourTown", seed: 1337, view: { x: -20.125, y: 183, z: 25.5, yaw: -0.6, pitch: 0.62, mode: "orbit", distance: 60 } },
  { id: "angled-junction", label: "Angled junction", note: "Slabs, kerbs and world-grid junction", preset: "angledOldHarbourTown", seed: 1337, view: { x: -20.125, y: 183, z: 25.5, yaw: 1.95, pitch: 0.35, mode: "orbit", distance: 32 } },
  { id: "turned-building", label: "Turned building", note: "Separate oriented building lattice", preset: "angledCities", seed: 1337, view: { x: 159.125, y: -113.25, z: 58.125, yaw: -2.1, pitch: 0.58, mode: "orbit", distance: 60 } },
  { id: "hillside", label: "Hillside", note: "Terrain terraces and vegetation", preset: "angledNordicTown", variant: "fjord", seed: 1337, view: { x: -234.75, y: 262.375, z: 8.5, yaw: 0.75, pitch: 0.58, mode: "orbit", distance: 150 } },
  { id: "highway-ramp", label: "Highway ramp", note: "Deck, ramp and ground transitions", preset: "angledCities", seed: 1337, view: { x: -652, y: -467.5, z: null, yaw: 2.25, pitch: 0.66, mode: "orbit", distance: 180 } },
  { id: "skyline", label: "Skyline", note: "Far LODs and atmospheric depth", preset: "angledInfiniteCity", seed: 1337, view: { x: 0, y: 0, z: null, yaw: -0.8, pitch: 0.55, mode: "orbit", distance: 900 } },
  { id: "interior", label: "Interior", note: "Near geometry, AO and indoor lighting", preset: "angledCities", seed: 1337, view: { x: -166.75, y: 72.125, z: null, yaw: 0.25, pitch: 0, mode: "walk" } },
];

export function renderLabUrl(entry) {
  const q = new URLSearchParams({ preset: entry.preset, seed: String(entry.seed), view: entry.id });
  if (entry.variant) q.set("variant", entry.variant);
  const v = entry.view;
  q.set("at", [v.x, v.y, v.z ?? "", v.yaw, v.pitch, v.mode, v.distance ?? ""].join(","));
  return `${window.location.pathname}?${q}`;
}

