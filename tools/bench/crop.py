"""Test wykrywania czarnych pasow bocznych w BK7238 (komponent syncbox v15.1).

Pasy gorny i dolny wycina ESP32 (bench/letterbox.py); BK7238 wykrywa pasy boczne. Pas to kolumna, w ktorej kazda
komorka jest czarna; pasy musza byc symetryczne, najwyzej 1/4 szerokosci z kazdej strony, a kadr zmniejsza sie
dopiero po 1,5 s. Ciemna scena z jasnym fragmentem na srodku ani ciemny obszar z jednej strony nie sa pasami.

Uzycie:  python crop.py 192.168.1.50 <plik YAML urzadzenia z kluczem API>
"""
import re
import sys
import time

import bench as _m
from bench import post
from bkapi import Bk

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
bk = Bk(sys.argv[1], sys.argv[2])
W, H = 48, 27


def cells(fn):
    return {"mode": "cells", "nx": W, "ny": H, "cells": [list(fn(x, y)) for y in range(H) for x in range(W)]}


def crop_now():
    line = bk.wait_log("sections L:", 4.0)
    m = re.search(r"kadr x (\d+)\.\.(\d+) y (\d+)\.\.(\d+)", line or "")
    return tuple(int(v) for v in m.groups()) if m else None


GREY = (150, 150, 150)
TESTS = [
    ("pelny obraz", cells(lambda x, y: GREY), (0, 47)),
    ("pasy boczne 4:3 (po 6 kolumn)", cells(lambda x, y: (0, 0, 0) if x < 6 or x > 41 else GREY), (6, 41)),
    ("powrot do pelnego obrazu", cells(lambda x, y: GREY), (0, 47)),
    ("obraz 1:1 (pasy po 10 kolumn)", cells(lambda x, y: (0, 0, 0) if x < 10 or x > 37 else GREY), (10, 37)),
    ("pionowe wideo (pasy po 16 kolumn): za szerokie na pas", cells(lambda x, y: (0, 0, 0) if x < 16 or x > 31 else GREY), (0, 47)),
    ("ciemna scena, jasny fragment na srodku (kolumny 20..27)",
     cells(lambda x, y: (200, 200, 200) if 20 <= x <= 27 and 8 <= y <= 18 else (0, 0, 0)), (0, 47)),
    ("ciemny pulpit: tlo 20, jasny fragment na srodku",
     cells(lambda x, y: (200, 200, 200) if 20 <= x <= 27 and 8 <= y <= 18 else (20, 20, 20)), (0, 47)),
    ("czarny obszar tylko z lewej (10 kolumn)", cells(lambda x, y: (0, 0, 0) if x < 10 else GREY), (0, 47)),
]

if not _m.page_open():
    print("UWAGA: strona testowa nie odpytuje serwera - wyniki beda bez znaczenia")
time.sleep(1.0)
bk.set_switch("Diagnostics: section log", True)
post(TESTS[0][1])
time.sleep(3.0)
ok = True
try:
    for name, pattern, want in TESTS:
        post(pattern)
        time.sleep(4.5)                 # strona zmienia wzor do 1 s, zmniejszenie kadru wymaga 1,5 s stalego ukladu
        c = crop_now()
        good = c is not None and (c[0], c[1]) == want
        ok &= good
        print(f"{name}: kadr x {c[0]}..{c[1]} y {c[2]}..{c[3]}" if c else f"{name}: brak odczytu",
              f"(oczekiwane x {want[0]}..{want[1]})", "OK" if good else "ZLE")
finally:
    bk.set_switch("Diagnostics: section log", False)
    post({"mode": "idle"})
    time.sleep(0.5)
if bk.drops:
    print(f"UWAGA: polaczenie z BK7238 zerwane {bk.drops} razy")
    ok = False
bk.close()
print("WYNIK:", "wszystko OK" if ok else "sa bledy")
