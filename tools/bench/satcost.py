"""Ile satelity E1.31 kosztuja tasme TV: FPS wideo, zapisy tasmy, ramki uciete i czas wysylki, z przelacznikiem
"Satelity (E1.31)" wlaczonym, wylaczonym i znow wlaczonym (na koniec zostaje wlaczony).

Czyta linie "klatki: ..." i "satelity: ..." z logu BK7238 (wymaga `log_frames: true`).

Uzycie:  python satcost.py 192.168.1.50 <yaml z kluczem API> [sekundy na pomiar]
"""
import re
import statistics as st
import sys
import time

from bkapi import Bk

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
host, yaml_path = sys.argv[1:3]
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 10
bk = None
for _ in range(8):                       # tuz po wgraniu firmware urzadzenie jeszcze startuje
    try:
        bk = Bk(host, yaml_path)
        break
    except Exception:
        time.sleep(5)
if bk is None:
    sys.exit("brak polaczenia z " + host)
time.sleep(8)


def measure(label):
    t0 = time.time()
    time.sleep(SECS)
    v, s, part, sat = [], [], [], []
    for t, text in bk.logs:
        if t < t0 + 2:
            continue
        m = re.search(r"(\d+) częściowe, (\d+) zgubione.*wideo ([\d.]+) fps, taśma ([\d.]+) fps", text)
        if m:
            part.append((int(m.group(1)), int(m.group(2))))
            v.append(float(m.group(3)))
            s.append(float(m.group(4)))
        m = re.search(r"satelity: (\d+) pakiet\S*, obsługa śr\. (\d+) us, max (\d+) us, błędy wysyłki (\d+)", text)
        if m:
            sat.append(tuple(int(x) for x in m.groups()))
    txt = (f"{label}: wideo {st.mean(v):.1f} fps, tasma {st.mean(s):.1f} zapisow/s, ramki uciete "
           f"+{part[-1][0] - part[0][0]}, zgubione +{part[-1][1] - part[0][1]} w {len(part) - 1} s")
    if sat:
        txt += (f"; pakietow/s {st.mean(x[0] for x in sat):.0f}, obsluga sr. {st.mean(x[1] for x in sat):.0f} us, "
                f"max {max(x[2] for x in sat)} us, bledy wysylki {sat[-1][3]}")
    print(txt)


measure("satelity wlaczone")
bk.set_switch("Satelity (E1.31)", False)
time.sleep(1)
measure("satelity wylaczone")
bk.set_switch("Satelity (E1.31)", True)
time.sleep(1)
measure("satelity wlaczone ponownie")
print("na koniec przelacznik satelitow:", bk.states.get("Satelity (E1.31)"), "| zerwan polaczenia:", bk.drops)
bk.close()
