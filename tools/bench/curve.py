"""Charakterystyka toru: szary pełny ekran o jasności 0..255 (co `krok`) i odczyt z ESP32 (bez wygładzania).
Pokazuje martwe strefy przetwornika (kilka kolejnych poziomów dających ten sam odczyt) i skoki.
Użycie: python curve.py COM4 [krok=2] [plik.csv] ["komendy konsoli; ..."]
"""
import importlib.util, os, statistics as st, sys, time
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
esp = _m.Esp(sys.argv[1])
step = int(sys.argv[2]) if len(sys.argv) > 2 else 2
out = sys.argv[3] if len(sys.argv) > 3 else ""
for c in (sys.argv[4].split(";") if len(sys.argv) > 4 else []):
    if c.strip():
        esp.cmd(c.strip(), wait=0.35)
esp.cmd("set smooth 100", wait=0.3); esp.cmd("set hold 0", wait=0.3); esp.cmd("set cut 0", wait=0.3)

def mean_rgb(f):
    H = len(f); W = len(f[0])
    px = [f[y][x] for y in range(H // 4, 3 * H // 4) for x in range(W // 4, 3 * W // 4)]
    return tuple(st.mean(p[k] for p in px) for k in range(3))

rows = []
for v in range(0, 256, step):
    _m.show({"mode": "solid", "color": [v, v, v]}, settle=0.45)
    fr = esp.frames(1)
    if not fr:
        continue
    rows.append((v,) + mean_rgb(fr[-1]))
esp.cmd("set smooth 4", wait=0.3); esp.cmd("set hold 15", wait=0.3); esp.cmd("set cut 10", wait=0.3)
_m.post({"mode": "idle"})
if out:
    with open(out, "w", encoding="utf-8") as f:
        f.write("poziom,R,G,B\n")
        for r in rows:
            f.write("%d,%.1f,%.1f,%.1f\n" % r)
for k, name in enumerate("RGB"):
    err = [r[1 + k] - r[0] for r in rows]
    # martwe strefy: kolejne poziomy, przy ktorych odczyt rosnie o mniej niz 25 % kroku
    flats = []; start = None
    for i in range(1, len(rows)):
        if rows[i][1 + k] - rows[i - 1][1 + k] < 0.25 * step:
            if start is None:
                start = i - 1
        elif start is not None:
            if rows[i - 1][0] - rows[start][0] >= 2 * step:
                flats.append((rows[start][0], rows[i - 1][0], rows[start][1 + k]))
            start = None
    print(f"{name}: błąd odczytu średnio {st.mean(abs(e) for e in err):.2f}, największy {max(err, key=abs):+.1f}; martwe strefy (od..do -> odczyt): "
          + (", ".join(f"{a}..{b} -> {c:.0f}" for a, b, c in flats) if flats else "brak"))
