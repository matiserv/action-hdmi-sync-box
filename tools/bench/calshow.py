"""Plansze do kalibracji kolorow tasmy zdjeciami z telefonu.

Na ekranie za boxem (okno na caly wyswietlacz) ida kolejno pelnoekranowe plansze: czern, biel, dwie szarosci,
R, G, B, C, M, Y. Tasma je powtarza, a telefon fotografuje ekran razem z poswiata na scianie. Na czas pokazu
doswietlenie ze srodka i przygaszanie sa wylaczone, a nasycenie ustawione na 1.0 (po pokazie wracaja poprzednie wartosci);
balans bieli i gamma zostaja, bo to one sa kalibrowane. Dla kazdej planszy skrypt zapisuje, co box wyslal na tasme.

Uzycie: python calshow.py <host> <yaml z kluczem API> <wyswietlacz> [sekundy na plansze = 8] [ile obiegow = 1] [plik wynikowy .json]
                          [plansze po przecinku, np. biel,szary 128,turkus,purpura,zolty]
"""
import json
import re
import statistics as st
import sys
import time

from bkapi import Bk
from hdrwin import HdrWindow, describe, find_display, srgb_to_hdr10

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
host, yaml_path, which = sys.argv[1:4]
SECS = float(sys.argv[4]) if len(sys.argv) > 4 else 8
LOOPS = int(sys.argv[5]) if len(sys.argv) > 5 else 1
OUT = sys.argv[6] if len(sys.argv) > 6 else ""
PATCHES = [("czern", (0, 0, 0)), ("biel", (255, 255, 255)), ("szary 128", (128, 128, 128)), ("szary 64", (64, 64, 64)),
           ("czerwony", (255, 0, 0)), ("zielony", (0, 255, 0)), ("niebieski", (0, 0, 255)),
           ("turkus", (0, 255, 255)), ("purpura", (255, 0, 255)), ("zolty", (255, 255, 0))]
if len(sys.argv) > 7:
    want = [x.strip() for x in sys.argv[7].split(",")]
    PATCHES = [p for n in want for p in PATCHES if p[0] == n]
NEUTRAL = {"Wideo: doswietlenie ze srodka": 0.0, "Wideo: przygaszanie przy ciemnym srodku": 0.0, "Wideo: nasycenie": 1.0}
SEC_RE = re.compile(r"sekcje L:((?:[0-9A-F]{6} )*)G:((?:[0-9A-F]{6} )*)P:((?:[0-9A-F]{6} )*)")

disp = find_display(which)
print("wyswietlacz:", describe(disp))
bk = Bk(host, yaml_path)
time.sleep(4)
before = {k: bk.states.get(k) for k in NEUTRAL}
keep = {k: bk.states.get(k) for k in ("Wideo: balans R", "Wideo: balans G", "Wideo: balans B", "Wideo: gamma", "Wideo: jasnosc",
                                      "Wideo: poziom czerni", "Wideo: poziom bieli", "Wideo: glebokosc probkowania")}
print("ustawienia tasmy w czasie pokazu:", ", ".join(f"{k.replace('Wideo: ', '')} {v}" for k, v in keep.items()))
for k, v in NEUTRAL.items():
    if before[k] is not None:
        bk.set_number(k, v)
bk.set_switch("Diagnostyka: log sekcji", True)
hdr = bool(disp["hdr_enabled"])
conv = (lambda c: srgb_to_hdr10(c, 203.0)) if hdr else (lambda c: tuple(round(v * 1023 / 255) for v in c))
win = HdrWindow(disp, hdr=hdr)
rows = []
try:
    for loop in range(LOOPS):
        for name, rgb in PATCHES:
            t0 = time.time()
            win.solid(conv(rgb))
            win.hold(min(2.5, SECS * 0.5))
            line = bk.wait_log("sekcje L:", 3.0)
            m = SEC_RE.search(line or "")
            strip = None
            if m:
                cols = [tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) for g in m.groups() for h in g.split()]
                strip = [round(st.mean(c[i] for c in cols), 1) for i in range(3)]
            win.hold(max(0.0, SECS - (time.time() - t0)))
            rows.append({"obieg": loop + 1, "plansza": name, "srgb": rgb, "start": time.strftime("%H:%M:%S", time.localtime(t0)),
                         "koniec": time.strftime("%H:%M:%S"), "tasma": strip})
            print(f"{rows[-1]['start']}-{rows[-1]['koniec']}  {name:10s} ekran {rgb}  -> tasma (srednia sekcji) {strip}", flush=True)
finally:
    win.close()
    bk.set_switch("Diagnostyka: log sekcji", False)
    for k, v in before.items():
        if v is not None:
            bk.set_number(k, float(v))
    time.sleep(1)
    print("przywrocone:", {k.replace("Wideo: ", ""): bk.states.get(k) for k in NEUTRAL})
    bk.close()
if OUT:
    json.dump({"hdr": hdr, "ustawienia": keep, "plansze": rows}, open(OUT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("zapisano", OUT)
