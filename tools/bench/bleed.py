"""Test "łapania środka": czerwona ramka przy krawędzi ekranu, wnętrze zielone. Idealnie komórki przy krawędzi
są czysto czerwone; udział zieleni pokazuje, ile koloru z głębi obrazu przenika do pasa używanego przez taśmę.
Wynik osobno dla każdego boku służy też do kalibracji czasu próbek: przenikanie po lewej = za małe dig_d,
po prawej = za duże.
Użycie: python bleed.py COM4 [grubość ramki w komórkach pola = 1] ["komendy konsoli; ..."]
"""
import importlib.util, os, statistics as st, sys, time
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
esp = _m.Esp(sys.argv[1])
thick = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
for c in (sys.argv[3].split(";") if len(sys.argv) > 3 else []):
    if c.strip():
        esp.cmd(c.strip(), wait=0.35)
probe = esp.frames(1)
H = len(probe[0]); W = len(probe[0][0])
# wzór: 4 pola wzoru na komórkę pola, więc ramka ma dokładnie zadaną grubość w komórkach
nx, ny = W * 4, H * 4
t = max(1, round(thick * 4))
cells = [[255, 0, 0] if (x < t or x >= nx - t or y < t or y >= ny - t) else [0, 255, 0] for y in range(ny) for x in range(nx)]
_m.show({"mode": "cells", "nx": nx, "ny": ny, "cells": cells}, settle=3.0)
frames = esp.frames(4) + esp.frames(4)

def green(y, x):
    r = st.mean(f[y][x][0] for f in frames); g = st.mean(f[y][x][1] for f in frames)
    return 100 * g / (r + g) if r + g > 0 else 0

print(f"pole {W} x {H}; ramka {t / 4:g} komórki ({t * 1080 // ny} px przy 1080p)")
mid_x, mid_y = W // 2, H // 2
print("udział zieleni (0 % = idealnie) w komórkach licząc od krawędzi w głąb:")
print("  od góry  (środek szerokości):", " ".join(f"{green(y, mid_x):3.0f}" for y in range(min(5, H))))
print("  od lewej (środek wysokości): ", " ".join(f"{green(mid_y, x):3.0f}" for x in range(min(5, W))))
sides = {"góra": [green(0, x) for x in range(2, W - 2)], "dół": [green(H - 1, x) for x in range(2, W - 2)],
         "lewa": [green(y, 0) for y in range(2, H - 2)], "prawa": [green(y, W - 1) for y in range(2, H - 2)]}
print("  " + "; ".join(f"{k}: średnio {st.mean(v):.1f} %, max {max(v):.0f} %" for k, v in sides.items()))
allv = [v for vs in sides.values() for v in vs]
print(f"pierwsza komórka od krawędzi, cały obwód: zieleń średnio {st.mean(allv):.1f} %, max {max(allv):.0f} %")
_m.post({"mode": "idle"})
