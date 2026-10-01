import * as THREE from "three";
import { WorkerPool } from "./workerPool.js";
import { TileStreamer } from "./streamer.js";
import { makeVoxelMaterials } from "./voxelMaterial.js";
import { CameraRig } from "./controls.js";
import { makeConfig } from "../engine/config/defaults.js";
import { lookById } from "./looks.js";

const VOXEL = 0.125;

/** Display options (setDisplay) and their defaults. */
export const DISPLAY_DEFAULTS = {
  shadows: true,
  fog: true,
  /** render resolution: a fraction of the device's pixel ratio (capped at 1.5) */
  resolution: 1,
  fov: 60,
  wireframe: false,
  /** debug tint: "none", "lod" (tiles by their LOD), "parts" (the angled world's parts) */
  tint: "none",
  /** debug: the borders of the tiles shown, coloured by LOD */
  tileBorders: false,
};

/**
 * Owns the three.js renderer, the worker pool, the tile streamer and the
 * camera rig. React components drive it through plain methods and listen
 * to a snapshot (`on`) ten times a second.
 *
 * The far plane and the fog follow the camera: from the ground they are
 * set by the view distance; zoomed out (orbit) or high up (fly) they reach
 * past the camera's distance to the ground, so the world never fades into
 * the fog or past the far plane. Entering walk mode puts the walker on the
 * ground under the view; a walker fallen out of what has streamed in is
 * put back. A lost WebGL context is reported (`status.context`) and
 * restored when the browser allows.
 */
export class ViewerEngine {
  constructor(container, configOverrides = {}, { atmosphere = null, timeOfDay = 13, display = {}, look = "classic" } = {}) {
    this.container = container;
    this.config = makeConfig(configOverrides);
    /** sky / light mood of the world preset (config/presets.js) */
    this.atmosphere = atmosphere ?? {};
    this.fogMul = this.atmosphere.fogDensity ?? 1;
    this.viewDistance = 4000;
    this.display = { ...DISPLAY_DEFAULTS, ...display };
    // logarithmic depth: from 5 cm interiors to mountain ranges 30 km away
    this.renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: "high-performance", logarithmicDepthBuffer: true });
    this.renderer.setPixelRatio(this.pixelRatio());
    this.renderer.setClearColor(0xb8c9d9);
    container.appendChild(this.renderer.domElement);
    this.scene = new THREE.Scene();
    this.renderer.shadowMap.enabled = this.display.shadows;
    this.renderer.shadowMap.type = THREE.PCFShadowMap;
    this.renderer.localClippingEnabled = true;
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.gpuTimer = makeGpuTimer(this.renderer.getContext());
    this.fog = new THREE.FogExp2(0xb8c9d9, 1 / 2400);
    this.scene.fog = this.display.fog ? this.fog : null;
    this.hemi = new THREE.HemisphereLight(0xc8d8ec, 0x6b6258, 1.35);
    this.hemi.position.set(0, 0, 1);
    this.scene.add(this.hemi);
    this.sun = new THREE.DirectionalLight(0xfff1dc, 2.1);
    this.sunDir = new THREE.Vector3(0.45, -0.35, 0.82).normalize();
    this.sun.castShadow = this.display.shadows;
    this.sun.shadow.mapSize.set(4096, 4096);
    this.sun.shadow.bias = -0.0004;
    this.sun.shadow.normalBias = 0.06;
    const sc = this.sun.shadow.camera;
    sc.left = -140;
    sc.right = 140;
    sc.top = 140;
    sc.bottom = -140;
    sc.near = 1;
    sc.far = 900;
    this.scene.add(this.sun);
    this.scene.add(this.sun.target);
    this.rain = makeRain();
    this.scene.add(this.rain);
    this.clipPlane = new THREE.Plane(new THREE.Vector3(0, 0, -1), 1e9);
    this.camera = new THREE.PerspectiveCamera(this.display.fov, 1, 0.05, 6000);
    this.camera.up.set(0, 0, 1);
    this.materials = makeVoxelMaterials();
    this.materials.opaque.clippingPlanes = [this.clipPlane];
    this.materials.transparent.clippingPlanes = [this.clipPlane];
    const workers = this.config.streaming.workers || Math.max(2, Math.min(8, (navigator.hardwareConcurrency || 4) - 2));
    this.pool = new WorkerPool(this.config, workers);
    this.streamer = new TileStreamer({ scene: this.scene, pool: this.pool, materials: this.materials, config: this.config });
    this.rig = new CameraRig(this.camera, this.renderer.domElement, {
      solidAt: (x, y, z) => this.streamer.solidAt(x, y, z),
      climbAt: (x, y, z) => this.streamer.climbAt(x, y, z),
      floorAt: (x, y) => this.streamer.floorAt(x, y),
      onLost: (x, y) => this.recover(x, y),
    });
    this.clock = new THREE.Clock();
    this.running = true;
    this.listeners = new Set();
    this.frame = 0;
    this.fps = 0;
    this.fpsAcc = { n: 0, t: 0 };
    this.frameMs = 0;
    /** what the UI shows beside the stats: the WebGL context, the walker, the last recovery */
    this.status = { context: "ok", recovered: 0, lastRecovery: null };
    this.resize();
    this._onResize = () => this.resize();
    window.addEventListener("resize", this._onResize);
    this._onLost = (e) => {
      // (three keeps the context restorable: its own handler calls preventDefault)
      e.preventDefault();
      this.status.context = "lost";
      this.emit();
    };
    this._onRestored = () => {
      this.status.context = "restored";
      this.emit();
    };
    this.renderer.domElement.addEventListener("webglcontextlost", this._onLost);
    this.renderer.domElement.addEventListener("webglcontextrestored", this._onRestored);
    this.applyDisplay();
    this.setLook(look);
    this.setTimeOfDay(timeOfDay);
    this.spawn(0, 0);
    this.loop();
  }

  /** The renderer's pixel ratio for the display's resolution. */
  pixelRatio() {
    return Math.min(window.devicePixelRatio || 1, 1.5) * (this.display?.resolution ?? 1);
  }

  /** Ground (feet) height (m) at a point, from the query worker. */
  async groundAt(xm, ym) {
    const info = await this.pool.request("probe", { x: Math.round(xm / VOXEL), y: Math.round(ym / VOXEL) });
    return (info.groundZ + 1) * VOXEL;
  }

  async spawn(xm, ym) {
    const gz = await this.groundAt(xm, ym);
    if (!Number.isFinite(gz)) return;
    this.rig.target.set(xm, ym, gz);
    this.rig.teleport(xm, ym, gz + 0.01);
    this.rig.target.set(xm, ym, gz);
  }

  /** A walker fallen out of what has streamed in: back on the ground (at most once a second). */
  recover(xm, ym) {
    const now = performance.now();
    if (this._recovering && now - this._recovering < 1000) return;
    this._recovering = now;
    this.status.recovered += 1;
    this.status.lastRecovery = [xm, ym];
    this.spawn(xm, ym);
  }

  /** Jump to a point of interest (world voxels; z = feet level or null for ground). */
  async goTo(poi) {
    const xm = poi.x * VOXEL;
    const ym = poi.y * VOXEL;
    const zm = poi.z == null ? await this.groundAt(xm, ym) : poi.z * VOXEL;
    if (poi.mode === "orbit") {
      this.rig.setMode("orbit");
      this.rig.target.set(xm, ym, zm);
      this.rig.distance = Math.min(poi.distance ?? 420, this.rig.maxOrbit);
      this.rig.pitch = 0.75;
    } else {
      this.rig.setMode(poi.mode ?? "walk");
      this.rig.teleport(xm, ym, zm + 0.02);
      this.rig.vel.set(0, 0, 0);
    }
    this.streamer.lastSelect = null;
  }

  /** The view as plain numbers (a link, a bookmark): { mode, x, y, z (m), yaw, pitch, distance }. */
  getView() {
    const r = this.rig;
    const p = r.mode === "orbit" ? r.target : r.mode === "walk" ? r.pos : this.camera.position;
    const round = (v, k = 100) => Math.round(v * k) / k;
    return { mode: r.mode, x: round(p.x), y: round(p.y), z: round(p.z), yaw: round(r.yaw, 1000), pitch: round(r.pitch, 1000), ...(r.mode === "orbit" ? { distance: round(r.distance, 10) } : {}) };
  }

  /** Restore a view from getView(); z null puts it on the ground. */
  async setView(v) {
    const z = v.z ?? (await this.groundAt(v.x, v.y));
    this.rig.setMode(v.mode ?? "walk");
    if (this.rig.mode === "orbit") {
      this.rig.target.set(v.x, v.y, z);
      if (v.distance) this.rig.distance = v.distance;
    } else {
      this.rig.teleport(v.x, v.y, z);
      if (this.rig.mode === "fly") this.rig.pos.set(v.x, v.y, z);
    }
    if (Number.isFinite(v.yaw)) this.rig.yaw = v.yaw;
    if (Number.isFinite(v.pitch)) this.rig.pitch = v.pitch;
    this.streamer.lastSelect = null;
  }

  pois() {
    return this.pool.request("pois", { x: 0, y: 0 });
  }

  on(fn) {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  resize() {
    const w = this.container.clientWidth || 800;
    const h = this.container.clientHeight || 600;
    this.renderer.setPixelRatio(this.pixelRatio());
    this.renderer.setSize(w, h, false);
    this.renderer.domElement.style.width = "100%";
    this.renderer.domElement.style.height = "100%";
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
  }

  /**
   * Switch camera mode. Into walk mode, the walker lands on the ground under
   * the view (orbit: its target; fly: the camera), not in the air where the
   * camera was.
   */
  setMode(mode) {
    const from = this.rig.mode;
    if (mode === from) return;
    const at = from === "orbit" ? this.rig.target.clone() : this.camera.position.clone();
    this.rig.setMode(mode);
    if (mode === "walk") {
      const yaw = this.rig.yaw;
      this.spawn(at.x, at.y).then(() => {
        this.rig.yaw = yaw;
        this.rig.pitch = 0;
      });
    }
    this.streamer.lastSelect = null;
  }

  setClip(zMeters) {
    // keep everything below z = zMeters
    this.clipPlane.constant = zMeters ?? 1e9;
  }

  /** Display options (see DISPLAY_DEFAULTS); unknown keys are ignored. */
  setDisplay(opts) {
    this.display = { ...this.display, ...opts };
    this.applyDisplay();
  }

  applyDisplay() {
    const d = this.display;
    const shadowsChanged = this.renderer.shadowMap.enabled !== d.shadows;
    const fogChanged = (this.scene.fog !== null) !== d.fog;
    this.renderer.shadowMap.enabled = d.shadows;
    this.sun.castShadow = d.shadows;
    this.scene.fog = d.fog ? this.fog : null;
    for (const m of this.materials.all()) {
      m.wireframe = d.wireframe;
      // (shadows and fog change the shader)
      if (shadowsChanged || fogChanged) m.needsUpdate = true;
    }
    if (this.camera.fov !== d.fov) {
      this.camera.fov = d.fov;
      this.camera.updateProjectionMatrix();
    }
    if (Math.abs(this.renderer.getPixelRatio() - this.pixelRatio()) > 1e-6) this.resize();
    if (this.streamer.tint !== d.tint) this.streamer.setTint(d.tint);
    this.streamer.showBorders(d.tileBorders);
  }

  /** Debug: keep the tiles as they are while the camera moves on. */
  setFrozen(on) {
    this.streamer.frozen = on;
    if (!on) this.streamer.lastSelect = null;
  }

  /** Walker options: noclip (no collision, no gravity) and its speed factor. */
  setWalker({ noclip, speed } = {}) {
    if (noclip !== undefined) this.rig.noclip = noclip;
    if (speed !== undefined) this.rig.speed = speed;
  }

  /**
   * Time of day in hours (0-24): the sun arcs from east (6h) through the
   * south to west (18h); below the horizon a dim moon takes over. Sky, fog
   * and ambient shift through dusk to night, when lamps, signs and a share
   * of the windows light up (uNight).
   */
  setTimeOfDay(h) {
    this.timeOfDay = h;
    const atm = this.atmosphere;
    const a = ((h - 6) / 12) * Math.PI;
    // a northern winter sun stays low (atmosphere.sunElevation caps its altitude)
    const sa = Math.sin(a);
    const elev = sa > 0 ? sa * (atm.sunElevation ?? 1) : sa;
    const day = smooth(-0.12, 0.25, elev);
    const dusk = Math.max(0, 1 - Math.abs(elev) / 0.25) * smooth(-0.2, 0.0, elev);
    const dir = elev > -0.05 ? new THREE.Vector3(Math.cos(a), 0.45, Math.max(0.08, elev) * 1.3) : new THREE.Vector3(-Math.cos(a), 0.3, 0.9);
    this.sunDir.copy(dir.normalize());
    const grey = atm.desaturate ?? 0;
    const noon = new THREE.Color(0xfff1dc).lerp(new THREE.Color(0xe4e8ec), grey);
    const low = new THREE.Color(0xffa060).lerp(new THREE.Color(0xd8c0b0), grey);
    this.sun.color.copy(low).lerp(noon, smooth(0.05, 0.5, elev));
    if (day < 0.05) this.sun.color.set(0x8fa6d8);
    this.sun.intensity = (2.1 * day + 0.28 * (1 - day)) * (atm.sun ?? 1) * (this.look?.sun ?? 1);
    const skyDay = new THREE.Color(atm.sky ?? 0xb8c9d9);
    const skyDusk = new THREE.Color(0xd99a6c).lerp(skyDay, grey);
    const skyNight = new THREE.Color(0x0b1224);
    const sky = skyNight.clone().lerp(skyDay, day).lerp(skyDusk, dusk * 0.7);
    this.renderer.setClearColor(sky);
    this.fog.color.copy(sky);
    this.hemi.intensity = (0.22 + 1.13 * day) * (atm.ambient ?? 1) * (this.look?.ambient ?? 1);
    this.hemi.color.set(0x2a3a5c).lerp(new THREE.Color(0xc8d8ec).lerp(new THREE.Color(0xc4c9ce), grey), day);
    // snow on the ground throws back a pale light
    this.hemi.groundColor.set(0x1c1a1e).lerp(new THREE.Color(0x6b6258).lerp(new THREE.Color(0x9aa0a6), grey), day);
    this.materials.uniforms.uNight.value = 1 - smooth(-0.08, 0.12, elev);
  }

  setLook(id) {
    this.lookId = id;
    this.look = lookById(id);
    const l = this.look;
    this.materials.uniforms.uLook.value.set(l.desaturate, l.grime, l.wetness, l.grain);
    this.renderer.toneMapping = l.toneMapping === "agx" ? THREE.AgXToneMapping : l.toneMapping === "aces" ? THREE.ACESFilmicToneMapping : THREE.NoToneMapping;
    this.renderer.toneMappingExposure = l.exposure;
    this.fogMul = (this.atmosphere.fogDensity ?? 1) * l.fog;
    this.rain.visible = !!l.rain;
    this.rain.material.opacity = 0.18 + (l.rain ?? 0) * 0.3;
    this.setTimeOfDay(l.time ?? this.timeOfDay ?? 13);
  }

  setQuality({ lodFactor, viewDistance }) {
    if (lodFactor) this.streamer.lodFactor = lodFactor;
    if (viewDistance) {
      this.viewDistance = viewDistance;
      this.streamer.viewDistance = viewDistance;
      // (no point zooming out past what streams)
      this.rig.maxOrbit = viewDistance * 2.5;
      this.rig.distance = Math.min(this.rig.distance, this.rig.maxOrbit);
    }
    this.streamer.lastSelect = null;
  }

  /**
   * Far plane and fog for the camera's distance to what it looks at: the
   * orbit distance, or the height over the ground under the camera. From
   * the ground both follow the view distance; from afar they reach past.
   */
  fitDepth() {
    const vd = this.viewDistance;
    let away = 0;
    if (this.rig.mode === "orbit") away = this.rig.distance;
    else {
      const [, hi] = this.streamer.groundRange(0, Math.floor(this.camera.position.x / VOXEL / 32), Math.floor(this.camera.position.y / VOXEL / 32));
      away = Math.max(0, this.camera.position.z - hi * VOXEL);
    }
    const far = Math.max(vd * 1.6, away * 1.2 + vd);
    if (Math.abs(far - this.camera.far) > this.camera.far * 0.05) {
      this.camera.far = far;
      this.camera.updateProjectionMatrix();
    }
    this.fog.density = this.fogMul / Math.max(vd * 1.6, away * 2.5);
  }

  loop() {
    if (!this.running) return;
    // rAF stalls in hidden tabs; keep streaming with a timer there
    if (document.hidden) setTimeout(() => this.loop(), 50);
    else requestAnimationFrame(() => this.loop());
    const t0 = performance.now();
    const dt = this.clock.getDelta();
    this.rig.update(dt);
    // sun + shadow camera follow the viewer (snapped to texels to avoid shimmer)
    const focusPt = this.rig.mode === "orbit" ? this.rig.target : this.camera.position;
    this.rain.position.set(focusPt.x, focusPt.y, focusPt.z - 8);
    const snap = 280 / 4096;
    const fx = Math.round(focusPt.x / snap) * snap;
    const fy = Math.round(focusPt.y / snap) * snap;
    const fz = Math.round(focusPt.z / snap) * snap;
    this.sun.target.position.set(fx, fy, fz);
    this.sun.position.set(fx + this.sunDir.x * 400, fy + this.sunDir.y * 400, fz + this.sunDir.z * 400);
    this.sun.target.updateMatrixWorld();
    if (this.frame % 6 === 0) this.indoorTarget = this.coveredAbove() ? 1 : 0;
    const ind = this.materials.uniforms.uIndoor;
    ind.value += ((this.indoorTarget ?? 0) - ind.value) * Math.min(1, dt * 3);
    const focus = this.rig.mode === "orbit" ? this.rig.target.clone().setZ(this.camera.position.z) : this.camera.position;
    this.streamer.update(focus);
    this.fitDepth();
    if (this.status.context !== "lost") {
      this.gpuTimer.begin();
      this.renderer.render(this.scene, this.camera);
      this.gpuTimer.end();
      this.gpuTimer.poll();
    }
    this.frame += 1;
    this.frameMs += (performance.now() - t0 - this.frameMs) * 0.1;
    this.fpsAcc.n += 1;
    this.fpsAcc.t += dt;
    if (this.fpsAcc.t >= 0.5) {
      this.fps = this.fpsAcc.n / this.fpsAcc.t;
      this.fpsAcc = { n: 0, t: 0 };
    }
    if (this.frame % 10 === 0) this.emit();
  }

  /** The snapshot the UI shows. */
  snapshot() {
    const p = this.rig.mode === "walk" ? this.rig.pos : this.rig.mode === "orbit" ? this.rig.target : this.camera.position;
    const info = this.renderer.info;
    return {
      mode: this.rig.mode,
      pos: [p.x, p.y, p.z],
      cam: [this.camera.position.x, this.camera.position.y, this.camera.position.z],
      yaw: this.rig.yaw,
      pitch: this.rig.pitch,
      walker: this.rig.mode === "walk" ? this.rig.status : null,
      fps: this.fps,
      frameMs: this.frameMs,
      gpuMs: this.gpuTimer.ms,
      far: this.camera.far,
      stats: { ...this.streamer.stats, calls: info.render.calls, geometries: info.memory.geometries, programs: info.programs?.length ?? 0, lastError: this.streamer.lastError, underground: !!this.streamer.underground, frozen: this.streamer.frozen },
      status: { ...this.status },
    };
  }

  emit() {
    const snap = this.snapshot();
    for (const fn of this.listeners) fn(snap);
  }

  /** Render the current camera immediately and return a PNG of the canvas. */
  capturePng() {
    this.renderer.render(this.scene, this.camera);
    return new Promise((resolve, reject) => this.renderer.domElement.toBlob((blob) => (blob ? resolve(blob) : reject(new Error("Could not encode the WebGL canvas"))), "image/png"));
  }

  /**
   * What is under a point of the screen (normalized device coordinates):
   * the voxel the ray hits first, asked of the query worker (queries.js
   * inspect): its material, the part, building, road and district there.
   * Null if the ray hits nothing loaded.
   */
  async inspectAt(ndcX, ndcY) {
    const ray = new THREE.Raycaster();
    ray.setFromCamera(new THREE.Vector2(ndcX, ndcY), this.camera);
    const hits = ray.intersectObject(this.streamer.root, true).filter((h) => h.object.visible && h.object.parent?.visible !== false && !h.object.material.transparent);
    const h = hits[0];
    if (!h) return null;
    // the voxel behind the face hit (half a voxel in, along the ray)
    const p = h.point.clone().addScaledVector(ray.ray.direction, VOXEL * 0.5 * (1 << (h.object.userData.lod ?? 0)));
    const x = Math.floor(p.x / VOXEL);
    const y = Math.floor(p.y / VOXEL);
    const z = Math.floor(p.z / VOXEL);
    const data = await this.pool.request("inspect", { x, y, z, part: h.object.userData.partId ?? null });
    return { ...data, lod: h.object.userData.lod ?? 0, distance: h.distance, partHit: h.object.userData.part ? { id: h.object.userData.partId, kind: h.object.userData.partKind } : null };
  }

  /** Is there a roof / ceiling above the camera (within 30 m)? */
  coveredAbove() {
    const p = this.camera.position;
    const x = Math.floor(p.x / VOXEL);
    const y = Math.floor(p.y / VOXEL);
    const z = Math.floor(p.z / VOXEL);
    for (let k = 1; k < 240; k += 1) {
      const s = this.streamer.solidAt(x, y, z + k);
      if (s === null) return false;
      if (s) return true;
    }
    return false;
  }

  dispose() {
    this.running = false;
    window.removeEventListener("resize", this._onResize);
    this.renderer.domElement.removeEventListener("webglcontextlost", this._onLost);
    this.renderer.domElement.removeEventListener("webglcontextrestored", this._onRestored);
    this.rig.dispose();
    this.streamer.dispose();
    this.pool.dispose();
    this.rain.geometry.dispose();
    this.rain.material.dispose();
    this.renderer.dispose();
    this.renderer.domElement.remove();
  }
}

function makeGpuTimer(gl) {
  const ext = gl.getExtension("EXT_disjoint_timer_query_webgl2");
  let active = null;
  const pending = [];
  return {
    ms: null,
    begin() {
      if (!ext || active || pending.length >= 2) return;
      active = gl.createQuery();
      gl.beginQuery(ext.TIME_ELAPSED_EXT, active);
    },
    end() {
      if (!active) return;
      gl.endQuery(ext.TIME_ELAPSED_EXT);
      pending.push(active);
      active = null;
    },
    poll() {
      const q = pending[0];
      if (!q || !gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE)) return;
      pending.shift();
      if (!gl.getParameter(ext.GPU_DISJOINT_EXT)) this.ms = gl.getQueryParameter(q, gl.QUERY_RESULT) / 1e6;
      gl.deleteQuery(q);
    },
  };
}

function makeRain() {
  let seed = 0x51f15e;
  const rand = () => ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296);
  const p = new Float32Array(2400 * 3);
  for (let k = 0; k < p.length; k += 3) {
    p[k] = (rand() - 0.5) * 120;
    p[k + 1] = (rand() - 0.5) * 120;
    p[k + 2] = rand() * 70;
  }
  const g = new THREE.BufferGeometry();
  g.setAttribute("position", new THREE.BufferAttribute(p, 3));
  const m = new THREE.PointsMaterial({ color: 0xb8c8d8, size: 0.065, transparent: true, opacity: 0.4, depthWrite: false });
  const rain = new THREE.Points(g, m);
  rain.visible = false;
  rain.frustumCulled = false;
  rain.renderOrder = 2;
  return rain;
}

function smooth(a, b, x) {
  const t = Math.max(0, Math.min(1, (x - a) / (b - a)));
  return t * t * (3 - 2 * t);
}
