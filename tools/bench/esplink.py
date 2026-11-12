"""Polaczenie z konsola ESP32: port szeregowy (np. COM4) albo siec (tcp://192.168.1.50:2323).

Po sieci konsola idzie przez modul Wi-Fi boxa (komponent syncbox, opcja `esp32_console`): klient laczy sie z portem TCP,
wysyla haslo jako pierwsza linie i dalej rozmawia jak przez UART. Haslo jest brane ze zmiennej srodowiskowej
SYNCBOX_CONSOLE_PASSWORD (pusta = serwer bez hasla).

    port = open_port("COM4")                      # albo "tcp://192.168.1.50:2323"
    port.write(b"stat\r"); dane = port.read(4096)

Obiekt ma to, czego uzywaja skrypty z pyserial: read(n), write(b), reset_input_buffer(), close(), pole timeout.
"""
import os
import socket
import time


class TcpPort:
    def __init__(self, url, timeout=0.05):
        host, _, port = url[len("tcp://"):].partition(":")
        self.timeout = timeout
        self.sock = socket.create_connection((host, int(port or 2323)), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        pw = os.environ.get("SYNCBOX_CONSOLE_PASSWORD", "")
        if pw:
            self.sock.sendall(pw.encode() + b"\n")
        time.sleep(0.3)             # zerowanie tunelu po stronie boxa trwa kilka ramek

    def read(self, n=1):
        self.sock.settimeout(self.timeout if self.timeout else 0.001)
        try:
            data = self.sock.recv(n)
        except (socket.timeout, BlockingIOError):
            return b""
        if data == b"":
            raise ConnectionError("box zamknal polaczenie konsoli (zle haslo albo restart)")
        return data

    def write(self, data):
        self.sock.settimeout(30)
        self.sock.sendall(data)
        return len(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        self.sock.settimeout(0.001)
        try:
            while self.sock.recv(65536):
                pass
        except (socket.timeout, BlockingIOError):
            pass

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def open_port(addr, timeout=0.05):
    if addr.startswith("tcp://"):
        return TcpPort(addr, timeout)
    import serial
    s = serial.Serial()
    s.port = addr
    s.baudrate = 115200
    s.timeout = timeout
    s.rts = False
    s.dtr = False              # ustawione przed otwarciem: zadnego impulsu na EN/GPIO0
    s.open()
    return s
