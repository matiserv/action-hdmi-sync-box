// Uklad HDMI: GSV2002 (repeater) sterowany przez wlasne MCU (HK32). ESP32 trzyma tylko reset w stanie
// nieaktywnym i slucha UART2: HK32 wysyla tam swoj log tekstowy (115200 8N1). Fabryczny firmware
// (gs2002_uart_task @ 0x400db3fc) nic nie nadaje i szuka w tekscie wylacznie napisow "HDR:...",
// z ktorych wynika tryb HDR; przy trybie innym niz SDR podbija nasycenie (sw_amb_hdr_gain).
#include "hdmi.h"
#include "pins.h"
#include "params.h"
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
    for (;;) {
        int n = uart_read_bytes(HDMI_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        s_rx_bytes += (uint32_t) n;
        bool added = false;
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == 0) { s_rx_zero++; continue; }        // linia w stanie niskim daje same zera: pomijamy
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
        if (added) scan_hdr();
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
