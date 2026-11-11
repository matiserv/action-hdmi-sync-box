"""Test czarnych pasów (letterbox 2,39:1): pasy u góry i u dołu, w obrazie czerwona ramka grubości ok. jednej
komórki pola i zielone wnętrze. Po wykryciu pasów pole powinno obejmować sam obraz: skrajne wiersze czerwone
(nie czarne), bez wierszy mieszanych z pasem.
Użycie: python letterbox.py COM4 ["komendy konsoli; ..."]
"""
import importlib.util, os, re, statistics as st, sys, time
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
esp = _m.Esp(sys.argv[1])
for c in (sys.argv[2].split(";") if len(sys.argv) > 2 else []):
    if c.strip():
        esp.cmd(c.strip(), wait=0.35)
nx, ny = 192, 108                       # pole wzoru = 10 px przy 1080p
bar = 14                                # 140 px = 13 % wysokości (film 2,39:1 na ekranie 16:9)
ty, tx = 3, 4                           # ramka: 30 px w pionie, 40 px w poziomie (ok. jedna komórka pola 48x27 po przycięciu)
def color(x, y):
    if y < bar or y >= ny - bar:
        return [0, 0, 0]
    if y < bar + ty or y >= ny - bar - ty or x < tx or x >= nx - tx:
        return [255, 0, 0]
    return [0, 255, 0]
cells = [color(x, y) for y in range(ny) for x in range(nx)]
_m.show({"mode": "cells", "nx": nx, "ny": ny, "cells": cells}, settle=4.0)   # wykrycie pasów trwa 1,5 s
for l in esp.cmd("stat", wait=0.8).splitlines():
    if l.startswith("Obraz bez pasow"):
        print(l.strip())
frames = esp.frames(4) + esp.frames(4)
H = len(frames[0]); W = len(frames[0][0])
def px(y, x):
    return tuple(round(st.mean(f[y][x][k] for f in frames)) for k in range(3))
def green(y, x):
    r, g, _ = px(y, x)
    return 100 * g / (r + g) if r + g > 0 else -1
mid = W // 2
print(f"pole {W} x {H}; kolumna środkowa od góry (R,G,B):", " ".join("%3d,%3d,%3d" % px(y, mid) for y in range(5)))
print("                 kolumna środkowa od dołu:        ", " ".join("%3d,%3d,%3d" % px(H - 1 - y, mid) for y in range(5)))
top = [px(0, x) for x in range(2, W - 2)]; bot = [px(H - 1, x) for x in range(2, W - 2)]
print(f"górny wiersz: czerwień średnio {st.mean(p[0] for p in top):.0f}, zieleń {st.mean(p[1] for p in top):.0f}; "
      f"dolny wiersz: czerwień {st.mean(p[0] for p in bot):.0f}, zieleń {st.mean(p[1] for p in bot):.0f} (idealnie 255 i mało zieleni)")
_m.post({"mode": "idle"})
