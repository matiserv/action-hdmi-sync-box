"""Rozdziela niestabilność na jasność i odcień: dla komórek obwodu liczy zmiany w czasie jasności (R+G+B)/3
oraz różnic między składowymi (R-G, B-G). Obraz na ekranie: dowolny (to, co akurat jest).
Użycie: python chroma.py COM4 [serie=6] ["komendy konsoli; ..."]
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
H = len(frames[0]); W = len(frames[0][0])
print(f"ramek: {len(frames)}, pole {W} x {H}")
edge = [(y, x) for y in range(H) for x in range(W) if y in (0, H - 1) or x in (0, W - 1)]
def stats(cells):
    lum_sd = []; chr_sd = []; lum = []; chr_abs = []
    for y, x in cells:
        L = [sum(f[y][x]) / 3 for f in frames]
        rg = [f[y][x][0] - f[y][x][1] for f in frames]
        bg = [f[y][x][2] - f[y][x][1] for f in frames]
        lum.append(st.mean(L)); lum_sd.append(st.pstdev(L))
        chr_sd.append(max(st.pstdev(rg), st.pstdev(bg)))
        chr_abs.append(max(abs(st.mean(rg)), abs(st.mean(bg))))
    return st.mean(lum), st.mean(lum_sd), st.mean(chr_sd), st.mean(chr_abs), max(chr_sd)
for name, cells in (("obwód (taśma)", edge), ("cała siatka", [(y, x) for y in range(H) for x in range(W)])):
    l, ls, cs, ca, cm = stats(cells)
    print(f"{name}: jasność średnio {l:.1f}; wahania jasności {ls:.2f}; wahania odcienia (R-G, B-G) {cs:.2f} (max {cm:.1f}); "
          f"stałe zabarwienie {ca:.1f}")
print("górny wiersz, 4 kolejne ramki (RRGGBB):")
for f in frames[:4]:
    print(" ".join("%02X%02X%02X" % f[0][x] for x in range(W)))
print("dolny wiersz, 4 kolejne ramki:")
for f in frames[:4]:
    print(" ".join("%02X%02X%02X" % f[H - 1][x] for x in range(W)))
print("komórka (dół,8) w czasie:", " ".join("%02X%02X%02X" % f[H - 1][W // 2] for f in frames[:24]))
print("komórka (8,0) w czasie: ", " ".join("%02X%02X%02X" % f[H // 2][0] for f in frames[:24]))
