"""Podgląd "oczami ESP32": nagrywa siatki 16x16 dla kolorowej szachownicy w kilku wariantach filtrowania
i zapisuje stronę HTML z animacją (ekran obok tego, co widzi ESP32, plus mapa migotania).
Użycie: python visualize.py COM4 [plik_wyjściowy.html]
"""
import importlib.util, json, os, random, statistics as st, sys, time
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
Esp, show, post = _m.Esp, _m.show, _m.post

out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "podglad_esp.html")
esp = Esp(sys.argv[1])
VARIANTS = [
    ("surowe", "Bez filtrów", "set blur 0 ; set smooth 100"),
    ("blur", "Lekkie rozmycie", "set blur 1 ; set smooth 100"),
    ("pelne", "Rozmycie + wygładzanie w czasie (ustawienie domyślne)", "set blur 1 ; set smooth 25"),
]
PATTERNS = [("Średnia szachownica 32 × 18 (pola 60 px)", 32, 18), ("Drobna szachownica 96 × 54 (pola 20 px)", 96, 54)]
rnd = random.Random(7)
data = []
for title, nx, ny in PATTERNS:
    cells = [[rnd.choice((0, 255)), rnd.choice((0, 255)), rnd.choice((0, 255))] for _ in range(nx * ny)]
    show({"mode": "cells", "nx": nx, "ny": ny, "cells": cells}, settle=1.0)
    variants = []
    for key, label, cmds in VARIANTS:
        for c in cmds.split(";"):
            esp.cmd(c.strip(), wait=0.35)
        time.sleep(1.2)
        frames = []
        for i in range(3):
            frames += esp.frames(8)
        H = len(frames[0]); W = len(frames[0][0])
        sds = []; rng = [[0] * W for _ in range(H)]
        for y in range(H):
            for x in range(W):
                r = 0
                for k in range(3):
                    v = [f[y][x][k] for f in frames]
                    sds.append(st.pstdev(v)); r = max(r, max(v) - min(v))
                rng[y][x] = r
        edge = [rng[y][x] for y in range(H) for x in range(W) if y in (0, H - 1) or x in (0, W - 1)]
        variants.append({"key": key, "label": label, "std": round(st.mean(sds), 1), "edge": round(st.mean(edge), 1),
                         "rng": rng, "w": W, "frames": ["".join("%02x%02x%02x" % p for row in f for p in row) for f in frames]})
        print(f"{title} / {label}: {len(frames)} ramek, odchylenie {st.mean(sds):.1f}, zakres na brzegu {st.mean(edge):.1f}")
    data.append({"title": title, "nx": nx, "ny": ny, "cells": "".join("%02x%02x%02x" % tuple(c) for c in cells), "variants": variants})
for c in VARIANTS[-1][2].split(";"):
    esp.cmd(c.strip(), wait=0.35)
post({"mode": "idle"})

HTML = r"""<!doctype html>
<html lang="pl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Podgląd ESP32</title>
<style>
:root { --bg:#f6f6f3; --fg:#1c1c1a; --mut:#6b6b66; --card:#ffffff; --line:#d9d9d2; --acc:#b4530a; }
@media (prefers-color-scheme: dark) { :root { --bg:#161615; --fg:#ecece8; --mut:#9a9a93; --card:#1f1f1d; --line:#35352f; --acc:#f0a35e; } }
* { box-sizing:border-box; }
body { margin:0; padding:20px 16px 40px; background:var(--bg); color:var(--fg); font:15px/1.5 system-ui, "Segoe UI", sans-serif; }
main { max-width:1040px; margin:0 auto; }
h1 { font-size:22px; margin:0 0 4px; } h2 { font-size:17px; margin:28px 0 10px; }
p { margin:6px 0; color:var(--mut); max-width:75ch; }
.ctl { display:flex; flex-wrap:wrap; gap:14px; align-items:center; margin:14px 0 4px; }
button { font:inherit; padding:6px 14px; border-radius:6px; border:1px solid var(--line); background:var(--card); color:var(--fg); cursor:pointer; }
.row { display:grid; grid-template-columns:repeat(auto-fit, minmax(210px, 1fr)); gap:14px; }
.card { background:var(--card); border:1px solid var(--line); border-radius:10px; padding:12px; }
.card h3 { font-size:14px; margin:0 0 8px; min-height:2.9em; font-weight:600; }
canvas { width:100%; aspect-ratio:16/9; image-rendering:pixelated; border-radius:4px; display:block; background:#000; }
.cap { font-size:12.5px; color:var(--mut); margin:6px 0 2px; }
.num { font-variant-numeric:tabular-nums; color:var(--fg); font-weight:600; }
.strip { color:var(--acc); }
</style></head><body><main>
<h1>Co widzi ESP32 na kolorowej szachownicy</h1>
<p>Po lewej obraz wysyłany na ekran. Dalej pole (np. 32 × 18), które ESP32 wysyła do sterownika taśmy, odtwarzana klatka po klatce
(24 nagrane ramki, każda to 1/60 s). Obraz na ekranie jest nieruchomy, więc każda zmiana w siatce to migotanie.
Taśma LED korzysta tylko z komórek na obwodzie (zaznaczona ramka).</p>
<div class="ctl"><button id="play">Pauza</button>
<label>Tempo <input id="fps" type="range" min="1" max="60" value="12"> <span id="fpsv" class="num">12</span> ramek/s</label>
<label><input id="ring" type="checkbox" checked> pokaż ramkę taśmy</label></div>
<div id="root"></div>
<h2>Jak czytać mapę migotania</h2>
<p>Mapa pod każdą animacją pokazuje, o ile zmieniała się każda komórka w ciągu 24 ramek: czarne pole to brak zmian,
im jaśniejsze, tym większe skoki (biel to zmiana o 60 lub więcej w skali 0–255).</p>
</main>
<script>
const DATA = __DATA__;
const hex = (s, i) => parseInt(s.substr(i, 2), 16);
const anims = [];
function drawGrid(cv, f, ring, w) {
  const g = cv.getContext('2d'), S = 320 / w; cv.width = 320; cv.height = 180;
  const R = f.length / (6 * w), ch = cv.height / R;
  for (let y = 0; y < R; y++) for (let x = 0; x < w; x++) {
    const i = (y * w + x) * 6; g.fillStyle = `rgb(${hex(f, i)},${hex(f, i + 2)},${hex(f, i + 4)})`;
    g.fillRect(x * S, y * ch, S, ch + 1);
  }
  if (ring) { g.strokeStyle = 'rgba(255,255,255,.85)'; g.lineWidth = 2; g.strokeRect(1, 1, cv.width - 2, cv.height - 2);
    g.strokeRect(S, ch, cv.width - 2 * S, cv.height - 2 * ch); }
}
const root = document.getElementById('root');
for (const p of DATA) {
  const h = document.createElement('h2'); h.textContent = p.title; root.appendChild(h);
  const row = document.createElement('div'); row.className = 'row'; root.appendChild(row);
  const c0 = document.createElement('div'); c0.className = 'card';
  c0.innerHTML = '<h3>Ekran (źródło)</h3><canvas></canvas><div class="cap">' + p.nx + ' × ' + p.ny + ' pól, losowe kolory</div>';
  row.appendChild(c0);
  const sc = c0.querySelector('canvas'), sg = sc.getContext('2d'); sc.width = p.nx * 10; sc.height = p.ny * 10;
  for (let y = 0; y < p.ny; y++) for (let x = 0; x < p.nx; x++) {
    const i = (y * p.nx + x) * 6; sg.fillStyle = `rgb(${hex(p.cells, i)},${hex(p.cells, i + 2)},${hex(p.cells, i + 4)})`;
    sg.fillRect(x * 10, y * 10, 10, 10);
  }
  for (const v of p.variants) {
    const c = document.createElement('div'); c.className = 'card';
    c.innerHTML = '<h3>ESP32: ' + v.label + '</h3><canvas class="a"></canvas>' +
      '<div class="cap">średnie migotanie komórki: <span class="num">' + v.std + '</span> / 255<br>' +
      '<span class="strip">skoki na obwodzie (taśma): <span class="num">' + v.edge + '</span></span></div>' +
      '<canvas class="h"></canvas><div class="cap">mapa migotania</div>';
    row.appendChild(c);
    anims.push({ cv: c.querySelector('canvas.a'), frames: v.frames, w: v.w });
    const hc = c.querySelector('canvas.h'), hg = hc.getContext('2d'); hc.width = 160; hc.height = 90;
    const R = v.rng.length, Wc = v.w;
    for (let y = 0; y < R; y++) for (let x = 0; x < Wc; x++) {
      const t = Math.min(1, v.rng[y][x] / 60), l = Math.round(255 * Math.sqrt(t));
      hg.fillStyle = `rgb(${l},${Math.round(l * .72)},${Math.round(l * .35)})`; hg.fillRect(x * 160 / Wc, y * 90 / R, 160 / Wc + 1, 90 / R + 1);
    }
  }
}
let playing = true, fps = 12, n = 0, last = 0;
const ring = document.getElementById('ring');
function tick(t) {
  if (playing && t - last >= 1000 / fps) { last = t; n++; }
  for (const a of anims) drawGrid(a.cv, a.frames[n % a.frames.length], ring.checked, a.w);
  requestAnimationFrame(tick);
}
document.getElementById('play').onclick = e => { playing = !playing; e.target.textContent = playing ? 'Pauza' : 'Odtwarzaj'; };
document.getElementById('fps').oninput = e => { fps = +e.target.value; document.getElementById('fpsv').textContent = fps; };
requestAnimationFrame(tick);
</script></body></html>
"""
open(out_path, "w", encoding="utf-8").write(HTML.replace("__DATA__", json.dumps(data, separators=(",", ":"))))
print("zapisano", out_path)
