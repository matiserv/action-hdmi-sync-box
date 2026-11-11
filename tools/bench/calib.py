"""Kalibracja poziomów czerni i bieli: wyświetla czarny i biały ekran, czyta średnie kody ADC ze `stat`
i ustawia black_mv / white_mv (bez zapisu; `save` w konsoli zapisuje).
Użycie: python calib.py COM4
"""
import importlib.util, os, re, sys, time
_sp = importlib.util.spec_from_file_location("syncbench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"))
_m = importlib.util.module_from_spec(_sp); _sp.loader.exec_module(_m)
esp = _m.Esp(sys.argv[1])

def codes():
    txt = esp.cmd("stat", wait=1.0)
    m = re.search(r"ADC kody R/G/B: srednia (\d+)/(\d+)/(\d+)", txt)
    return [int(x) for x in m.groups()] if m else None

_m.show({"mode": "solid", "color": [0, 0, 0]}, settle=1.5)
b = codes()
_m.show({"mode": "solid", "color": [255, 255, 255]}, settle=1.5)
w = codes()
_m.post({"mode": "idle"})
print("kody czerni R/G/B:", b, " kody bieli:", w)
if b and w:
    bc = sum(b) / 3; wc = sum(w) / 3
    bmv = round(bc * 1500 / 4095); wmv = round(wc * 1500 / 4095)
    print(f"black_mv {bmv}, white_mv {wmv} (kody {bc:.0f} i {wc:.0f}; zakres {wc - bc:.0f} kodów na 255 poziomów)")
    esp.cmd(f"set black_mv {bmv}", wait=0.4); esp.cmd(f"set white_mv {wmv}", wait=0.4)
