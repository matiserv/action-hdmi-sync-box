"""Sprawdzenie poprawek ESP32 v13.1 na stanowisku.

1. Male pole (4x4, 5x5): licznik wag probek byl 16-bitowy i przy takim polu sie przewijal (szary 128 czytal sie jako biel).
2. Dlugi wydruk konsoli (`frames 4`, ok. 3 s) nie moze zatrzymywac ramek wysylanych do BK7238.
3. `set spi_hz` z wartoscia spoza zakresu nie moze wylozyc firmware.

Uzycie:  python fixes131.py COM4
"""
import re
import sys
import time

import bench as _m
from bench import Esp, post

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
esp = Esp(sys.argv[1])


def grid():
    txt = esp.cmd("grid", wait=2.0)
    cells = [tuple(int(t[i:i + 2], 16) for i in (0, 2, 4)) for t in re.findall(r"\b[0-9A-Fa-f]{6}\b", txt)]
    return cells


def sent():
    m = re.search(r"wyslanych (\d+) ramek", esp.cmd("stat", wait=1.5))
    return int(m.group(1)) if m else -1


ok = True
if not _m.page_open():
    print("UWAGA: strona testowa nie odpytuje serwera - wyniki kolorow beda bez znaczenia")

print("== 1. male pole ==")
post({"mode": "solid", "color": [128, 128, 128]})
time.sleep(2.0)
for w, h in ((4, 4), (5, 5), (8, 8)):
    esp.cmd(f"set grid_w {w}", wait=0.6)
    esp.cmd(f"set grid_h {h}", wait=1.5)
    c = grid()
    if len(c) != w * h:
        print(f"  {w}x{h}: odczytano {len(c)} komorek zamiast {w * h}")
        ok = False
        continue
    lo = min(min(p) for p in c)
    hi = max(max(p) for p in c)
    good = 120 <= lo and hi <= 136
    ok &= good
    print(f"  {w}x{h}: szary 128 -> min {lo}, max {hi}  {'OK' if good else 'ZLE'}")
esp.cmd("set grid_w 48", wait=0.6)
esp.cmd("set grid_h 27", wait=1.5)
c = grid()
print(f"  powrot do 48x27: {len(c)} komorek, min {min(min(p) for p in c)}, max {max(max(p) for p in c)}")

print("== 2. wydruk konsoli a ramki ==")
a = sent()
t0 = time.time()
esp.cmd("frames 4", wait=4.5)
b = sent()
dt = time.time() - t0
rate = (b - a) / dt
good = rate > 55
ok &= good
print(f"  podczas `frames 4`: {b - a} ramek w {dt:.1f} s = {rate:.1f} na sekunde  {'OK' if good else 'ZLE'}")

print("== 3. spi_hz spoza zakresu ==")
txt = esp.cmd("set spi_hz 8M", wait=1.5)
a = sent()
time.sleep(1.0)
b = sent()
good = "poza zakresem" in txt and b > a
ok &= good
print(f"  `set spi_hz 8M`: {'odrzucone' if 'poza zakresem' in txt else 'PRZYJETE'}, ramki ida dalej: {b > a}  {'OK' if good else 'ZLE'}")

post({"mode": "idle"})
print("WYNIK:", "wszystko OK" if ok else "sa bledy")
