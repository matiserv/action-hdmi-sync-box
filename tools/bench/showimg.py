"""Wyświetla obraz na stronie testowej (jako siatkę kolorowych pól nx x ny, proporcje 16:9).
Użycie: python showimg.py <plik_obrazu> [nx=240]
"""
import json, sys, urllib.request
from PIL import Image

nx = int(sys.argv[2]) if len(sys.argv) > 2 else 240
ny = nx * 9 // 16
im = Image.open(sys.argv[1]).convert("RGB").resize((nx, ny), Image.LANCZOS)
cells = [list(p) for p in im.getdata()]
req = urllib.request.Request("http://localhost:8765/state", data=json.dumps({"mode": "cells", "nx": nx, "ny": ny, "cells": cells}).encode(),
                             headers={"Content-Type": "application/json"})
print(urllib.request.urlopen(req, timeout=5).read().decode()[:60], f"{nx}x{ny}")
