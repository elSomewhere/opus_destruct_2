const d = window.__destructionDebug;
const st = document.createElement('style');
st.textContent = `#topBar, .overlay, #viewport ~ *, #app > :not(#viewport) { display: none !important; }
  #viewport { position: fixed !important; inset: 0 !important; width: 100vw !important; height: 100vh !important; }
  #renderCanvas { width: 100% !important; height: 100% !important; }`;
document.head.appendChild(st);
const cap = document.createElement('div');
cap.textContent = 'prototype (XPBD, 1 m cells): building3, front row of ground columns blasted';
cap.style.cssText = 'position:fixed;left:12px;top:12px;z-index:99999;padding:6px 10px;background:rgba(0,0,0,0.6);color:#fff;font:600 16px/1.3 system-ui,sans-serif;border-radius:4px;pointer-events:none';
document.body.appendChild(cap);
window.dispatchEvent(new Event('resize'));
await new Promise((r) => setTimeout(r, 300));
const R = d.renderer;
R.rotation = Math.PI / 4 + Math.PI;  // looking at the (0, 0) corner
R.tilt = 0.35;
R.zoom = 17;
const P = [5, 4, 6];
const r = R.rotation, t = R.tilt;
const right = [Math.cos(r), -Math.sin(r), 0], up = [-Math.sin(r) * Math.sin(t), -Math.cos(r) * Math.sin(t), Math.cos(t)];
R.panX = -R.zoom * (right[0] * P[0] + right[1] * P[1] + right[2] * P[2]);
R.panY = -R.zoom * (up[0] * P[0] + up[1] * P[1] + up[2] * P[2]);
R.render();
await new Promise((r) => setTimeout(r, 1200));
const slider = document.getElementById('blastRadius');
slider.value = '3.5';
slider.dispatchEvent(new Event('input', { bubbles: true }));
for (const ix of [0, 3, 6, 9]) await d.handlePrimaryAction({ ix, iy: 0, iz: 1, id: -1 }, { shiftKey: false });
await d.handlePrimaryAction({ ix: 0, iy: 3, iz: 1, id: -1 }, { shiftKey: false });
