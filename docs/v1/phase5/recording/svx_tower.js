const sv = window.__structvox;
document.getElementById('ui').style.display = 'none';
const cap = document.createElement('div');
cap.textContent = 'structvox (converged RBSM, 12.5 cm voxels): 10-storey tower, front row of ground columns blasted';
cap.style.cssText = 'position:fixed;left:12px;top:12px;z-index:99999;padding:6px 10px;background:rgba(0,0,0,0.6);color:#fff;font:600 16px/1.3 system-ui,sans-serif;border-radius:4px;pointer-events:none';
document.body.appendChild(cap);
sv.engine.setParams({ compliance: 9, amplification: 1, fragility: 0.1, damping: 0.05, debugView: 0, paused: false });
sv.noclip(true);
sv.teleport(-14, -14, 6);
sv.look(45, 16);
await new Promise((r) => setTimeout(r, 1200));
// the front row of ground columns (x = 5 .. 14.75 m at y = 5 m) and the next row's first
for (const x of [5.0, 8.25, 11.5, 14.75]) sv.engine.blast([x, 5.0, 1.0], 1.6, 4e6);
sv.engine.blast([5.0, 8.25, 1.0], 1.6, 4e6);
