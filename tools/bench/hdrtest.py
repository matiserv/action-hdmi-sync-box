"""Tester toru HDR sync boxa: komputer nadaje przez box pola HDR10 o znanych wartościach, ESP32 mówi, co zmierzył.

Wymaga: Windows z HDR włączonym dla wyświetlacza podłączonego przez box, konsoli ESP32 (COMx albo tcp://adres:2323,
hasło w zmiennej SYNCBOX_CONSOLE_PASSWORD). Okno testowe otwiera się samo na wskazanym wyświetlaczu (hdrwin.py).

    python hdrtest.py info                                   lista wyświetlaczy
    python hdrtest.py tcp://192.168.1.50:2323 3 [testy...]   testy na wyświetlaczu \\\\.\\DISPLAY3

Testy (domyślnie: raw prim colors bright):
  raw     szara skala w kodach PQ 0..1023, odczyt bez obróbki barw (ESP32 chwilowo w trybie SDR, bez wygładzania):
          czy tor jest liniowy względem kodu, gdzie się nasyca i czy szarość jest neutralna (balans bieli)
  prim    same R, same G, same B w kodach PQ, odczyt bez obróbki: przesłuch między kanałami
  colors  kolory zadane jak w sRGB, wyświetlone w HDR z bielą o jasności --white nitów; odczyt Z obróbką boxa
          (tak, jak idzie na taśmę). Ideał: box oddaje ten sam kolor sRGB. Pokazuje błąd barwy w stopniach.
  bright  szarość 25..4000 nitów z obróbką boxa: przy jakiej jasności obrazu taśma dochodzi do pełnej mocy

Opcje: --white <nity> (domyślnie 203), --csv <plik>, --force (uruchom mimo wyłączonego HDR; wyniki bez znaczenia),
       --sdr (pola w zwykłym sRGB zamiast HDR10 — porównanie toru SDR tym samym narzędziem).
Parametry ESP32 zmienione na czas testu są na końcu przywracane (nic nie jest zapisywane do pamięci boxa).
"""
import colorsys
import re
import statistics as st
import sys
import time

from esplink import open_port
from hdrwin import HdrWindow, describe, displays, find_display, nits_from_pq, pq_code, srgb_to_hdr10

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

RAW = {"hdr_mode": 1, "smooth": 100, "hold": 0, "cut": 0, "blur": 0, "lbox": 0, "gamma": 0, "gain": 100, "sat": 100,
       "m_rr": 1000, "m_rg": 0, "m_rb": 0, "m_gr": 0, "m_gg": 1000, "m_gb": 0, "m_br": 0, "m_bg": 0, "m_bb": 1000}
ASIS = {"smooth": 100, "hold": 0, "blur": 0, "lbox": 0}

COLORS = [("biały", (255, 255, 255)), ("szary 192", (192, 192, 192)), ("szary 128", (128, 128, 128)), ("szary 64", (64, 64, 64)),
          ("czerwony", (255, 0, 0)), ("zielony", (0, 255, 0)), ("niebieski", (0, 0, 255)), ("żółty", (255, 255, 0)),
          ("turkus", (0, 255, 255)), ("purpura", (255, 0, 255)), ("pomarańcz", (255, 128, 0)), ("skóra", (224, 172, 105)),
          ("ciepła biel", (255, 214, 170)), ("niebo", (40, 90, 200)), ("trawa", (60, 140, 40)), ("ciemna czerwień", (128, 0, 0))]


class Esp:
    def __init__(self, port):
        self.s = open_port(port)
        self.saved = {}

    def cmd(self, c, wait=0.6):
        self.s.reset_input_buffer()
        self.s.write((c + "\r").encode())
        buf = b""
        t_end = time.time() + wait
        while time.time() < t_end:
            buf += self.s.read(4096)
        return re.sub(r"\x1b\[[0-9;]*m", "", buf.decode("utf-8", "replace"))

    def params(self):
        for _ in range(3):
            p = dict(re.findall(r"^\s*(\w+)\s*=\s*(-?\d+)\s*$", self.cmd("params", wait=1.6), re.M))
            if "hdr_mode" in p and "m_bb" in p:
                return {k: int(v) for k, v in p.items()}
        raise SystemExit("ESP32 nie odpowiada na 'params'")

    def apply(self, want):
        """Ustaw parametry na czas testu; pierwotne wartości zapamiętane do restore()."""
        cur = self.params()
        for k, v in want.items():
            if k in cur and cur[k] != v:
                self.saved.setdefault(k, cur[k])
                self.cmd(f"set {k} {v}", wait=0.3)

    def restore(self):
        for k, v in self.saved.items():
            self.cmd(f"set {k} {v}", wait=0.3)
        now = self.params() if self.saved else {}
        left = {k: v for k, v in self.saved.items() if now.get(k) != v}
        self.saved = {}
        return left

    def hdmi(self):
        t = self.cmd("hdmi", wait=0.8)
        mode = re.search(r"tryb:\s*(\S+)", t)
        act = re.findall(r"Active:(\S+)", t)
        return (mode.group(1) if mode else "?"), (act[-1] if act else "?")

    def trace(self, n=6, timeout=3.0):
        """Średni kolor środka pola w n kolejnych ramkach -> lista (r, g, b)."""
        for _ in range(3):
            self.s.reset_input_buffer()
            self.s.write(f"trace {n}\r".encode())
            buf = b""
            t_end = time.time() + timeout
            while time.time() < t_end:
                buf += self.s.read(4096)
                m = re.search(rb"przebieg \d+:\s*((?:[0-9A-F]{6}\s+){%d})" % n, buf)
                if m:
                    return [(int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)) for h in m.group(1).decode().split()]
        return None


def measure(win, esp, code, settle=0.4, cells=None):
    """Pokaż pole o kodach `code` i zwróć średni odczyt (r, g, b) oraz rozrzut między ramkami."""
    win.solid(code)
    win.hold(settle)
    tr = esp.trace(6)
    win.pump()
    if not tr:
        return None, None
    tr = tr[2:]
    mean = tuple(st.mean(p[k] for p in tr) for k in range(3))
    spread = max(max(p[k] for p in tr) - min(p[k] for p in tr) for k in range(3))
    return mean, spread


def fmt(v):
    return "   --    --    --" if v is None else f"{v[0]:5.1f} {v[1]:5.1f} {v[2]:5.1f}"


def hue_sat_val(rgb):
    h, s, v = colorsys.rgb_to_hsv(*(max(0.0, min(255.0, c)) / 255.0 for c in rgb))
    return h * 360.0, s, v


def test_raw(win, esp, rows):
    print("\n== raw: szara skala w kodach PQ, odczyt bez obróbki barw ==")
    print(" kod   PQ     nity    odczyt R     G     B   oczekiwane   G-R   B-R  rozrzut")
    pts = []
    for code in list(range(0, 1024, 32)) + [1023]:
        m, sp = measure(win, esp, (code, code, code))
        exp = code / 1023 * 255
        if m is None:
            print(f"{code:4d}  brak odczytu"); continue
        pts.append((code, m))
        print(f"{code:4d}  {code / 1023:5.3f} {nits_from_pq(code / 1023):7.1f}   {fmt(m)}     {exp:5.1f}   {m[1] - m[0]:+5.1f} {m[2] - m[0]:+5.1f}   {sp:3d}")
        rows.append(("raw", code, code, code, *m))
    if len(pts) < 6:
        return
    # prosta odczyt = a * kod + b na punktach bez nasycenia (odczyt 8..247)
    for k, name in enumerate("RGB"):
        xy = [(c, m[k]) for c, m in pts if 8 < m[k] < 247]
        if len(xy) < 4:
            print(f"{name}: za mało punktów poza nasyceniem"); continue
        mx, my = st.mean(x for x, _ in xy), st.mean(y for _, y in xy)
        a = sum((x - mx) * (y - my) for x, y in xy) / sum((x - mx) ** 2 for x, _ in xy)
        b = my - a * mx
        res = max(abs(y - (a * x + b)) for x, y in xy)
        zero = -b / a if a else 0
        full = (255 - b) / a if a else 0
        print(f"{name}: odczyt = {a * 1023 / 255:.3f} x (kod/1023 x 255) {b:+.1f}; odczyt 0 przy kodzie {zero:.0f}, 255 przy kodzie {full:.0f} "
              f"({nits_from_pq(min(1.0, max(0.0, full / 1023))):.0f} nitów); największe odstępstwo od prostej {res:.1f}")
    neutral = [max(m) - min(m) for c, m in pts if 8 < max(m) and min(m) < 247]
    if neutral:
        print(f"neutralność szarości (max - min kanałów): średnio {st.mean(neutral):.1f}, najwięcej {max(neutral):.1f} / 255")


def test_prim(win, esp, rows):
    print("\n== prim: pojedyncze kanały w kodach PQ, odczyt bez obróbki (przesłuch) ==")
    print("nadane R    G    B     odczyt R     G     B")
    for lvl in (384, 640, 896):
        for k in range(3):
            code = tuple(lvl if i == k else 0 for i in range(3))
            m, _ = measure(win, esp, code)
            print(f"     {code[0]:4d} {code[1]:4d} {code[2]:4d}     {fmt(m)}")
            if m:
                rows.append(("prim", *code, *m))


def test_colors(win, esp, rows, conv, white):
    print(f"\n== colors: kolory sRGB wyświetlone w HDR (biel = {white:.0f} nitów), odczyt z obróbką boxa ==")
    print("kolor            zadany sRGB      kody HDR10        odczyt R     G     B    barwa zad./odcz.  błąd barwy  nasycenie zad./odcz.")
    hue_err, gray_err = [], []
    for name, c in COLORS:
        code = conv(c)
        m, _ = measure(win, esp, code)
        if m is None:
            print(f"{name:15s} brak odczytu"); continue
        rows.append(("colors", *code, *m))
        ht, s_t, _ = hue_sat_val(c)
        hm, s_m, _ = hue_sat_val(m)
        if s_t < 0.05:                    # szarość: liczy się neutralność
            gray_err.append(max(m) - min(m))
            print(f"{name:15s} {c[0]:3d} {c[1]:3d} {c[2]:3d}   {code[0]:4d} {code[1]:4d} {code[2]:4d}    {fmt(m)}        (szary)   kanały rozjechane o {max(m) - min(m):4.1f}")
        else:
            d = (hm - ht + 180) % 360 - 180
            hue_err.append(abs(d))
            print(f"{name:15s} {c[0]:3d} {c[1]:3d} {c[2]:3d}   {code[0]:4d} {code[1]:4d} {code[2]:4d}    {fmt(m)}     {ht:5.0f}° / {hm:5.0f}°     {d:+6.1f}°       {s_t:4.2f} / {s_m:4.2f}")
    if hue_err:
        print(f"błąd barwy: średnio {st.mean(hue_err):.1f}°, największy {max(hue_err):.1f}°;  "
              f"szarości: rozjazd kanałów średnio {st.mean(gray_err) if gray_err else 0:.1f}, największy {max(gray_err) if gray_err else 0:.1f} / 255")


def test_bright(win, esp, rows):
    print("\n== bright: szarość o rosnącej jasności, odczyt z obróbką boxa ==")
    print("   nity   kod    odczyt R     G     B")
    for nits in (25, 50, 100, 203, 300, 400, 600, 1000, 2000, 4000):
        c = pq_code(nits)
        m, _ = measure(win, esp, (c, c, c))
        print(f"{nits:7d}  {c:4d}    {fmt(m)}")
        if m:
            rows.append(("bright", c, c, c, *m))


def main():
    args = sys.argv[1:]
    if not args or args[0] == "info":
        for d in displays():
            print(describe(d))
        return 0
    opts = {"--white": 203.0, "--csv": ""}
    flags = set()
    rest = []
    i = 0
    while i < len(args):
        if args[i] in opts:
            opts[args[i]] = type(opts[args[i]])(args[i + 1]); i += 2
        elif args[i].startswith("--"):
            flags.add(args[i]); i += 1
        else:
            rest.append(args[i]); i += 1
    if len(rest) < 2:
        print(__doc__); return 2
    port, which, tests = rest[0], rest[1], rest[2:] or ["raw", "prim", "colors", "bright"]
    disp = find_display(which)
    print("Wyświetlacz:", describe(disp))
    sdr = "--sdr" in flags
    if not sdr and not disp["hdr_enabled"]:
        print("HDR jest dla tego wyświetlacza WYŁĄCZONY w Windows — system przeliczy pola na SDR i pomiar nie będzie nic znaczył.")
        if "--force" not in flags:
            print("Włącz HDR (Ustawienia > System > Ekran > wybierz ten ekran > Użyj HDR) albo dodaj --force.")
            return 2
    esp = Esp(port)
    mode, active = esp.hdmi()
    print(f"Box widzi na wejściu: {active}, tryb {mode}")
    if not sdr and mode in ("SDR", "?"):
        print("UWAGA: box nie zgłasza sygnału HDR na wejściu — to nie jest pomiar toru HDR.")
    white = opts["--white"]
    # w trybie --sdr kody 10-bitowe są zwykłymi wartościami sRGB (x 1023/255), bez krzywej PQ
    conv = (lambda c: tuple(round(v * 1023 / 255) for v in c)) if sdr else (lambda c: srgb_to_hdr10(c, white))
    rows = []
    win = HdrWindow(disp, hdr=not sdr)
    try:
        win.solid((0, 0, 0)); win.hold(1.0)
        for t in tests:
            if t in ("raw", "prim"):
                esp.restore(); esp.apply(RAW)
                (test_raw if t == "raw" else test_prim)(win, esp, rows)
            elif t in ("colors", "bright"):
                esp.restore(); esp.apply(ASIS)
                if t == "colors":
                    test_colors(win, esp, rows, conv, white)
                else:
                    test_bright(win, esp, rows)
            else:
                print("nieznany test:", t)
    finally:
        win.close()
        left = esp.restore()
        print("\nParametry ESP32 przywrócone." if not left else f"\nUWAGA: nie udało się przywrócić: {left}")
    if opts["--csv"]:
        with open(opts["--csv"], "w", encoding="utf-8") as f:
            f.write("test,kod_R,kod_G,kod_B,odczyt_R,odczyt_G,odczyt_B\n")
            for r in rows:
                f.write("%s,%d,%d,%d,%.1f,%.1f,%.1f\n" % r)
        print("zapisano", opts["--csv"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
