"""Kalibracja tasmy z plikow RAW (DNG): srednie liniowe kolory ekranu, sufitu i sciany dla kazdej planszy.

Dwa przeliczenia tych samych danych: (a) surowe kanaly czujnika, (b) liniowe sRGB wg macierzy kolorow z pliku
(przyblizenie tego, co widzi oko). Wynik: JSON ze srednimi i podglady z zaznaczonymi polami pomiaru.

Uzycie: python rawcal.py <katalog z DNG> <katalog wynikow> <nazwy plansz po przecinku, w kolejnosci plikow> [pola pomiaru jako JSON]
"""
import json
import os
import sys

import numpy as np
import rawpy
from PIL import Image, ImageDraw

src, out, names = sys.argv[1], sys.argv[2], [x.strip() for x in sys.argv[3].split(",")]
REG = {"ekran": (0.10, 0.55, 0.45, 0.80), "sufit": (0.03, 0.04, 0.25, 0.17), "sciana": (0.66, 0.45, 0.80, 0.75)}
if len(sys.argv) > 4:
    REG = json.loads(sys.argv[4])
files = sorted(f for f in os.listdir(src) if f.lower().endswith(".dng"))
assert len(files) == len(names), (files, names)
res = {}
for f, name in zip(files, names):
    with rawpy.imread(os.path.join(src, f)) as raw:
        common = dict(half_size=True, no_auto_bright=True, gamma=(1, 1), output_bps=16, highlight_mode=rawpy.HighlightMode.Clip)
        cam = raw.postprocess(output_color=rawpy.ColorSpace.raw, use_camera_wb=False, use_auto_wb=False, user_wb=[1, 1, 1, 1], **common)
        srgb = raw.postprocess(output_color=rawpy.ColorSpace.sRGB, use_camera_wb=True, **common)
        wb = [round(float(x), 3) for x in raw.camera_whitebalance[:3]]
    h, w = cam.shape[:2]
    res[name] = {"plik": f, "rozmiar": [w, h], "wb_aparatu": wb}
    for k, b in REG.items():
        x0, y0, x1, y1 = int(b[0] * w), int(b[1] * h), int(b[2] * w), int(b[3] * h)
        res[name][k] = {"czujnik": [float(v) / 65535.0 for v in cam[y0:y1, x0:x1].reshape(-1, 3).mean(axis=0)],
                        "srgb": [float(v) / 65535.0 for v in srgb[y0:y1, x0:x1].reshape(-1, 3).mean(axis=0)],
                        "max_czujnik": float(cam[y0:y1, x0:x1].max()) / 65535.0}
    view = (np.clip(srgb.astype(np.float32) / 65535.0 * 6.0, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
    im = Image.fromarray(view)
    d = ImageDraw.Draw(im)
    for k, b in REG.items():
        d.rectangle((b[0] * w, b[1] * h, b[2] * w, b[3] * h), outline=(255, 255, 0), width=6)
    im.thumbnail((640, 640))
    im.save(os.path.join(out, "podglad_" + name.replace(" ", "_") + ".jpg"), quality=85)
json.dump(res, open(os.path.join(out, "raw_means.json"), "w", encoding="utf-8"), indent=1)

sh = lambda v: " ".join(f"{100 * x / (sum(v) or 1):5.1f}" for x in v)
for space in ("czujnik", "srgb"):
    print(f"\n[{space}] udzialy R G B (liniowo) i jasnosc poswiaty wzgledem ekranu (kanal G):")
    for name in names:
        r = res[name]
        print(f"  {name:10s} ekran {sh(r['ekran'][space])} | sufit {sh(r['sufit'][space])} | sciana {sh(r['sciana'][space])} | "
              f"G sufit/ekran {r['sufit'][space][1] / r['ekran'][space][1]:.3f}, sciana/ekran {r['sciana'][space][1] / r['ekran'][space][1]:.3f}"
              + (f" | max czujnika ekran {r['ekran']['max_czujnik']:.2f}" if space == "czujnik" else ""))
print("\nrozmiar po odczycie:", res[names[0]]["rozmiar"], "| balans bieli aparatu:", res[names[0]]["wb_aparatu"])
