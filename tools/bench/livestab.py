"""Stabilnosc tasmy na tym, co jest teraz na ekranie (bez strony testowej): ktore sekcje sie ruszaja i o ile.

Zbiera grupy kolejnych ramek pola z ESP32, przepuszcza je przez model toru BK7238 (bkmodel.py, aktualne suwaki)
i dla kazdej sekcji podaje sredni kolor oraz rozstep docelowego poziomu: w kolejnych ramkach (szybkie drgania)
i miedzy grupami oddalonymi o kilka sekund (wolne przeskoki). Dodatkowo: szum samego pola w komorkach przy krawedzi
i log BK7238 (zmiany kadru, statystyki) z czasu pomiaru.

Uzycie:  python livestab.py COM4 192.168.1.50 <plik YAML urzadzenia z kluczem API> [liczba grup = 12]
"""
import re
import statistics as st
import sys
import time

from bench import Esp
from bkapi import Bk
from bkmodel import CFG, H, SIDES, W, model, read_cfg

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
esp = Esp(sys.argv[1])
bk = Bk(sys.argv[2], sys.argv[3])
NG = int(sys.argv[4]) if len(sys.argv) > 4 else 12
time.sleep(1.5)
read_cfg(bk.states)
mix = float(bk.states.get("Video: inner fill") or 0) / 100.0
print("Tor BK7238:", CFG, "doswietlenie", mix, "wygladzanie", bk.states.get("Video: smoothing"))

txt = esp.cmd("stat", wait=1.5)
for l in txt.splitlines():
    if l.startswith("Obraz bez pasow") or l.startswith("VSync:"):
        print("ESP32:", l.strip()[:120])

groups = []
for _ in range(NG):
    g = esp.frames(4)
    if len(g) >= 3 and len(g[0]) == H:
        groups.append(g[:3])
print(f"grup po 3 kolejne ramki: {len(groups)}")
if not groups:
    raise SystemExit("brak ramek z ESP32")

crop = (0, W - 1, 0, H - 1)
names = [(side, i) for side in ("L", "G", "P") for i in range(SIDES[side])]
outs = [[model(f, crop, mix, as_float=True) for f in g] for g in groups]          # [grupa][ramka]{bok: [sekcje]}
print("\nsekcja   sredni kolor (poziomy tasmy)    rozstep w 3 kolejnych ramkach   rozstep miedzy grupami (ok. 3 s)")
worst = []
for side, i in names:
    mean = [st.mean(o[side][i][c] for g in outs for o in g) for c in range(3)]
    fast = max(max(o[side][i][c] for o in g) - min(o[side][i][c] for o in g) for g in outs for c in range(3))
    gm = [[st.mean(o[side][i][c] for o in g) for c in range(3)] for g in outs]
    slow = max(max(m[c] for m in gm) - min(m[c] for m in gm) for c in range(3))
    worst.append((max(fast, slow), side, i))
    flag = "  <--" if max(fast, slow) >= 1.0 else ""
    print(f"{side}{i:<2d}      ({mean[0]:6.1f},{mean[1]:6.1f},{mean[2]:6.1f})          {fast:6.2f}                          {slow:6.2f}{flag}")

# szum pola w komorkach, z ktorych korzysta tasma (brzeg pola, 3 komorki w glab)
edge = [(y, x) for y in range(H) for x in range(W) if x < 3 or x >= W - 3 or y < 3]
fast, slow = [], []
for (y, x) in edge:
    for c in range(3):
        fast.append(max(max(f[y][x][c] for f in g) - min(f[y][x][c] for f in g) for g in groups))
        gm = [st.mean(f[y][x][c] for f in g) for g in groups]
        slow.append(max(gm) - min(gm))
print(f"\npole ESP32 przy krawedziach (0..255): rozstep w 3 ramkach mediana {st.median(fast):.0f}, 95 % {sorted(fast)[int(len(fast) * 0.95)]:.0f}, "
      f"max {max(fast):.0f}; miedzy grupami mediana {st.median(slow):.1f}, 95 % {sorted(slow)[int(len(slow) * 0.95)]:.1f}, max {max(slow):.1f}")
bad = sorted(((max(max(f[y][x][c] for g in groups for f in g) - min(f[y][x][c] for g in groups for f in g) for c in range(3)), y, x)
              for (y, x) in edge), reverse=True)[:8]
print("najbardziej ruchome komorki (rozstep, wiersz, kolumna, przykladowe wartosci):")
for d, y, x in bad:
    print(f"   {d:3d}  y={y:2d} x={x:2d}  ", " ".join("%02X%02X%02X" % groups[k][0][y][x] for k in range(min(6, len(groups)))))

print("\nlog BK7238 z czasu pomiaru:")
for _, t in bk.logs:
    if "Crop:" in t or "Field from" in t or "Signal from" in t:
        print("  ", t.strip()[:150])
kl = [t for _, t in bk.logs if "frames:" in t]
if kl:
    print("  ", re.sub(r"^\S+ ", "", kl[-1].strip())[:170])
txt = esp.cmd("stat", wait=1.5)
for l in txt.splitlines():
    if l.startswith("Obraz bez pasow"):
        print("ESP32 po pomiarze:", l.strip()[:120])
bk.close()
