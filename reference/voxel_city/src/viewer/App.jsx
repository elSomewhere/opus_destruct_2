import { Component, useCallback, useEffect, useMemo, useRef, useState } from "react";
import { ViewerEngine, DISPLAY_DEFAULTS } from "./ViewerEngine.js";
import { LOD_TINTS } from "./streamer.js";
import { MapPanel } from "./MapPanel.jsx";
import { PRESETS, presetConfig, presetSize, presetSeason, presetViewer } from "../engine/config/presets.js";
import { makeConfig } from "../engine/config/defaults.js";
import { SEASONS } from "../engine/world/season.js";
import { RENDER_LAB_VIEWS, renderLabUrl } from "./renderLab.js";
import { LOOKS } from "./looks.js";
import { MESHERS } from "../engine/voxel/meshers.js";
import "./app.css";

/**
 * The viewer's UI: a HUD (camera modes, map, inspector, the panel; a
 * compact readout and status chips), and a panel of four tabs: World
 * (preset, seed, season, what is generated), View (light, detail, camera,
 * walker), Debug (views, stats, the inspector's last answer) and Places
 * (points of interest, bookmarks, links to a view). The world's settings
 * and the view live in the URL (a link reopens the same place); display
 * settings and bookmarks in the browser.
 */

const MODES = [
  ["orbit", "Orbit", "1"],
  ["fly", "Fly", "2"],
  ["walk", "Walk", "3"],
];
/** World sections that can be switched off (config.<key>.enabled). */
const SECTIONS = [
  ["highways", "Highways"],
  ["subway", "Subway"],
  ["rivers", "Rivers"],
  ["lakes", "Lakes"],
  ["caves", "Caves"],
];
/** The angled world's features (config.world.angles.features). */
const ANGLE_FEATURES = [
  ["roads", "Angled streets"],
  ["vegetation", "Organic vegetation"],
  ["buildings", "Turned buildings"],
  ["ramps", "Pitched ramps"],
  ["wings", "Wings & bays"],
];
const TINTS = [
  ["none", "None"],
  ["lod", "By LOD"],
  ["parts", "Parts"],
];
const TABS = ["World", "View", "Debug", "Places"];
const STORE = "voxelCity.viewer.v1";
const MARKS = "voxelCity.bookmarks.v1";

function load(key, fallback) {
  try {
    const v = JSON.parse(localStorage.getItem(key) ?? "null");
    return v ?? fallback;
  } catch {
    return fallback;
  }
}
function save(key, value) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // (private window, storage blocked: settings just don't persist)
  }
}

/** The angle features a preset generates by default (merged config). */
function presetAngles(preset, variant) {
  const a = makeConfig(presetConfig(preset, { size: variant })).world.angles;
  return a.enabled ? { ...a.features } : null;
}

/** Start-up world and view from the URL: ?preset=…&variant=…&seed=…&season=…&parts=grid&off=highways,caves&angles=-wings&at=x,y,z,yaw,pitch,mode[,distance] */
function initialWorld() {
  const q = new URLSearchParams(window.location.search);
  const preset = PRESETS.has(q.get("preset")) ? q.get("preset") : "cities";
  const seed = Number.isFinite(Number(q.get("seed"))) && q.get("seed") !== null ? Number(q.get("seed")) : 1337;
  const variant = presetSize(preset, q.get("variant"))?.id ?? null;
  const angles = presetAngles(preset, variant);
  if (angles && q.get("angles"))
    for (const f of q.get("angles").split(",")) {
      const on = !f.startsWith("-");
      const k = f.replace(/^[-+]/, "");
      if (k in angles) angles[k] = on;
    }
  return {
    preset,
    variant,
    seed,
    season: presetSeason(preset, q.get("season")),
    parked: q.get("parked") === "1",
    off: (q.get("off") ?? "").split(",").filter((k) => SECTIONS.some(([id]) => id === k)),
    parts: q.get("parts") === "grid" ? "grid" : "separate",
    angles,
    mesher: MESHERS.includes(q.get("mesher")) ? q.get("mesher") : "greedy",
  };
}

function initialLook() {
  const id = new URLSearchParams(window.location.search).get("look");
  return id in LOOKS ? id : "classic";
}

function initialView() {
  const at = new URLSearchParams(window.location.search).get("at");
  if (!at) return null;
  const [x, y, z, yaw, pitch, mode, distance] = at.split(",");
  const n = (v) => (v === undefined || v === "" ? null : Number(v));
  if (!Number.isFinite(n(x)) || !Number.isFinite(n(y))) return null;
  return { x: n(x), y: n(y), z: n(z), yaw: n(yaw), pitch: n(pitch), mode: MODES.some(([m]) => m === mode) ? mode : "walk", distance: n(distance) };
}

/** The engine's config for world settings. */
function buildConfig(w) {
  const cfg = presetConfig(w.preset, { size: w.variant, seed: w.seed, season: w.season });
  for (const [k] of SECTIONS) if (w.off.includes(k)) cfg[k] = { ...(cfg[k] ?? {}), enabled: false };
  cfg.vehicles = { parked: w.parked };
  if (w.angles) cfg.world.angles = { ...(cfg.world.angles ?? {}), partsMode: w.parts, features: { ...(cfg.world.angles?.features ?? {}), ...w.angles } };
  cfg.rendering = { ...(cfg.rendering ?? {}), mesher: w.mesher };
  return cfg;
}

/** The URL of world settings (and a view). */
function worldUrl(w, view = null, look = "classic") {
  const q = new URLSearchParams({ preset: w.preset, seed: String(w.seed), season: w.season });
  if (w.variant) q.set("variant", w.variant);
  if (w.parked) q.set("parked", "1");
  if (w.off.length) q.set("off", w.off.join(","));
  if (w.angles) {
    if (w.parts === "grid") q.set("parts", "grid");
    const base = presetAngles(w.preset, w.variant) ?? {};
    const diff = Object.keys(w.angles).filter((k) => w.angles[k] !== base[k]);
    if (diff.length) q.set("angles", diff.map((k) => (w.angles[k] ? k : `-${k}`)).join(","));
  }
  if (w.mesher !== "greedy") q.set("mesher", w.mesher);
  if (look !== "classic") q.set("look", look);
  if (view) q.set("at", [view.x, view.y, view.z, view.yaw, view.pitch, view.mode, view.distance ?? ""].join(","));
  return `${window.location.pathname}?${q}`;
}

const tintHex = (k) => `#${(LOD_TINTS[k] ?? 0xc0c0c0).toString(16).padStart(6, "0")}`;

/**
 * A part of the UI that fails to render shows its error in its place; the
 * rest of the UI and the 3D view (outside every boundary) go on.
 */
class Guard extends Component {
  constructor(props) {
    super(props);
    this.state = { error: null };
  }
  static getDerivedStateFromError(error) {
    return { error };
  }
  componentDidCatch(error) {
    console.error(`[viewer UI: ${this.props.name}]`, error);
  }
  render() {
    if (!this.state.error) return this.props.children;
    return (
      <div className="chip warn guard">
        {this.props.name} failed: {String(this.state.error.message ?? this.state.error)}
        <button onClick={() => this.setState({ error: null })}>Retry</button>
      </div>
    );
  }
}

const fmtTime = (h) => `${String(Math.floor(h)).padStart(2, "0")}:${String(Math.round((h % 1) * 60) % 60).padStart(2, "0")}`;
const fmtNum = (v, d = 1) => (Number.isFinite(v) ? v.toFixed(d) : "–");

export default function App() {
  const hostRef = useRef(null);
  const engineRef = useRef(null);
  const init = useRef(null);
  if (!init.current) init.current = { world: initialWorld(), view: initialView() };
  const stored = useRef(load(STORE, {})).current;
  const [world, setWorld] = useState(init.current.world);
  const [applied, setApplied] = useState(init.current.world);
  const [generation, setGeneration] = useState(0);
  const [snap, setSnap] = useState(null);
  const [display, setDisplay] = useState({ ...DISPLAY_DEFAULTS, ...(stored.display ?? {}) });
  const [lodFactor, setLodFactor] = useState(stored.lodFactor ?? 5);
  const [viewDistance, setViewDistance] = useState(stored.viewDistance ?? 4000);
  const [timeOfDay, setTimeOfDay] = useState(presetViewer(init.current.world.preset, init.current.world.season).timeOfDay);
  const [look, setLook] = useState(initialLook);
  const [clipOn, setClipOn] = useState(false);
  const [clipZ, setClipZ] = useState(12);
  const [walkSpeed, setWalkSpeed] = useState(1);
  const [noclip, setNoclip] = useState(false);
  const [frozen, setFrozen] = useState(false);
  const [inspectOn, setInspectOn] = useState(false);
  const [inspected, setInspected] = useState(null);
  const [showMap, setShowMap] = useState(false);
  const [panelOpen, setPanelOpen] = useState(stored.panelOpen ?? window.innerWidth > 720);
  const [tab, setTab] = useState(TABS.includes(stored.tab) ? stored.tab : "World");
  const [places, setPlaces] = useState([]);
  const [marks, setMarks] = useState(() => load(MARKS, {}));
  const [toast, setToast] = useState(null);
  const [captureA, setCaptureA] = useState(() => ({ display: { ...DISPLAY_DEFAULTS }, look: "classic" }));
  const [captureB, setCaptureB] = useState(() => ({ display: { ...DISPLAY_DEFAULTS }, look: "bleak-overcast" }));
  const [capturing, setCapturing] = useState(false);
  const presetDef = PRESETS.get(world.preset);
  const pending = JSON.stringify(world) !== JSON.stringify(applied);

  const note = useCallback((text) => {
    setToast({ text, t: Date.now() });
  }, []);
  useEffect(() => {
    if (!toast) return undefined;
    const t = setTimeout(() => setToast(null), 3500);
    return () => clearTimeout(t);
  }, [toast]);

  // the engine: made anew for each generation (Regenerate)
  useEffect(() => {
    const w = applied;
    const cfg = buildConfig(w);
    const mood = presetViewer(w.preset, w.season);
    setTimeOfDay(mood.timeOfDay);
    window.history.replaceState(null, "", worldUrl(w, generation === 0 ? init.current.view : null, look));
    const engine = new ViewerEngine(hostRef.current, cfg, { atmosphere: mood.atmosphere, timeOfDay: mood.timeOfDay, display, look });
    engineRef.current = engine;
    window.__engine = engine;
    const off = engine.on(setSnap);
    engine.pois().then(setPlaces).catch(() => setPlaces([]));
    if (generation === 0 && init.current.view) engine.setView(init.current.view);
    return () => {
      off();
      engine.dispose();
      engineRef.current = null;
    };
    // a new engine only on Regenerate; display, quality and the rest are applied below
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [generation]);

  useEffect(() => engineRef.current?.setClip(clipOn ? clipZ : null), [clipOn, clipZ, generation]);
  useEffect(() => engineRef.current?.setQuality({ lodFactor, viewDistance }), [lodFactor, viewDistance, generation]);
  useEffect(() => engineRef.current?.setTimeOfDay(timeOfDay), [timeOfDay, generation]);
  useEffect(() => engineRef.current?.setDisplay(display), [display, generation]);
  useEffect(() => engineRef.current?.setLook(look), [look, generation]);
  useEffect(() => engineRef.current?.setWalker({ speed: walkSpeed, noclip }), [walkSpeed, noclip, generation]);
  useEffect(() => engineRef.current?.setFrozen(frozen), [frozen, generation]);
  useEffect(() => save(STORE, { display, lodFactor, viewDistance, tab, panelOpen }), [display, lodFactor, viewDistance, tab, panelOpen]);
  useEffect(() => save(MARKS, marks), [marks]);
  // (the inspector clicks without taking the pointer)
  useEffect(() => {
    if (engineRef.current) engineRef.current.rig.noLock = inspectOn;
  }, [inspectOn, generation]);

  const regenerate = () => {
    setApplied(world);
    setGeneration((g) => g + 1);
  };
  const setW = (patch) => setWorld((w) => ({ ...w, ...patch }));
  const choosePreset = (id) => {
    const variant = presetSize(id, null)?.id ?? null;
    setWorld((w) => ({ ...w, preset: id, variant, season: presetSeason(id, null), angles: presetAngles(id, variant) }));
  };
  const disp = (patch) => setDisplay((d) => ({ ...d, ...patch }));

  const viewLink = () => {
    const e = engineRef.current;
    if (!e) return null;
    return `${window.location.origin}${worldUrl(applied, e.getView(), look)}`;
  };
  const copyLink = async () => {
    const link = viewLink();
    if (!link) return;
    window.history.replaceState(null, "", link.slice(window.location.origin.length));
    try {
      await navigator.clipboard.writeText(link);
      note("Link to this view copied");
    } catch {
      note("Link in the address bar");
    }
  };
  const markKey = `${applied.preset}/${applied.variant ?? ""}/${applied.seed}`;
  const myMarks = marks[markKey] ?? [];
  const addMark = () => {
    const e = engineRef.current;
    if (!e) return;
    const v = e.getView();
    const name = window.prompt("Bookmark name", `${v.mode} ${Math.round(v.x)}, ${Math.round(v.y)}`);
    if (!name) return;
    setMarks((m) => ({ ...m, [markKey]: [...(m[markKey] ?? []), { name, view: v }] }));
  };
  const delMark = (k) => setMarks((m) => ({ ...m, [markKey]: (m[markKey] ?? []).filter((_, i) => i !== k) }));
  const capturePair = async () => {
    const e = engineRef.current;
    if (!e || capturing) return;
    setCapturing(true);
    const original = { ...display };
    const records = [];
    try {
      for (const [name, settings] of [["A", captureA], ["B", captureB]]) {
        e.setDisplay(settings.display);
        e.setLook(settings.look);
        await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
        download(await e.capturePng(), `render-lab-${name.toLowerCase()}.png`);
        records.push({ name, settings, snapshot: e.snapshot() });
      }
      download(new Blob([JSON.stringify({ capturedAt: new Date().toISOString(), view: e.getView(), url: window.location.href, captures: records }, null, 2)], { type: "application/json" }), "render-lab.json");
      note("Saved A, B and capture metadata");
    } catch (err) {
      note(`Capture failed: ${err.message ?? err}`);
    } finally {
      e.setDisplay(original);
      e.setLook(look);
      setCapturing(false);
    }
  };

  const inspectAt = useCallback(
    async (ndcX, ndcY) => {
      const e = engineRef.current;
      if (!e) return;
      setInspected({ loading: true });
      try {
        const r = await e.inspectAt(ndcX, ndcY);
        setInspected(r ?? { none: true });
      } catch (err) {
        setInspected({ error: String(err) });
      }
    },
    [],
  );

  // keyboard shortcuts (not while typing)
  useEffect(() => {
    const onKey = (ev) => {
      const t = ev.target;
      if (t && (t.tagName === "INPUT" || t.tagName === "SELECT" || t.tagName === "TEXTAREA")) return;
      if (ev.ctrlKey || ev.metaKey || ev.altKey) return;
      const e = engineRef.current;
      switch (ev.key) {
        case "1":
        case "2":
        case "3":
          e?.setMode(MODES[Number(ev.key) - 1][0]);
          break;
        case "m":
          setShowMap((v) => !v);
          break;
        case "p":
          setPanelOpen((v) => !v);
          break;
        case "i":
          if (e && e.rig.mode !== "orbit") inspectAt(0, 0);
          else setInspectOn((v) => !v);
          break;
        case "b":
          disp({ tileBorders: !display.tileBorders });
          break;
        case "l":
          disp({ tint: TINTS[(TINTS.findIndex(([id]) => id === display.tint) + 1) % TINTS.length][0] });
          break;
        case "f":
          setFrozen((v) => !v);
          break;
        case "n":
          setNoclip((v) => !v);
          break;
        case "g":
          if (e) {
            const v = e.getView();
            e.spawn(v.x, v.y);
          }
          break;
        case "k":
          copyLink();
          break;
        default:
          break;
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });

  // orbit + inspector: a click (not a drag) on the view inspects there
  const down = useRef(null);
  const onDown = (ev) => {
    down.current = { x: ev.clientX, y: ev.clientY };
  };
  const onUp = (ev) => {
    const d = down.current;
    down.current = null;
    if (!inspectOn || !d || Math.hypot(ev.clientX - d.x, ev.clientY - d.y) > 4) return;
    const r = hostRef.current.getBoundingClientRect();
    inspectAt(((ev.clientX - r.left) / r.width) * 2 - 1, -(((ev.clientY - r.top) / r.height) * 2 - 1));
  };

  const pos = snap?.pos ?? [0, 0, 0];
  const st = snap?.stats ?? {};
  const status = snap?.status ?? {};
  // a wrapping world: the position on the world and how many laps from the start
  const wrapSize = engineRef.current?.config.world.chart === "torus" ? engineRef.current.config.world.size : 0;
  const wrapPos = (v) => (wrapSize ? (((v + wrapSize / 2) % wrapSize) + wrapSize) % wrapSize - wrapSize / 2 : v);
  const laps = wrapSize ? [Math.floor((pos[0] + wrapSize / 2) / wrapSize), Math.floor((pos[1] + wrapSize / 2) / wrapSize)] : null;
  const recentRecovery = useMemo(() => status.recovered, [status.recovered]);
  useEffect(() => {
    if (recentRecovery) note("Fell out of the streamed world: back on the ground");
  }, [recentRecovery, note]);

  const chips = [];
  if (status.context === "lost") chips.push(["warn", "GPU context lost – restoring…", "Regenerate", regenerate]);
  if (snap?.walker === "waiting") chips.push(["info", "Waiting for the ground to stream in…"]);
  if (snap?.walker === "noclip") chips.push(["info", "Noclip (N)"]);
  if (st.frozen) chips.push(["info", "Streaming frozen (F)"]);
  if (st.failed) chips.push(["warn", `${st.failed} tile${st.failed > 1 ? "s" : ""} failed`, "Debug", () => (setPanelOpen(true), setTab("Debug"))]);
  if (pending) chips.push(["info", "World settings changed", "Regenerate", regenerate]);

  return (
    <div className="app">
      <div className="viewport" ref={hostRef} onMouseDown={onDown} onMouseUp={onUp} />
      {inspectOn && snap?.mode !== "orbit" && <div className="crosshair" />}
      <Guard name="HUD">
      <div className="hud">
        <div className="bar">
          <div className="seg">
            {MODES.map(([id, label, key]) => (
              <button key={id} className={snap?.mode === id ? "on" : ""} title={`${label} (${key})`} onClick={() => engineRef.current?.setMode(id)}>
                {label}
              </button>
            ))}
          </div>
          <button className={showMap ? "on" : ""} title="Map (M)" onClick={() => setShowMap((v) => !v)}>
            Map
          </button>
          <button className={inspectOn ? "on" : ""} title="Inspect a voxel: click it (orbit) or press I at the crosshair (fly / walk)" onClick={() => setInspectOn((v) => !v)}>
            Inspect
          </button>
          <button className={panelOpen ? "on" : ""} title="Panel (P)" onClick={() => setPanelOpen((v) => !v)}>
            ☰
          </button>
        </div>
        <div className="readout" title={`${fmtNum(snap?.fps, 0)} fps · ${fmtNum(snap?.frameMs, 1)} ms CPU · ${fmtNum(snap?.gpuMs, 1)} ms GPU · tiles ${st.ready ?? 0}/${st.tiles ?? 0} · queue ${st.queued ?? 0}+${st.loading ?? 0} · ${((st.triangles ?? 0) / 1e6).toFixed(2)}M tris · ${st.calls ?? 0} draws`}>
          <span>
            {wrapPos(pos[0]).toFixed(1)}, {wrapPos(pos[1]).toFixed(1)}, {pos[2].toFixed(1)} m{laps && (laps[0] || laps[1]) ? ` (lap ${laps[0]}, ${laps[1]})` : ""}
          </span>
          <span className="dim">
            {fmtNum(snap?.frameMs, 1)} ms CPU · {fmtNum(snap?.gpuMs, 1)} ms GPU · {st.calls ?? 0} draws · {((st.triangles ?? 0) / 1e6).toFixed(2)}M tris
          </span>
        </div>
        {chips.map(([kind, text, action, fn]) => (
          <div key={text} className={`chip ${kind}`}>
            {text}
            {action && <button onClick={fn}>{action}</button>}
          </div>
        ))}
        {toast && <div className="chip ok">{toast.text}</div>}
        {snap?.mode && snap.mode !== "orbit" && <div className="hint">Click the view to look around · WASD move · Shift run · Space jump/up · Esc release</div>}
      </div>
      </Guard>
      {inspected && (
        <Guard name="Inspector">
          <InspectCard data={inspected} onClose={() => setInspected(null)} />
        </Guard>
      )}
      <Guard name="Panel">
      {panelOpen && (
        <div className="panel">
          <div className="panelHead">
            <div>
              <h1>Voxel World</h1>
              <div className="sub">{presetDef.label}</div>
            </div>
            <button className="close" title="Close (P)" onClick={() => setPanelOpen(false)}>
              ✕
            </button>
          </div>
          <div className="tabs">
            {TABS.map((t) => (
              <button key={t} className={tab === t ? "on" : ""} onClick={() => setTab(t)}>
                {t}
              </button>
            ))}
          </div>
          <div className="panelBody">
            {tab === "World" && (
              <>
                <label>
                  World preset
                  <select value={world.preset} onChange={(e) => choosePreset(e.target.value)}>
                    {PRESETS.all().map((p) => (
                      <option key={p.id} value={p.id}>
                        {p.label}
                      </option>
                    ))}
                  </select>
                </label>
                {presetDef.sizes && (
                  <label>
                    {presetDef.sizes.some((z) => z.label.includes("km")) ? "Size" : "Variant"}
                    <select value={world.variant ?? presetDef.defaultSize} onChange={(e) => setW({ variant: e.target.value, angles: presetAngles(world.preset, e.target.value) })}>
                      {presetDef.sizes.map((z) => (
                        <option key={z.id} value={z.id}>
                          {z.label}
                        </option>
                      ))}
                    </select>
                  </label>
                )}
                <div className="row2">
                  <label>
                    Seed
                    <input type="number" value={world.seed} onChange={(e) => setW({ seed: Number(e.target.value) })} />
                  </label>
                  <button title="A random seed" onClick={() => setW({ seed: Math.floor(Math.random() * 1e6) })}>
                    Random
                  </button>
                </div>
                <label>
                  Season
                  <select value={world.season} onChange={(e) => setW({ season: e.target.value })}>
                    {SEASONS.map((z) => (
                      <option key={z.id} value={z.id}>
                        {z.label}
                      </option>
                    ))}
                  </select>
                </label>
                <div className="small">{presetDef.description}</div>
                <Section title="Generate">
                  {SECTIONS.map(([id, label]) => (
                    <Toggle key={id} label={label} on={!world.off.includes(id)} set={(on) => setW({ off: on ? world.off.filter((k) => k !== id) : [...world.off, id] })} />
                  ))}
                  <Toggle label="Parked cars & vehicles" on={world.parked} set={(on) => setW({ parked: on })} />
                </Section>
                {world.angles && (
                  <Section title="Angled world">
                    <label>
                      Parts drawn
                      <select value={world.parts} onChange={(e) => setW({ parts: e.target.value })}>
                        <option value="separate">In their own lattices (as structvox)</option>
                        <option value="grid">Stepped into the world grid</option>
                      </select>
                    </label>
                    {ANGLE_FEATURES.map(([id, label]) => (
                      <Toggle key={id} label={label} on={!!world.angles[id]} set={(on) => setW({ angles: { ...world.angles, [id]: on } })} />
                    ))}
                  </Section>
                )}
                <label>
                  Surface mesher
                  <select value={world.mesher} onChange={(e) => setW({ mesher: e.target.value })}>
                    {MESHERS.map((id) => <option key={id} value={id}>{id}</option>)}
                  </select>
                </label>
                <button className={`primary${pending ? " pending" : ""}`} onClick={regenerate}>
                  {pending ? "Apply & regenerate" : "Regenerate"}
                </button>
              </>
            )}
            {tab === "View" && (
              <>
                <label>
                  Look
                  <select value={look} onChange={(e) => setLook(e.target.value)}>
                    {Object.entries(LOOKS).map(([id, v]) => <option key={id} value={id}>{v.label}</option>)}
                  </select>
                </label>
                <Slider label={`Time of day ${fmtTime(timeOfDay)}`} min={0} max={24} step={0.25} value={timeOfDay} set={setTimeOfDay} />
                <div className="chips">
                  {[
                    ["Dawn", 6.5],
                    ["Noon", 13],
                    ["Dusk", 18.5],
                    ["Night", 22.5],
                  ].map(([l, h]) => (
                    <button key={l} onClick={() => setTimeOfDay(h)}>
                      {l}
                    </button>
                  ))}
                </div>
                <Section title="Detail">
                  <Slider label={`Detail (LOD factor) ${lodFactor}`} min={2} max={9} step={0.5} value={lodFactor} set={setLodFactor} />
                  <Slider label={`View distance ${viewDistance} m`} min={500} max={30000} step={500} value={viewDistance} set={setViewDistance} />
                  <Slider label={`Resolution ${Math.round(display.resolution * 100)}%`} min={0.4} max={1} step={0.05} value={display.resolution} set={(v) => disp({ resolution: v })} />
                  <Toggle label="Shadows" on={display.shadows} set={(on) => disp({ shadows: on })} />
                  <Toggle label="Fog" on={display.fog} set={(on) => disp({ fog: on })} />
                </Section>
                <Section title="Camera">
                  <Slider label={`Field of view ${display.fov}°`} min={35} max={100} step={1} value={display.fov} set={(v) => disp({ fov: v })} />
                  <Toggle label="Cutaway above" on={clipOn} set={setClipOn} />
                  <div className="row2">
                    <input type="range" min={-40} max={400} step={0.125} value={clipZ} disabled={!clipOn} onChange={(e) => setClipZ(Number(e.target.value))} />
                    <button title="Cut at the camera's height" onClick={() => (setClipZ(Math.round((snap?.cam?.[2] ?? 12) * 8) / 8), setClipOn(true))}>
                      Here
                    </button>
                  </div>
                  <div className="small">{clipZ.toFixed(2)} m</div>
                </Section>
                <Section title="Walker">
                  <Slider label={`Speed ×${walkSpeed}`} min={0.5} max={3} step={0.25} value={walkSpeed} set={setWalkSpeed} />
                  <Toggle label="Noclip: through walls, no gravity (N)" on={noclip} set={setNoclip} />
                  <button onClick={() => engineRef.current && engineRef.current.spawn(pos[0], pos[1])}>Put me on the ground (G)</button>
                </Section>
              </>
            )}
            {tab === "Debug" && <DebugTab snap={snap} display={display} look={look} disp={disp} frozen={frozen} setFrozen={setFrozen} inspectOn={inspectOn} setInspectOn={setInspectOn} inspected={inspected} captureA={captureA} captureB={captureB} setCaptureA={setCaptureA} setCaptureB={setCaptureB} capturePair={capturePair} capturing={capturing} />}
            {tab === "Places" && (
              <>
                <Section title="Render lab viewpoints">
                  <div className="list">
                    {RENDER_LAB_VIEWS.map((v) => (
                      <a className="viewLink" key={v.id} href={renderLabUrl(v)} title={v.note}>
                        {v.label}<span>{v.note}</span>
                      </a>
                    ))}
                  </div>
                </Section>
                <Section title="Places">
                  <div className="list">
                    {places.map((p, k) => (
                      <div key={p.label} className="list">
                        {p.group && places[k - 1]?.group !== p.group && <div className="small groupHead">{p.group}</div>}
                        <button onClick={() => engineRef.current?.goTo(p)}>{p.label}</button>
                      </div>
                    ))}
                    {!places.length && <div className="small">Finding places…</div>}
                  </div>
                </Section>
                <Section title="Bookmarks">
                  <div className="list">
                    {myMarks.map((m, k) => (
                      <div key={`${m.name}${k}`} className="markRow">
                        <button onClick={() => engineRef.current?.setView(m.view)}>{m.name}</button>
                        <button className="x" title="Remove" onClick={() => delMark(k)}>
                          ✕
                        </button>
                      </div>
                    ))}
                    {!myMarks.length && <div className="small">None for this world yet.</div>}
                  </div>
                  <div className="row2">
                    <button onClick={addMark}>Bookmark this view</button>
                    <button title="Copy a link to this view (K)" onClick={copyLink}>
                      Copy link
                    </button>
                  </div>
                </Section>
                <GoTo onGo={(x, y) => engineRef.current?.setView({ mode: snap?.mode === "orbit" ? "orbit" : "walk", x, y, z: null, yaw: engineRef.current.rig.yaw, pitch: snap?.mode === "orbit" ? 0.75 : 0 })} />
              </>
            )}
          </div>
          <div className="keys small">1 2 3 modes · M map · P panel · I inspect · L tint · B borders · F freeze · N noclip · G ground · K link</div>
        </div>
      )}
      </Guard>
      {showMap && (
        <Guard name="Map">
          <MapPanel engine={engineRef.current} center={pos} onTeleport={(x, y) => engineRef.current?.spawn(x, y)} onClose={() => setShowMap(false)} />
        </Guard>
      )}
    </div>
  );
}

function Section({ title, children }) {
  const [open, setOpen] = useState(true);
  return (
    <div className="section">
      <button className="sectionHead" onClick={() => setOpen((v) => !v)}>
        <span>{title}</span>
        <span className="dim">{open ? "–" : "+"}</span>
      </button>
      {open && <div className="sectionBody">{children}</div>}
    </div>
  );
}

function Toggle({ label, on, set }) {
  return (
    <label className="row">
      <input type="checkbox" checked={on} onChange={(e) => set(e.target.checked)} /> {label}
    </label>
  );
}

function Slider({ label, min, max, step, value, set }) {
  return (
    <label>
      {label}
      <input type="range" min={min} max={max} step={step} value={value} onChange={(e) => set(Number(e.target.value))} />
    </label>
  );
}

function GoTo({ onGo }) {
  const [x, setX] = useState("0");
  const [y, setY] = useState("0");
  return (
    <Section title="Go to">
      <div className="row2">
        <input type="number" value={x} onChange={(e) => setX(e.target.value)} aria-label="x (m)" placeholder="x m" />
        <input type="number" value={y} onChange={(e) => setY(e.target.value)} aria-label="y (m)" placeholder="y m" />
        <button onClick={() => onGo(Number(x), Number(y))}>Go</button>
      </div>
      <div className="small">Metres, on the ground there.</div>
    </Section>
  );
}

function DebugTab({ snap, display, look, disp, frozen, setFrozen, inspectOn, setInspectOn, inspected, captureA, captureB, setCaptureA, setCaptureB, capturePair, capturing }) {
  const st = snap?.stats ?? {};
  const status = snap?.status ?? {};
  const copy = () => {
    const info = { view: window.__engine?.getView(), url: window.location.href, stats: st, status, fps: snap?.fps, frameMs: snap?.frameMs, far: snap?.far, walker: snap?.walker, inspected };
    navigator.clipboard?.writeText(JSON.stringify(info, null, 2)).catch(() => {});
  };
  return (
    <>
      <Section title="Views">
        <label>
          Tint
          <select value={display.tint} onChange={(e) => disp({ tint: e.target.value })}>
            {TINTS.map(([id, label]) => (
              <option key={id} value={id}>
                {label}
              </option>
            ))}
          </select>
        </label>
        {display.tint === "lod" && (
          <div className="legend">
            {(snap?.stats?.byLod ?? LOD_TINTS.slice(0, 6)).map((_, k) => (
              <span key={k}>
                <i style={{ background: tintHex(k) }} />
                {k}
              </span>
            ))}
          </div>
        )}
        <Toggle label="Tile borders (B)" on={display.tileBorders} set={(on) => disp({ tileBorders: on })} />
        <Toggle label="Wireframe" on={display.wireframe} set={(on) => disp({ wireframe: on })} />
        <Toggle label="Freeze streaming (F)" on={frozen} set={setFrozen} />
        <Toggle label="Inspect voxels (I)" on={inspectOn} set={setInspectOn} />
      </Section>
      <Section title="Frame">
        <table className="stats">
          <tbody>
            <tr>
              <td>fps</td>
              <td>{fmtNum(snap?.fps, 0)}</td>
              <td>frame</td>
              <td>{fmtNum(snap?.frameMs, 1)} ms</td>
            </tr>
            <tr>
              <td>draws</td>
              <td>{st.calls ?? 0}</td>
              <td>tris</td>
              <td>{((st.triangles ?? 0) / 1e6).toFixed(2)}M</td>
            </tr>
            <tr>
              <td>geometries</td>
              <td>{st.geometries ?? 0}</td>
              <td>geometry</td>
              <td>{fmtBytes(st.gpuBytes)}</td>
            </tr>
            <tr>
              <td>far plane</td>
              <td>{fmtNum(snap?.far, 0)} m</td>
              <td>GPU time</td>
              <td className={status.context === "lost" ? "warn" : ""}>{fmtNum(snap?.gpuMs, 1)} ms</td>
            </tr>
          </tbody>
        </table>
      </Section>
      <Section title="Streaming">
        <table className="stats">
          <thead>
            <tr>
              <td>LOD</td>
              <td>shown</td>
              <td>ready</td>
              <td>queued</td>
              <td>loading</td>
            </tr>
          </thead>
          <tbody>
            {(st.byLod ?? []).map((r, k) => (
              <tr key={k}>
                <td>
                  <i className="dot" style={{ background: tintHex(k) }} />
                  {k}
                </td>
                <td>{r.shown}</td>
                <td>{r.ready}</td>
                <td>{r.queued}</td>
                <td>{r.loading}</td>
              </tr>
            ))}
          </tbody>
        </table>
        <div className="small">
          generation {fmtNum(st.genMs, 0)} ms per tile (max {fmtNum(st.genMsMax, 0)}) · {st.underground ? "underground" : "above ground"}
        </div>
        <table className="stats">
          <thead><tr><td>LOD</td><td>mesh/chunk</td><td>verts/chunk</td><td>tris/chunk</td></tr></thead>
          <tbody>{(st.meshByLod ?? []).map((r, k) => (
            <tr key={k}><td>{k}</td><td>{fmtNum(r.msPerChunk, 2)} ms</td><td>{fmtNum(r.verticesPerChunk, 0)}</td><td>{fmtNum(r.trianglesPerChunk, 0)}</td></tr>
          ))}</tbody>
        </table>
        <div className={`small${st.failed ? " warn" : ""}`}>
          failed tiles {st.failed ?? 0}
          {st.lastError ? `: ${st.lastError}` : ""}
        </div>
      </Section>
      <Section title="A/B capture">
        <div className="small">Save the current display options into A and B, then download two PNGs and a JSON record from this exact camera.</div>
        <div className="row2">
          <button onClick={() => setCaptureA({ display: { ...display }, look })}>Set A current</button>
          <button onClick={() => setCaptureB({ display: { ...display }, look })}>Set B current</button>
        </div>
        <div className="small">A: {displaySummary(captureA)}<br />B: {displaySummary(captureB)}</div>
        <button disabled={capturing} onClick={capturePair}>{capturing ? "Capturing…" : "Capture A + B"}</button>
      </Section>
      <Section title="Camera & walker">
        <div className="small">
          {snap?.mode} at {snap ? snap.pos.map((v) => v.toFixed(2)).join(", ") : "–"} m · yaw {fmtNum(snap?.yaw, 2)} · pitch {fmtNum(snap?.pitch, 2)}
          <br />
          walker {snap?.walker ?? "–"} · recovered from falls {status.recovered ?? 0}
        </div>
        <button onClick={copy}>Copy debug info</button>
      </Section>
      {inspected && (
        <Section title="Inspected voxel">
          <pre className="json">{JSON.stringify(inspected, null, 1)}</pre>
        </Section>
      )}
    </>
  );
}

function displaySummary(d) {
  return `${d.look}; ${d.display.shadows ? "shadows" : "no shadows"}, ${d.display.fog ? "fog" : "no fog"}, ${Math.round(d.display.resolution * 100)}%, ${d.display.wireframe ? "wire" : "solid"}`;
}

function fmtBytes(n) {
  return Number.isFinite(n) ? `${(n / 1048576).toFixed(1)} MB` : "–";
}

function download(blob, name) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

function InspectCard({ data, onClose }) {
  return (
    <div className="inspectCard">
      <div className="panelHead">
        <b>Voxel</b>
        <button className="close" onClick={onClose}>
          ✕
        </button>
      </div>
      {data.loading && <div className="small">Asking the world…</div>}
      {data.none && <div className="small">Nothing loaded under that point.</div>}
      {data.error && <div className="small warn">{data.error}</div>}
      {data.voxel && (
        <div className="small">
          <div>
            <b>{data.parts?.find((p) => p.material)?.material?.name ?? data.grid?.name ?? "air"}</b> at {data.metres.map((v) => v.toFixed(2)).join(", ")} m (voxel {data.voxel.join(", ")}, LOD {data.lod})
          </div>
          {data.grid && <div>world grid: {data.grid.name}</div>}
          {data.parts?.map((p) => (
            <div key={p.id}>
              part {p.id} ({p.kind} {p.key}) local {p.local.join(", ")}: {p.material?.name ?? "air"}
            </div>
          ))}
          {data.building && (
            <div>
              building {data.building.id} · {data.building.archetype}
              {data.building.turned ? " · turned" : ""} · cell {data.building.local.join(", ")}
            </div>
          )}
          {data.road && (
            <div>
              road {data.road.id} ({data.road.cls}) · level {data.road.level} · {data.road.sidewalk ? "sidewalk" : "carriageway"}
            </div>
          )}
          {data.highway && (
            <div>
              highway {data.highway.edge} · arc {data.highway.arc} m · offset {(data.highway.offset * 0.125).toFixed(1)} m
            </div>
          )}
          <div className="dim">
            {data.district ?? "–"} · cell {data.cell?.join(", ")} · {data.settlement ?? "no settlement"} · urban {fmtNum(data.u, 2)} · ground {((data.groundZ + 1) * 0.125).toFixed(2)} m
          </div>
        </div>
      )}
    </div>
  );
}
