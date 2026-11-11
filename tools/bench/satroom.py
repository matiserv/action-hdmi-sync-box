"""Satelita "obwod pokoju" (mode: room): czy BK7238 rozklada kolory krawedzi ekranu po scianach tak, jak wynika z YAML.

Wlacza na kilka sekund przelacznik "Diagnostyka: log sekcji" i czyta z logu BK trzy linie z tej samej ramki:
  sat-we   kolory sekcji tasmy TV (L od dolu, G od lewej, P od gory)
  sat-cel  kolor docelowy kazdej diody lampy (przed wygladzaniem w czasie)
  sat-wy   bajty wyslane do lampy (po wygladzaniu, jasnosci i kodowaniu gamma)
Kolory docelowe sa liczone niezaleznie w Pythonie z sat-we i porownywane z sat-cel. Dla sat-wy podawane jest, jak
daleko wyslane bajty sa od zakodowanego gamma koloru docelowego (przy ruchomym obrazie roznica to wygladzanie).

Uzycie:  python satroom.py 192.168.1.50 <yaml z kluczem API> "back:14,left:16,front:15,right:16" [gamma] [sekundy] [blur]
"""
import re
import sys
import time

from bkapi import Bk

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
host, yaml_path, walls_arg = sys.argv[1:4]
GAMMA = float(sys.argv[4]) if len(sys.argv) > 4 else 2.8
SECS = float(sys.argv[5]) if len(sys.argv) > 5 else 8
BLUR = int(sys.argv[6]) if len(sys.argv) > 6 else 0
WALLS = ["left", "front", "right", "back"]          # = boki ekranu: lewy, gorny, prawy, dolny
walls = [(w.split(":")[0].strip(), int(w.split(":")[1])) for w in walls_arg.split(",")]


def hexes(s):
    return [tuple(int(s[i + k:i + k + 2], 16) for k in (0, 2, 4)) for i in range(0, len(s) - 5, 6)]


def parse_in(line):
    sides = {"L": [], "G": [], "P": [], "D": []}
    for m in re.finditer(r"([LGPD]):((?:[0-9A-F]{6} ?)+)", line):
        sides[m.group(1)] = hexes(m.group(2).replace(" ", ""))
    return [sides["L"], sides["G"], sides["P"], sides["D"]]


def sample(ring, u):
    u %= 4.0
    side = min(int(u), 3)
    t = u - side
    px = ring[side]
    if px:
        x = min(max(t * len(px) - 0.5, 0.0), len(px) - 1.0)
        i0 = int(x)
        i1 = min(i0 + 1, len(px) - 1)
        f = x - i0
        a, b = px[i0], px[i1]
    else:
        before = 0
        while before < 3 and not ring[(side - 1 - before) % 4]:
            before += 1
        after = 0
        while after < 3 and not ring[(side + 1 + after) % 4]:
            after += 1
        a, b = ring[(side - 1 - before) % 4][-1], ring[(side + 1 + after) % 4][0]
        f = (before + t) / (before + 1 + after)
    return tuple(a[c] * (1 - f) + b[c] * f for c in range(3))


def model(ring):
    idx = [WALLS.index(w) for w, _ in walls]
    steps = [(b - a) % 4 for a, b in zip(idx, idx[1:])]
    steps.append(steps[-1] if steps else 1)
    out = []
    for (w, n), i, st in zip(walls, idx, steps):
        u0, u1 = (i, i + 1) if st == 1 else (i + 1, i)
        out += [sample(ring, u0 + (u1 - u0) * (j + 0.5) / n) for j in range(n)]
    # rozmycie wzdluz tasmy lampy: dwa razy srednia z diod w promieniu BLUR; petla zamknieta, gdy sa wszystkie 4 sciany
    closed = len({w for w, _ in walls}) == 4
    for _ in range(2 if BLUR else 0):
        n = len(out)
        new = []
        for k in range(n):
            idx = [j % n if closed else j for j in range(k - BLUR, k + BLUR + 1) if closed or 0 <= j < n]
            new.append(tuple(sum(out[q][c] for q in idx) / len(idx) for c in range(3)))
        out = new
    return out


bk = Bk(host, yaml_path)
time.sleep(1.5)
bk.set_switch("Diagnostyka: log sekcji", True)
t0 = time.time()
time.sleep(SECS)
bk.set_switch("Diagnostyka: log sekcji", False)
time.sleep(0.5)
lines = [text for t, text in bk.logs if t >= t0]
fps = [(float(m.group(1)), float(m.group(2))) for l in lines for m in [re.search(r"wideo ([\d.]+) fps, taśma ([\d.]+) fps", l)] if m]
frames, cur = [], {}
for l in lines:
    m = re.search(r"sat-(we|cel|wy) (.*)", l)
    if not m:
        continue
    cur[m.group(1)] = m.group(2).strip()
    if m.group(1) == "wy":
        if len(cur) == 3:
            frames.append(cur)
        cur = {}
print(f"ramek diagnostycznych: {len(frames)}; zerwan polaczenia: {bk.drops}")
if fps:
    print(f"wideo {sum(f[0] for f in fps) / len(fps):.1f} fps, tasma {sum(f[1] for f in fps) / len(fps):.1f} zapisow/s")
worst_cel, worst_wy = 0.0, 0
n_leds = sum(n for _, n in walls)
for fr in frames:
    ring = parse_in(fr["we"])
    exp = model(ring)
    cel, wy = hexes(fr["cel"]), hexes(fr["wy"])
    assert len(cel) == n_leds == len(wy), f"liczba diod: cel {len(cel)}, wy {len(wy)}, YAML {n_leds}"
    worst_cel = max(worst_cel, max(abs(cel[k][c] - exp[k][c]) for k in range(n_leds) for c in range(3)))
    enc = [tuple(round(255 * (v / 255.0) ** (1 / GAMMA)) for v in px) for px in exp]
    worst_wy = max(worst_wy, max(abs(wy[k][c] - enc[k][c]) for k in range(n_leds) for c in range(3)))
print(f"kolor docelowy BK - model: najwieksza roznica {worst_cel:.2f} poziomu (dopuszczalne 1: zaokraglenia)")
print(f"bajty wyslane - zakodowany gamma {GAMMA} kolor docelowy: najwieksza roznica {worst_wy} (ruchomy obraz: to wygladzanie)")
if frames:
    fr = frames[-1]
    ring = parse_in(fr["we"])
    cel, wy = hexes(fr["cel"]), hexes(fr["wy"])
    hx = lambda px: "".join(f"{v:02X}" for v in px)
    print("ostatnia ramka: tasma TV  L(dol->gora):", " ".join(hx(p) for p in ring[0]))
    print("                          G(lewo->prawo):", " ".join(hx(p) for p in ring[1]))
    print("                          P(gora->dol):", " ".join(hx(p) for p in ring[2]))
    k = 0
    for w, n in walls:
        print(f"  {w:5s} cel: {' '.join(hx(p) for p in cel[k:k + n])}")
        print(f"        wy:  {' '.join(hx(p) for p in wy[k:k + n])}")
        k += n
bk.close()
sys.exit(0 if frames and worst_cel <= 1.0 else 1)
