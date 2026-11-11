"""Test stabilności: statyczny obraz, 3 x 8 kolejnych ramek; ile komórek zmienia się w czasie i o ile.
Użycie: python stability.py COM4 "<komendy konsoli; ...>"   (komendy ustawiają wariant przed pomiarem)
"""
import sys, time, statistics as st
import importlib.util, os
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
Esp, show, post = _m.Esp, _m.show, _m.post

esp = Esp(sys.argv[1])
for c in (sys.argv[2].split(";") if len(sys.argv) > 2 else []):
    if c.strip():
        esp.cmd(c.strip(), wait=0.4)
time.sleep(1.5)
txt = esp.cmd("stat", wait=0.8)
for l in txt.splitlines():
    if l.startswith(("Tryb ADC", "Ramki")):
        print(l[:120])
print("kolor            komórek niestabilnych (zakres > 6)   max zakres   kolumny z niestabilnością")
for col in [(255, 255, 255), (128, 128, 128), (32, 32, 32), (255, 0, 0), (40, 90, 200), (255, 128, 0)]:
    show({"mode": "solid", "color": list(col)}, settle=0.8)
    frames = []
    for i in range(3):
        frames += esp.frames(8)
    bad = 0; worst = 0; cols = set()
    for y in range(len(frames[0])):
        for x in range(len(frames[0][0])):
            rng = max(max(f[y][x][k] for f in frames) - min(f[y][x][k] for f in frames) for k in range(3))
            worst = max(worst, rng)
            if rng > 6:
                bad += 1; cols.add(x)
    print(f"{str(col):16s} {bad:4d} / 256                          {worst:4d}        {sorted(cols)}")
post({"mode": "idle"})
