import { useEffect, useRef, useState } from "react";
import { BuildingInspector } from "./BuildingInspector.jsx";

const VOXEL = 0.125;
const DISTRICT_COLORS = {
  downtown: "#c96b5a",
  midtown: "#d98f5c",
  mixed: "#e0b35e",
  residential: "#9cc27a",
  suburban: "#b7d98c",
  industrial: "#9a9aa8",
  park: "#4f9a4a",
  rural: "#6f8f4f",
};
const ARCH_COLORS = {
  house: "#e8d6b0",
  rowhouse: "#d9a47a",
  walkup: "#c98a6a",
  midrise: "#b77862",
  office: "#7f95b0",
  tower: "#5d7896",
  warehouse: "#8c8c96",
  factory: "#77777f",
  garage: "#9a8f7a",
  school: "#d8b86a",
};
const ROAD_COLORS = {
  arterial: "#f3f1ea",
  collector: "#e6e3da",
  local: "#d9d6cc",
  alley: "#b8b3a8",
  pedestrian: "#e9d8b9",
  rural: "#cfc6a8",
};

/**
 * 2D vector map of the generated plan. Wheel zoom, drag pan, click a
 * building to inspect its floors, double-click to teleport there.
 */

/** Is (x, y) inside a convex polygon [[x, y], ...] (either winding)? */
function inPoly(poly, x, y) {
  let sign = 0;
  for (let k = 0; k < poly.length; k += 1) {
    const [ax, ay] = poly[k];
    const [bx, by] = poly[(k + 1) % poly.length];
    const c = Math.sign((bx - ax) * (y - ay) - (by - ay) * (x - ax));
    if (c !== 0 && sign !== 0 && c !== sign) return false;
    if (c !== 0) sign = c;
  }
  return true;
}

export function MapPanel({ engine, center, onTeleport, onClose }) {
  const canvasRef = useRef(null);
  const [view, setView] = useState(() => ({ cx: center[0], cy: center[1], scale: 1.2 }));
  const [data, setData] = useState(null);
  const [layer, setLayer] = useState("districts");
  const [selected, setSelected] = useState(null);
  const dragRef = useRef(null);

  const world = layer === "world";
  useEffect(() => {
    if (!engine) return;
    const halfW = 700 * view.scale;
    const halfH = 450 * view.scale;
    const rect = {
      x0: Math.floor((view.cx - halfW) / VOXEL),
      y0: Math.floor((view.cy - halfH) / VOXEL),
      x1: Math.ceil((view.cx + halfW) / VOXEL),
      y1: Math.ceil((view.cy + halfH) / VOXEL),
    };
    let alive = true;
    const t = setTimeout(() => {
      if (world) engine.pool.request("overview", { rect, w: 350, h: 225 }).then((d) => alive && setData({ overview: d }));
      else engine.pool.request("map", { rect }).then((d) => alive && setData(d));
    }, 60);
    return () => {
      alive = false;
      clearTimeout(t);
    };
  }, [engine, view.cx, view.cy, view.scale, world]);

  useEffect(() => {
    const c = canvasRef.current;
    if (!c || !data) return;
    if (data.overview) {
      drawOverview(c, data.overview, view, center);
      return;
    }
    if (!data.subcells) return;
    const ctx = c.getContext("2d");
    const W = c.width;
    const H = c.height;
    ctx.fillStyle = "#1c2026";
    ctx.fillRect(0, 0, W, H);
    const tx = (x) => (x * VOXEL - view.cx) / view.scale + W / 2;
    const ty = (y) => (y * VOXEL - view.cy) / view.scale + H / 2;
    // (a turned building's rect carries its outline, `poly`)
    const rectPath = (r) => {
      if (!r.poly) return ctx.rect(tx(r.x0), ty(r.y0), (r.x1 - r.x0 + 1) * VOXEL / view.scale, (r.y1 - r.y0 + 1) * VOXEL / view.scale);
      r.poly.forEach(([x, y], k) => (k ? ctx.lineTo(tx(x), ty(y)) : ctx.moveTo(tx(x), ty(y))));
      ctx.closePath();
    };

    for (const s of data.subcells) {
      ctx.fillStyle = DISTRICT_COLORS[s.district] ?? "#555";
      ctx.globalAlpha = layer === "districts" ? 0.55 : 0.18;
      ctx.beginPath();
      rectPath(s.rect);
      ctx.fill();
    }
    ctx.globalAlpha = 1;
    for (const sp of data.spaces) {
      ctx.fillStyle = sp.kind === "park" ? "#3f7d3c" : sp.kind === "riverside" ? "#6f9a5c" : sp.kind === "plaza" ? "#b9ae98" : sp.kind === "parking" ? "#555a60" : "#4b7d8a";
      ctx.beginPath();
      rectPath(sp.rect);
      ctx.fill();
    }
    ctx.strokeStyle = "rgba(0,0,0,0.25)";
    ctx.lineWidth = 1;
    for (const l of data.lots) {
      ctx.beginPath();
      rectPath(l.rect);
      ctx.stroke();
    }
    if (data.rivers) {
      const rv = data.rivers;
      ctx.fillStyle = "#3f78a8";
      const cs = (rv.step * VOXEL) / view.scale + 0.6;
      for (let j = 0; j < rv.h; j += 1)
        for (let i = 0; i < rv.w; i += 1) if (rv.mask[i + j * rv.w]) ctx.fillRect(tx(rv.x0 + i * rv.step), ty(rv.y0 + j * rv.step), cs, cs);
    }
    for (const r of data.roads) {
      ctx.strokeStyle = ROAD_COLORS[r.cls] ?? "#ccc";
      ctx.lineWidth = Math.max(1, (r.hr * 2 * VOXEL) / view.scale);
      ctx.lineCap = "butt";
      ctx.beginPath();
      r.pts.forEach(([x, y], k) => (k ? ctx.lineTo(tx(x), ty(y)) : ctx.moveTo(tx(x), ty(y))));
      ctx.stroke();
    }
    for (const b of data.buildings) {
      const col = ARCH_COLORS[b.archetype] ?? "#aaa";
      for (const t of b.tiers) {
        ctx.fillStyle = col;
        ctx.globalAlpha = t.f0 === 0 ? 0.85 : 1;
        for (const r of t.rects) {
          ctx.beginPath();
          rectPath(r);
          ctx.fill();
        }
      }
      ctx.globalAlpha = 1;
      for (const a of b.annexes) {
        ctx.fillStyle = "#a89c86";
        ctx.beginPath();
        rectPath(a);
        ctx.fill();
      }
      if (selected === b.id) {
        ctx.strokeStyle = "#ffd84a";
        ctx.lineWidth = 2;
        for (const r of b.tiers[0].rects) {
          ctx.beginPath();
          rectPath(r);
          ctx.stroke();
        }
      }
    }
    if (data.highways) {
      for (const h of data.highways) {
        ctx.strokeStyle = "rgba(240,190,70,0.9)";
        ctx.lineWidth = Math.max(2, (h.width * VOXEL) / view.scale);
        ctx.beginPath();
        h.pts.forEach(([x, y], k) => (k ? ctx.lineTo(tx(x), ty(y)) : ctx.moveTo(tx(x), ty(y))));
        ctx.stroke();
      }
    }
    for (const r of data.skybridges ?? []) {
      ctx.fillStyle = "rgba(120,220,240,0.9)";
      ctx.beginPath();
      rectPath(r);
      ctx.fill();
    }
    if (data.sewers) {
      ctx.strokeStyle = "rgba(150,110,80,0.75)";
      ctx.lineWidth = 1.5;
      ctx.setLineDash([2, 3]);
      ctx.beginPath();
      for (const [[ax, ay], [bx, by]] of data.sewers.lines) {
        ctx.moveTo(tx(ax), ty(ay));
        ctx.lineTo(tx(bx), ty(by));
      }
      ctx.stroke();
      ctx.setLineDash([]);
      for (const h of data.sewers.halls) {
        ctx.fillStyle = "#9a6e50";
        ctx.fillRect(tx(h.x) - 5, ty(h.y) - 5, 10, 10);
      }
    }
    if (data.subway) {
      for (const l of data.subway.lines) {
        ctx.strokeStyle = "rgba(90,170,255,0.85)";
        ctx.setLineDash([6, 4]);
        ctx.lineWidth = 3;
        ctx.beginPath();
        l.pts.forEach(([x, y], k) => (k ? ctx.lineTo(tx(x), ty(y)) : ctx.moveTo(tx(x), ty(y))));
        ctx.stroke();
        ctx.setLineDash([]);
      }
      for (const s of data.subway.stations) {
        ctx.fillStyle = "#5aaaff";
        ctx.beginPath();
        ctx.arc(tx(s.x), ty(s.y), 6, 0, Math.PI * 2);
        ctx.fill();
      }
    }
    // deep link tunnels between sites
    if (data.siteLinks) {
      ctx.strokeStyle = "rgba(90,210,230,0.85)";
      ctx.lineWidth = 3;
      ctx.setLineDash([4, 6]);
      for (const L of data.siteLinks) {
        ctx.beginPath();
        L.pts.forEach((p, k) => (k ? ctx.lineTo(tx(p.x), ty(p.y)) : ctx.moveTo(tx(p.x), ty(p.y))));
        ctx.stroke();
      }
      ctx.setLineDash([]);
    }
    if (data.sites) {
      for (const st of data.sites) {
        ctx.strokeStyle = "#c8d86a";
        ctx.lineWidth = 2;
        ctx.setLineDash([8, 5]);
        ctx.beginPath();
        rectPath(st.rect);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.fillStyle = "#c8d86a";
        ctx.font = "12px ui-monospace, monospace";
        ctx.fillText(st.type, tx(st.rect.x0) + 4, ty(st.rect.y0) - 6);
      }
    }
    // player
    ctx.fillStyle = "#ff4a4a";
    ctx.beginPath();
    ctx.arc(tx(center[0] / VOXEL), ty(center[1] / VOXEL), 5, 0, Math.PI * 2);
    ctx.fill();
  }, [data, view, layer, selected, center]);

  const toWorld = (e) => {
    const c = canvasRef.current;
    const r = c.getBoundingClientRect();
    const px = ((e.clientX - r.left) / r.width) * c.width;
    const py = ((e.clientY - r.top) / r.height) * c.height;
    return [(px - c.width / 2) * view.scale + view.cx, (py - c.height / 2) * view.scale + view.cy];
  };

  const pick = (e) => {
    if (!data?.buildings) return null;
    const [wx, wy] = toWorld(e);
    const vx = wx / VOXEL;
    const vy = wy / VOXEL;
    for (const b of data.buildings) {
      for (const r of b.tiers[0].rects) if (vx >= r.x0 && vx <= r.x1 && vy >= r.y0 && vy <= r.y1 && (!r.poly || inPoly(r.poly, vx, vy))) return b;
    }
    return null;
  };

  return (
    <div className="mapPanel">
      <div className="mapBar">
        <b>Plan map</b>
        <select
          value={layer}
          onChange={(e) => {
            const next = e.target.value;
            setLayer(next);
            // the world layer opens at a regional scale, plan layers at street scale
            setView((v) => ({ ...v, scale: next === "world" ? Math.max(v.scale, 60) : Math.min(v.scale, 20) }));
          }}
        >
          <option value="districts">Districts</option>
          <option value="plain">Plain</option>
          <option value="world">World (biomes)</option>
        </select>
        <span className="small">wheel zoom · drag pan · click building · dbl-click teleport</span>
        <button onClick={onClose}>×</button>
      </div>
      <div className="mapBody">
        <canvas
          ref={canvasRef}
          width={1400}
          height={900}
          onWheel={(e) => {
            const f = Math.exp(e.deltaY * 0.0015);
            setView((v) => ({ ...v, scale: Math.max(0.05, Math.min(world ? 400 : 20, v.scale * f)) }));
          }}
          onMouseDown={(e) => (dragRef.current = { x: e.clientX, y: e.clientY, moved: false })}
          onMouseMove={(e) => {
            const d = dragRef.current;
            if (!d) return;
            const c = canvasRef.current;
            const k = c.width / c.getBoundingClientRect().width;
            const dx = (e.clientX - d.x) * k;
            const dy = (e.clientY - d.y) * k;
            if (Math.abs(dx) + Math.abs(dy) > 2) d.moved = true;
            d.x = e.clientX;
            d.y = e.clientY;
            setView((v) => ({ ...v, cx: v.cx - dx * v.scale, cy: v.cy - dy * v.scale }));
          }}
          onMouseUp={(e) => {
            const d = dragRef.current;
            dragRef.current = null;
            if (d && !d.moved) {
              const b = pick(e);
              setSelected(b ? b.id : null);
            }
          }}
          onDoubleClick={(e) => {
            const [wx, wy] = toWorld(e);
            onTeleport(wx, wy);
          }}
        />
        {selected && <BuildingInspector engine={engine} buildingId={selected} onClose={() => setSelected(null)} />}
      </div>
    </div>
  );
}

/** Biome / relief raster with settlements, highways and sites on top. */
function drawOverview(c, ov, view, center) {
  const ctx = c.getContext("2d");
  const W = c.width;
  const H = c.height;
  ctx.fillStyle = "#1c2026";
  ctx.fillRect(0, 0, W, H);
  const img = new ImageData(new Uint8ClampedArray(ov.rgba), ov.w, ov.h);
  const tmp = document.createElement("canvas");
  tmp.width = ov.w;
  tmp.height = ov.h;
  tmp.getContext("2d").putImageData(img, 0, 0);
  const tx = (x) => (x * VOXEL - view.cx) / view.scale + W / 2;
  const ty = (y) => (y * VOXEL - view.cy) / view.scale + H / 2;
  ctx.imageSmoothingEnabled = true;
  ctx.drawImage(tmp, tx(ov.rect.x0), ty(ov.rect.y0), tx(ov.rect.x1) - tx(ov.rect.x0), ty(ov.rect.y1) - ty(ov.rect.y0));
  ctx.strokeStyle = "rgba(240,190,70,0.95)";
  ctx.lineWidth = 2;
  for (const h of ov.highways) {
    ctx.beginPath();
    h.pts.forEach(([x, y], k) => (k ? ctx.lineTo(tx(x), ty(y)) : ctx.moveTo(tx(x), ty(y))));
    ctx.stroke();
  }
  ctx.font = "12px ui-monospace, monospace";
  ctx.fillStyle = "rgba(255,245,220,0.9)";
  for (const v of ov.villages ?? []) {
    ctx.beginPath();
    ctx.arc(tx(v.x), ty(v.y), 1.8, 0, Math.PI * 2);
    ctx.fill();
  }
  for (const s of ov.cities) {
    ctx.fillStyle = "rgba(20,20,20,0.75)";
    ctx.fillText(s.flavor, tx(s.x) + 6, ty(s.y) - 6);
    ctx.fillStyle = "#fff";
    ctx.beginPath();
    ctx.arc(tx(s.x), ty(s.y), 3, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.strokeStyle = "rgba(90,210,230,0.9)";
  ctx.setLineDash([3, 4]);
  for (const L of ov.links ?? []) {
    ctx.beginPath();
    L.pts.forEach((p, k) => (k ? ctx.lineTo(tx(p.x), ty(p.y)) : ctx.moveTo(tx(p.x), ty(p.y))));
    ctx.stroke();
  }
  ctx.setLineDash([]);
  ctx.strokeStyle = "#e05050";
  for (const st of ov.sites) {
    ctx.strokeRect(tx(st.rect.x0), ty(st.rect.y0), Math.max(4, tx(st.rect.x1) - tx(st.rect.x0)), Math.max(4, ty(st.rect.y1) - ty(st.rect.y0)));
  }
  // legend
  let ly = 16;
  for (const b of ov.legend) {
    ctx.fillStyle = b.color;
    ctx.fillRect(10, ly, 14, 12);
    ctx.fillStyle = "#e8e8e8";
    ctx.fillText(b.label, 30, ly + 10);
    ly += 18;
  }
  ctx.fillStyle = "#ff4a4a";
  ctx.beginPath();
  ctx.arc(tx(center[0] / VOXEL), ty(center[1] / VOXEL), 5, 0, Math.PI * 2);
  ctx.fill();
}
