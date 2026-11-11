"""Przebieg składowych R,G,B w kolejnych ramkach przy przełączaniu dwóch bliskich kolorów (średnia ze środka pola).
Użycie: python huetrace.py COM4 R,G,B R,G,B [okres_klatek=17] [serie=6] ["komendy; ..."]
"""
import importlib.util, os, statistics as st, sys, time
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
esp = _m.Esp(sys.argv[1])
a = [int(v) for v in sys.argv[2].split(",")]; b = [int(v) for v in sys.argv[3].split(",")]
period = int(sys.argv[4]) if len(sys.argv) > 4 else 17
series = int(sys.argv[5]) if len(sys.argv) > 5 else 6
for c in (sys.argv[6].split(";") if len(sys.argv) > 6 else []):
    if c.strip():
        esp.cmd(c.strip(), wait=0.35)

def mean_rgb(f):
    H = len(f); W = len(f[0])
    px = [f[y][x] for y in range(H // 4, 3 * H // 4) for x in range(W // 4, 3 * W // 4)]
    return tuple(round(st.mean(p[k] for p in px), 1) for k in range(3))

for name, col in (("stały a", a), ("stały b", b)):
    _m.show({"mode": "solid", "color": col}, settle=2.0)
    fr = esp.frames(4)
    print(f"{name} {tuple(col)}: zmierzone {mean_rgb(fr[-1])}")
_m.show({"mode": "flash", "a": a, "b": b, "frames": period}, settle=1.0)
print("przełączanie, kolejne ramki w seriach po 4:")
for s in range(series):
    fr = esp.frames(4)
    print("  " + "  ".join("%5.1f,%5.1f,%5.1f" % mean_rgb(f) for f in fr))
_m.post({"mode": "idle"})
