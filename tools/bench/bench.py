"""Benchmark toru wideo syncbox-esp32: ustawia wzór na stronie testowej i porównuje z siatką z ESP32 (domyślnie 48x27).

Wymaga: działającego server.py, strony otwartej na ekranie za sync boxem (pełny ekran), ESP32 na porcie szeregowym.
Użycie:  python bench.py COM4 [testy...]      testy: levels cells flash sweep (domyślnie wszystkie)
"""
import json
import re
import statistics as st
import sys
import time
import urllib.request

from esplink import open_port

URL = "http://localhost:8765"
sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def post(state):
    req = urllib.request.Request(URL + "/state", data=json.dumps(state).encode(), headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=3).read()


def page_open():
    try:
        d = json.loads(urllib.request.urlopen(URL + "/clients", timeout=3).read())
        return d["age_s"] < 1.0
    except Exception:
        return False


class Esp:
    def __init__(self, port):
        self.s = open_port(port)      # COMx albo tcp://adres:2323 (konsola przez Wi-Fi)

    def cmd(self, c, wait=1.0):
        self.s.reset_input_buffer()
        self.s.write((c + "\r").encode())
        buf = b""
        t_end = time.time() + wait
        while time.time() < t_end:
            buf += self.s.read(4096)
        return re.sub(r"\x1b\[[0-9;]*m", "", buf.decode("utf-8", "replace"))

    def frames(self, n):
        """n ramek jako lista siatek [wiersz][kolumna] krotek (r, g, b). Firmware nagrywa najwyżej 4 kolejne ramki
        na komendę, więc większe n jest zbierane porcjami (między porcjami jest przerwa na wydruk)."""
        if n > 4:
            out = []
            while len(out) < n:
                part = self.frames(min(4, n - len(out)))
                if not part:
                    break
                out += part
            return out
        txt = self.cmd(f"frames {n}", wait=1.0 + n * 0.5)   # pole 32x18: ok. 4 kB tekstu na ramke
        # ramki sa oddzielone liniami "ramka N:"; liczba wierszy zalezy od ustawienia siatki (16 albo 9)
        out = []
        cur = None
        for l in txt.splitlines():
            if l.startswith("ramka"):
                cur = []
                out.append(cur)
            elif cur is not None and re.fullmatch(r"([0-9A-F]{6} ){3,63}[0-9A-F]{6} ?", l):
                cur.append([((int(h, 16) >> 16) & 255, (int(h, 16) >> 8) & 255, int(h, 16) & 255) for h in l.split()])
        h = max((len(f) for f in out), default=0)
        out = [f for f in out if len(f) == h and h > 0]
        return out


def show(state, settle=0.6):
    post(state)
    time.sleep(settle)


def test_levels(esp):
    print("\n== Poziomy i kolory (pełny ekran, 4 ramki; oczekiwane = zadane) ==")
    print("zadane          zmierzone (średnia)   błąd max kanału   rozrzut komórek (max odchyłka od średniej)")
    cols = [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255), (32, 32, 32), (64, 64, 64), (128, 128, 128),
            (192, 192, 192), (224, 224, 224), (255, 255, 0), (0, 255, 255), (255, 0, 255), (255, 128, 0), (40, 90, 200)]
    worst = 0
    for c in cols:
        show({"mode": "solid", "color": list(c)})
        fr = esp.frames(4)
        if not fr:
            print(c, "brak ramek"); continue
        ch = [[p[k] for f in fr for row in f for p in row] for k in range(3)]
        mean = [st.mean(v) for v in ch]
        spread = max(max(abs(x - mean[k]) for x in ch[k]) for k in range(3))
        err = max(abs(mean[k] - c[k]) for k in range(3))
        worst = max(worst, err)
        print(f"{str(c):15s} {mean[0]:6.1f} {mean[1]:6.1f} {mean[2]:6.1f}      {err:5.1f}            {spread:5.1f}")
    print(f"największy błąd średniej: {worst:.1f} / 255")


def test_cells(esp):
    print("\n== Mapowanie przestrzenne (wzór z komórek; R koduje kolumnę, G wiersz) ==")
    for n in (8, 16):
        cells = [[(x * 255) // (n - 1), (y * 255) // (n - 1), 128 if (x + y) % 2 else 40] for y in range(n) for x in range(n)]
        show({"mode": "cells", "nx": n, "ny": n, "cells": cells})
        fr = esp.frames(4)
        if not fr:
            print("brak ramek"); continue
        errs = []; col_err = [[] for _ in range(64)]; row_err = [[] for _ in range(64)]
        for f in fr:
            for y in range(len(f)):
                for x in range(len(f[0])):
                    exp = cells[(y * n // len(f)) * n + (x * n // len(f[0]))]
                    e = max(abs(f[y][x][k] - exp[k]) for k in range(3))
                    errs.append(e); col_err[x].append(f[y][x][0] - exp[0]); row_err[y].append(f[y][x][1] - exp[1])
        print(f"wzór {n}x{n}: błąd średni {st.mean(errs):.1f}, mediana {st.median(errs):.0f}, max {max(errs)}; komórek z błędem > 30: "
              f"{sum(e > 30 for e in errs) * 100 // len(errs)} %")
        print("  błąd R (pozycja pozioma) wg kolumny:", " ".join(f"{st.mean(c):+.0f}" for c in col_err if c))
        print("  błąd G (pozycja pionowa) wg wiersza:", " ".join(f"{st.mean(r):+.0f}" for r in row_err if r))


def test_flash(esp):
    print("\n== Skok czerń/biel co 6 klatek ekranu (100 ms), 8 kolejnych ramek siatki ==")
    show({"mode": "flash", "a": [0, 0, 0], "b": [255, 255, 255], "frames": 6})
    inter = 0; total = 0
    for i in range(4):
        fr = esp.frames(8)
        lum = [st.mean(sum(p) / 3 for row in f for p in row) for f in fr]
        inter += sum(20 < v < 235 for v in lum); total += len(lum)
        print("  jasność ramek:", " ".join(f"{v:5.0f}" for v in lum))
    print(f"ramek pośrednich (ani czerń, ani biel): {inter} z {total}")


def test_sweep(esp):
    print("\n== Ruch: biały pas 1/8 ekranu, przejazd w 2 s, 8 kolejnych ramek siatki ==")
    for vertical, name in ((False, "poziomo"), (True, "pionowo")):
        show({"mode": "sweep", "width": 0.125, "period": 2000, "color": [255, 255, 255], "bg": [0, 0, 0], "vertical": vertical})
        steps = []
        for i in range(4):
            fr = esp.frames(8)
            pos = []
            for f in fr:
                prof = [st.mean(sum(f[y][x]) / 3 for y in range(len(f))) for x in range(len(f[0]))] if not vertical else \
                       [st.mean(sum(f[y][x]) / 3 for x in range(len(f[0]))) for y in range(len(f))]
                tot = sum(prof)
                pos.append(sum(i2 * v for i2, v in enumerate(prof)) / tot if tot > 200 else None)
            print(f"  {name}: środek pasa w ramkach:", " ".join("  --" if p is None else f"{p:5.2f}" for p in pos))
            steps += [b - a for a, b in zip(pos, pos[1:]) if a is not None and b is not None and b > a]
        exp = ((len(fr[0]) if vertical else len(fr[0][0])) if fr else 16) * (1 + 0.125) * (1000 / 60) / 2000
        if steps:
            print(f"  {name}: krok na ramkę średnio {st.mean(steps):.3f} (oczekiwane {exp:.3f}), odchylenie {st.pstdev(steps):.3f}")


if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    tests = sys.argv[2:] or ["levels", "cells", "flash", "sweep"]
    if not page_open():
        print("Strona testowa nie jest otwarta (brak odpytań /state w ostatniej sekundzie). Otwórz", URL, "na ekranie za sync boxem.")
        sys.exit(2)
    esp = Esp(port)
    for t in tests:
        {"levels": test_levels, "cells": test_cells, "flash": test_flash, "sweep": test_sweep}[t](esp)
    post({"mode": "idle"})
