"""Stabilność na tym, co akurat jest na ekranie (bez strony testowej): nagrywa serie ramek i pokazuje,
które komórki siatki 16x16 zmieniają się w czasie i przy jakiej jasności.
Użycie: python live.py COM4 [serie=6] ["komendy konsoli; ..."]
"""
import importlib.util, os, statistics as st, sys, time
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
esp = _m.Esp(sys.argv[1])
series = int(sys.argv[2]) if len(sys.argv) > 2 else 6
for c in (sys.argv[3].split(";") if len(sys.argv) > 3 else []):
    if c.strip():
        esp.cmd(c.strip(), wait=0.35)
time.sleep(1.0)
frames = []
for i in range(series):
    frames += esp.frames(8)
n = len(frames)
H = len(frames[0]); W = len(frames[0][0])
print(f"ramek: {n}, pole {W} x {H}")
mean = [[[st.mean(f[y][x][k] for f in frames) for k in range(3)] for x in range(W)] for y in range(H)]
rng = [[max(max(f[y][x][k] for f in frames) - min(f[y][x][k] for f in frames) for k in range(3)) for x in range(W)] for y in range(H)]
sd = [[max(st.pstdev([f[y][x][k] for f in frames]) for k in range(3)) for x in range(W)] for y in range(H)]
print("średnia jasność komórek (max z R,G,B):")
for y in range(H):
    print(" ".join(f"{max(mean[y][x]):4.0f}" for x in range(W)))
print("zakres zmian w czasie (max z R,G,B):")
for y in range(H):
    print(" ".join(f"{rng[y][x]:4d}" for x in range(W)))
edge = [(y, x) for y in range(H) for x in range(W) if y in (0, H - 1) or x in (0, W - 1)]
print(f"obwód (taśma): zakres średnio {st.mean(rng[y][x] for y, x in edge):.1f}, max {max(rng[y][x] for y, x in edge)}; "
      f"odchylenie średnio {st.mean(sd[y][x] for y, x in edge):.2f}")
print(f"cała siatka: zakres średnio {st.mean(rng[y][x] for y in range(H) for x in range(W)):.1f}, "
      f"odchylenie średnio {st.mean(sd[y][x] for y in range(H) for x in range(W)):.2f}")
# zależność od jasności
for lo, hi in ((0, 8), (8, 24), (24, 64), (64, 256)):
    sel = [(y, x) for y in range(H) for x in range(W) if lo <= max(mean[y][x]) < hi]
    if sel:
        print(f"  jasność {lo:3d}..{hi:3d}: {len(sel):3d} komórek, zakres średnio {st.mean(rng[y][x] for y, x in sel):.1f}, max {max(rng[y][x] for y, x in sel)}")
# surowe wartości najbardziej niestabilnej komórki obwodu
wy, wx = max(edge, key=lambda p: rng[p[0]][p[1]])
print(f"najgorsza komórka obwodu ({wy},{wx}):", " ".join("%02X%02X%02X" % f[wy][wx] for f in frames[:16]))
