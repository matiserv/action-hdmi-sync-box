"""Serwer strony testowej dla HDMI Sync Box.

Uruchom:  python server.py [port]        (domyślnie 8765)
Otwórz na ekranie podłączonym przez sync box:  http://<adres>:8765/  i przejdź na pełny ekran (klik albo F11).
Stan wzoru:  GET /state  (JSON),  POST /state  (JSON z polem "mode"; serwer nadaje numer "seq").
"""
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
state = {"seq": 0, "mode": "idle"}
lock = threading.Lock()
last_poll = {"t": 0.0, "n": 0}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):  # cicho
        pass

    def _send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/state"):
            import time
            last_poll["t"] = time.time()
            last_poll["n"] += 1
            with lock:
                self._send(200, json.dumps(state).encode())
        elif self.path.startswith("/clients"):          # czy strona jest otwarta (ostatnie odpytanie)
            import time
            self._send(200, json.dumps({"age_s": time.time() - last_poll["t"], "polls": last_poll["n"]}).encode())
        elif self.path in ("/", "/index.html"):
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                self._send(200, f.read(), "text/html; charset=utf-8")
        else:
            self._send(404, b"{}")

    def do_POST(self):
        if not self.path.startswith("/state"):
            self._send(404, b"{}")
            return
        n = int(self.headers.get("Content-Length", "0"))
        try:
            new = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            self._send(400, b"{}")
            return
        with lock:
            new["seq"] = state["seq"] + 1
            state.clear()
            state.update(new)
            self._send(200, json.dumps({"seq": state["seq"]}).encode())


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    srv = ThreadingHTTPServer(("0.0.0.0", port), H)
    print(f"bench: http://localhost:{port}/", flush=True)
    srv.serve_forever()
