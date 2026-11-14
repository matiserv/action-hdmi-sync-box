"""Zgubione klatki BEZ obserwatora logu: liczniki sa czytane na poczatku i na koncu, a w srodku nikt nie odbiera logu
(wyslanie kazdej linii do klienta API kosztuje box kilka ms). Przelacznik satelitow wraca do stanu sprzed pomiaru.

Uzycie: python satquiet.py <bench> <host> <yaml> [sekundy bez obserwatora = 60]
"""
import re
import sys
import time

sys.path.insert(0, sys.argv[1])
from bkapi import Bk  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
host, yaml_path = sys.argv[2:4]
SECS = float(sys.argv[4]) if len(sys.argv) > 4 else 60
RE = re.compile(r"klatki: (\d+) pełne, (\d+) częściowe, (\d+) zgubione, (\d+) błędne")
SW = "Satelity (E1.31)"


def counters(sat=None):
    bk = Bk(host, yaml_path)
    time.sleep(2.5)
    st = bk.states.get(SW)
    if sat is not None:
        bk.set_switch(SW, sat)
    line = bk.wait_log("klatki:", 4.0)
    t = time.time()
    bk.close()
    m = RE.search(line or "")
    return ([int(x) for x in m.groups()] if m else None), t, st


_, _, before = counters()
for label, sat in (("satelity WL", True), ("satelity WYL", False)):
    counters(sat)
    time.sleep(3)
    a, ta, _ = counters()
    time.sleep(SECS)
    b, tb, _ = counters()
    if a and b:
        dt = tb - ta
        print(f"{label}, bez obserwatora: {dt:.0f} s | klatek +{b[0] - a[0]} ({(b[0] - a[0]) / dt:.2f}/s) | zgubione +{b[2] - a[2]}, "
              f"czesciowe +{b[1] - a[1]}, bledne +{b[3] - a[3]}")
    else:
        print(label, ": brak linii licznikow")
counters(bool(before))
print("przelacznik przywrocony do:", before)
