"""Opoznienie wygladzania w ESP32: po ilu ramkach wyjscie dochodzi do nowej wartosci po skoku obrazu.

Strona testowa przelacza dwa kolory co `okres` klatek ekranu, a ESP32 zapisuje sredni kolor srodka pola w 240
kolejnych ramkach (komenda `trace`, firmware v14.1). Dla kazdego skoku liczona jest liczba ramek od pierwszej
ramki, w ktorej wyjscie ruszylo, do ramki, w ktorej przeszlo 90 % drogi (0 = caly skok w jednej ramce), oraz jaka
czesc pelnego skoku wyjscie w ogole osiaga.

Mierzy tylko filtr czasowy ESP32. Staly czas toru (probkowanie ramki, obliczenia, SPI, zapis tasmy) i wygladzanie
w BK7238 dochodza do tego osobno.

Uzycie:  python latency.py COM4 [smooth_nz ...]        np.  python latency.py COM4 0 30
"""
import re
import statistics as st
import sys
import time

import bench as _m
from bench import Esp, post

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
esp = Esp(sys.argv[1])
NZ = [int(v) for v in sys.argv[2:]] or [30]


def trace(n=240):
    txt = esp.cmd(f"trace {n}", wait=n / 60.0 + 3.0)
    i = txt.find("przebieg")
    vals = re.findall(r"\b[0-9A-F]{6}\b", txt[i:]) if i >= 0 else []
    return [sum(int(v[k:k + 2], 16) for k in (0, 2, 4)) / 3.0 for v in vals]


def analyse(tr, lo, hi):
    """Skoki miedzy poziomami lo/hi: lista (ramek do 90 %, osiagniety ulamek skoku)."""
    span = hi - lo
    out = []
    i = 1
    while i < len(tr):
        d = tr[i] - tr[i - 1]
        if abs(d) < 0.08 * span:
            i += 1
            continue
        start = tr[i - 1]
        up = d > 0
        target = hi if up else lo
        j = i
        # koniec tego skoku: az wyjscie przestanie isc w te sama strone
        while j + 1 < len(tr) and ((tr[j + 1] - tr[j]) > 0.02 * span) == up and abs(tr[j + 1] - tr[j]) > 0.02 * span:
            j += 1
        reached = abs(tr[j] - start) / span
        k = i
        while k <= j and abs(tr[k] - start) < 0.9 * abs(target - start):
            k += 1
        frames = (k - i) if k <= j else None
        if j + 1 < len(tr):                      # pomijamy skok uciety koncem zapisu
            out.append((frames, reached))
        i = j + 1
    return out


if not _m.page_open():
    print("UWAGA: strona testowa nie odpytuje serwera - wyniki beda bez znaczenia")
TESTS = [
    ("czern <-> biel co 30 klatek (pojedyncze skoki)", [0, 0, 0], [255, 255, 255], 30),
    ("czern <-> biel co 6 klatek (miganie 5 Hz)", [0, 0, 0], [255, 255, 255], 6),
    ("szary 60 <-> 180 co 30 klatek", [60, 60, 60], [180, 180, 180], 30),
    ("szary 110 <-> 140 co 30 klatek", [110, 110, 110], [140, 140, 140], 30),
    ("szary 120 <-> 128 co 30 klatek", [120, 120, 120], [128, 128, 128], 30),
]
# Strona liczy okres w klatkach WLASNEGO odswiezania (np. 240 Hz), a ESP32 widzi 60 ramek na sekunde. Skala okresu
# jest wyznaczana z pierwszego przebiegu, tak zeby "co 30 klatek" znaczylo 30 ramek wideo (0,5 s).
post({"mode": "flash", "a": [0, 0, 0], "b": [255, 255, 255], "frames": 120})
time.sleep(3.0)
_t = trace(240)
_n = sum(1 for a, b in zip(_t, _t[1:]) if abs(b - a) > 100)
SCALE = max(1, round(_n * 120 / 240.0)) if _n else 1
print(f"strona odswieza sie ok. {SCALE} x szybciej niz 60 Hz (zmian w 240 ramkach przy okresie 120: {_n}); okresy przeliczone")
for nz in NZ:
    esp.cmd(f"set smooth_nz {nz}", wait=0.5)
    print(f"\n===== smooth_nz {nz}")
    for name, a, b, period in TESTS:
        post({"mode": "flash", "a": a, "b": b, "frames": period * SCALE})
        time.sleep(3.0)
        tr = trace(240)
        if len(tr) < 200:
            print(f"{name}: brak przebiegu ({len(tr)} ramek)")
            continue
        lo, hi = sorted(tr)[len(tr) // 20], sorted(tr)[-len(tr) // 20]
        res = analyse(tr, lo, hi)
        fr = [f for f, _ in res if f is not None]
        never = sum(1 for f, _ in res if f is None)
        reach = [r for _, r in res]
        txt = f"{name}: poziomy {lo:.0f}..{hi:.0f}, skokow {len(res)}"
        if fr:
            txt += f"; do 90 % srednio {st.mean(fr):.1f} ramki (max {max(fr)})"
        if never:
            txt += f"; {never} skokow nie doszlo do 90 %"
        if reach:
            txt += f"; osiagniete srednio {100 * st.mean(reach):.0f} % skoku"
        print(txt)
        k = next((i for i in range(1, len(tr) - 12) if abs(tr[i] - tr[i - 1]) > 0.08 * (hi - lo)), 0)
        print("   przyklad:", " ".join(f"{v:.0f}" for v in tr[max(0, k - 2):k + 10]))
post({"mode": "idle"})
esp.cmd("set smooth_nz 30", wait=0.5)
