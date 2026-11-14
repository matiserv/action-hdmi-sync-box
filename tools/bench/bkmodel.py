"""Model toru BK7238 (komponent syncbox v15) w Pythonie: pole 48x27 z ESP32 -> kolory 28 sekcji tasmy.

Prostokaty sekcji, tablica poziomy / balans / gamma, nasycenie, doswietlenie ze srodka. Ustawienia (CFG) czyta
sie z urzadzenia funkcja read_cfg(stany encji z bkapi.Bk).
"""
import math

# ustawienia toru BK7238 (jak w hdmi-syncbox.yaml; odczytywane z urzadzenia nizej)
CFG = {"gamma": 1.8, "wb": [1.0, 1.0, 0.85], "black": 0.0, "white": 255.0, "sat": 1.5, "depth": 2, "bright": 1.0,
       "dim": 0.0,
       "off": {"L": 0, "G": 0, "P": 0}}
SIDES = {"L": 7, "G": 14, "P": 7}
KNEE = 16.0
MIN_CELLS = 4
W, H = 48, 27



def read_cfg(states):
    """Stany suwakow (od v15 sa tozsame z tym, na czym pracuje komponent)."""
    names = {"Wideo: gamma": "gamma", "Wideo: poziom czerni": "black", "Wideo: poziom bieli": "white",
             "Wideo: nasycenie": "sat", "Wideo: glebokosc probkowania": "depth", "Wideo: jasnosc": "bright",
             "Wideo: przygaszanie przy ciemnym srodku": "dim"}
    wb = {"Wideo: balans R": 0, "Wideo: balans G": 1, "Wideo: balans B": 2}
    offs = {"Kalibracja: odstep lewy bok": "L", "Kalibracja: odstep gorny bok": "G", "Kalibracja: odstep prawy bok": "P"}
    for n, v in states.items():
        if v is None:
            continue
        if n in names:
            CFG[names[n]] = float(v) / 100.0 if names[n] in ("bright", "dim") else float(v)
        elif n in wb:
            CFG["wb"][wb[n]] = float(v)
        elif n in offs:
            CFG["off"][offs[n]] = int(v)
    CFG["depth"] = int(CFG["depth"])



# ---------------------------------------------------------------- model toru BK7238
def lutf(c, code):
    span = CFG["white"] - CFG["black"]
    v = min(max((code - CFG["black"]) / (span if span > 0 else 1.0), 0.0), 1.0) * CFG["wb"][c]
    return min(max(v, 0.0), 1.0) ** CFG["gamma"] * 255.0


def lut_interp(c, mean):
    i = int(mean)
    if i >= 255:
        return lutf(c, 255)
    return lutf(c, i) + (lutf(c, i + 1) - lutf(c, i)) * (mean - i)


def span(base, total, count, i, reverse):
    step = total / count
    k = (count - 1 - i) if reverse else i
    a = base + math.floor(k * step + 1e-4)
    b = base + math.ceil((k + 1) * step - 1e-4) - 1
    return a, max(a, b)


def model(field, crop, mix, as_float=False, dim=None):
    """Kolory sekcji wg toru BK7238 w stanie ustalonym. field[y][x] = (r,g,b); crop = (x0,x1,y0,y1); mix 0..1.
    as_float: wartosci ulamkowe (przed zaokragleniem do poziomow tasmy). dim: przygaszanie przy ciemnym srodku
    0..1 (None = z ustawien urzadzenia, CFG)."""
    if dim is None:
        dim = CFG["dim"]
    cx0, cx1, cy0, cy1 = crop
    cw, ch = cx1 - cx0 + 1, cy1 - cy0 + 1
    d = CFG["depth"]
    out = {}
    for side, n in SIDES.items():
        out[side] = []
        for idx in range(n):
            off = CFG["off"][side]
            clx = lambda v: min(max(v, 0), W - 1)
            cly = lambda v: min(max(v, 0), H - 1)
            if side == "L":
                ya, yb = span(cy0, ch, n, idx, True)
                xa = clx(cx0 + off)
                xb = max(xa, clx(cx0 + off + d - 1))
                inner = (xb + 1, cx0 + cw // 2 - 1, ya, yb)
            elif side == "G":
                xa, xb = span(cx0, cw, n, idx, False)
                ya = cly(cy0 + off)
                yb = max(ya, cly(cy0 + off + d - 1))
                inner = (xa, xb, yb + 1, cy0 + ch // 2 - 1)
            else:
                ya, yb = span(cy0, ch, n, idx, False)
                xb = clx(cx1 - off)
                xa = min(xb, clx(cx1 - off - d + 1))
                inner = (cx1 - cw // 2 + 1, xa - 1, ya, yb)
            cells = [field[y][x] for y in range(ya, yb + 1) for x in range(xa, xb + 1)]
            f = [lut_interp(c, sum(p[c] for p in cells) / len(cells)) for c in range(3)]
            ia, ib, ja, jb = inner
            deep = (ib - ia + 1) if side != "G" else (jb - ja + 1)
            sat = lambda v: [min(max(sum(v) / 3.0 + (t - sum(v) / 3.0) * CFG["sat"], 0.0), 255.0) for t in v]
            f = sat(f)
            if (mix > 0 or dim > 0) and deep >= MIN_CELLS:   # doswietlenie / przygaszanie: po nasyceniu krawedzi i srodka
                ic = [field[y][x] for y in range(ja, jb + 1) for x in range(ia, ib + 1)]
                m = sat([sum(round(lutf(c, p[c])) for p in ic) / len(ic) for c in range(3)])
                vm, ve = max(m), max(f)
                lift = mix * (vm - KNEE) - ve
                if lift > 0:
                    f = [f[c] + lift / vm * m[c] for c in range(3)]
                if dim > 0 and vm < ve:
                    f = [v * (1.0 - dim * (ve - vm) / ve) for v in f]
            out[side].append(tuple(v * CFG["bright"] for v in f) if as_float
                             else tuple(int(v * CFG["bright"] + 0.5) for v in f))
    return out
