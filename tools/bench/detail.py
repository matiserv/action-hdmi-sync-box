"""Stabilność na obrazie z drobnymi szczegółami (losowe kolorowe komórki 96x54, statyczne).
Użycie: python detail.py COM4 "<komendy konsoli; ...>"
"""
import importlib.util, os, random, statistics as st, sys, time
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
Esp, show, post = _m.Esp, _m.show, _m.post

esp = Esp(sys.argv[1])
for c in (sys.argv[2].split(";") if len(sys.argv) > 2 else []):
    if c.strip():
        esp.cmd(c.strip(), wait=0.4)
time.sleep(1.5)
for l in esp.cmd("stat", wait=0.8).splitlines():
    if l.startswith(("Tryb ADC", "Ramki")):
        print(l[:110])
rnd = random.Random(7)
for name, nx, ny in (("drobne 96x54", 96, 54), ("srednie 32x18", 32, 18)):
    cells = [[rnd.choice((0, 255)), rnd.choice((0, 255)), rnd.choice((0, 255))] for _ in range(nx * ny)]
    show({"mode": "cells", "nx": nx, "ny": ny, "cells": cells}, settle=1.0)
    frames = []
    for i in range(3):
        frames += esp.frames(8)
    rngs = []; sds = []
    H = len(frames[0]); W = len(frames[0][0])
    for y in range(H):
        for x in range(W):
            for k in range(3):
                v = [f[y][x][k] for f in frames]
                rngs.append(max(v) - min(v)); sds.append(st.pstdev(v))
    edge = [max(max(f[y][x][k] for f in frames) - min(f[y][x][k] for f in frames) for k in range(3))
            for y in range(H) for x in range(W) if y < 2 or y > H - 3 or x < 2 or x > W - 3]
    print(f"{name}: odchylenie std w czasie średnio {st.mean(sds):.1f}, zakres średnio {st.mean(rngs):.0f}, max {max(rngs)}; "
          f"komórki brzegowe (używane przez taśmę): zakres średnio {st.mean(edge):.0f}, max {max(edge)}")
post({"mode": "idle"})
