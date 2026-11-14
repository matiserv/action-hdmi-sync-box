"""Przygaszanie przy ciemnym srodku na prawdziwym torze: wzory na ekranie za boxem (okno na calym wyswietlaczu),
pole odczytane z ESP32, kolory sekcji z logu BK7238 i model (bkmodel.py) - dla kilku wartosci suwaka.
Na koniec suwak wraca do wartosci sprzed testu. Uruchamiac przez tcp_env.py (haslo konsoli ESP32 w srodowisku).

Uzycie: python dimtest.py <katalog bench> <host> <yaml z kluczem API> <wyswietlacz> [wartosci suwaka = 0,50,100]
"""
import re
import statistics as st
import sys
import time

sys.path.insert(0, sys.argv[1])
from bench import Esp  # noqa: E402
from bkapi import Bk  # noqa: E402
from bkmodel import CFG, model, read_cfg  # noqa: E402
from hdrwin import HdrWindow, describe, find_display, srgb_to_hdr10  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
host, yaml_path, which = sys.argv[2:5]
vals = [float(x) for x in (sys.argv[5] if len(sys.argv) > 5 else "0,50,100").split(",")]
DIM = "Wideo: przygaszanie przy ciemnym srodku"
MIX = "Wideo: doswietlenie ze srodka"
SEC_RE = re.compile(r"sekcje L:((?:[0-9A-F]{6} )*)G:((?:[0-9A-F]{6} )*)P:((?:[0-9A-F]{6} )*)\s*kadr x (\d+)\.\.(\d+) y (\d+)\.\.(\d+)")
W, H = 48, 27


def frame(edge, inner, band):
    return [edge if (x < band or x >= W - band or y < band or y >= H - band) else inner for y in range(H) for x in range(W)]


G = lambda v: (v, v, v)
PATTERNS = [
    ("A jasny brzeg (5 komorek), czarny srodek", frame(G(230), G(0), 5)),
    ("B jasny brzeg (5 komorek), szary srodek 110", frame(G(230), G(110), 5)),
    ("C jednolity szary 150", frame(G(150), G(150), 5)),
    ("D ciemny brzeg 30, jasny srodek", frame(G(30), G(230), 5)),
    ("E waska jasna ramka (1 komorka), reszta 20", frame(G(230), G(20), 1)),
    ("F kolorowy brzeg (pomarancz), czarny srodek", frame((255, 120, 0), G(0), 5)),
]

disp = find_display(which)
print("wyswietlacz:", describe(disp))
bk = Bk(host, yaml_path)
time.sleep(4)
read_cfg(bk.states)
before = bk.states.get(DIM)
mix = float(bk.states.get(MIX) or 0) / 100.0
print(f"ustawienia: glebokosc {CFG['depth']}, nasycenie {CFG['sat']}, gamma {CFG['gamma']}, doswietlenie {mix * 100:.0f} %, "
      f"przygaszanie przed testem {before}")
esp = Esp("tcp://" + host + ":2323")
bk.set_switch("Diagnostyka: log sekcji", True)
hdr = bool(disp["hdr_enabled"])
conv = (lambda c: srgb_to_hdr10(c, 203.0)) if hdr else (lambda c: tuple(round(v * 1023 / 255) for v in c))
win = HdrWindow(disp, hdr=hdr)


def sections():
    line = bk.wait_log("sekcje L:", 4.0)
    m = SEC_RE.search(line) if line else None
    if not m:
        return None, None
    cols = lambda t: [tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) for h in t.split()]
    return {"L": cols(m.group(1)), "G": cols(m.group(2)), "P": cols(m.group(3))}, tuple(int(m.group(i)) for i in (4, 5, 6, 7))


worst = 0
try:
    for name, cells in PATTERNS:
        win.cells(W, H, [conv(c) for c in cells])
        win.hold(2.5)                                  # kadr i wygladzanie w ESP32
        print(name)
        for v in vals:
            bk.set_number(DIM, v)
            win.hold(3.0)
            fr = esp.frames(1)
            win.hold(1.2)
            sec, crop = sections()
            if not fr or sec is None:
                print(f"   {v:3.0f} %: brak danych (ramka {bool(fr)}, sekcje {sec is not None})")
                continue
            f = fr[0]
            m = model(f, crop, mix, dim=v / 100.0)
            diffs = [max(abs(a[c] - b[c]) for c in range(3)) for side in ("L", "G", "P") for a, b in zip(sec[side], m[side])]
            worst = max(worst, max(diffs))
            br = {side: st.mean(max(c) for c in sec[side]) for side in ("L", "G", "P")}
            mid = sec["G"][len(sec["G"]) // 2]
            print(f"   {v:3.0f} %: jasnosc sekcji L {br['L']:5.1f}  G {br['G']:5.1f}  P {br['P']:5.1f} | srodkowa gorna "
                  f"{'%02X%02X%02X' % mid} | tasma-model: max {max(diffs)}, srednio {st.mean(diffs):.1f} | kadr {crop}")
finally:
    win.close()
    bk.set_switch("Diagnostyka: log sekcji", False)
    if before is not None:
        bk.set_number(DIM, float(before))
    time.sleep(1)
    print("najwieksza roznica tasma-model w calym tescie:", worst, "| suwak po tescie:", bk.states.get(DIM))
    bk.close()
