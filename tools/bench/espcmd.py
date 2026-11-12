"""Wysyla komendy do konsoli ESP32 i wypisuje odpowiedzi - po UART albo przez Wi-Fi.

Uzycie:  python espcmd.py <COM4 | tcp://adres:2323> "<komenda> ; <komenda> ..." [czas_s=2] [maks_znakow=3000]

Przez Wi-Fi haslo konsoli podaje zmienna srodowiskowa SYNCBOX_CONSOLE_PASSWORD.
"""
import re
import sys
import time

from esplink import open_port

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
port = open_port(sys.argv[1], 0.05)
wait = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
limit = int(sys.argv[4]) if len(sys.argv) > 4 else 3000
for c in sys.argv[2].split(";"):
    c = c.strip()
    if not c:
        continue
    port.reset_input_buffer()
    t0 = time.time()
    port.write((c + "\r").encode())
    buf = b""
    while time.time() - t0 < wait:
        buf += port.read(4096)
    txt = re.sub(r"\x1b\[[0-9;]*m", "", buf.decode("utf-8", "replace"))
    print(f"--- {c}: {len(buf)} B")
    print(txt.strip()[:limit])
port.close()
