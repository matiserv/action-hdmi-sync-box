"""Generuje main/hdr_tables.h: stałe tablice konwersji HDR10 -> sRGB (leżą we flashu, nie zajmują RAM).

  HDR_PQ_NITS[256]  odczyt 0..255 (wartość sygnału PQ, ST 2084) -> luminancja w nitach
  HDR_ENC[4096]     światło liniowe 0..1 (indeks = wartość x 4095) -> kod sRGB 0..255

Uruchom po zmianie:  python gen_hdr_tables.py
"""
import os

M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq_to_nits(v):
    p = v ** (1 / M2)
    return 10000.0 * (max(p - C1, 0.0) / (C2 - C3 * p)) ** (1 / M1)


def srgb_encode(x):
    return 12.92 * x if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055


def table(name, ctype, vals, per_line, f):
    out = [f"static const {ctype} {name}[{len(vals)}] = {{"]
    for i in range(0, len(vals), per_line):
        out.append("    " + ", ".join(f(v) for v in vals[i:i + per_line]) + ",")
    out.append("};")
    return "\n".join(out)


nits = [pq_to_nits(i / 255.0) for i in range(256)]
enc = [min(255, int(round(srgb_encode(i / 4095.0) * 255))) for i in range(4096)]
text = "\n".join([
    "// Plik wygenerowany przez gen_hdr_tables.py - nie edytowac recznie.",
    "#pragma once",
    "#include <stdint.h>",
    "",
    "// odczyt 0..255 = wartosc sygnalu PQ (SMPTE ST 2084) -> luminancja w nitach",
    table("HDR_PQ_NITS", "float", nits, 6, lambda v: (lambda t: t if ("." in t or "e" in t) else t + ".0")(f"{v:.7g}") + "f"),
    "",
    "// swiatlo liniowe 0..1 (indeks = wartosc x 4095) -> kod sRGB 0..255",
    table("HDR_ENC", "uint8_t", enc, 24, str),
    "",
])
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main", "hdr_tables.h")
with open(path, "w", encoding="ascii", newline="\n") as fh:
    fh.write(text)
print(f"{path}: PQ 255 -> {nits[255]:.0f} nitow, PQ 148 -> {nits[148]:.1f} nitow; ENC[4095] = {enc[4095]}, ENC[1] = {enc[1]}, ENC[20] = {enc[20]}")
