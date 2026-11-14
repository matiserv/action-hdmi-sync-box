"""Czy sam ruch Wi-Fi psuje odbior klatek po SPI w BK7238 (bez udzialu kodu satelitow)?
Satelity wylaczone przez caly pomiar. Fazy: spokoj; UDP do zamknietego portu boxa (odbior + odpowiedz ICMP = nadawanie);
UDP rozgloszeniowy (sam odbior, bez odpowiedzi).

Uzycie: python wifiload.py <host> <yaml z kluczem API> [sekundy na faze = 25] [pakietow/s = 120]
"""
import re
import socket
import statistics as st
import sys
import threading
import time

from bkapi import Bk

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
host, yaml_path = sys.argv[1:3]
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 25
RATE = float(sys.argv[4]) if len(sys.argv) > 4 else 120
bk = Bk(host, yaml_path)
time.sleep(4)
SW = "Satellites (E1.31)"
before = bk.states.get(SW)
if before:
    bk.set_switch(SW, False)
RE = re.compile(r"frames: (\d+) ok, (\d+) partial, (\d+) dropped, (\d+) bad; video ([\d.]+) fps, strip ([\d.]+) fps")
stop = threading.Event()
stats = {"sent": 0, "icmp": 0}


def flood(dst, port, bcast):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if bcast:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setblocking(False)
    pkt = bytes(638)
    nxt = time.perf_counter()
    while not stop.is_set():
        try:
            s.sendto(pkt, (dst, port))
            stats["sent"] += 1
        except ConnectionResetError:
            stats["icmp"] += 1          # Windows zglasza tak odpowiedz ICMP "port nieosiagalny"
        except OSError:
            pass
        try:
            s.recvfrom(64)
        except ConnectionResetError:
            stats["icmp"] += 1
        except OSError:
            pass
        nxt += 1.0 / RATE
        d = nxt - time.perf_counter()
        if d > 0:
            time.sleep(d)
    s.close()


def phase(label, target=None):
    stop.clear()
    stats["sent"] = stats["icmp"] = 0
    th = None
    if target:
        th = threading.Thread(target=flood, args=target, daemon=True)
        th.start()
    time.sleep(2)
    t0 = time.time()
    time.sleep(SECS)
    stop.set()
    if th:
        th.join()
    rows = []
    for t, text in bk.logs:
        if t < t0:
            continue
        m = RE.search(text)
        if m:
            rows.append([int(m.group(i)) for i in (1, 2, 3, 4)] + [float(m.group(5)), float(m.group(6))])
    if len(rows) < 3:
        print(label, ": za malo linii logu", len(rows))
        return
    d = [rows[-1][i] - rows[0][i] for i in range(4)]
    n = len(rows) - 1
    print(f"{label}: {n} s | wideo sr. {st.mean(r[4] for r in rows):.1f} | tasma sr. {st.mean(r[5] for r in rows):.1f} "
          f"min {min(r[5] for r in rows):.0f} | czesciowe +{d[1]} ({d[1] / n:.2f}/s), zgubione +{d[2]}, bledne +{d[3]}"
          f" | wyslane {stats['sent']}, odpowiedzi ICMP {stats['icmp']}")


bc = host.rsplit(".", 1)[0] + ".255"
try:
    phase("spokoj (1)")
    phase("UDP do boxa, port zamkniety", (host, 9, False))
    phase("UDP rozgloszeniowy", (bc, 9, True))
    phase("spokoj (2)")
finally:
    stop.set()
    if before:
        bk.set_switch(SW, True)
    time.sleep(1)
    print("przelacznik satelitow po pomiarze:", bk.states.get(SW), "| zerwan polaczenia:", bk.drops)
    bk.close()
