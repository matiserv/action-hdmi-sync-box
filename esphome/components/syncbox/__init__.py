"""Eurodomest / LSC HDMI Sync Box (2. gen, T1-U / BK7238) - komponent ESPHome."""

from esphome import pins
import esphome.codegen as cg
from esphome.components import binary_sensor, sensor, socket, text_sensor
from esphome.components.light.effects import register_addressable_effect
from esphome.components.light.types import AddressableLightEffect
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_BRIGHTNESS,
    CONF_ENABLE_PIN,
    CONF_ID,
    CONF_INVERTED,
    CONF_GAMMA_CORRECT,
    CONF_MODE,
    CONF_NAME,
    CONF_NUMBER,
    CONF_PASSWORD,
    CONF_PORT,
    STATE_CLASS_MEASUREMENT,
)

CODEOWNERS = ["@mati"]
DEPENDENCIES = ["libretiny"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "socket"]

syncbox_ns = cg.esphome_ns.namespace("syncbox")
SyncBox = syncbox_ns.class_("SyncBox", cg.Component)
SyncBoxVideoEffect = syncbox_ns.class_("SyncBoxVideoEffect", AddressableLightEffect)

CONF_SYNCBOX_ID = "syncbox_id"
CONF_LAYOUT = "layout"
CONF_LEFT = "left"
CONF_TOP = "top"
CONF_RIGHT = "right"
CONF_BOTTOM = "bottom"
CONF_START_CORNER = "start_corner"
CONF_DIRECTION = "direction"
CONF_COLOR_ORDER = "color_order"
CONF_VIDEO = "video"
CONF_ENABLED = "enabled"
CONF_SPI_MODE = "spi_mode"
CONF_FRAME_SIZE = "frame_size"
CONF_GRID_WIDTH = "grid_width"
CONF_GRID_HEIGHT = "grid_height"
CONF_PIXEL_ORDER = "pixel_order"
CONF_DEPTH = "depth"
CONF_SMOOTHING = "smoothing"
CONF_SATURATION = "saturation"
CONF_LETTERBOX_DETECTION = "letterbox_detection"
CONF_SIGNAL_TIMEOUT = "signal_timeout"
CONF_AUTO_OFF = "auto_off"
CONF_LOG_FRAMES = "log_frames"
CONF_INNER_MIX = "inner_mix"
CONF_INNER_DIM = "inner_dim"
CONF_INTERPOLATION = "interpolation"
CONF_VIDEO_FPS = "video_fps"
CONF_LED_FPS = "led_fps"
CONF_HDMI_SIGNAL = "hdmi_signal"
CONF_HDMI_INPUT = "hdmi_input"
CONF_HDMI_DROPOUTS = "hdmi_dropouts"
CONF_ESP32_CONSOLE = "esp32_console"
CONF_RESET_TIME = "reset_time"
CONF_REFRESH_INTERVAL = "refresh_interval"
CONF_WHITE_BALANCE = "white_balance"
CONF_RED = "red"
CONF_GREEN = "green"
CONF_BLUE = "blue"
CONF_BLACK_LEVEL = "black_level"
CONF_WHITE_LEVEL = "white_level"
CONF_AVERAGE_COLOR = "average_color"
CONF_SATELLITES = "satellites"
CONF_UNIVERSE = "universe"
CONF_LEDS = "leds"
CONF_MAX_FPS = "max_fps"
CONF_WALLS = "walls"
CONF_TRANSITION = "transition"
CONF_GAMMA = "gamma"
CONF_BLUR = "blur"

# co dostaje satelita (patrz SatelliteMode w syncbox.h)
SATELLITE_MODES = {
    "average": 0,
    "mirror": 1,
    "left": 2,
    "top": 3,
    "right": 4,
    "bottom": 5,
    "full": 6,  # cala siatka 16x16 (256 px) w dwoch kolejnych universach
    "room": 7,  # obwod pokoju: kolory krawedzi ekranu rozlozone po scianach (patrz walls)
}

# Sciany pokoju, patrzac w strone TV, w kolejnosci obwodu ekranu: lewa sciana = lewy bok obrazu (od tylu pokoju do
# przodu = od dolu do gory obrazu), front (sciana z TV) = gorna krawedz (od lewej do prawej), prawa = prawy bok (od
# przodu do tylu = od gory do dolu), back (sciana za widzem) = dolna krawedz (od prawej do lewej).
WALLS = ["left", "front", "right", "back"]

CORNERS = {
    "bottom_left": 0,
    "top_left": 1,
    "top_right": 2,
    "bottom_right": 3,
}
DIRECTIONS = {"clockwise": True, "counter_clockwise": False}
ORDERS = {
    "RGB": (0, 1, 2),
    "RBG": (0, 2, 1),
    "GRB": (1, 0, 2),
    "GBR": (1, 2, 0),
    "BRG": (2, 0, 1),
    "BGR": (2, 1, 0),
}


def _validate_layout(value):
    if value[CONF_BOTTOM] == 0:
        start = value[CONF_START_CORNER]
        direction = value[CONF_DIRECTION]
        ok = (start == "bottom_left" and direction == "clockwise") or (
            start == "bottom_right" and direction == "counter_clockwise"
        )
        if not ok:
            raise cv.Invalid(
                "Przy montazu 3-stronnym (bottom: 0) tasma zaczyna sie w bottom_left "
                "(clockwise) albo bottom_right (counter_clockwise)."
            )
    return value


LAYOUT_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_LEFT, default=7): cv.int_range(min=0, max=64),
            cv.Optional(CONF_TOP, default=14): cv.int_range(min=0, max=64),
            cv.Optional(CONF_RIGHT, default=7): cv.int_range(min=0, max=64),
            cv.Optional(CONF_BOTTOM, default=0): cv.int_range(min=0, max=64),
            cv.Optional(CONF_START_CORNER, default="bottom_left"): cv.one_of(
                *CORNERS, lower=True
            ),
            cv.Optional(CONF_DIRECTION, default="clockwise"): cv.one_of(
                *DIRECTIONS, lower=True
            ),
        }
    ),
    _validate_layout,
)

VIDEO_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ENABLED, default=True): cv.boolean,
        cv.Optional(CONF_SPI_MODE, default=0): cv.int_range(min=0, max=3),
        cv.Optional(CONF_FRAME_SIZE, default=768): cv.int_range(min=3, max=8192),
        cv.Optional(CONF_GRID_WIDTH, default=16): cv.int_range(min=1, max=64),
        cv.Optional(CONF_GRID_HEIGHT, default=16): cv.int_range(min=1, max=64),
        cv.Optional(CONF_PIXEL_ORDER, default="RGB"): cv.one_of(*ORDERS, upper=True),
        cv.Optional(CONF_DEPTH, default=3): cv.int_range(min=1, max=8),
        cv.Optional(CONF_SMOOTHING, default=0.5): cv.float_range(min=0.0, max=0.99),
        cv.Optional(CONF_SATURATION, default=1.3): cv.float_range(min=0.0, max=3.0),
        cv.Optional(CONF_BRIGHTNESS, default=1.0): cv.percentage,
        cv.Optional(CONF_LETTERBOX_DETECTION, default=True): cv.boolean,
        cv.Optional(
            CONF_SIGNAL_TIMEOUT, default="1000ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_AUTO_OFF, default=True): cv.boolean,
        cv.Optional(CONF_LOG_FRAMES, default=False): cv.boolean,
        # doswietlenie ze srodka obrazu: 0 = wylaczone, 25% = krawedz swieci co najmniej 1/4 swiatla z glebi obrazu
        cv.Optional(CONF_INNER_MIX, default=0.0): cv.percentage,
        # przygaszanie przy ciemnym srodku: 0 = wylaczone, 100% = krawedz nie swieci jasniej niz obraz w glebi
        cv.Optional(CONF_INNER_DIM, default=0.0): cv.percentage,
        cv.Optional(CONF_INTERPOLATION, default=True): cv.boolean,
        # kalibracja kolorow obrazu (do zmiany w locie przez set_video_levels / set_white_balance / set_video_gamma)
        cv.Optional(CONF_BLACK_LEVEL, default=0): cv.int_range(min=0, max=200),
        cv.Optional(CONF_WHITE_LEVEL, default=255): cv.int_range(min=50, max=255),
        cv.Optional(CONF_WHITE_BALANCE, default={}): cv.Schema(
            {
                cv.Optional(CONF_RED, default=1.0): cv.float_range(min=0.0, max=2.0),
                cv.Optional(CONF_GREEN, default=1.0): cv.float_range(min=0.0, max=2.0),
                cv.Optional(CONF_BLUE, default=1.0): cv.float_range(min=0.0, max=2.0),
            }
        ),
        cv.Optional(CONF_GAMMA_CORRECT, default=1.0): cv.float_range(min=0.3, max=4.0),
    }
)

def _validate_address(value):
    value = cv.string_strict(value)
    if value in ("", "multicast"):
        return ""
    parts = value.split(".")
    if len(parts) != 4 or not all(p.isdigit() and 0 <= int(p) <= 255 for p in parts):
        raise cv.Invalid("address: podaj IPv4 (np. 192.168.1.50) albo 'multicast'")
    return value


def _validate_wall(value):
    if not isinstance(value, dict) or len(value) != 1:
        raise cv.Invalid("walls: kazdy wpis to jedna sciana z liczba diod, np. '- left: 16'")
    ((wall, leds),) = value.items()
    return {cv.one_of(*WALLS, lower=True)(wall): cv.int_range(min=1, max=170)(leds)}


def _validate_satellite(config):
    walls = config.get(CONF_WALLS)
    if config[CONF_MODE] != "room":
        if walls:
            raise cv.Invalid("walls: tylko dla mode: room")
        return config
    if not walls:
        raise cv.Invalid("mode: room wymaga listy walls (sciany w kolejnosci tasmy lampy, od pierwszej diody)")
    idx = [WALLS.index(next(iter(w))) for w in walls]
    # kierunek na kazdej scianie wynika z nastepnej sciany na liscie (dla ostatniej: z poprzedniej)
    steps = [(b - a) % 4 for a, b in zip(idx, idx[1:])]
    if any(st not in (1, 3) for st in steps):
        raise cv.Invalid("walls: kolejne wpisy musza byc sasiednimi scianami (np. back, left, front, right)")
    steps = steps + [steps[-1] if steps else 1]
    segs = []
    for w, i, st in zip(walls, idx, steps):
        leds = next(iter(w.values()))
        segs.append((float(i), float(i + 1), leds) if st == 1 else (float(i + 1), float(i), leds))
    total = sum(sg[2] for sg in segs)
    if total > 170:
        raise cv.Invalid("walls: razem najwyzej 170 diod (jeden universe)")
    config = dict(config)
    config[CONF_LEDS] = total
    config["_segments"] = segs
    return config


# Satelita = inne swiatlo w sieci odbierajace E1.31/sACN (WLED, inny box z ESPHome
# z efektem e131, kontroler DMX...). Box wysyla do niego kolory taśmy.
SATELLITE_SCHEMA = cv.All(cv.Schema(
    {
        cv.Optional(CONF_NAME, default=""): cv.string,
        cv.Optional(CONF_ADDRESS, default="multicast"): _validate_address,
        cv.Optional(CONF_UNIVERSE, default=1): cv.int_range(min=1, max=63999),
        cv.Optional(CONF_MODE, default="average"): cv.one_of(
            *SATELLITE_MODES, lower=True
        ),
        cv.Optional(CONF_LEDS, default=1): cv.int_range(min=1, max=170),
        cv.Optional(CONF_BRIGHTNESS, default=1.0): cv.percentage,
        cv.Optional(CONF_MAX_FPS, default=30): cv.int_range(min=1, max=60),
        cv.Optional(CONF_WALLS): cv.ensure_list(_validate_wall),
        # stala czasowa dochodzenia lampy do nowego koloru (0ms = bez wygladzania); nie dotyczy mode: full
        cv.Optional(CONF_TRANSITION, default="300ms"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(seconds=10))
        ),
        # gamma, ktora odbiornik sam naklada na odebrane wartosci (swiatla ESPHome: 2.8, chyba ze maja gamma_correct);
        # box wysyla wtedy wartosci zakodowane odwrotnie. 1.0 = wartosci wprost jak dla tasmy (odbiornik bez gammy).
        cv.Optional(CONF_GAMMA, default=1.0): cv.float_range(min=1.0, max=4.0),
        # mode: room - rozmycie wzdluz tasmy lampy: srednia z tylu sasiednich diod z kazdej strony, liczona dwa razy
        # (0 = bez). Wieksza wartosc = mniej szczegolow, lagodniejsze przejscia miedzy scianami.
        cv.Optional(CONF_BLUR, default=0): cv.int_range(min=0, max=40),
    }
), _validate_satellite)

def _console_sockets(config):
    # serwer konsoli zajmuje jedno gniazdo nasluchujace i jedno polaczenie (ESPHome liczy z tego limity lwIP)
    if CONF_ESP32_CONSOLE in config:
        socket.consume_sockets(1, "syncbox_console", socket.SocketType.TCP_LISTEN)(config)
        socket.consume_sockets(1, "syncbox_console")(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SyncBox),
            cv.Optional(
                CONF_REFRESH_INTERVAL, default="1s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SATELLITES, default=[]): cv.ensure_list(SATELLITE_SCHEMA),
            cv.Optional(CONF_AVERAGE_COLOR): text_sensor.text_sensor_schema(),
            cv.Optional(
                CONF_ENABLE_PIN, default={CONF_NUMBER: "P24", CONF_INVERTED: True}
            ): pins.gpio_output_pin_schema,
            cv.Optional(CONF_LAYOUT, default={}): LAYOUT_SCHEMA,
            cv.Optional(CONF_COLOR_ORDER, default="BRG"): cv.one_of(*ORDERS, upper=True),
            cv.Optional(
                CONF_RESET_TIME, default="300us"
            ): cv.positive_time_period_microseconds,
            cv.Optional(CONF_VIDEO, default={}): VIDEO_SCHEMA,
            cv.Optional(CONF_VIDEO_FPS): sensor.sensor_schema(
                unit_of_measurement="fps",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_LED_FPS): sensor.sensor_schema(
                unit_of_measurement="fps",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_HDMI_SIGNAL): binary_sensor.binary_sensor_schema(),
            # Stan wejscia HDMI zglaszany przez ESP32 (firmware v14.4+): format i tryb HDR jako tekst, licznik zanikow.
            cv.Optional(CONF_HDMI_INPUT): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_HDMI_DROPOUTS): sensor.sensor_schema(accuracy_decimals=0),
            # Konsola ESP32 po sieci: serwer TCP, przez ktory dzialaja komendy konsoli i wgrywanie firmware ESP32
            # (bench/*.py i esp32_upload.py przyjmuja adres tcp://IP:port zamiast portu COM). Haslo = pierwsza linia
            # wyslana przez klienta; bez hasla kazdy w sieci lokalnej moze sterowac ESP32 i wgrac mu firmware.
            cv.Optional(CONF_ESP32_CONSOLE): cv.Schema(
                {
                    cv.Optional(CONF_PORT, default=2323): cv.port,
                    cv.Optional(CONF_PASSWORD, default=""): cv.string,
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on(["bk72xx"]),
    _console_sockets,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    pin = await cg.gpio_pin_expression(config[CONF_ENABLE_PIN])
    cg.add(var.set_enable_pin(pin))

    lay = config[CONF_LAYOUT]
    cg.add(
        var.set_sides(lay[CONF_LEFT], lay[CONF_TOP], lay[CONF_RIGHT], lay[CONF_BOTTOM])
    )
    cg.add(var.set_start_corner(CORNERS[lay[CONF_START_CORNER]]))
    cg.add(var.set_clockwise(DIRECTIONS[lay[CONF_DIRECTION]]))

    o = ORDERS[config[CONF_COLOR_ORDER]]
    cg.add(var.set_color_order(o[0], o[1], o[2]))
    cg.add(var.set_reset_time(config[CONF_RESET_TIME]))
    cg.add(var.set_refresh_interval(config[CONF_REFRESH_INTERVAL]))

    if config[CONF_SATELLITES]:
        cg.add_define("SYNCBOX_SATELLITES")
        for sat in config[CONF_SATELLITES]:
            cg.add(
                var.add_satellite(
                    sat[CONF_NAME],
                    sat[CONF_ADDRESS],
                    sat[CONF_UNIVERSE],
                    SATELLITE_MODES[sat[CONF_MODE]],
                    sat[CONF_LEDS],
                    sat[CONF_BRIGHTNESS],
                    sat[CONF_MAX_FPS],
                    sat[CONF_TRANSITION].total_milliseconds,
                    sat[CONF_GAMMA],
                )
            )
            for u0, u1, leds in sat.get("_segments", []):
                cg.add(var.add_satellite_segment(u0, u1, leds))
            if sat[CONF_MODE] == "room":
                # tasma lampy zamknieta w petle (wszystkie cztery sciany): rozmycie przechodzi przez jej poczatek
                closed = len({next(iter(w)) for w in sat[CONF_WALLS]}) == 4
                cg.add(var.set_satellite_blur(sat[CONF_BLUR], closed))
    if CONF_AVERAGE_COLOR in config:
        t = await text_sensor.new_text_sensor(config[CONF_AVERAGE_COLOR])
        cg.add(var.set_average_color_sensor(t))

    v = config[CONF_VIDEO]
    cg.add(var.set_video_enabled(v[CONF_ENABLED]))
    cg.add(var.set_spi_mode(v[CONF_SPI_MODE]))
    cg.add(var.set_frame_size(v[CONF_FRAME_SIZE]))
    cg.add(var.set_grid(v[CONF_GRID_WIDTH], v[CONF_GRID_HEIGHT]))
    # pixel_order opisuje kolejnosc bajtow w klatce (np. GRB = bajt0 to G);
    # komponent potrzebuje pozycji kanalow R, G, B, czyli permutacji odwrotnej
    po = ORDERS[v[CONF_PIXEL_ORDER]]
    inv = [0, 0, 0]
    for pos, chan in enumerate(po):
        inv[chan] = pos
    cg.add(var.set_pixel_order(inv[0], inv[1], inv[2]))
    cg.add(var.set_depth(v[CONF_DEPTH]))
    cg.add(var.set_smoothing(v[CONF_SMOOTHING]))
    cg.add(var.set_saturation(v[CONF_SATURATION]))
    cg.add(var.set_video_brightness(v[CONF_BRIGHTNESS]))
    cg.add(var.set_letterbox_detection(v[CONF_LETTERBOX_DETECTION]))
    cg.add(var.set_signal_timeout(v[CONF_SIGNAL_TIMEOUT]))
    cg.add(var.set_auto_off(v[CONF_AUTO_OFF]))
    cg.add(var.set_log_frames(v[CONF_LOG_FRAMES]))
    cg.add(var.set_inner_mix(v[CONF_INNER_MIX]))
    cg.add(var.set_inner_dim(v[CONF_INNER_DIM]))
    cg.add(var.set_interpolation(v[CONF_INTERPOLATION]))
    cg.add(var.set_video_levels(v[CONF_BLACK_LEVEL], v[CONF_WHITE_LEVEL]))
    wb = v[CONF_WHITE_BALANCE]
    cg.add(var.set_white_balance(wb[CONF_RED], wb[CONF_GREEN], wb[CONF_BLUE]))
    cg.add(var.set_video_gamma(v[CONF_GAMMA_CORRECT]))

    if v[CONF_ENABLED]:
        # Sterownik SPI slave z DMA jest w SDK Bekena, ale LibreTiny ma go wylaczonego.
        # To ten sam mechanizm, ktorym ESPHome wylacza BLE (custom_options.sys_config#h).
        cg.add_platformio_option(
            "custom_options.sys_config#h",
            ["CFG_USE_SPI=1", "CFG_USE_SPI_DMA_SLAVE=1"],
        )
        cg.add_define("SYNCBOX_VIDEO")

    if CONF_VIDEO_FPS in config:
        s = await sensor.new_sensor(config[CONF_VIDEO_FPS])
        cg.add(var.set_video_fps_sensor(s))
    if CONF_LED_FPS in config:
        s = await sensor.new_sensor(config[CONF_LED_FPS])
        cg.add(var.set_led_fps_sensor(s))
    if CONF_HDMI_INPUT in config:
        t = await text_sensor.new_text_sensor(config[CONF_HDMI_INPUT])
        cg.add(var.set_hdmi_input_sensor(t))
    if CONF_HDMI_DROPOUTS in config:
        s = await sensor.new_sensor(config[CONF_HDMI_DROPOUTS])
        cg.add(var.set_hdmi_dropouts_sensor(s))
    if CONF_ESP32_CONSOLE in config:
        c = config[CONF_ESP32_CONSOLE]
        cg.add_define("SYNCBOX_CONSOLE")
        cg.add(var.set_console(c[CONF_PORT], c[CONF_PASSWORD]))
    if CONF_HDMI_SIGNAL in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_HDMI_SIGNAL])
        cg.add(var.set_signal_sensor(b))


# Efekt swiatla "Synchronizacja HDMI": wlaczony = kolory z obrazu, wylaczony (inny efekt / brak) = zwykle swiatlo.
# ESPHome pamieta aktywny efekt swiatla we flashu, wiec po zaniku zasilania box wraca do synchronizacji.
@register_addressable_effect(
    "syncbox_video",
    SyncBoxVideoEffect,
    "Synchronizacja HDMI",
    {
        cv.GenerateID(CONF_SYNCBOX_ID): cv.use_id(SyncBox),
    },
)
async def syncbox_video_effect_to_code(config, effect_id):
    parent = await cg.get_variable(config[CONF_SYNCBOX_ID])
    effect = cg.new_Pvariable(effect_id, config[CONF_NAME])
    cg.add(effect.set_parent(parent))
    return effect
