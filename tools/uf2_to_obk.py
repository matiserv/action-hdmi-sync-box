"""Zamienia UF2 z ESPHome (LibreTiny, BK7238, uklad generic-bk7238) na jeden surowy obraz
do wgrania flasherem OpenBeken (BK7231GUIFlashTool) pod adres 0x11000.

Uzycie:  python uf2_to_obk.py firmware.uf2 [wyjscie.bin]
Wymaga:  pip install ltchiptool
"""
import os, re, subprocess, sys, tempfile

APP_START = 0x11000
APP_END = 0x132000  # koniec partycji app w ukladzie generic-bk7238 (download zaczyna sie tutaj)

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + "_obk_0x11000.bin"
    with tempfile.TemporaryDirectory() as tmp:
        r = subprocess.run([sys.executable, "-m", "ltchiptool", "uf2", "dump", os.path.abspath(src)],
                           cwd=tmp, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout, r.stderr); sys.exit(2)
        img = bytearray(b"\xff" * (APP_END - APP_START))
        parts = []
        for fn in sorted(os.listdir(tmp)):
            m = re.search(r"_flasher_0x([0-9A-Fa-f]+)\.bin$", fn)
            if not m:
                continue
            off = int(m.group(1), 16)
            data = open(os.path.join(tmp, fn), "rb").read()
            if off < APP_START or off + len(data) > APP_END:
                print(f"POMIJAM {fn}: poza partycja app ({off:#x}+{len(data)})"); continue
            img[off - APP_START: off - APP_START + len(data)] = data
            parts.append((off, len(data)))
    if not parts:
        print("Brak czesci 'flasher' w UF2 - to nie jest UF2 LibreTiny dla UART?"); sys.exit(3)
    open(dst, "wb").write(img)
    print(f"Zapisano {dst}: {len(img)} B = {len(img)//4096} sektorow po 4 KB, wgraj pod 0x11000")
    for off, ln in parts:
        print(f"  czesc @ {off:#08x}, {ln} B")

if __name__ == "__main__":
    main()
