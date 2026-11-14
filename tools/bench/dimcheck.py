"""Przygaszanie przy ciemnym srodku na zywym obrazie: kolory sekcji z logu BK7238 kontra model (bkmodel.py)
dla kilku wartosci suwaka. Obraz powinien byc w tym czasie nieruchomy. Na koniec suwak wraca do wartosci sprzed testu.

Uzycie: python dimcheck.py <host> <yaml z kluczem API> [wartosci suwaka po przecinku = 0,50,100] [--pattern2]
--pattern2: zamiast obrazu wzor testowy ESP32 (boki i gora w pelnych kolorach, srodek czarny); dziala bez sygnalu HDMI.
Haslo konsoli ESP32: zmienna srodowiskowa SYNCBOX_CONSOLE_PASSWORD.
"""
import re
import statistics as st
import sys
import time

from bench import Esp
from bkapi import Bk
from bkmodel import CFG, model, read_cfg

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
host, yaml_path = sys.argv[1:3]
vals = [float(x) for x in (sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else "0,50,100").split(",")]
DIM = "Wideo: przygaszanie przy ciemnym srodku"
MIX = "Wideo: doswietlenie ze srodka"
SEC_RE = re.compile(r"sekcje L:((?:[0-9A-F]{6} )*)G:((?:[0-9A-F]{6} )*)P:((?:[0-9A-F]{6} )*)\s*kadr x (\d+)\.\.(\d+) y (\d+)\.\.(\d+)")
bk = Bk(host, yaml_path)
time.sleep(4)
read_cfg(bk.states)
before = bk.states.get(DIM)
mix = float(bk.states.get(MIX) or 0) / 100.0
print(f"ustawienia: glebokosc {CFG['depth']}, nasycenie {CFG['sat']}, gamma {CFG['gamma']}, doswietlenie {mix * 100:.0f} %, "
      f"przygaszanie przed testem {before}")
esp = Esp("tcp://" + host + ":2323")
PATTERN = "--pattern2" in sys.argv        # bez sygnalu HDMI: wzor testowy ESP32 (lewy bok R, gora G, prawy bok B, dol bialy 200, srodek czarny)
if PATTERN:
    esp.cmd("pattern 2", wait=0.5)
    time.sleep(2)


def pattern_field():
    f = [[(0, 0, 0)] * 48 for _ in range(27)]
    for y in range(27):
        for x in range(48):
            if x < 3:
                f[y][x] = (255, 0, 0)
            elif y < 3:
                f[y][x] = (0, 255, 0)
            elif x >= 45:
                f[y][x] = (0, 0, 255)
            elif y >= 24:
                f[y][x] = (200, 200, 200)
    return f


def grab():
    return pattern_field() if PATTERN else esp.frames(1)[0]


bk.set_switch("Diagnostyka: log sekcji", True)


def sections():
    line = bk.wait_log("sekcje L:", 4.0)
    m = SEC_RE.search(line) if line else None
    if not m:
        return None, None
    cols = lambda t: [tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) for h in t.split()]
    return {"L": cols(m.group(1)), "G": cols(m.group(2)), "P": cols(m.group(3))}, tuple(int(m.group(i)) for i in (4, 5, 6, 7))


try:
    for v in vals:
        bk.set_number(DIM, v)
        time.sleep(3.0)                       # wygladzanie tasmy i stala czasowa przygaszania
        f1 = grab()
        time.sleep(1.2)
        sec, crop = sections()
        f2 = grab()
        if sec is None:
            print(f"przygaszanie {v:.0f} %: brak linii sekcji w logu")
            continue
        move = max(abs(f1[y][x][c] - f2[y][x][c]) for y in range(len(f1)) for x in range(len(f1[0])) for c in range(3))
        m = model(f1, crop, mix, dim=v / 100.0)
        m0 = model(f1, crop, mix, dim=0.0)
        diffs, bright, bright0 = [], [], []
        for side in ("L", "G", "P"):
            for a, b, b0 in zip(sec[side], m[side], m0[side]):
                diffs.append(max(abs(a[c] - b[c]) for c in range(3)))
                bright.append(max(a))
                bright0.append(max(b0))
        if PATTERN:
            print("   L (od dolu):", " ".join("%02X%02X%02X" % c for c in sec["L"]), "| G (pierwsze 4):", " ".join("%02X%02X%02X" % c for c in sec["G"][:4]), "| kadr", crop)
        print(f"przygaszanie {v:3.0f} %: roznica tasma-model max {max(diffs)}, srednio {st.mean(diffs):.1f}, sekcji z roznica > 4: "
              f"{sum(d > 4 for d in diffs)}/{len(diffs)} | jasnosc tasmy srednio {st.mean(bright):.0f} (bez przygaszania wg modelu "
              f"{st.mean(bright0):.0f}) | ruch obrazu miedzy odczytami {move}")
finally:
    bk.set_switch("Diagnostyka: log sekcji", False)
    if PATTERN:
        esp.cmd("pattern 0", wait=0.5)
    if before is not None:
        bk.set_number(DIM, float(before))
    time.sleep(1)
    print("suwak po tescie:", bk.states.get(DIM))
    bk.close()
