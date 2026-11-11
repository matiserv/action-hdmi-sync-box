"""Aktualizacja firmware syncbox-esp32 przez konsole UART0 (bez przycisku GPIO0).
Uzycie: python esp32_upload.py COM4 .pio/build/syncbox/firmware.bin
Wymaga pyserial. Urzadzenie musi pracowac (nie w trybie programowania).
"""
import re
import sys
import time
import zlib

import serial
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

port, path = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
crc = zlib.crc32(data) & 0xFFFFFFFF

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
ser.rts = False
ser.dtr = False          # ustawione przed otwarciem: zadnego impulsu na EN/GPIO0
ser.open()


def read_until(token, timeout):
    buf = b""
    t_end = time.time() + timeout
    while time.time() < t_end:
        buf += ser.read(4096)
        if token.encode() in buf or b"ERR" in buf:
            break
    return re.sub(r"\x1b\[[0-9;]*m", "", buf.decode("utf-8", "replace"))


ser.reset_input_buffer()
ser.write(b"\r")
time.sleep(0.3)
ser.reset_input_buffer()
ser.write(f"upload {len(data)} {crc:08x}\r".encode())   # tylko CR: LF trafilby do strumienia danych
txt = read_until("READY", 5)
if "READY" not in txt:
    print("brak READY:\n" + txt)
    sys.exit(1)
print(f"wysylam {len(data)} B, crc {crc:08x} ...")
t0 = time.time()
chunk = 4096             # tyle urzadzenie czyta na raz, po czym zapisuje flash: w tym czasie nic nie wysylamy
for i in range(0, len(data), chunk):
    ser.write(data[i:i + chunk])
    ser.flush()
    time.sleep(0.12)     # zapis 4 KB do flash (przerwanie UART bywa wtedy wylaczone, FIFO ma 128 B)
    if (i // chunk) % 8 == 0:
        print(f"  {i * 100 // len(data)}%", end="\r", flush=True)
ser.flush()
print(f"  100% w {time.time() - t0:.1f} s")
txt = read_until("restart", 20)
print(txt.strip())
ser.close()
sys.exit(0 if "OK" in txt else 1)
