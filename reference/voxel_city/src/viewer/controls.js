import * as THREE from "three";

const VOXEL = 0.125;
const UP = new THREE.Vector3(0, 0, 1);

/**
 * Camera controllers (z-up world, meters):
 *   orbit – overview around a target; drag rotate, right-drag pan, wheel zoom
 *   fly   – free flight with mouse look (pointer lock), WASD/QE, shift = fast
 *   walk  – first person with gravity and voxel collision; climbs 1–2 voxel
 *           steps automatically so stairs and curbs are walkable, and
 *           climbs ladders (look up + W to climb, look down or S to descend,
 *           Space to let go)
 *
 * The walker moves in sub-steps of at most STEP, so no speed carries it
 * through a one-voxel slab; a walker found inside solid voxels (a teleport
 * onto a wall, a part that streamed in around it) is moved to the nearest
 * free spot; one that falls far below every ground known there is reported
 * (`onLost`) so the viewer can put it back on the ground.
 */

/** Longest move (m) per collision sub-step: under one voxel. */
const STEP = 0.1;
/** Fastest fall (m/s). */
const TERMINAL = 50;
export class CameraRig {
  constructor(camera, dom, { solidAt, climbAt = () => false, floorAt = () => null, onLost = () => {} }) {
    this.camera = camera;
    this.dom = dom;
    this.solidAt = solidAt;
    this.climbAt = climbAt;
    /** lowest ground known under a point (m) or null: how far a walker may fall before it is lost */
    this.floorAt = floorAt;
    this.onLost = onLost;
    /** walk: what the walker is doing ("walking", "waiting" for its ground to stream in) */
    this.status = "walking";
    /** walk mode without collision (passes through walls, no gravity) */
    this.noclip = false;
    this.maxOrbit = 40000;
    this.onLadder = false;
    this.letGo = 0;
    this.mode = "orbit";
    this.keys = new Set();
    this.target = new THREE.Vector3(0, 0, 8);
    this.yaw = -Math.PI * 0.75;
    this.pitch = 0.75;
    this.distance = 420;
    this.pos = new THREE.Vector3(0, 0, 30);
    this.vel = new THREE.Vector3();
    this.onGround = false;
    this.speed = 1;
    this.drag = null;
    this.locked = false;
    this.eye = 1.6;
    this.radius = 0.25;
    this.height = 1.75;
    this.bind();
  }

  bind() {
    const d = this.dom;
    this._kd = (e) => {
      if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "SELECT")) return;
      this.keys.add(e.code);
      if (e.code === "Space" && this.mode === "walk" && (this.onGround || this.onLadder)) {
        this.vel.z = this.onLadder ? 2.5 : 5.2;
        this.onGround = false;
        if (this.onLadder) this.letGo = 0.4;
      }
    };
    this._ku = (e) => this.keys.delete(e.code);
    // (every listener kept, so dispose removes them: a regenerated world makes a new rig)
    this._on = [];
    const on = (target, type, fn, opts) => {
      target.addEventListener(type, fn, opts);
      this._on.push([target, type, fn, opts]);
    };
    on(window, "keydown", this._kd);
    on(window, "keyup", this._ku);
    // (a lost focus loses the key-ups: forget the keys held)
    on(window, "blur", () => this.keys.clear());
    on(d, "contextmenu", (e) => e.preventDefault());
    // pointers: a mouse drags (orbit) or takes the pointer (fly, walk); a finger drags to orbit or
    // to look, two fingers pinch to zoom (orbit)
    const touches = new Map();
    let pinch = null;
    on(d, "pointerdown", (e) => {
      if (e.pointerType === "touch") {
        touches.set(e.pointerId, { x: e.clientX, y: e.clientY });
        d.setPointerCapture?.(e.pointerId);
        if (touches.size === 2) {
          const [a, b] = [...touches.values()];
          pinch = { d: Math.hypot(a.x - b.x, a.y - b.y), dist: this.distance };
          this.drag = null;
        } else this.drag = { x: e.clientX, y: e.clientY, button: 0, shift: false, touch: true };
        return;
      }
      if (this.mode !== "orbit") {
        if (!this.locked && !this.noLock) d.requestPointerLock?.();
        return;
      }
      this.drag = { x: e.clientX, y: e.clientY, button: e.button, shift: e.shiftKey };
    });
    const up = (e) => {
      touches.delete(e.pointerId);
      if (touches.size < 2) pinch = null;
      if (!touches.size || e.pointerType !== "touch") this.drag = null;
    };
    on(window, "pointerup", up);
    on(window, "pointercancel", up);
    on(window, "pointermove", (e) => {
      if (e.pointerType === "touch" && touches.has(e.pointerId)) {
        touches.set(e.pointerId, { x: e.clientX, y: e.clientY });
        if (pinch && this.mode === "orbit") {
          const [a, b] = [...touches.values()];
          const dd = Math.hypot(a.x - b.x, a.y - b.y);
          if (dd > 0) this.distance = Math.max(3, Math.min(this.maxOrbit, (pinch.dist * pinch.d) / dd));
          return;
        }
        if (this.drag?.touch && this.mode !== "orbit") {
          // a finger looks round in fly and walk
          this.yaw -= (e.clientX - this.drag.x) * 0.005;
          this.pitch = Math.max(-1.5, Math.min(1.5, this.pitch - (e.clientY - this.drag.y) * 0.005));
          this.drag.x = e.clientX;
          this.drag.y = e.clientY;
          return;
        }
      }
      if (this.mode !== "orbit") {
        if (!this.locked) return;
        this.yaw -= e.movementX * 0.0022;
        this.pitch = Math.max(-1.5, Math.min(1.5, this.pitch - e.movementY * 0.0022));
        return;
      }
      if (!this.drag) return;
      const dx = e.clientX - this.drag.x;
      const dy = e.clientY - this.drag.y;
      this.drag.x = e.clientX;
      this.drag.y = e.clientY;
      if (this.drag.button === 2 || this.drag.shift) {
        // drag moves the ground under the cursor
        const s = this.distance * 0.0016;
        const rx = -Math.sin(this.yaw);
        const ry = Math.cos(this.yaw);
        const fx = -Math.cos(this.yaw);
        const fy = -Math.sin(this.yaw);
        this.target.x += (-dx * rx + dy * fx) * s;
        this.target.y += (-dx * ry + dy * fy) * s;
      } else {
        this.yaw -= dx * 0.005;
        this.pitch = Math.max(0.05, Math.min(1.52, this.pitch + dy * 0.005));
      }
    });
    on(
      d,
      "wheel",
      (e) => {
        e.preventDefault();
        if (this.mode === "orbit") {
          this.distance = Math.max(3, Math.min(this.maxOrbit, this.distance * Math.exp(e.deltaY * 0.0012)));
        } else {
          this.speed = Math.max(0.1, Math.min(40, this.speed * Math.exp(-e.deltaY * 0.001)));
        }
      },
      { passive: false },
    );
    on(document, "pointerlockchange", () => {
      this.locked = document.pointerLockElement === d;
      if (!this.locked) this.keys.clear();
    });
  }

  dispose() {
    for (const [target, type, fn, opts] of this._on) target.removeEventListener(type, fn, opts);
    this._on = [];
  }

  setMode(mode) {
    if (mode === this.mode) return;
    const cam = this.camera;
    if (mode === "orbit") {
      const dir = new THREE.Vector3();
      cam.getWorldDirection(dir);
      this.target.copy(cam.position).addScaledVector(dir, Math.min(60, this.distance));
      this.distance = cam.position.distanceTo(this.target);
      this.yaw = Math.atan2(-dir.y, -dir.x);
      this.pitch = Math.asin(Math.max(-1, Math.min(1, -dir.z)));
      document.exitPointerLock?.();
    } else {
      this.pos.copy(cam.position);
      if (mode === "walk") this.pos.z -= this.eye;
      const dir = new THREE.Vector3();
      cam.getWorldDirection(dir);
      this.yaw = Math.atan2(dir.y, dir.x);
      this.pitch = Math.asin(Math.max(-1, Math.min(1, dir.z)));
      this.vel.set(0, 0, 0);
    }
    this.mode = mode;
  }

  /** Place the walker at a point (meters, feet position). */
  teleport(x, y, z) {
    this.pos.set(x, y, z);
    this.vel.set(0, 0, 0);
    this.target.set(x, y, z);
  }

  update(dt) {
    dt = Math.min(dt, 0.05);
    if (this.mode === "orbit") this.updateOrbit(dt);
    else if (this.mode === "fly") this.updateFly(dt);
    else this.updateWalk(dt);
  }

  updateOrbit(dt) {
    const k = this.keys;
    const s = this.distance * 0.9 * dt * (k.has("ShiftLeft") ? 3 : 1);
    const fx = -Math.cos(this.yaw);
    const fy = -Math.sin(this.yaw);
    if (k.has("KeyW")) this.target.x += fx * s, (this.target.y += fy * s);
    if (k.has("KeyS")) this.target.x -= fx * s, (this.target.y -= fy * s);
    if (k.has("KeyA")) this.target.x += -fy * s, (this.target.y += fx * s);
    if (k.has("KeyD")) this.target.x -= -fy * s, (this.target.y -= fx * s);
    if (k.has("KeyQ")) this.target.z -= s * 0.5;
    if (k.has("KeyE")) this.target.z += s * 0.5;
    const cp = Math.cos(this.pitch);
    const off = new THREE.Vector3(Math.cos(this.yaw) * cp, Math.sin(this.yaw) * cp, Math.sin(this.pitch)).multiplyScalar(this.distance);
    this.camera.position.copy(this.target).add(off);
    this.camera.up.copy(UP);
    this.camera.lookAt(this.target);
  }

  lookDir() {
    const cp = Math.cos(this.pitch);
    return new THREE.Vector3(Math.cos(this.yaw) * cp, Math.sin(this.yaw) * cp, Math.sin(this.pitch));
  }

  updateFly(dt) {
    const k = this.keys;
    const base = 12 * this.speed * (k.has("ShiftLeft") ? 5 : 1);
    const f = this.lookDir();
    const r = new THREE.Vector3().crossVectors(f, UP).normalize();
    const m = new THREE.Vector3();
    if (k.has("KeyW")) m.add(f);
    if (k.has("KeyS")) m.sub(f);
    if (k.has("KeyD")) m.add(r);
    if (k.has("KeyA")) m.sub(r);
    if (k.has("KeyE") || k.has("Space")) m.z += 1;
    if (k.has("KeyQ") || k.has("KeyC")) m.z -= 1;
    if (m.lengthSq() > 0) this.pos.addScaledVector(m.normalize(), base * dt);
    this.camera.position.copy(this.pos);
    this.camera.up.copy(UP);
    this.camera.lookAt(this.pos.clone().add(f));
  }

  /** AABB (feet at pos) overlaps any solid voxel? unknown voxels count as solid. */
  blocked(px, py, pz) {
    return this.overlap(px, py, pz) !== false;
  }

  /** The walker's box at (px, py, pz): false if free, true if it overlaps a solid voxel, null if any voxel is unknown. */
  overlap(px, py, pz) {
    let unknown = false;
    const r = this.radius;
    const x0 = Math.floor((px - r) / VOXEL);
    const x1 = Math.floor((px + r) / VOXEL);
    const y0 = Math.floor((py - r) / VOXEL);
    const y1 = Math.floor((py + r) / VOXEL);
    const z0 = Math.floor((pz + 0.001) / VOXEL);
    const z1 = Math.floor((pz + this.height) / VOXEL);
    for (let z = z0; z <= z1; z += 1)
      for (let y = y0; y <= y1; y += 1)
        for (let x = x0; x <= x1; x += 1) {
          const s = this.solidAt(x, y, z);
          if (s) return true;
          if (s === null) unknown = true;
        }
    return unknown ? null : false;
  }

  /**
   * A walker inside solid voxels: the nearest free spot, round it first (a
   * wall it was put against), then up (a slab it was put under), or null.
   */
  freeSpot(px, py, pz) {
    for (let r = 0.25; r <= 1.5; r += 0.25)
      for (let a = 0; a < 8; a += 1) {
        const x = px + Math.cos((a * Math.PI) / 4) * r;
        const y = py + Math.sin((a * Math.PI) / 4) * r;
        for (let dz = 0; dz <= 2; dz += 1) if (this.overlap(x, y, pz + dz * VOXEL) === false) return [x, y, pz + dz * VOXEL];
      }
    for (let dz = 1; dz <= 480; dz += 1) if (this.overlap(px, py, pz + dz * VOXEL) === false) return [px, py, pz + dz * VOXEL];
    return null;
  }

  /** Any climbable voxel within arm's reach (so a fall down a shaft grabs its ladder)? */
  touchingLadder(px, py, pz) {
    const r = this.radius + 0.4;
    const x0 = Math.floor((px - r) / VOXEL);
    const x1 = Math.floor((px + r) / VOXEL);
    const y0 = Math.floor((py - r) / VOXEL);
    const y1 = Math.floor((py + r) / VOXEL);
    // reach a little below the feet so the top rung still holds while
    // stepping off onto the floor above
    const z0 = Math.floor((pz - 0.25) / VOXEL);
    const z1 = Math.floor((pz + this.height * 0.6) / VOXEL);
    for (let z = z0; z <= z1; z += 1)
      for (let y = y0; y <= y1; y += 1)
        for (let x = x0; x <= x1; x += 1) if (this.climbAt(x, y, z)) return true;
    return false;
  }

  updateWalk(dt) {
    const k = this.keys;
    const speed = (k.has("ShiftLeft") ? 7.5 : 3.2) * Math.max(0.5, Math.min(3, this.speed));
    const fwd = new THREE.Vector3(Math.cos(this.yaw), Math.sin(this.yaw), 0);
    const right = new THREE.Vector3(fwd.y, -fwd.x, 0);
    const want = new THREE.Vector3();
    if (k.has("KeyW")) want.add(fwd);
    if (k.has("KeyS")) want.sub(fwd);
    if (k.has("KeyD")) want.add(right);
    if (k.has("KeyA")) want.sub(right);
    if (want.lengthSq() > 0) want.normalize().multiplyScalar(speed);
    if (this.noclip) {
      // a ghost: no gravity, no walls (Space up, C down)
      this.pos.addScaledVector(want, dt);
      if (k.has("Space") || k.has("KeyE")) this.pos.z += speed * dt;
      if (k.has("KeyC") || k.has("KeyQ")) this.pos.z -= speed * dt;
      this.vel.set(0, 0, 0);
      this.status = "noclip";
      this.applyCamera();
      return;
    }
    this.vel.x = want.x;
    this.vel.y = want.y;
    // freeze until the ground here is streamed in
    const here = this.overlap(this.pos.x, this.pos.y, this.pos.z);
    const under = this.solidAt(Math.floor(this.pos.x / VOXEL), Math.floor(this.pos.y / VOXEL), Math.floor(this.pos.z / VOXEL) - 1);
    if (here === null || under === null) {
      this.status = "waiting";
      this.applyCamera();
      return;
    }
    // inside solid voxels (put onto a wall, or something streamed in round it): step out first
    if (here) {
      const free = this.freeSpot(this.pos.x, this.pos.y, this.pos.z);
      if (free) this.pos.set(...free);
      this.vel.set(0, 0, 0);
      this.applyCamera();
      return;
    }
    this.status = "walking";
    this.letGo = Math.max(0, this.letGo - dt);
    this.onLadder = this.letGo === 0 && this.touchingLadder(this.pos.x, this.pos.y, this.pos.z);
    if (this.onLadder) {
      // ladders: forward climbs (or descends when looking down), back descends
      const climb = 2.4;
      this.vel.z = 0;
      if (k.has("KeyW")) this.vel.z = this.pitch < -0.45 ? -climb : climb;
      if (k.has("KeyS")) this.vel.z = -climb;
    } else {
      this.vel.z = Math.max(-TERMINAL, this.vel.z - 18 * dt);
    }
    // in sub-steps of at most STEP, so nothing is passed through
    const n = Math.max(1, Math.ceil((Math.max(Math.abs(this.vel.x), Math.abs(this.vel.y), Math.abs(this.vel.z)) * dt) / STEP));
    const h = dt / n;
    for (let i = 0; i < n; i += 1) this.subStep(h);
    // fallen far below every ground known here (a hole in what has streamed in): report it
    const floor = this.floorAt(this.pos.x, this.pos.y);
    if (floor !== null && this.pos.z < floor - 8) {
      this.vel.set(0, 0, 0);
      this.onLost(this.pos.x, this.pos.y);
    }
    this.applyCamera();
  }

  /** One collision sub-step of the walker: horizontal moves with auto step-up (max 2 voxels), then vertical. */
  subStep(dt) {
    for (const axis of ["x", "y"]) {
      const d = this.vel[axis] * dt;
      if (!d) continue;
      const next = this.pos.clone();
      next[axis] += d;
      if (!this.blocked(next.x, next.y, next.z)) {
        this.pos.copy(next);
        continue;
      }
      let stepped = false;
      if (this.onGround || this.onLadder) {
        for (let s = 1; s <= 2; s += 1) {
          const up = next.clone();
          up.z = (Math.floor(this.pos.z / VOXEL + 0.001) + s) * VOXEL;
          if (!this.blocked(up.x, up.y, up.z)) {
            this.pos.copy(up);
            stepped = true;
            break;
          }
        }
      }
      if (!stepped) this.vel[axis] = 0;
    }
    const nz = this.pos.clone();
    nz.z += this.vel.z * dt;
    if (!this.blocked(nz.x, nz.y, nz.z)) {
      this.pos.copy(nz);
      this.onGround = false;
    } else {
      if (this.vel.z < 0) {
        this.onGround = true;
        this.pos.z = Math.floor(this.pos.z / VOXEL + 0.001) * VOXEL;
      }
      this.vel.z = 0;
    }
  }

  applyCamera() {
    this.camera.position.set(this.pos.x, this.pos.y, this.pos.z + this.eye);
    this.camera.up.copy(UP);
    this.camera.lookAt(this.camera.position.clone().add(this.lookDir()));
  }
}
