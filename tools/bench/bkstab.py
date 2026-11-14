"""Stabilnosc tasmy mierzona na wyjsciu BK7238: kolory sekcji z logu (raz na sekunde) przez N sekund.

Nie dotyka ESP32 w trakcie pomiaru (komenda `frames` wstrzymuje probkowanie i sama wprowadza zaburzenie), wiec
pokazuje to, co faktycznie idzie na tasme przy tym, co jest teraz na ekranie. Dla kazdej sekcji: sredni kolor
i rozstep (max - min) kazdej skladowej w czasie pomiaru, w poziomach tasmy 0..255.

Uzycie:  python bkstab.py 192.168.1.50 <plik YAML urzadzenia z kluczem API> [sekundy = 30] [etykieta]
"""
import re
import statistics as st
import sys
import time

from bkapi import Bk

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
bk = Bk(sys.argv[1], sys.argv[2])
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 30
label = sys.argv[4] if len(sys.argv) > 4 else ""
SEC_RE = re.compile(r"sections L:((?:[0-9A-F]{6} )*)T:((?:[0-9A-F]{6} )*)R:((?:[0-9A-F]{6} )*)\s*crop x (\d+)\.\.(\d+) y (\d+)\.\.(\d+)")
time.sleep(1.0)
bk.set_switch("Diagnostics: section log", True)
time.sleep(secs + 1.5)
bk.set_switch("Diagnostics: section log", False)
time.sleep(0.5)
rows, crops = [], set()
for _, t in bk.logs:
    m = SEC_RE.search(t)
    if not m:
        continue
    cols = lambda s: [tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) for h in s.split()]
    rows.append(cols(m.group(1)) + cols(m.group(2)) + cols(m.group(3)))
    crops.add(tuple(int(m.group(i)) for i in (4, 5, 6, 7)))
n = [len(r) for r in rows]
rows = [r for r in rows if len(r) == max(n)] if rows else []
print(f"{label} odczytow: {len(rows)}, kadr: {sorted(crops)}, zerwania polaczenia: {bk.drops}")
if len(rows) < 5:
    bk.close()
    raise SystemExit("za malo odczytow")
ns = len(rows[0])
names = [f"L{i}" for i in range(7)] + [f"G{i}" for i in range(14)] + [f"P{i}" for i in range(7)]
spans = []
line = []
for s in range(ns):
    sp = max(max(r[s][c] for r in rows) - min(r[s][c] for r in rows) for c in range(3))
    mean = tuple(st.mean(r[s][c] for r in rows) for c in range(3))
    # ile razy sekcja zmienila kolor miedzy kolejnymi odczytami
    ch = sum(1 for a, b in zip(rows, rows[1:]) if a[s] != b[s])
    spans.append(sp)
    line.append(f"{names[s] if s < len(names) else s}:{sp}" + (f"({mean[0]:.0f},{mean[1]:.0f},{mean[2]:.0f};zm {ch})" if sp >= 2 else ""))
print("rozstep sekcji (poziomy tasmy; w nawiasie sredni kolor i liczba zmian miedzy odczytami):")
print("  " + "  ".join(line))
print(f"suma rozstepow {sum(spans)}, najwiekszy {max(spans)}, sekcji z rozstepem >= 2: {sum(s >= 2 for s in spans)}, >= 5: {sum(s >= 5 for s in spans)}")
for _, t in bk.logs:
    if "Kadr:" in t or "Sygna" in t:
        print("  ", t.strip()[:120])
bk.close()
