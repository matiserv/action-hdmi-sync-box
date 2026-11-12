// Uklad HDMI: GSV2002 (repeater) sterowany przez wlasne MCU (HK32). ESP32 trzyma tylko reset w stanie
// nieaktywnym i slucha UART2: HK32 wysyla tam swoj log tekstowy (115200 8N1). Fabryczny firmware
// (gs2002_uart_task @ 0x400db3fc) nic nie nadaje i szuka w tekscie wylacznie napisow "HDR:...",
// z ktorych wynika tryb HDR; przy trybie innym niz SDR podbija nasycenie (sw_amb_hdr_gain).
#include "hdmi.h"
#include "pins.h"
#include "params.h"
#include "sync.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "hdmi";

volatile uint8_t g_hdr_mode = HDR_UNKNOWN;

static volatile uint32_t s_rx_bytes = 0, s_rx_zero = 0, s_rx_tokens = 0;
static char s_win[384];        // przesuwne okno ostatniego tekstu (bez zer i znakow sterujacych)
static size_t s_win_len = 0;

// ---------------------------------------------------------------------------------------------------------------
// Dziennik zdarzen: zmiany tego, co zglasza MCU ukladu HDMI, i zaniki VSync, z czasem od startu. Sluzy do ustalenia,
// co dzieje sie w chwili zaniku obrazu: zrodlo przestalo nadawac ("NO SIGNAL"), zmienil sie format albo tryb HDR
// (zrodlo przelacza tryb i lacze zestawia sie od nowa), czy po stronie wejscia nic sie nie zmienilo (wtedy przyczyna
// lezy miedzy boxem a TV). Pierscien ostatnich EV_MAX wpisow w RAM; odczyt komenda `hdmi log`.
#define EV_MAX 32
#define EV_TXT 132
typedef struct { uint32_t ms; char txt[EV_TXT]; } hdmi_event_t;
static hdmi_event_t s_ev[EV_MAX];
static volatile uint32_t s_ev_total = 0;

static void ev_add(const char *fmt, ...)
{
    hdmi_event_t *e = &s_ev[s_ev_total % EV_MAX];
    e->ms = (uint32_t) (esp_timer_get_time() / 1000);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->txt, sizeof(e->txt), fmt, ap);
    va_end(ap);
    s_ev_total = s_ev_total + 1;
}

// Komunikaty "<xx>TimingInfo: ..." (np. Rx = wejscie, Tx = wyjscie): MCU powtarza je bez przerwy, wiec do dziennika
// trafia tylko zmiana tresci. Komunikat jest brany, gdy widac juz poczatek nastepnego (czyli jest caly).
static struct { char label[3]; char text[96]; } s_ti[4];
static volatile bool s_in_active = false, s_vs_alive = false;
static volatile uint16_t s_in_w = 0, s_in_h = 0, s_dropouts = 0, s_fmt_changes = 0;
static volatile uint8_t s_in_hz = 0;
static volatile char s_in_scan = 0;

void hdmi_get_link(hdmi_link_t *l)
{
    l->in_active = s_in_active;
    l->vsync = s_vs_alive;
    l->w = s_in_w; l->h = s_in_h; l->hz = s_in_hz; l->scan = s_in_scan;
    l->dropouts = s_dropouts;
    l->fmt_changes = s_fmt_changes;
}

// Komunikat wejscia ("Rx"): "NO SIGNAL" albo "Active:3840x2160P60Hz HDR:...".
static void parse_rx(const char *txt)
{
    int w = 0, h = 0, hz = 0;
    char scan = 0;
    if (strncmp(txt, "NO SIGNAL", 9) == 0) {
        s_in_active = false;
    } else if (sscanf(txt, "Active:%dx%d%c%dHz", &w, &h, &scan, &hz) == 4) {
        if (s_in_w != 0 && (s_in_w != w || s_in_h != h || s_in_hz != hz || s_in_scan != scan)) s_fmt_changes = s_fmt_changes + 1;
        s_in_w = (uint16_t) w; s_in_h = (uint16_t) h; s_in_hz = (uint8_t) hz; s_in_scan = scan;
        s_in_active = true;
    }
}

static void track_timing(void)
{
    static const char TAG_TI[] = "TimingInfo:";
    const size_t tl = sizeof(TAG_TI) - 1;
    for (const char *p = s_win; (p = strstr(p, TAG_TI)) != NULL; p += tl) {
        if (p - s_win < 2 || !isalpha((unsigned char) p[-1]) || !isalpha((unsigned char) p[-2])) continue;
        const char *body = p + tl;
        const char *next = strstr(body, TAG_TI);
        if (next == NULL) break;                           // ostatni komunikat moze byc jeszcze niepelny
        const char *end = next - 2 > body ? next - 2 : body;    // dwie litery przed nastepnym znacznikiem naleza do niego
        while (body < end && *body == ' ') body++;
        while (end > body && end[-1] == ' ') end--;
        char txt[96];
        size_t n = (size_t) (end - body);
        if (n > sizeof(txt) - 1) n = sizeof(txt) - 1;
        memcpy(txt, body, n);
        txt[n] = 0;
        int slot = -1;
        for (int i = 0; i < 4; i++)
            if (s_ti[i].label[0] == p[-2] && s_ti[i].label[1] == p[-1]) slot = i;
        if (slot < 0) {
            for (int i = 0; i < 4 && slot < 0; i++) if (s_ti[i].label[0] == 0) slot = i;
            if (slot < 0) continue;
            s_ti[slot].label[0] = p[-2];
            s_ti[slot].label[1] = p[-1];
            s_ti[slot].label[2] = 0;
            s_ti[slot].text[0] = 0;
        }
        if (strcmp(s_ti[slot].text, txt) != 0) {
            strcpy(s_ti[slot].text, txt);
            ev_add("%s: %s", s_ti[slot].label, txt);
            if (s_ti[slot].label[0] == 'R' && s_ti[slot].label[1] == 'x') parse_rx(txt);
        }
    }
}

// Pozostale linie logu MCU (bez "TimingInfo"): do dziennika trafia linia inna niz poprzednia o tym samym poczatku,
// najwyzej 20 takich wpisow na minute (zeby komunikat z licznikiem nie wypchnal reszty z pierscienia).
static void track_line(const char *line)
{
    static char last[6][EV_TXT - 8];
    static uint32_t win_start = 0, win_cnt = 0;
    if (strstr(line, "TimingInfo") != NULL || strlen(line) < 3) return;
    int slot = -1;
    for (int i = 0; i < 6 && slot < 0; i++)
        if (last[i][0] && strncmp(last[i], line, 12) == 0) slot = i;
    if (slot >= 0 && strcmp(last[slot], line) == 0) return;
    if (slot < 0) {
        static int rr = 0;
        slot = rr;
        rr = (rr + 1) % 6;
    }
    strncpy(last[slot], line, sizeof(last[slot]) - 1);
    last[slot][sizeof(last[slot]) - 1] = 0;
    const uint32_t now = (uint32_t) (esp_timer_get_time() / 1000);
    if (now - win_start > 60000) { win_start = now; win_cnt = 0; }
    if (win_cnt++ < 20) ev_add("log: %s", last[slot]);
}

// Zanik i powrot VSync (obraz analogowy z konwertera): zanik = brak zbocza przez 150 ms.
static void track_vsync(void)
{
    static bool alive = false;
    static uint32_t lines = 0;
    const bool now_alive = g_frame_count != 0 && (esp_timer_get_time() - g_vsync_ts) < 150000;
    s_vs_alive = now_alive;
    if (now_alive != alive) {
        alive = now_alive;
        if (!alive) s_dropouts = s_dropouts + 1;
        if (alive) {
            lines = g_lines_last_frame;
            ev_add("VSYNC jest: %lu linii, %.2f Hz", (unsigned long) lines, sync_frame_rate());
        } else {
            ev_add("VSYNC brak");
        }
    } else if (alive) {
        const uint32_t l = g_lines_last_frame;
        if (l > lines + 3 || l + 3 < lines) {
            ev_add("format: %lu -> %lu linii w ramce, %.2f Hz", (unsigned long) lines, (unsigned long) l, sync_frame_rate());
            lines = l;
        }
    }
}

void hdmi_log_print(bool clear)
{
    const uint32_t total = s_ev_total;
    const uint32_t now = (uint32_t) (esp_timer_get_time() / 1000);
    const uint32_t first = total > EV_MAX ? total - EV_MAX : 0;
    printf("dziennik HDMI: %lu wpisow od startu (pokazane ostatnie %lu), teraz %lu.%03lu s od startu\n",
           (unsigned long) total, (unsigned long) (total - first), (unsigned long) (now / 1000), (unsigned long) (now % 1000));
    for (uint32_t i = first; i < total; i++) {
        const hdmi_event_t *e = &s_ev[i % EV_MAX];
        printf("  %7lu.%03lu s  %s\n", (unsigned long) (e->ms / 1000), (unsigned long) (e->ms % 1000), e->txt);
    }
    if (clear) {
        s_ev_total = 0;
        printf("dziennik wyczyszczony\n");
    }
}

const char *hdmi_hdr_name(uint8_t m)
{
    switch (m) {
        case HDR_SDR: return "SDR";
        case HDR_HDR10: return "HDR10";
        case HDR_HDR10P: return "HDR10+";
        case HDR_DOLBY: return "DolbyVision";
        case HDR_HLG: return "HLG";
        case HDR_INVALID: return "Invalid";
        default: return "nieznany";
    }
}

// Ostatnie wystapienie "HDR:" w oknie decyduje (nowszy komunikat wygrywa). Dluzsze nazwy przed krotszymi,
// bo "HDR10+" zawiera "HDR10". Niepelny napis na koncu okna czeka na reszte danych.
static void scan_hdr(void)
{
    static const struct { const char *s; uint8_t m; } tok[] = {
        {"HDR-DolbyVision", HDR_DOLBY}, {"HDR10+", HDR_HDR10P}, {"HDR10", HDR_HDR10},
        {"Invalid", HDR_INVALID}, {"HLG", HDR_HLG}, {"SDR", HDR_SDR},
    };
    const char *last = NULL;
    for (const char *p = s_win; (p = strstr(p, "HDR:")) != NULL; p += 4) last = p;
    if (last == NULL) return;
    const char *v = last + 4;
    const size_t rem = s_win_len - (size_t) (v - s_win);
    if (rem < 16) {
        // koncowka okna: napis moze byc uciety (np. "HDR:HDR1"); rozstrzygnij dopiero, gdy jest po nim separator
        bool complete = false;
        for (size_t i = 0; i < rem; i++) if (v[i] == ' ' || v[i] == ',' || v[i] == ';') complete = true;
        if (!complete) return;
    }
    for (size_t i = 0; i < sizeof(tok) / sizeof(tok[0]); i++) {
        if (strncmp(v, tok[i].s, strlen(tok[i].s)) == 0) {
            s_rx_tokens++;
            if (g_hdr_mode != tok[i].m) {
                g_hdr_mode = tok[i].m;
                ESP_LOGI(TAG, "tryb HDR z UART2: %s", hdmi_hdr_name(tok[i].m));
            }
            return;
        }
    }
}

static void hdmi_rx_task(void *arg)
{
    uint8_t buf[128];
    static char line[EV_TXT - 8];
    size_t line_len = 0;
    for (;;) {
        int n = uart_read_bytes(HDMI_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        track_vsync();
        if (n <= 0) continue;
        s_rx_bytes += (uint32_t) n;
        bool added = false;
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == 0) { s_rx_zero++; continue; }        // linia w stanie niskim daje same zera: pomijamy
            if (c == '\r' || c == '\n') {                 // koniec linii logu MCU
                if (line_len != 0) {
                    line[line_len] = 0;
                    track_line(line);
                    line_len = 0;
                }
            } else if (c >= 32 && c <= 126) {
                if (line_len < sizeof(line) - 1) line[line_len++] = (char) c;
            }
            if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            if (c < 32 || c > 126) continue;
            if (s_win_len >= sizeof(s_win) - 1) {          // przesun okno: zostaw druga polowe
                const size_t keep = sizeof(s_win) / 2;
                memmove(s_win, s_win + s_win_len - keep, keep);
                s_win_len = keep;
            }
            s_win[s_win_len++] = (char) c;
            s_win[s_win_len] = 0;
            added = true;
        }
        if (added) {
            scan_hdr();
            track_timing();
        }
    }
}

void hdmi_get_status(hdmi_status_t *st)
{
    st->rx_bytes = s_rx_bytes;
    st->rx_zero = s_rx_zero;
    st->tokens = s_rx_tokens;
    st->hdr_mode = g_hdr_mode;
    const size_t n = s_win_len;
    const size_t take = n < sizeof(st->last_text) - 1 ? n : sizeof(st->last_text) - 1;
    memcpy(st->last_text, s_win + n - take, take);
    st->last_text[take] = 0;
}

// Stosuje predkosc i polaryzacje RX z parametrow (po `set hdmi_baud` / `set hdmi_rx_inv`).
void hdmi_apply(void)
{
    uart_set_baudrate(HDMI_UART_NUM, (uint32_t) (g_params.hdmi_baud > 0 ? g_params.hdmi_baud : HDMI_UART_BAUD));
    uart_set_line_inverse(HDMI_UART_NUM, g_params.hdmi_rx_inv ? UART_SIGNAL_RXD_INV : UART_SIGNAL_INV_DISABLE);
    s_win_len = 0;
    s_win[0] = 0;
}

// Mini-oscyloskop linii RX: przez `ms` milisekund odpytuje poziom GPIO i zapisuje czasy trwania kolejnych
// stanow (us). Zwraca liczbe zapisanych odcinkow; first_level = poziom pierwszego odcinka.
int hdmi_scope(uint32_t ms, uint32_t *dur_us, int max, int *first_level)
{
    int n = 0;
    int level = gpio_get_level(PIN_HDMI_UART_RX);
    *first_level = level;
    int64_t t_prev = esp_timer_get_time();
    const int64_t t_end = t_prev + (int64_t) ms * 1000;
    while (n < max) {
        const int64_t now = esp_timer_get_time();
        if (now >= t_end) break;
        const int l = gpio_get_level(PIN_HDMI_UART_RX);
        if (l != level) {
            dur_us[n++] = (uint32_t) (now - t_prev);
            t_prev = now;
            level = l;
        }
    }
    if (n < max) dur_us[n++] = (uint32_t) (esp_timer_get_time() - t_prev);
    return n;
}

void hdmi_reset_pulse(void)
{
    gpio_set_level(PIN_HDMI_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_HDMI_RESET, 1);
    ESP_LOGI(TAG, "impuls resetu GSV2002 wyslany");
}

void hdmi_init(void)
{
    // Reset ukladu HDMI: aktywny LOW. Domyslnie tylko trzymamy go w stanie nieaktywnym (tak robi fabryczny
    // firmware przy hdmi_firmware_type = 0), zeby nie przerywac przelotu HDMI.
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_HDMI_RESET,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_set_level(PIN_HDMI_RESET, 1);      // poziom przed wlaczeniem wyjscia: bez krotkiego stanu niskiego po pelnym resecie
    gpio_config(&io);
    gpio_set_level(PIN_HDMI_RESET, 1);
    if (g_params.hdmi_reset_pulse) hdmi_reset_pulse();

    uart_config_t cfg = {
        .baud_rate = HDMI_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(HDMI_UART_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HDMI_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(HDMI_UART_NUM, PIN_HDMI_UART_TX, PIN_HDMI_UART_RX, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    gpio_set_pull_mode(PIN_HDMI_UART_RX, GPIO_PULLUP_ONLY);      // bez podciagania wolna linia czyta sie jako zera
    hdmi_apply();
    xTaskCreatePinnedToCore(hdmi_rx_task, "hdmi_rx", 3072, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "UART2 %d bps, RX=GPIO%d%s (tylko odbior: log tekstowy MCU ukladu HDMI), reset GPIO%d = 1",
             HDMI_UART_BAUD, PIN_HDMI_UART_RX, g_params.hdmi_rx_inv ? " odwrocony" : "", PIN_HDMI_RESET);
}

void hdmi_uart_send(const uint8_t *data, size_t len)
{
    uart_write_bytes(HDMI_UART_NUM, data, len);
}
