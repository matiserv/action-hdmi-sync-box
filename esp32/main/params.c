#include "params.h"
#include "pins.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "capture.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "params";
params_t g_params;

void params_defaults(void)
{
    g_params = (params_t){
        .spi_hz = 8000000,
        .adc_hz = 1500000,
        .black_mv = 584,
        .white_mv = 1408,
        .black2_mv = 398,
        .white2_mv = 991,
        .hoff_us10 = 0,
        .hblank_pct10 = 127,
        .vstart = 0,
        .vlines = 0,
        .cut = 10,
        .gain_pct = 100,
        .gamma = 0,
        .flip_h = 0,
        .flip_v = 0,
        .pattern = 0,
        .vsync_neg = 1,
        .hsync_neg = 1,
        .hdmi_reset_pulse = 0,
        .rep = 2,
        .sar_div = 1,
        .sar_cycle = 2,
        .rtc_xtal = 1,
        .hdmi_rx_inv = 0,
        .hdmi_baud = 9600,
        .adc_mode = 2,
        .dig_step = 3,
        .dig_block = 4,
        .dig_atten = 1,
        .dig_atten2 = 2,
        .dig_clkdiv = 2,
        .dig_standby = 0,
        .dig_phases = 9,
        .dig_prof_delay_us10 = 0,
        .dig_guard_us10 = 2,
        .dig_d_ns = 686,
        .dig_dt_ps = 0,
        .dig_span_ns = 0,
        .dig_pre_us10 = 16,
        .smooth_pct = 4,
        .hold_x10 = 10,
        .blur = 5,
        .blur_dir = 1,
        .blur_in = 80,
        .lbox = 1,
        .lbox_thr = 12,
        .smooth_t1 = 10,
        .smooth_tc = 25,
        .smooth_t2 = 32,
        .smooth_nz = 30,
        .dig_limit = 255,
        .hdr_mode = 0,
        .hdr_gain_pct = 200,
        .sat_pct = 100,
        .mat = {1000, 0, 0, 0, 1000, 0, 0, 0, 1000},
        .lat_us10 = 41,
        .hdr_conv = 1,
        .hdr_white = 203,
        .grid_w = 48,
        .grid_h = 27,
    };
}

uint8_t g_grid_w = GRID_W_MAX, g_grid_h = GRID_H_MAX;

// Klucz zapisu zawiera wersje ukladu struktury: po zmianie pol stary zapis jest pomijany (inaczej pola
// o tym samym rozmiarze struktury wczytalyby sie przesuniete). Zwieksz przy kazdej zmianie params_t.
// "p21" = uklad "p20" + pola dopisane na koncu (hdr_conv, hdr_white). Zapis "p20" z wczesniejszego firmware jest
// wczytywany jako poczatek struktury, a nowe pola zostaja domyslne - kalibracja przechodzi przez aktualizacje.
// Stary klucz nie jest kasowany, wiec powrot do poprzedniego firmware tez znajdzie swoje parametry.
#define PARAMS_KEY "p21"
#define PARAMS_KEY_OLD "p20"
#define PARAMS_OLD_SIZE offsetof(params_t, hdr_conv)

void params_load(void)
{
    params_defaults();
    nvs_handle_t h;
    if (nvs_open("syncbox", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "brak zapisanych parametrow, uzywam domyslnych");
        return;
    }
    size_t len = sizeof(params_t);
    params_t tmp;
    if (nvs_get_blob(h, PARAMS_KEY, &tmp, &len) == ESP_OK && len == sizeof(params_t)) {
        g_params = tmp;
        ESP_LOGI(TAG, "parametry wczytane z NVS");
    } else {
        len = sizeof(params_t);
        if (nvs_get_blob(h, PARAMS_KEY_OLD, &tmp, &len) == ESP_OK && len == PARAMS_OLD_SIZE) {
            memcpy(&g_params, &tmp, PARAMS_OLD_SIZE);
            ESP_LOGI(TAG, "parametry wczytane z NVS (zapis sprzed v14.5; nowe pola domyslne)");
        }
    }
    nvs_close(h);
}

void params_save(void)
{
    capture_pause();
    vTaskDelay(pdMS_TO_TICKS(50));
    nvs_handle_t h;
    if (nvs_open("syncbox", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open nieudane");
        capture_resume();
        return;
    }
    nvs_set_blob(h, PARAMS_KEY, &g_params, sizeof(params_t));
    nvs_commit(h);
    nvs_close(h);
    capture_resume();
    ESP_LOGI(TAG, "parametry zapisane");
}

typedef struct { const char *name; void *ptr; char type; } entry_t;  // type: 'u' uint32, 'i' int32, 'b' uint8

static const entry_t table[] = {
    {"spi_hz", &g_params.spi_hz, 'u'},
    {"black_mv", &g_params.black_mv, 'i'},
    {"white_mv", &g_params.white_mv, 'i'},
    {"hoff", &g_params.hoff_us10, 'i'},
    {"hblank", &g_params.hblank_pct10, 'i'},
    {"vstart", &g_params.vstart, 'i'},
    {"vlines", &g_params.vlines, 'i'},
    {"cut", &g_params.cut, 'i'},
    {"gain", &g_params.gain_pct, 'i'},
    {"gamma", &g_params.gamma, 'b'},
    {"flip_h", &g_params.flip_h, 'b'},
    {"flip_v", &g_params.flip_v, 'b'},
    {"pattern", &g_params.pattern, 'b'},
    {"vsync_neg", &g_params.vsync_neg, 'b'},
    {"hsync_neg", &g_params.hsync_neg, 'b'},
    {"hdmi_reset_pulse", &g_params.hdmi_reset_pulse, 'b'},
    {"rep", &g_params.rep, 'b'},
    {"sar_div", &g_params.sar_div, 'b'},
    {"sar_cycle", &g_params.sar_cycle, 'b'},
    {"lat", &g_params.lat_us10, 'i'},
    {"grid_w", &g_params.grid_w, 'b'},
    {"grid_h", &g_params.grid_h, 'b'},
    {"rtc_xtal", &g_params.rtc_xtal, 'b'},
    {"hdmi_rx_inv", &g_params.hdmi_rx_inv, 'b'},
    {"hdmi_baud", &g_params.hdmi_baud, 'i'},
    {"adc_mode", &g_params.adc_mode, 'b'},
    {"dig_step", &g_params.dig_step, 'b'},
    {"dig_block", &g_params.dig_block, 'b'},
    {"dig_atten", &g_params.dig_atten, 'b'},
    {"dig_atten2", &g_params.dig_atten2, 'b'},
    {"black2_mv", &g_params.black2_mv, 'i'},
    {"white2_mv", &g_params.white2_mv, 'i'},
    {"dig_clkdiv", &g_params.dig_clkdiv, 'b'},
    {"dig_standby", &g_params.dig_standby, 'b'},
    {"dig_d", &g_params.dig_d_ns, 'i'},
    {"dig_limit", &g_params.dig_limit, 'b'},
    {"dig_dt", &g_params.dig_dt_ps, 'i'},
    {"dig_span", &g_params.dig_span_ns, 'i'},
    {"dig_pre", &g_params.dig_pre_us10, 'i'},
    {"smooth", &g_params.smooth_pct, 'i'},
    {"hold", &g_params.hold_x10, 'i'},
    {"blur", &g_params.blur, 'b'},
    {"blur_dir", &g_params.blur_dir, 'b'},
    {"blur_in", &g_params.blur_in, 'b'},
    {"lbox", &g_params.lbox, 'b'},
    {"lbox_thr", &g_params.lbox_thr, 'b'},
    {"smooth_t1", &g_params.smooth_t1, 'i'},
    {"smooth_tc", &g_params.smooth_tc, 'i'},
    {"smooth_t2", &g_params.smooth_t2, 'i'},
    {"smooth_nz", &g_params.smooth_nz, 'i'},
    {"dig_guard", &g_params.dig_guard_us10, 'i'},
    {"dig_phases", &g_params.dig_phases, 'b'},
    {"dig_prof_delay", &g_params.dig_prof_delay_us10, 'i'},
    {"hdr_mode", &g_params.hdr_mode, 'b'},
    {"hdr_gain", &g_params.hdr_gain_pct, 'i'},
    {"hdr_conv", &g_params.hdr_conv, 'i'},
    {"hdr_white", &g_params.hdr_white, 'i'},
    {"sat", &g_params.sat_pct, 'i'},
    {"m_rr", &g_params.mat[0], 'i'},
    {"m_rg", &g_params.mat[1], 'i'},
    {"m_rb", &g_params.mat[2], 'i'},
    {"m_gr", &g_params.mat[3], 'i'},
    {"m_gg", &g_params.mat[4], 'i'},
    {"m_gb", &g_params.mat[5], 'i'},
    {"m_br", &g_params.mat[6], 'i'},
    {"m_bg", &g_params.mat[7], 'i'},
    {"m_bb", &g_params.mat[8], 'i'},
};

bool params_set(const char *name, const char *value)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(table[i].name, name) != 0) continue;
        long v = strtol(value, NULL, 0);
        if (table[i].ptr == (void *) &g_params.spi_hz && (v < 1000000 || v > 20000000)) return true;   // konsola odrzuca wczesniej
        switch (table[i].type) {
            case 'u': *(uint32_t *)table[i].ptr = (uint32_t)v; break;
            case 'i': *(int32_t *)table[i].ptr = (int32_t)v; break;
            default:  *(uint8_t *)table[i].ptr = (uint8_t)v; break;
        }
        return true;
    }
    return false;
}

void params_print(void)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        long v;
        switch (table[i].type) {
            case 'u': v = *(uint32_t *)table[i].ptr; break;
            case 'i': v = *(int32_t *)table[i].ptr; break;
            default:  v = *(uint8_t *)table[i].ptr; break;
        }
        printf("  %-18s = %ld\n", table[i].name, v);
    }
}
