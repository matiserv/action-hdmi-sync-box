"""Test doswietlenia ze srodka obrazu (BK7238, komponent syncbox v15).

Dla kazdego wzoru: pole 48x27 odczytane z ESP32 (konsola) przechodzi przez model toru BK7238 w Pythonie
(prostokaty sekcji, tablica poziomy/balans/gamma, doswietlenie, nasycenie), a wynik jest porownywany z kolorami
sekcji, ktore BK7238 wypisuje w logu (przelacznik "Diagnostics: section log"). Kazdy wzor idzie dwa razy:
z doswietleniem wylaczonym (0 %) i wlaczonym (domyslnie 25 %).

Wymaga: server.py, strony testowej na ekranie za sync boxem, ESP32 na porcie szeregowym, BK7238 w sieci.
Uzycie:  python innermix.py COM4 192.168.1.50 <plik YAML urzadzenia z kluczem API> [procent]
"""
import re
import sys
import time

import bench as _m
from bench import Esp, post
from bkapi import Bk
from bkmodel import CFG, H, SIDES, W, model, read_cfg

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
esp = Esp(sys.argv[1])
bk = Bk(sys.argv[2], sys.argv[3])
MIX = float(sys.argv[4]) if len(sys.argv) > 4 else 25.0
ONLY = sys.argv[5] if len(sys.argv) > 5 else ""          # prefiks nazwy testu, np. T2
VERBOSE = len(sys.argv) > 6                              # dowolny szosty argument: wypisz wszystkie sekcje

# ---------------------------------------------------------------- BK7238 przez API ESPHome (bkapi.py)
SEC_RE = re.compile(r"sections L:((?:[0-9A-F]{6} )*)T:((?:[0-9A-F]{6} )*)R:((?:[0-9A-F]{6} )*)\s*crop x (\d+)\.\.(\d+) y (\d+)\.\.(\d+) inner (\d+)%")


def bk_sections():
    """Kolory sekcji z kolejnej linii logu: ({'L': [...], 'G': [...], 'P': [...]}, kadr, procent)."""
    line = bk.wait_log("sections L:", 4.0)
    m = SEC_RE.search(line) if line else None
    if not m:
        return None
    cols = lambda t: [tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) for h in t.split()]
    return ({"L": cols(m.group(1)), "G": cols(m.group(2)), "P": cols(m.group(3))},
            tuple(int(m.group(i)) for i in (4, 5, 6, 7)), int(m.group(8)))


# ---------------------------------------------------------------- wzory
BLACK, WHITE, RED = (0, 0, 0), (255, 255, 255), (255, 0, 0)


def frame_pattern(edge, inner, right_edge=None, right_inner=None, lower_inner=None):
    """Biale pasy u gory i u dolu (wiersze 0..2 i 24..26) trzymaja kadr w miejscu; w wierszach 3..23 kolumny 0..3 maja
    kolor `edge`, reszta `inner`. Opcjonalnie inna prawa polowa i inny kolor srodka w dolnej czesci (wiersze 13..23)."""
    cells = []
    for y in range(H):
        for x in range(W):
            if y < 3 or y > 23:
                c = WHITE
            elif x < 4:
                c = edge
            elif right_edge is not None and x >= 44:
                c = right_edge
            elif right_inner is not None and x >= 24:
                c = right_inner
            elif lower_inner is not None and y >= 13:
                c = lower_inner
            else:
                c = inner
            cells.append(list(c))
    return {"mode": "cells", "nx": W, "ny": H, "cells": cells}


def border_pattern(border, inside, width=4):
    cells = [list(border if (x < width or x >= W - width or y < width) else inside) for y in range(H) for x in range(W)]
    return {"mode": "cells", "nx": W, "ny": H, "cells": cells}


def solid(c):
    return {"mode": "solid", "color": list(c)}


def read_field():
    txt = esp.cmd("grid", wait=2.0)
    cells = [tuple(int(t[i:i + 2], 16) for i in (0, 2, 4)) for t in re.findall(r"\b[0-9A-Fa-f]{6}\b", txt)]
    if len(cells) != W * H:
        return None
    return [cells[y * W:(y + 1) * W] for y in range(H)]


def measure(pattern, mix_pct):
    """Ustaw wzor i doswietlenie, poczekaj na ustalenie, oddaj (sekcje BK, model, kadr)."""
    bk.set_number("Video: inner fill", mix_pct)
    post(pattern)
    time.sleep(2.6)                       # wygladzanie ESP32 i BK7238, narastanie doswietlenia (0,2 s)
    field = None
    for _ in range(3):
        field = read_field()
        if field:
            break
    got = bk_sections()
    if not field or not got:
        return None
    sec, crop, pct = got
    if pct != int(round(mix_pct)):
        print(f"   UWAGA: BK podaje doswietlenie {pct} %, a ustawiono {mix_pct} %")
    m = model(field, crop, mix_pct / 100.0)
    # tolerancja: 2 poziomy + to, ile zmienia sie wynik modelu, gdy cale pole przesunie sie o 1 poziom (stroma gamma,
    # duze nasycenie i ostre granice wzoru wzmacniaja szum odczytu pola)
    shift = lambda k: [[tuple(min(max(v + k, 0), 255) for v in px) for px in row] for row in field]
    tol = 2 + max(diff(model(shift(1), crop, mix_pct / 100.0), m), diff(model(shift(-1), crop, mix_pct / 100.0), m))
    return sec, m, crop, tol


def diff(a, b):
    return max(abs(x - y) for s in a for p, q in zip(a[s], b[s]) for x, y in zip(p, q))


def fmt(c):
    return "(%3d,%3d,%3d)" % c


TESTS = [
    # nazwa, wzor, co ma wyjsc
    ("T0a jednolity szary 128", solid((128, 128, 128)), "same"),
    ("T0b jednolita biel", solid(WHITE), "same"),
    ("T0c jasna ramka, ciemny srodek", border_pattern(WHITE, BLACK), "same"),
    ("T0d niebieska ramka, pomaranczowy srodek", border_pattern((0, 0, 255), (255, 140, 20)), "same"),
    ("T1 czarna krawedz z lewej, bialy srodek", frame_pattern(BLACK, WHITE), "lift_left"),
    ("T2 czarna krawedz z lewej, czerwony srodek", frame_pattern(BLACK, RED), "lift_left"),
    ("T3a szara krawedz 48, bialy srodek", frame_pattern((48, 48, 48), WHITE), "lift_left"),
    ("T3b szara krawedz 96, bialy srodek", frame_pattern((96, 96, 96), WHITE), "lift_left"),
    ("T3c szara krawedz 144, bialy srodek", frame_pattern((144, 144, 144), WHITE), "not_darker"),
    ("T3d szara krawedz 200, bialy srodek", frame_pattern((200, 200, 200), WHITE), "same"),
    ("T6 ciemnoniebieska krawedz, pomaranczowy srodek", frame_pattern((20, 30, 80), (255, 140, 20)), "lift_left"),
    ("T7 srodek liczony osobno dla bokow (prawa polowa ciemna)",
     frame_pattern(BLACK, WHITE, right_edge=BLACK, right_inner=(40, 40, 40)), "left_only"),
    ("T8 srodek liczony osobno dla sekcji (dol srodka ciemny)", frame_pattern(BLACK, WHITE, lower_inner=(40, 40, 40)), "upper_only"),
    ("T11 ciemna scena: pole 16, blok 72 w srodku",
     {"mode": "cells", "nx": W, "ny": H,
      "cells": [[72, 72, 72] if 12 <= x <= 35 and 6 <= y <= 20 else [16, 16, 16] for y in range(H) for x in range(W)]}, "same"),
]


def main():
    if not _m.page_open():
        print("UWAGA: strona testowa nie odpytuje serwera - wyniki beda bez znaczenia")
    bk.set_switch("Diagnostics: section log", True)
    time.sleep(1.0)
    read_cfg(bk.states)
    print("Tor BK7238:", CFG)
    ok = True
    prev_left = None
    try:
        for name, pattern, expect in TESTS:
            if ONLY and not name.startswith(ONLY):
                continue
            r0 = measure(pattern, 0)
            r1 = measure(pattern, MIX)
            if not r0 or not r1:
                print(f"{name}: brak odczytu (pole z ESP32 albo log sekcji z BK7238)")
                ok = False
                if not bk.connected():          # restart BK7238: polacz ponownie i wlacz log sekcji
                    time.sleep(15)
                    bk.reconnect()
                    bk.set_switch("Diagnostics: section log", True)
                continue
            off, m_off, crop, tol0 = r0
            on, m_on, _, tol1 = r1
            d_off, d_on = diff(off, m_off), diff(on, m_on)
            mid = SIDES["L"] // 2
            change = diff(on, off)
            good = d_off <= tol0 and d_on <= tol1
            noise = max(1, max(tol0, tol1) - 2)     # o tyle moga sie roznic dwa odczyty tego samego wzoru
            not_darker = all(c1 >= c0 - noise for sd in SIDES for p0, p1 in zip(off[sd], on[sd]) for c0, c1 in zip(p0, p1))
            good &= not_darker                  # doswietlenie niczego nie przyciemnia
            if expect == "same":
                good &= change <= noise
            elif expect == "lift_left":
                good &= all(max(on["L"][i]) > max(off["L"][i]) for i in (mid - 1, mid, mid + 1))
                good &= all(c1 >= c0 - 1 for i in range(SIDES["L"]) for c0, c1 in zip(off["L"][i], on["L"][i]))
            elif expect == "left_only":
                good &= max(on["L"][mid]) > max(off["L"][mid]) + 10 and max(on["P"][mid]) <= max(off["P"][mid]) + 1
            elif expect == "upper_only":
                good &= max(on["L"][SIDES["L"] - 2]) > max(off["L"][SIDES["L"] - 2]) + 10 and max(on["L"][1]) <= max(off["L"][1]) + 3
            ok &= good
            print(f"{name}: {'OK' if good else 'ZLE'}   kadr x {crop[0]}..{crop[1]} y {crop[2]}..{crop[3]}")
            print(f"   lewy bok, srodkowa sekcja: bez {fmt(off['L'][mid])} -> z doswietleniem {fmt(on['L'][mid])}"
                  f"   (model: {fmt(m_off['L'][mid])} -> {fmt(m_on['L'][mid])})")
            print(f"   najwieksza roznica BK7238 - model: bez {d_off} (tolerancja {tol0}), z {d_on} (tolerancja {tol1}); "
                  f"najwieksza zmiana sekcji po wlaczeniu: {change}")
            if VERBOSE:
                for label, got, mod in (("bez", off, m_off), ("z  ", on, m_on)):
                    for side in ("L", "G", "P"):
                        print(f"   {label} {side} BK   :", " ".join("%02X%02X%02X" % c for c in got[side]))
                        print(f"   {label} {side} model:", " ".join("%02X%02X%02X" % c for c in mod[side]))
            if expect == "lift_left" and name.startswith("T3"):
                cur = max(on["L"][mid])
                if prev_left is not None and cur < prev_left - 1:
                    print(f"   ZLE: jasnosc spadla przy jasniejszej krawedzi ({prev_left} -> {cur})")
                    ok = False
                prev_left = cur
    finally:
        bk.set_number("Video: inner fill", MIX)
        bk.set_switch("Diagnostics: section log", False)
        post({"mode": "idle"})
        time.sleep(0.5)
    if bk.drops:
        print(f"UWAGA: polaczenie z BK7238 zerwane {bk.drops} razy w trakcie testu (restart urzadzenia?)")
        ok = False
    bk.close()
    print("WYNIK:", "wszystko OK" if ok else "sa bledy")


main()
