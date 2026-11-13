"""Okno testowe HDR10 dla Windows: pełnoekranowe okno na wybranym wyświetlaczu, do którego wpisuje się wprost
10-bitowe wartości sygnału (R'G'B' po krzywej PQ, barwy podstawowe BT.2020) — bez przeglądarki i bez odtwarzacza.

Jak to działa: łańcuch wymiany DXGI w formacie R10G10B10A2 z przestrzenią barw
DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 (HDR10). Obraz ma małą rozdzielczość (domyślnie 384 x 216) i jest
rozciągany przez system na cały ekran; jednolite pola przechodzą przez to bez zmiany wartości. Bufor jest wypełniany
z procesora (UpdateSubresource), więc nie ma shaderów ani żadnej korekcji po drodze.

Wymaga włączonego HDR dla tego wyświetlacza w Windows (Ustawienia > Ekran > Użyj HDR); bez tego system sam
przelicza obraz na SDR i wartości na wyjściu HDMI nie odpowiadają wpisanym.

    from hdrwin import displays, HdrWindow, pq_code
    for d in displays(): print(d)
    w = HdrWindow(displays()[2])
    w.solid((pq_code(203), pq_code(203), pq_code(203)))     # szary 203 nity
    w.hold(2.0)
    w.close()

Tylko biblioteka standardowa (ctypes).
"""
import ctypes
import ctypes.wintypes as wt
import struct
import time
import uuid

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

try:                                   # współrzędne i rozmiary w pikselach fizycznych, niezależnie od skalowania
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except (AttributeError, OSError):
    pass

# ---------------------------------------------------------------------------------------------------------------
# Krzywa PQ (SMPTE ST 2084) i przeliczenie barw
# ---------------------------------------------------------------------------------------------------------------
_M1, _M2 = 2610 / 16384, 2523 / 4096 * 128
_C1, _C2, _C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq_from_nits(nits):
    """Luminancja w nitach -> wartość sygnału PQ 0..1."""
    y = max(0.0, min(1.0, nits / 10000.0)) ** _M1
    return ((_C1 + _C2 * y) / (1 + _C3 * y)) ** _M2


def nits_from_pq(v):
    """Wartość sygnału PQ 0..1 -> luminancja w nitach."""
    p = max(0.0, min(1.0, v)) ** (1 / _M2)
    return 10000.0 * (max(p - _C1, 0.0) / (_C2 - _C3 * p)) ** (1 / _M1)


def pq_code(nits):
    """Luminancja w nitach -> kod 10-bitowy 0..1023 (pełny zakres)."""
    return int(round(pq_from_nits(nits) * 1023))


def srgb_to_linear(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


_709_TO_2020 = ((0.627404, 0.329283, 0.043313), (0.069097, 0.919540, 0.011362), (0.016391, 0.088013, 0.895595))


def srgb_to_hdr10(rgb, white_nits=203.0):
    """Kolor sRGB 0..255 wyświetlany tak, że biel (255,255,255) ma `white_nits` -> trzy kody 10-bitowe PQ BT.2020."""
    lin = [srgb_to_linear(c) for c in rgb]
    return tuple(pq_code(white_nits * sum(m * v for m, v in zip(row, lin))) for row in _709_TO_2020)


# ---------------------------------------------------------------------------------------------------------------
# Wyświetlacze
# ---------------------------------------------------------------------------------------------------------------
class _LUID(ctypes.Structure):
    _fields_ = [("LowPart", wt.DWORD), ("HighPart", wt.LONG)]


class _RATIONAL(ctypes.Structure):
    _fields_ = [("Numerator", ctypes.c_uint32), ("Denominator", ctypes.c_uint32)]


class _PATH_SOURCE(ctypes.Structure):
    _fields_ = [("adapterId", _LUID), ("id", ctypes.c_uint32), ("modeInfoIdx", ctypes.c_uint32), ("statusFlags", ctypes.c_uint32)]


class _PATH_TARGET(ctypes.Structure):
    _fields_ = [("adapterId", _LUID), ("id", ctypes.c_uint32), ("modeInfoIdx", ctypes.c_uint32),
                ("outputTechnology", ctypes.c_uint32), ("rotation", ctypes.c_uint32), ("scaling", ctypes.c_uint32),
                ("refreshRate", _RATIONAL), ("scanLineOrdering", ctypes.c_uint32), ("targetAvailable", wt.BOOL),
                ("statusFlags", ctypes.c_uint32)]


class _PATH_INFO(ctypes.Structure):
    _fields_ = [("sourceInfo", _PATH_SOURCE), ("targetInfo", _PATH_TARGET), ("flags", ctypes.c_uint32)]


class _MODE_INFO(ctypes.Structure):
    _fields_ = [("infoType", ctypes.c_uint32), ("id", ctypes.c_uint32), ("adapterId", _LUID), ("u", ctypes.c_byte * 48)]


class _HEADER(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("size", ctypes.c_uint32), ("adapterId", _LUID), ("id", ctypes.c_uint32)]


class _TARGET_NAME(ctypes.Structure):
    _fields_ = [("header", _HEADER), ("flags", ctypes.c_uint32), ("outputTechnology", ctypes.c_uint32),
                ("edidManufactureId", ctypes.c_uint16), ("edidProductCodeId", ctypes.c_uint16),
                ("connectorInstance", ctypes.c_uint32), ("monitorFriendlyDeviceName", ctypes.c_wchar * 64),
                ("monitorDevicePath", ctypes.c_wchar * 128)]


class _SOURCE_NAME(ctypes.Structure):
    _fields_ = [("header", _HEADER), ("viewGdiDeviceName", ctypes.c_wchar * 32)]


class _ADV_COLOR(ctypes.Structure):
    _fields_ = [("header", _HEADER), ("value", ctypes.c_uint32), ("colorEncoding", ctypes.c_uint32),
                ("bitsPerColorChannel", ctypes.c_uint32)]


class _SDR_WHITE(ctypes.Structure):
    _fields_ = [("header", _HEADER), ("SDRWhiteLevel", ctypes.c_ulong)]


class _DEVMODE(ctypes.Structure):
    _fields_ = [("dmDeviceName", ctypes.c_wchar * 32), ("dmSpecVersion", wt.WORD), ("dmDriverVersion", wt.WORD),
                ("dmSize", wt.WORD), ("dmDriverExtra", wt.WORD), ("dmFields", wt.DWORD), ("x", wt.LONG), ("y", wt.LONG),
                ("dmDisplayOrientation", wt.DWORD), ("dmDisplayFixedOutput", wt.DWORD), ("dmColor", ctypes.c_short),
                ("dmDuplex", ctypes.c_short), ("dmYResolution", ctypes.c_short), ("dmTTOption", ctypes.c_short),
                ("dmCollate", ctypes.c_short), ("dmFormName", ctypes.c_wchar * 32), ("dmLogPixels", wt.WORD),
                ("dmBitsPerPel", wt.DWORD), ("dmPelsWidth", wt.DWORD), ("dmPelsHeight", wt.DWORD),
                ("dmDisplayFlags", wt.DWORD), ("dmDisplayFrequency", wt.DWORD), ("pad", ctypes.c_byte * 32)]


def displays():
    """Aktywne wyświetlacze: lista słowników (gdi, name, x, y, w, h, hz, hdr_supported, hdr_enabled, bits, sdr_white_nits)."""
    n_path, n_mode = ctypes.c_uint32(), ctypes.c_uint32()
    user32.GetDisplayConfigBufferSizes(2, ctypes.byref(n_path), ctypes.byref(n_mode))
    paths = (_PATH_INFO * n_path.value)()
    modes = (_MODE_INFO * n_mode.value)()
    rc = user32.QueryDisplayConfig(2, ctypes.byref(n_path), paths, ctypes.byref(n_mode), modes, None)
    if rc != 0:
        raise OSError(f"QueryDisplayConfig: {rc}")
    out = []
    for p in paths[: n_path.value]:
        t = p.targetInfo

        def info(struct, kind, adapter=t.adapterId, ident=t.id):
            s = struct()
            s.header.type, s.header.size, s.header.adapterId, s.header.id = kind, ctypes.sizeof(s), adapter, ident
            return s, user32.DisplayConfigGetDeviceInfo(ctypes.byref(s)) == 0

        name, _ = info(_TARGET_NAME, 2)
        src, _ = info(_SOURCE_NAME, 1, p.sourceInfo.adapterId, p.sourceInfo.id)
        adv, _ = info(_ADV_COLOR, 9)
        sdr, sdr_ok = info(_SDR_WHITE, 11)
        dm = _DEVMODE()
        dm.dmSize = ctypes.sizeof(_DEVMODE)
        user32.EnumDisplaySettingsW(src.viewGdiDeviceName, -1, ctypes.byref(dm))
        hz = t.refreshRate.Numerator / t.refreshRate.Denominator if t.refreshRate.Denominator else 0
        out.append({
            "gdi": src.viewGdiDeviceName, "name": name.monitorFriendlyDeviceName, "x": dm.x, "y": dm.y,
            "w": dm.dmPelsWidth, "h": dm.dmPelsHeight, "hz": round(hz, 2),
            "hdr_supported": bool(adv.value & 1), "hdr_enabled": bool(adv.value & 2), "bits": adv.bitsPerColorChannel,
            "sdr_white_nits": (sdr.SDRWhiteLevel / 1000.0 * 80.0) if sdr_ok else None,
        })
    out.sort(key=lambda d: d["gdi"])
    return out


def describe(d):
    return (f"{d['gdi']}: {d['name'] or '(bez nazwy)'}, {d['w']}x{d['h']} @ {d['hz']} Hz, pozycja {d['x']},{d['y']}; "
            f"HDR {'WŁĄCZONY' if d['hdr_enabled'] else ('wyłączony' if d['hdr_supported'] else 'nieobsługiwany')}, "
            f"{d['bits']} bitów na kanał, biel SDR {d['sdr_white_nits']:.0f} nitów")


def find_display(which):
    """`which`: numer (3 -> \\\\.\\DISPLAY3) albo fragment nazwy."""
    ds = displays()
    for d in ds:
        if d["gdi"].upper().endswith("DISPLAY" + str(which)) or (str(which) and str(which).lower() in d["name"].lower()):
            return d
    raise SystemExit(f"Nie ma wyświetlacza '{which}'. Dostępne:\n" + "\n".join("  " + describe(d) for d in ds))


# ---------------------------------------------------------------------------------------------------------------
# COM bez bibliotek: wywołanie metody o danym numerze w tablicy wirtualnej
# ---------------------------------------------------------------------------------------------------------------
def _guid(s):
    return (ctypes.c_ubyte * 16).from_buffer_copy(uuid.UUID(s).bytes_le)


def _call(ptr, index, restype, argtypes, *args):
    vtbl = ctypes.cast(ptr, ctypes.POINTER(ctypes.c_void_p))[0]
    fn = ctypes.cast(vtbl, ctypes.POINTER(ctypes.c_void_p))[index]
    return ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(fn)(ptr, *args)


def _hr(ptr, index, what, argtypes, *args):
    r = _call(ptr, index, ctypes.c_long, argtypes, *args)
    if r < 0:
        raise OSError(f"{what}: 0x{r & 0xFFFFFFFF:08X}")
    return r


def _release(ptr):
    if ptr:
        _call(ptr, 2, ctypes.c_ulong, ())


IID_IDXGIDevice = _guid("54ec77fa-1377-44e6-8c32-88fd5f44c84c")
IID_IDXGIFactory2 = _guid("50c83a1c-e072-4c48-87b0-3630fa36a6d0")
IID_IDXGISwapChain3 = _guid("94d99bdb-f1f8-4ab0-b236-7da0170edab1")
IID_ID3D11Texture2D = _guid("6f15aaf2-d208-4e89-9ab4-489535d34f9c")

DXGI_FORMAT_R10G10B10A2_UNORM = 24
DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 = 0        # zwykłe sRGB (porównanie w SDR)
DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 = 12    # HDR10


class _SWAP_DESC1(ctypes.Structure):
    _fields_ = [("Width", ctypes.c_uint), ("Height", ctypes.c_uint), ("Format", ctypes.c_uint), ("Stereo", ctypes.c_int),
                ("Count", ctypes.c_uint), ("Quality", ctypes.c_uint), ("BufferUsage", ctypes.c_uint),
                ("BufferCount", ctypes.c_uint), ("Scaling", ctypes.c_uint), ("SwapEffect", ctypes.c_uint),
                ("AlphaMode", ctypes.c_uint), ("Flags", ctypes.c_uint)]


_WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class _WNDCLASS(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", _WNDPROC), ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HANDLE), ("hCursor", wt.HANDLE), ("hbrBackground", wt.HANDLE),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, wt.HWND, wt.HANDLE, wt.HINSTANCE, ctypes.c_void_p]
user32.CreateWindowExW.restype = wt.HWND
user32.DestroyWindow.argtypes = [wt.HWND]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.PeekMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT]
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
user32.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
user32.SetCursor.argtypes = [wt.HANDLE]
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
kernel32.GetModuleHandleW.restype = wt.HINSTANCE

_wndproc_ref = None
_class_done = False


def _wndproc(hwnd, msg, wp, lp):
    if msg == 0x0020:                  # WM_SETCURSOR: bez kursora nad polem pomiarowym
        user32.SetCursor(None)
        return 1
    return user32.DefWindowProcW(hwnd, msg, wp, lp)


class HdrWindow:
    """Okno na cały wyświetlacz `disp` (słownik z displays()). hdr=False daje zwykłe sRGB 10-bit (do porównań)."""

    def __init__(self, disp, width=384, height=216, hdr=True):
        global _wndproc_ref, _class_done
        self.w, self.h, self.hdr = width, height, hdr
        self.dev = self.ctx = self.swap = self.swap3 = None
        hinst = kernel32.GetModuleHandleW(None)
        if not _class_done:
            _wndproc_ref = _WNDPROC(_wndproc)
            wc = _WNDCLASS()
            wc.lpfnWndProc, wc.hInstance, wc.lpszClassName = _wndproc_ref, hinst, "SyncboxHdrTest"
            if not user32.RegisterClassW(ctypes.byref(wc)):
                raise OSError(f"RegisterClassW: {ctypes.get_last_error()}")
            _class_done = True
        # WS_POPUP; WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE: nad innymi oknami, bez przycisku na pasku zadań,
        # bez zabierania klawiatury programowi, w którym ktoś pracuje na innym monitorze
        self.hwnd = user32.CreateWindowExW(0x00000008 | 0x00000080 | 0x08000000, "SyncboxHdrTest", "Syncbox HDR test",
                                           0x80000000, disp["x"], disp["y"], disp["w"], disp["h"], None, None, hinst, None)
        if not self.hwnd:
            raise OSError(f"CreateWindowExW: {ctypes.get_last_error()}")
        user32.ShowWindow(self.hwnd, 4)                       # SW_SHOWNOACTIVATE
        try:
            self._init_d3d()
        except Exception:
            self.close()
            raise

    def _init_d3d(self):
        d3d11 = ctypes.WinDLL("d3d11")
        dev, ctx = ctypes.c_void_p(), ctypes.c_void_p()
        r = d3d11.D3D11CreateDevice(None, 1, None, 0, None, 0, 7, ctypes.byref(dev), None, ctypes.byref(ctx))
        if r < 0:
            raise OSError(f"D3D11CreateDevice: 0x{r & 0xFFFFFFFF:08X}")
        self.dev, self.ctx = dev, ctx
        dxdev, adapter, factory = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
        vp = ctypes.c_void_p
        _hr(dev, 0, "QueryInterface(IDXGIDevice)", (vp, vp), ctypes.byref(IID_IDXGIDevice), ctypes.byref(dxdev))
        _hr(dxdev, 7, "IDXGIDevice::GetAdapter", (vp,), ctypes.byref(adapter))
        _hr(adapter, 6, "IDXGIAdapter::GetParent(IDXGIFactory2)", (vp, vp), ctypes.byref(IID_IDXGIFactory2), ctypes.byref(factory))
        desc = _SWAP_DESC1(self.w, self.h, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 1, 0, 0x20, 2, 0, 4, 0, 0)   # STRETCH, FLIP_DISCARD
        swap = ctypes.c_void_p()
        _hr(factory, 15, "CreateSwapChainForHwnd", (vp, wt.HWND, vp, vp, vp, vp), dev, self.hwnd, ctypes.byref(desc), None, None,
            ctypes.byref(swap))
        self.swap = swap
        for p in (factory, adapter, dxdev):
            _release(p)
        swap3 = ctypes.c_void_p()
        _hr(swap, 0, "QueryInterface(IDXGISwapChain3)", (vp, vp), ctypes.byref(IID_IDXGISwapChain3), ctypes.byref(swap3))
        self.swap3 = swap3
        cs = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 if self.hdr else DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
        support = ctypes.c_uint()
        _hr(swap3, 37, "CheckColorSpaceSupport", (ctypes.c_uint, vp), cs, ctypes.byref(support))
        if not support.value & 1:
            raise OSError("ten wyświetlacz/sterownik nie przyjmuje łańcucha wymiany w przestrzeni HDR10")
        _hr(swap3, 38, "SetColorSpace1", (ctypes.c_uint,), cs)

    # --- obraz ---
    @staticmethod
    def pixel(code):
        """Trzy kody 10-bitowe (R, G, B) -> 4 bajty piksela R10G10B10A2."""
        r, g, b = (max(0, min(1023, int(c))) for c in code)
        return struct.pack("<I", r | (g << 10) | (b << 20) | (3 << 30))

    def show(self, data):
        """`data`: dokładnie w*h*4 bajtów w formacie R10G10B10A2, wierszami od góry."""
        if len(data) != self.w * self.h * 4:
            raise ValueError("zły rozmiar obrazu")
        vp = ctypes.c_void_p
        buf = ctypes.create_string_buffer(data, len(data))
        for _ in range(3):                      # FLIP_DISCARD: po Present bufor jest do wypełnienia od nowa
            tex = ctypes.c_void_p()
            _hr(self.swap, 9, "GetBuffer", (ctypes.c_uint, vp, vp), 0, ctypes.byref(IID_ID3D11Texture2D), ctypes.byref(tex))
            _call(self.ctx, 48, None, (vp, ctypes.c_uint, vp, vp, ctypes.c_uint, ctypes.c_uint), tex, 0, None, buf, self.w * 4, 0)
            _release(tex)
            _hr(self.swap, 8, "Present", (ctypes.c_uint, ctypes.c_uint), 1, 0)
        self.pump()

    def solid(self, code):
        self.show(self.pixel(code) * (self.w * self.h))

    def cells(self, nx, ny, codes):
        """Siatka nx x ny pól; `codes`: lista nx*ny trójek kodów, wierszami."""
        rows = []
        for y in range(ny):
            line = b""
            for x in range(nx):
                x0, x1 = round(x * self.w / nx), round((x + 1) * self.w / nx)
                line += self.pixel(codes[y * nx + x]) * (x1 - x0)
            rows.append(line)
        img = b""
        for y in range(ny):
            y0, y1 = round(y * self.h / ny), round((y + 1) * self.h / ny)
            img += rows[y] * (y1 - y0)
        self.show(img)

    def pump(self):
        msg = wt.MSG()
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

    def hold(self, seconds):
        """Czekaj, obsługując komunikaty okna (bez tego system uzna okno za zawieszone)."""
        t_end = time.time() + seconds
        while time.time() < t_end:
            self.pump()
            time.sleep(0.01)

    def close(self):
        for name in ("swap3", "swap", "ctx", "dev"):
            p = getattr(self, name, None)
            if p:
                _release(p)
            setattr(self, name, None)
        if getattr(self, "hwnd", None):
            user32.DestroyWindow(self.hwnd)
            self.hwnd = None
        self.pump()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


if __name__ == "__main__":
    import sys
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    for d in displays():
        print(describe(d))
