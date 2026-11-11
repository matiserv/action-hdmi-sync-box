"""Wzor "drobne detale przy krawedziach" na stronie testowej: ciemne tlo, a w pasie przy krawedziach ekranu rzadkie,
jasne, kolorowe punkty 10x10 px (jak ikony, tekst i zegar w oknie przegladarki z paskiem zadan). Srodek czarny.
Nieruchomy obraz, na ktorym komorki pola przy krawedziach dostaja co ramke inne probki - do pomiaru stabilnosci
tasmy (bench/bkstab.py).

Uzycie:  python edgedetail.py [on|off]
"""
import random
import sys

from bench import post

NX, NY, BAND = 192, 108, 14          # siatka 10x10 px, pas detali 140 px od krawedzi
if len(sys.argv) > 1 and sys.argv[1] == "off":
    post({"mode": "idle"})
    sys.exit()
rnd = random.Random(20261111)
PAL = [(255, 255, 255), (255, 60, 40), (60, 140, 255), (255, 200, 40), (80, 255, 120), (200, 80, 255)]
cells = []
for y in range(NY):
    for x in range(NX):
        edge = x < BAND or x >= NX - BAND or y < BAND or y >= NY - BAND
        if not edge:
            cells.append([0, 0, 0])
        elif rnd.random() < 0.18:
            cells.append(list(rnd.choice(PAL)))
        else:
            cells.append([36, 36, 40])
post({"mode": "cells", "nx": NX, "ny": NY, "cells": cells})
print("wzor z detalami przy krawedziach wyswietlony")
