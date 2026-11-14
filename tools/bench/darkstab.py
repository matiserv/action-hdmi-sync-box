"""Stabilnosc ciemnych kolorow na wyjsciu BK7238: czy nieruchomy ciemny obraz daje nieruchome poziomy diod.

Dla kilku nieruchomych, ciemnych wzorow zbiera kolejne ramki pola z ESP32 (grupy po 4 ramki, 60 na sekunde),
przepuszcza je przez model toru BK7238 (bkmodel.py, aktualne suwaki, bez zaokraglania) razem z wygladzaniem BK
i liczy, o ile zmienia sie docelowy poziom kazdej skladowej kazdej sekcji:
  - rozstep (max - min) w jednostkach tasmy 0..255,
  - jak czesto cel po zaokragleniu do kroku ditheringu (1/4 poziomu) rozni sie miedzy sasiednimi ramkami.
Cel, ktory stoi w miejscu, daje staly wzor ditheringu (najwolniejsza skladowa 30 Hz przy kroku 1/4, 60 Hz przy 1/2).
Cel, ktory skacze o krok, dodaje do tego nieregularne zmiany - to jest "migotanie od szumu".

Uzycie:  python darkstab.py COM4 192.168.1.50 <plik YAML urzadzenia z kluczem API>
"""
import statistics as st
import sys
import time

import bench as _m
from bench import Esp, post
from bkapi import Bk
from bkmodel import CFG, H, W, model, read_cfg

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
esp = Esp(sys.argv[1])
bk = Bk(sys.argv[2], sys.argv[3])
time.sleep(1.5)
read_cfg(bk.states)
mix = float(bk.states.get("Video: inner fill") or 0) / 100.0
smoothing = float(bk.states.get("Video: smoothing") or 0)
bk.close()
print("Tor BK7238:", CFG, "doswietlenie", mix, "wygladzanie", smoothing)
if not _m.page_open():
    print("UWAGA: strona testowa nie odpytuje serwera - wyniki beda bez znaczenia")


def cells(fn):
    return {"mode": "cells", "nx": W, "ny": H, "cells": [list(fn(x, y)) for y in range(H) for x in range(W)]}


PATTERNS = [
    ("szary 30", {"mode": "solid", "color": [30, 30, 30]}),
    ("szary 50", {"mode": "solid", "color": [50, 50, 50]}),
    ("szary 80", {"mode": "solid", "color": [80, 80, 80]}),
    ("ciemny braz (60,40,24)", {"mode": "solid", "color": [60, 40, 24]}),
    ("ciemny niebieski (20,30,70)", {"mode": "solid", "color": [20, 30, 70]}),
    ("gradient poziomy 16..96", cells(lambda x, y: (16 + x * 80 // 47,) * 3)),
    ("ciemna szachownica 6x4 (40 / 70)", cells(lambda x, y: (40, 40, 40) if (x // 8 + y // 7) % 2 else (70, 60, 50))),
]

for name, pattern in PATTERNS:
    post(pattern)
    time.sleep(3.0)
    groups = [esp.frames(4) for _ in range(8)]
    # przy polu 48x27 czwarta ramka czesto nie miesci sie w czasie odczytu konsoli: wystarcza trzy kolejne
    groups = [g for g in groups if len(g) >= 3 and len(g[0]) == H]
    if not groups:
        print(f"{name}: brak ramek")
        continue
    spans, steps, levels, firsts = [], [], [], []
    for g in groups:
        outs = [model(f, (0, W - 1, 0, H - 1), mix, as_float=True) for f in g]
        firsts.append([c for side in ("L", "G", "P") for px in outs[0][side] for c in px])
        sm = None
        seq = []
        for o in outs:                                   # wygladzanie BK7238 (start od pierwszej ramki grupy)
            flat = [c for side in ("L", "G", "P") for px in o[side] for c in px]
            sm = flat if sm is None else [a + (b - a) * (1.0 - smoothing) for a, b in zip(sm, flat)]
            seq.append(sm)
        for i in range(len(seq[0])):
            vals = [s[i] for s in seq]
            spans.append(max(vals) - min(vals))
            levels.append(st.mean(vals))
            q = [round(v * 4) for v in vals]
            steps += [abs(a - b) for a, b in zip(q, q[1:])]
    # to samo w dluzszym czasie: pierwsze ramki kolejnych grup (odstep ok. 3 s)
    slow = [max(col) - min(col) for col in zip(*firsts)]
    print(f"   miedzy pomiarami co ok. 3 s ({len(firsts)} pomiarow): rozstep mediana {st.median(slow):.2f}, 95 % "
          f"{sorted(slow)[int(len(slow) * 0.95)]:.2f}, max {max(slow):.2f}; skladowych z rozstepem >= 0,25: "
          f"{100 * sum(s >= 0.25 for s in slow) / len(slow):.0f} %")
    moved = sum(1 for s in steps if s) / len(steps)
    print(f"{name}: poziom diod srednio {st.mean(levels):.1f} (max {max(levels):.1f}); rozstep w 3-4 ramkach: mediana {st.median(spans):.2f}, "
          f"95 % {sorted(spans)[int(len(spans) * 0.95)]:.2f}, max {max(spans):.2f}; cel zmienia krok 1/4 w {100 * moved:.0f} % przejsc miedzy ramkami")
post({"mode": "idle"})
