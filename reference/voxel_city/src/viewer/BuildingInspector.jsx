import { useEffect, useRef, useState } from "react";

const ROOM_COLORS = {
  corridor: "#d8d2c4",
  lobby: "#e6dcc4",
  hall: "#ddd5c6",
  foyer: "#ddd5c6",
  landing: "#d0c8b8",
  stair: "#9b8f7c",
  elevator: "#6f7a86",
  shaft: "#555",
  living: "#e7b98a",
  kitchen: "#f0d98a",
  dining: "#eecb8a",
  bedroom: "#a9c7e8",
  bath: "#9fd8d0",
  wc: "#9fd8d0",
  closet: "#c9bba5",
  storage: "#b9ad98",
  utility: "#b0a898",
  office: "#b8c9a4",
  openOffice: "#c8d9b4",
  meeting: "#9fbf8a",
  breakroom: "#e8d08a",
  restroom: "#9fd8d0",
  retail: "#e8a0a0",
  backroom: "#c99a9a",
  warehouse: "#b8b8c0",
  deck: "#a5a9ae",
  classroom: "#e8d9a8",
  gym: "#c9a36b",
  cafeteria: "#b8e0c8",
  library: "#b0c8a0",
  teachers: "#e8c0a0",
  loading: "#a8a8b0",
  garage: "#a0a0a0",
  study: "#c7b8e8",
  laundry: "#a8d0e8",
  mechanical: "#8a8a8a",
  bike: "#b0c0a0",
  parking: "#8c9096",
};

/** Floor-by-floor plan viewer for one building. */
export function BuildingInspector({ engine, buildingId, onClose }) {
  const [data, setData] = useState(null);
  const [floorIdx, setFloorIdx] = useState(0);
  const canvasRef = useRef(null);

  useEffect(() => {
    let alive = true;
    setData(null);
    engine?.pool.request("building", { buildingId }).then((d) => {
      if (!alive) return;
      setData(d);
      setFloorIdx(d?.plan ? d.plan.floors.findIndex((f) => f.index === 0) : 0);
    });
    return () => {
      alive = false;
    };
  }, [engine, buildingId]);

  const floor = data?.plan?.floors?.[floorIdx];

  useEffect(() => {
    const c = canvasRef.current;
    if (!c || !floor) return;
    const ctx = c.getContext("2d");
    const { U, V, cells, rooms } = floor;
    const s = Math.min(c.width / U, c.height / V);
    ctx.fillStyle = "#1c2026";
    ctx.fillRect(0, 0, c.width, c.height);
    const img = ctx.createImageData(U, V);
    const hex = (h) => [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)];
    const roomCol = rooms.map((r) => hex(ROOM_COLORS[r.type] ?? "#cccccc"));
    for (let v = 0; v < V; v += 1) {
      for (let u = 0; u < U; u += 1) {
        const lab = cells[u + v * U];
        let col = [28, 32, 38];
        if (lab === 1) col = [40, 40, 44];
        else if (lab === 2) col = [70, 70, 76];
        else if (lab === 3) col = [250, 250, 250];
        else if (lab >= 16) col = roomCol[lab - 16] ?? [200, 200, 200];
        const o = (u + v * U) * 4;
        img.data[o] = col[0];
        img.data[o + 1] = col[1];
        img.data[o + 2] = col[2];
        img.data[o + 3] = 255;
      }
    }
    const tmp = document.createElement("canvas");
    tmp.width = U;
    tmp.height = V;
    tmp.getContext("2d").putImageData(img, 0, 0);
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(tmp, 0, 0, U * s, V * s);
    // ramps up to the next floor: chevrons pointing up the slope (+v)
    ctx.strokeStyle = "rgba(230,190,40,0.9)";
    ctx.lineWidth = 1.5;
    for (const { rect: r } of floor.ramps ?? []) {
      ctx.strokeRect(r.x0 * s, r.y0 * s, (r.x1 - r.x0 + 1) * s, (r.y1 - r.y0 + 1) * s);
      const cx = ((r.x0 + r.x1 + 1) / 2) * s;
      const hw = ((r.x1 - r.x0) / 2 - 3) * s;
      for (let v = r.y0 + 12; v < r.y1 - 6; v += 24) {
        ctx.beginPath();
        ctx.moveTo(cx - hw, v * s);
        ctx.lineTo(cx, (v + 8) * s);
        ctx.lineTo(cx + hw, v * s);
        ctx.stroke();
      }
    }
    ctx.fillStyle = "rgba(60,40,20,0.55)";
    for (const [x0, y0, x1, y1] of floor.furniture) ctx.fillRect(x0 * s, y0 * s, (x1 - x0 + 1) * s, (y1 - y0 + 1) * s);
    ctx.font = "11px ui-monospace, monospace";
    ctx.fillStyle = "#222";
    ctx.textAlign = "center";
    for (const r of rooms) {
      const a = r.rects[0];
      if (!a) continue;
      if ((a.x1 - a.x0) * s < 30) continue;
      ctx.fillText(r.type, ((a.x0 + a.x1) / 2) * s, ((a.y0 + a.y1) / 2) * s + 4);
    }
    ctx.fillStyle = "#ffd84a";
    ctx.fillText("street ↑ front", (U * s) / 2, 12);
  }, [floor]);

  return (
    <div className="inspector">
      <div className="mapBar">
        <b>{data?.envelope?.archetype ?? "…"}</b>
        <span className="small">
          {data?.envelope ? `${data.envelope.floors} floors · ${data.envelope.basements} basements · ${data.envelope.style}` : ""}
        </span>
        <button onClick={onClose}>×</button>
      </div>
      {data?.plan ? (
        <>
          <div className="floorTabs">
            {data.plan.floors.map((f, k) => (
              <button key={k} className={k === floorIdx ? "on" : ""} onClick={() => setFloorIdx(k)}>
                {f.index < 0 ? `B${-f.index}` : f.index === 0 ? "G" : f.index}
              </button>
            ))}
          </div>
          <canvas ref={canvasRef} width={520} height={520} />
          {data.plan.issues?.length ? <div className="small warn">{data.plan.issues.length} validation issues</div> : <div className="small ok">all rooms reachable</div>}
        </>
      ) : (
        <div className="small">{data ? "No interior plan" : "Loading…"}</div>
      )}
    </div>
  );
}
