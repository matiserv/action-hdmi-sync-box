"""Satelity a tasma TV: liczniki BK7238 sekunda po sekundzie, z satelitami wylaczonymi, wlaczonymi i znow wylaczonymi.
Na koniec przelacznik wraca do stanu sprzed pomiaru.

Uzycie: python satflick.py <host> <yaml z kluczem API> [sekundy na faze = 40]
"""
import re
import statistics as st
import sys
import time

from bkapi import Bk

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
host, yaml_path = sys.argv[1:3]
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 40
bk = Bk(host, yaml_path)
time.sleep(4)
SW = "Satellites (E1.31)"
before = bk.states.get(SW)
print("przelacznik przed pomiarem:", before)
RE = re.compile(r"frames: (\d+) ok, (\d+) partial, (\d+) dropped, (\d+) bad; video ([\d.]+) fps, strip ([\d.]+) fps")
RS = re.compile(r"satellites: (\d+) packets, service avg (\d+) us, max (\d+) us, send errors (\d+)")


def phase(label, on):
    bk.set_switch(SW, on)
    time.sleep(2)
    t0 = time.time()
    time.sleep(SECS)
    rows, sat = [], []
    for t, text in bk.logs:
        if t < t0:
            continue
        m = RE.search(text)
        if m:
            rows.append([int(m.group(i)) for i in (1, 2, 3, 4)] + [float(m.group(5)), float(m.group(6))])
        m = RS.search(text)
        if m:
            sat.append([int(x) for x in m.groups()])
    if len(rows) < 3:
        print(label, ": za malo linii logu (", len(rows), ")")
        return
    led = [r[5] for r in rows]
    vid = [r[4] for r in rows]
    d = [rows[-1][i] - rows[0][i] for i in range(4)]
    print(f"{label}: {len(rows)} s | wideo sr. {st.mean(vid):.1f} min {min(vid):.1f} | tasma sr. {st.mean(led):.1f} "
          f"min {min(led):.1f} max {max(led):.1f} | pelne +{d[0]}, czesciowe +{d[1]}, zgubione +{d[2]}, bledne +{d[3]}")
    print("   tasma zapisy/s po kolei:", " ".join(f"{x:.0f}" for x in led))
    if sat:
        print(f"   satelity: pakietow/s {st.mean(x[0] for x in sat):.0f}, obsluga sr. {st.mean(x[1] for x in sat):.0f} us, "
              f"max {max(x[2] for x in sat)} us, bledy wysylki {sat[-1][3]}")


try:
    phase("satelity WYL (1)", False)
    phase("satelity WL", True)
    phase("satelity WYL (2)", False)
finally:
    bk.set_switch(SW, bool(before))
    time.sleep(1)
    print("przelacznik po pomiarze:", bk.states.get(SW), "| zerwan polaczenia:", bk.drops)
    bk.close()
