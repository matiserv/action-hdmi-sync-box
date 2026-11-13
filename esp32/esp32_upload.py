"""Aktualizacja firmware syncbox-esp32 przez konsole: po UART0 albo przez Wi-Fi (bez przycisku GPIO0).

Uzycie: python esp32_upload.py COM4 .pio/build/syncbox/firmware.bin
        python esp32_upload.py tcp://192.168.1.50:2323 .pio/build/syncbox/firmware.bin

Urzadzenie musi pracowac (nie w trybie programowania). Po UART wymaga pyserial. Przez Wi-Fi konsola idzie przez modul
Wi-Fi boxa (komponent syncbox, opcja `esp32_console`, firmware ESP32 v14.4+); haslo konsoli podaje zmienna
srodowiskowa SYNCBOX_CONSOLE_PASSWORD. Firmware wgrany przez Wi-Fi startuje "na probe": jesli po restarcie lacze
z modulem Wi-Fi nie zadziala w ciagu 120 s, ESP32 sam wraca do poprzedniej wersji. Dlatego po wgraniu skrypt zostaje
polaczony i czeka na potwierdzenie.
"""
import os
import re
import socket
import sys
import time
import zlib

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
port, path = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
crc = zlib.crc32(data) & 0xFFFFFFFF
tcp = port.startswith("tcp://")


class Tcp:
    def __init__(self, url):
        host, _, p = url[len("tcp://"):].partition(":")
        self.addr = (host, int(p or 2323))
        self.connect()

    def connect(self):
        self.s = socket.create_connection(self.addr, timeout=5)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        pw = os.environ.get("SYNCBOX_CONSOLE_PASSWORD", "")
        if pw:
            self.s.sendall(pw.encode() + b"\n")
        time.sleep(0.3)

    def read(self, n):
        self.s.settimeout(0.2)
        try:
            d = self.s.recv(n)
        except (socket.timeout, BlockingIOError):
            return b""
        if d == b"":
            raise ConnectionError("box zamknal polaczenie konsoli")
        return d

    def write(self, d):
        self.s.settimeout(60)
        self.s.sendall(d)

    def flush(self):
        pass

    def reset_input_buffer(self):
        self.s.settimeout(0.01)
        try:
            while self.s.recv(65536):
                pass
        except (socket.timeout, BlockingIOError):
            pass

    def close(self):
        self.s.close()


if tcp:
    ser = Tcp(port)
else:
    import serial
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
txt = read_until("READY", 8)
if "READY" not in txt:
    print("brak READY:\n" + txt[-600:])
    sys.exit(1)
print(f"wysylam {len(data)} B, crc {crc:08x} ...")
t0 = time.time()
if tcp:
    # tunel ma wlasne potwierdzenia i sam dawkuje dane; wystarczy pisac do gniazda
    step = 8192
    for i in range(0, len(data), step):
        ser.write(data[i:i + step])
        print(f"  {i * 100 // len(data)}%", end="\r", flush=True)
else:
    chunk = 4096             # tyle urzadzenie czyta na raz, po czym zapisuje flash: w tym czasie nic nie wysylamy
    for i in range(0, len(data), chunk):
        ser.write(data[i:i + chunk])
        ser.flush()
        time.sleep(0.12)     # zapis 4 KB do flash (przerwanie UART bywa wtedy wylaczone, FIFO ma 128 B)
        if (i // chunk) % 8 == 0:
            print(f"  {i * 100 // len(data)}%", end="\r", flush=True)
ser.flush()
print(f"  100% w {time.time() - t0:.1f} s")
txt = read_until("restart", 60 if tcp else 20)
print(txt.strip()[-300:])
ok = "OK" in txt
if ok and tcp and not os.environ.get("SYNCBOX_UPLOAD_NO_CONFIRM"):   # zmienna tylko do testu samoczynnego powrotu
    # Nowy firmware jest na probie: potwierdza sie sam, gdy po restarcie znow dziala lacze z modulem Wi-Fi, a ono dziala
    # tylko przy podlaczonym kliencie konsoli. Zostajemy wiec polaczeni (albo laczymy sie od nowa) i pytamy o wersje.
    confirmed = False
    reverted = False
    mt = re.search(r"OK (ota_\d)", txt)
    target = mt.group(1) if mt else ""       # partycja, do ktorej trafil obraz
    t_end = time.time() + 100
    while time.time() < t_end and not confirmed:
        time.sleep(3)
        try:
            ser.reset_input_buffer()
            ser.write(b"ver\r")
            ans = read_until("proba", 4)
        except (ConnectionError, OSError):
            try:
                ser.close()
                ser.connect()
            except OSError:
                pass
            continue
        m = re.search(r"syncbox ESP32 v[\d.]+, partycja (\w+), proba (\d)", ans)
        if m:
            print(m.group(0))
            if target and m.group(1) != target:
                # ESP32 odpowiada, ale z drugiej partycji: nowy obraz sie nie utrzymal i wrocil poprzedni firmware
                reverted = True
                break
            confirmed = m.group(2) == "0"
    if reverted:
        print(f"BLAD: nowy firmware ({target}) sie nie utrzymal - ESP32 dziala z poprzedniej partycji. Komenda `ver` pokazuje historie startow.")
    else:
        print("nowy firmware potwierdzony" if confirmed else "UWAGA: brak potwierdzenia - ESP32 wroci do poprzedniej wersji po 120 s od startu")
    ok = confirmed and not reverted
ser.close()
sys.exit(0 if ok else 1)
