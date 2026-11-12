"""Dziennik zdarzen czesci HDMI z ESP32 (firmware v14.3, komenda `hdmi log`) z czasem zegarowym.

ESP32 zapisuje w pamieci ostatnie 64 zdarzenia: zmiany komunikatow MCU ukladu HDMI (format wejscia i wyjscia, tryb
HDR, brak sygnalu, inne linie logu) oraz zaniki i powroty VSync. Czas w dzienniku liczy sie od startu ESP32; skrypt
przelicza go na godzine wedlug zegara PC. Dziennik ginie po wylaczeniu zasilania boxa.

Uzycie:  python hdmilog.py COM4 [clear]
"""
import re
import sys
import time

from bench import Esp

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
esp = Esp(sys.argv[1])
clear = len(sys.argv) > 2 and sys.argv[2] == "clear"
t_pc = time.time()
txt = esp.cmd("hdmi log clear" if clear else "hdmi log", wait=2.5)
m = re.search(r"teraz (\d+)\.(\d+) s od startu", txt)
if not m:
    sys.exit("brak odpowiedzi na `hdmi log` (firmware starszy niz v14.3?)\n" + txt[-400:])
up_now = int(m.group(1)) + int(m.group(2)) / 1000.0
print(txt[txt.find("dziennik HDMI"):].splitlines()[0])
prev = None
for mm in re.finditer(r"^\s+(\d+)\.(\d+) s  (.*)$", txt, re.M):
    up = int(mm.group(1)) + int(mm.group(2)) / 1000.0
    wall = time.strftime("%H:%M:%S", time.localtime(t_pc - (up_now - up)))
    gap = "" if prev is None else f"  (+{up - prev:.1f} s)"
    print(f"{wall}  {mm.group(3).rstrip()}{gap}")
    prev = up
