"""Polaczenie z BK7238 (ESPHome) przez natywne API: jedno stale polaczenie do logow, stanow i ustawien.

Serwer WWW ESPHome (/events) wymaga nowego polaczenia przy kazdym odczycie; przy wielu zrywanych polaczeniach
BK7238 potrafil sie zrestartowac, dlatego testy korzystaja z API.

Klucz szyfrowania API jest czytany z pliku YAML z konfiguracja urzadzenia (pole `key:` w sekcji api/encryption) -
nie trafia do wiersza polecen ani do wynikow.

    bk = Bk("192.168.1.50", "sciezka/do/konfiguracji.yaml")
    bk.set_number("Video: inner fill", 25)
    bk.set_switch("Diagnostics: section log", True)
    line = bk.wait_log("sections L:", 3.0)
    bk.close()
"""
import asyncio
import os
import re
import threading
import time

from aioesphomeapi import APIClient, LogLevel


def read_key(yaml_path):
    """Klucz API z pliku YAML urzadzenia: wpisany wprost albo jako `!secret nazwa` (wtedy z secrets.yaml obok)."""
    txt = open(yaml_path, encoding="utf-8").read()
    m = re.search(r"encryption:\s*\n\s*key:\s*(!secret\s+)?\"?([A-Za-z0-9+/=_]+)\"?", txt)
    if not m:
        raise SystemExit("brak api.encryption.key w " + yaml_path)
    if not m.group(1):
        return m.group(2)
    sec = os.path.join(os.path.dirname(os.path.abspath(yaml_path)), "secrets.yaml")
    if not os.path.exists(sec):
        raise SystemExit(f"klucz API jest podany jako !secret {m.group(2)}, a obok {yaml_path} nie ma secrets.yaml")
    k = re.search(r"^" + re.escape(m.group(2)) + r":\s*\"?([A-Za-z0-9+/=]+)\"?\s*(#.*)?$", open(sec, encoding="utf-8").read(), re.M)
    if not k:
        raise SystemExit(f"w {sec} nie ma wpisu {m.group(2)}")
    return k.group(1)


class Bk:
    def __init__(self, host, yaml_path):
        self.host = host
        self._key = read_key(yaml_path)
        self.logs = []                 # (czas, tekst)
        self.states = {}               # nazwa encji -> ostatni stan
        self.drops = 0                 # ile razy polaczenie zostalo zerwane (np. restart urzadzenia)
        self._names = {}               # klucz encji -> nazwa
        self._keys = {}                # nazwa -> (klucz, rodzaj)
        self._loop = asyncio.new_event_loop()
        self._thr = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._thr.start()
        self._cli = None
        self._run(self._connect())

    def _run(self, coro, timeout=20):
        return asyncio.run_coroutine_threadsafe(coro, self._loop).result(timeout)

    async def _connect(self):
        self._cli = APIClient(self.host, 6053, None, noise_psk=self._key)

        async def on_stop(expected):
            if not expected:
                self.drops += 1

        await self._cli.connect(on_stop=on_stop, login=True)
        entities, _ = await self._cli.list_entities_services()
        for e in entities:
            kind = type(e).__name__.replace("Info", "").lower()
            self._names[e.key] = e.name
            self._keys[e.name] = (e.key, kind)

        def on_state(s):
            name = self._names.get(s.key)
            if name is not None:
                self.states[name] = getattr(s, "state", None)

        def on_log(msg):
            text = msg.message.decode("utf-8", "replace") if isinstance(msg.message, bytes) else str(msg.message)
            self.logs.append((time.time(), re.sub(r"\x1b\[[0-9;]*m", "", text)))
            if len(self.logs) > 2000:
                del self.logs[:1000]

        self._cli.subscribe_states(on_state)
        self._cli.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_DEBUG)

    def connected(self):
        return self._cli is not None and self._cli._connection is not None and self._cli._connection.is_connected

    def reconnect(self):
        try:
            self._run(self._cli.disconnect(), 5)
        except Exception:
            pass
        self._run(self._connect(), 40)

    def set_number(self, name, value):
        key, _ = self._keys[name]
        self._loop.call_soon_threadsafe(self._cli.number_command, key, float(value))

    def set_switch(self, name, on):
        key, _ = self._keys[name]
        self._loop.call_soon_threadsafe(self._cli.switch_command, key, bool(on))

    def wait_log(self, pattern, timeout, after=None):
        """Pierwsza linia logu pasujaca do wzorca, ktora przyszla po chwili `after` (domyslnie: od teraz)."""
        t0 = time.time() if after is None else after
        t_end = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < t_end:
            for t, text in list(self.logs):
                if t >= t0 and rx.search(text):
                    return text
            time.sleep(0.05)
        return None

    def close(self):
        try:
            self._run(self._cli.disconnect(), 5)
        except Exception:
            pass
        self._loop.call_soon_threadsafe(self._loop.stop)
