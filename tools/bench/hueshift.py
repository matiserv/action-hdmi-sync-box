"""Test "przesunięcia koloru przy lekkiej zmianie": pełny ekran przełącza się co kilkanaście klatek między dwoma
kolorami o tym samym odcieniu (drugi nieco jaśniejszy). Idealnie odcień (proporcje R:G:B) w danych z ESP32 jest
stały także w chwili zmiany. Skrypt nagrywa wiele krótkich serii ramek i podaje największe odchylenie odcienia.
Użycie: python hueshift.py COM4 [serie=24] ["komendy konsoli; ..."]
"""
import importlib.util, os, statistics as st, sys, time
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
esp = _m.Esp(sys.argv[1])
series = int(sys.argv[2]) if len(sys.argv) > 2 else 24
for c in (sys.argv[3].split(";") if len(sys.argv) > 3 else []):
    if c.strip():
        esp.cmd(c.strip(), wait=0.35)

CASES = [("szary ciemny", (40, 40, 40), (44, 44, 44)), ("szary średni", (120, 120, 120), (128, 128, 128)),
         ("pomarańcz ciemny", (48, 24, 8), (54, 27, 9)), ("pomarańcz", (160, 80, 26), (172, 86, 28)),
         ("niebieskawy ciemny", (12, 20, 36), (14, 23, 41))]
for name, a, b in CASES:
    _m.show({"mode": "flash", "a": list(a), "b": list(b), "frames": 17}, settle=1.5)
    # odcień odniesienia: proporcje składowych względem największej składowej koloru a
    ref_i = max(range(3), key=lambda k: a[k])
    ref = [a[k] / a[ref_i] for k in range(3)]
    worst = 0.0; worst_px = None; devs = []; lum_seen = set()
    for s in range(series):
        for f in esp.frames(4):
            H = len(f); W = len(f[0])
            for y in range(1, H - 1, 3):
                for x in range(1, W - 1, 3):
                    p = f[y][x]
                    if p[ref_i] < 4:
                        continue
                    # odchylenie odcienia: różnica między składową a tym, co wynika z proporcji, w skali 0..255
                    d = max(abs(p[k] - ref[k] * p[ref_i]) for k in range(3))
                    devs.append(d)
                    if d > worst:
                        worst = d; worst_px = p
            lum_seen.add(round(st.mean(f[H // 2][x][ref_i] for x in range(4, W - 4, 4))))
    print(f"{name:20s} {a} <-> {b}: odchylenie odcienia średnio {st.mean(devs):.2f}, 99. percentyl "
          f"{sorted(devs)[int(len(devs) * 0.99)]:.1f}, największe {worst:.1f} (piksel {worst_px}); widziane jasności: "
          f"{min(lum_seen)}..{max(lum_seen)}")
_m.post({"mode": "idle"})
