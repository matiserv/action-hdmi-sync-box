"""Czeka na zdarzenie w dzienniku HDMI ESP32 (firmware v14.3) i wypisuje je z czasem zegarowym.

Co `okres` sekund czyta `hdmi log`, nowe wpisy dopisuje do pliku. Konczy prace po pierwszym zdarzeniu, ktore nastapilo
po uruchomieniu skryptu (zanik VSync, zmiana formatu lub komunikatu wejscia, inna linia logu MCU): czeka jeszcze
kilkanascie sekund na ciag dalszy, wypisuje caly zapis zdarzenia i wychodzi. Bez zdarzenia konczy po `limit` minutach.

Z piatym argumentem "ciagly" nie konczy po pierwszym zdarzeniu, tylko zapisuje wszystko do pliku az do limitu
(do liczenia, ile zanikow na godzine jest przy danym ustawieniu).

Uzycie:  python hdmiwatch.py COM4 [plik.txt] [limit_min=180] [okres_s=10] [ciagly]
"""
import re
import sys
import time

from bench import Esp

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
port = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else "hdmi_events.txt"
limit_s = float(sys.argv[3]) * 60 if len(sys.argv) > 3 else 180 * 60
period = float(sys.argv[4]) if len(sys.argv) > 4 else 10
continuous = len(sys.argv) > 5 and sys.argv[5] == "ciagly"


def read_log():
    """Lista (czas od startu w s, tekst, czas zegarowy) albo None, gdy ESP32 nie odpowiedzial."""
    try:
        esp = Esp(port)
        t_pc = time.time()
        txt = esp.cmd("hdmi log", wait=2.0)
        esp.s.close()
    except Exception as e:                      # port zajety albo adapter odlaczony: sprobuj za chwile
        return None, str(e)
    m = re.search(r"teraz (\d+)\.(\d+) s od startu", txt)
    if not m:
        return None, "brak odpowiedzi"
    up_now = int(m.group(1)) + int(m.group(2)) / 1000.0
    rows = []
    for mm in re.finditer(r"^\s+(\d+)\.(\d+) s  (.*)$", txt, re.M):
        up = int(mm.group(1)) + int(mm.group(2)) / 1000.0
        rows.append((up, mm.group(3).rstrip(), t_pc - (up_now - up)))
    return rows, up_now


seen = set()
t0 = time.time()
first_new = None
last_up = None
f = open(out_path, "a", encoding="utf-8")
f.write(f"# start obserwacji {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
baseline = True
collected = []
while True:
    rows, info = read_log()
    if rows is None:
        f.write(f"# {time.strftime('%H:%M:%S')} brak odczytu: {info}\n")
        f.flush()
    else:
        if last_up is not None and info < last_up - 5:
            line = f"{time.strftime('%H:%M:%S')}  ESP32 zrestartowal sie (czas od startu {info:.0f} s)"
            f.write(line + "\n")
            collected.append(line)
            seen.clear()
            if first_new is None:
                first_new = time.time()
        last_up = info
        for up, text, wall in rows:
            key = (round(up, 3), text)
            if key in seen:
                continue
            seen.add(key)
            line = f"{time.strftime('%H:%M:%S', time.localtime(wall))}.{int((wall % 1) * 10)}  {text}"
            f.write(line + ("   (stan poczatkowy)\n" if baseline else "\n"))
            if not baseline:
                collected.append(line)
                if first_new is None:
                    first_new = time.time()
        f.flush()
        baseline = False
    if not continuous and first_new is not None and time.time() - first_new > 25:
        print(f"zdarzenie po {(first_new - t0) / 60:.1f} min obserwacji:")
        print("\n".join(collected))
        break
    if time.time() - t0 > limit_s:
        print(f"brak zdarzen przez {limit_s / 60:.0f} min obserwacji")
        break
    time.sleep(period)
f.close()
