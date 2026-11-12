#include "spi_out.h"
#include "pins.h"
#include "params.h"
#include "link.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "spi";

static spi_device_handle_t s_dev = NULL;
static uint8_t *s_buf[2];            // bufory DMA
static volatile int s_pending = -1;  // indeks bufora czekajacego na wysylke
static volatile int s_busy = -1;     // indeks bufora wlasnie wysylanego (-1 = zaden)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_sem;      // "jest nowa ramka"
static volatile uint32_t s_sent = 0;
static volatile float s_fps = 0;
static volatile int64_t s_last_tx_us = 0;   // koniec ostatniej wyslanej ramki
static volatile bool s_reconf = false;
static const uint32_t MIN_GAP_US = 600;  // odbiornik BK potrzebuje chwili na ponowne uzbrojenie DMA
static uint8_t *s_rx;                // poczatek danych z BK (MISO) z ostatniej ramki
static uint8_t *s_idle;              // ramka bez obrazu (ten sam bufor co wzor testowy: nie wystepuja razem)
static volatile int s_in_delay_ns = 0;   // kompensacja opoznienia MISO (komenda `link dly`)

static void dev_add(void)
{
    // Zakres, ktory przyjmuje sterownik przy tych pinach (matryca GPIO) i w ktorym ramka miesci sie w okresie obrazu
    // w rozsadnym czasie. Wartosc spoza zakresu (literowka w `set spi_hz`, np. "8M" = 8) konczyla sie abortem,
    // a zapisana - petla restartow przed startem konsoli.
    uint32_t hz = g_params.spi_hz;
    if (hz < 1000000 || hz > 20000000) {
        ESP_LOGW(TAG, "spi_hz %lu poza zakresem 1..20 MHz, przyjmuje 8 MHz", (unsigned long) hz);
        hz = 8000000;
        g_params.spi_hz = hz;
    }
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = (int) hz,
        .mode = 0,
        .spics_io_num = PIN_SPI_CS,
        .queue_size = 2,
        .cs_ena_pretrans = 2,   // CS w dol troche przed zegarem
        .cs_ena_posttrans = 2,
        .input_delay_ns = s_in_delay_ns,
    };
    if (spi_bus_add_device(SPI2_HOST, &devcfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "sterownik SPI odrzucil %lu Hz, przyjmuje 8 MHz", (unsigned long) hz);
        g_params.spi_hz = 8000000;
        devcfg.clock_speed_hz = 8000000;
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_dev));
    }
    ESP_LOGI(TAG, "SPI master %lu Hz, SCLK=%d CS=%d MOSI=%d MISO=%d", (unsigned long)g_params.spi_hz,
             PIN_SPI_SCLK, PIN_SPI_CS, PIN_SPI_MOSI, PIN_SPI_MISO);
}

// Ramka na lacze: naglowek z ukladem pola, dane pola, reszta zera.
static void wire_frame(uint8_t *dst, const uint8_t *pix)
{
    static uint8_t seq = 0;
    const uint8_t w = (uint8_t) GRID_W, h = (uint8_t) GRID_H;
    const size_t n = (size_t) w * h * 3;
    uint32_t sum = 0;                                   // suma bajtow pola (16 bitow), bajt 11 = 1: suma obecna
    for (size_t i = 0; i < n; i++) sum += pix[i];
    const uint8_t hdr[WIRE_HDR] = {'S', 'B', 'G', '2', w, h, (uint8_t) ~w, (uint8_t) ~h, seq++, (uint8_t) sum, (uint8_t) (sum >> 8), 1,
                                   0x5A, 0xA5, 0xC3, 0x3C};
    memcpy(dst, hdr, WIRE_HDR);
    memcpy(dst + WIRE_HDR, pix, n);
}

static void make_pattern(uint8_t *f, uint32_t t)
{
    if (g_params.pattern == 1) {               // tecza przesuwajaca sie w czasie
        for (int y = 0; y < GRID_H; y++)
            for (int x = 0; x < GRID_W; x++) {
                float h = fmodf((x + y) * 16.0f + t * 4.0f, 256.0f) / 256.0f * 6.0f;
                int i = (int)h; float fr = h - i;
                uint8_t q = (uint8_t)(255 * (1 - fr)), tt = (uint8_t)(255 * fr);
                uint8_t r, g, b;
                switch (i % 6) {
                    case 0: r = 255; g = tt; b = 0; break;
                    case 1: r = q; g = 255; b = 0; break;
                    case 2: r = 0; g = 255; b = tt; break;
                    case 3: r = 0; g = q; b = 255; break;
                    case 4: r = tt; g = 0; b = 255; break;
                    default: r = 255; g = 0; b = q; break;
                }
                uint8_t *p = f + (y * GRID_W + x) * 3;
                p[0] = r; p[1] = g; p[2] = b;
            }
    } else {                                    // 2: lewy bok czerwony, gora zielona, prawy bok niebieski, dol bialy
        memset(f, 0, PIX_BYTES_MAX);
        for (int y = 0; y < GRID_H; y++)
            for (int x = 0; x < GRID_W; x++) {
                uint8_t *p = f + (y * GRID_W + x) * 3;
                if (x < 3) { p[0] = 255; }
                else if (y < 3) { p[1] = 255; }
                else if (x >= GRID_W - 3) { p[2] = 255; }
                else if (y >= GRID_H - 3) { p[0] = p[1] = p[2] = 200; }
            }
    }
}

static void spi_task(void *arg)
{
    int64_t last_end = 0, win_start = esp_timer_get_time();
    uint32_t win_frames = 0, tick = 0;
    uint8_t *pat = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_DMA);
    s_idle = pat;
    uint8_t *pat_pix = NULL;                    // piksele wzoru testowego: ze sterty przy pierwszym uzyciu
    for (;;) {
        const uint8_t *src = NULL;
        if (g_params.pattern != 0) {
            vTaskDelay(pdMS_TO_TICKS(16));
            if (pat_pix == NULL) pat_pix = malloc(PIX_BYTES_MAX);
            if (pat_pix == NULL) continue;
            make_pattern(pat_pix, tick++);
            wire_frame(pat, pat_pix);
            src = pat;
        } else {
            // Bez nowej ramki obrazu (brak sygnalu wideo) idzie ramka bez pola: sam naglowek i blok pomocniczy. Niesie
            // stan czesci HDMI i tunel konsoli; przy podlaczonej konsoli co 16 ms, inaczej 5 razy na sekunde.
            int idx = -1;
            if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(link_attached() ? 16 : 200)) == pdTRUE) {
                portENTER_CRITICAL(&s_mux);
                idx = s_pending;
                s_pending = -1;
                s_busy = idx;             // od teraz spi_out_submit() nie pisze do tego bufora
                portEXIT_CRITICAL(&s_mux);
            }
            if (idx >= 0) {
                src = s_buf[idx];
            } else {
                static uint8_t iseq = 0;
                const uint8_t hdr[WIRE_HDR] = {'S', 'B', 'G', '2', 0, 0, 0xFF, 0xFF, iseq++, 0, 0, 1, 0x5A, 0xA5, 0xC3, 0x3C};
                memset(s_idle, 0, IDLE_BYTES);
                memcpy(s_idle, hdr, WIRE_HDR);
                src = s_idle;
            }
        }
        if (s_reconf) {
            s_reconf = false;
            spi_bus_remove_device(s_dev);
            dev_add();
        }
        int64_t now = esp_timer_get_time();
        if (now - last_end < MIN_GAP_US) {
            esp_rom_delay_us((uint32_t)(MIN_GAP_US - (now - last_end)));
        }
        // naglowek + pole (uklad z naglowka ramki) + blok pomocniczy; w tej samej transmisji po MISO przychodzi blok z BK
        const size_t pix = (size_t) src[4] * src[5] * 3;
        const bool idle = pix == 0;
        const size_t total = idle ? IDLE_BYTES : WIRE_HDR + pix + AUX_BYTES;
        link_build_aux((uint8_t *) src + WIRE_HDR + pix);
        const size_t rxn = total < LINK_DN_RX ? total : LINK_DN_RX;
        spi_transaction_t t = {
            .length = total * 8,
            .rxlength = rxn * 8,
            .tx_buffer = src,
            .rx_buffer = s_rx,
        };
        esp_err_t e = spi_device_transmit(s_dev, &t);   // CS w dol na cala ramke
        last_end = esp_timer_get_time();
        if (!idle) s_last_tx_us = last_end;
        s_busy = -1;
        if (e == ESP_OK) {
            link_parse_downlink(s_rx, rxn);
            if (!idle) {
                s_sent++;
                win_frames++;
            }
        } else {
            ESP_LOGW(TAG, "transmit: %s", esp_err_to_name(e));
        }
        if (last_end - win_start >= 1000000) {
            s_fps = win_frames * 1000000.0f / (float)(last_end - win_start);
            win_frames = 0;
            win_start = last_end;
        }
    }
}

void spi_out_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    dev_add();
    s_buf[0] = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_DMA);
    s_buf[1] = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_DMA);
    s_rx = heap_caps_calloc(1, (LINK_DN_RX + 3) & ~3u, MALLOC_CAP_DMA);
    s_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(spi_task, "spi_out", 4096, NULL, 10, NULL, 0);   // rdzen 0; rdzen 1 zostaje dla przerwania HSync
}

void spi_out_submit(const uint8_t *frame)
{
    // Dwa bufory: jeden moze byc wlasnie wysylany (s_busy), drugi jest wolny albo czeka z ramka, ktorej watek SPI jeszcze
    // nie pobral. Piszemy zawsze do tego, ktory nie jest wysylany; czekajaca w nim ramke wycofujemy na czas zapisu, zeby
    // watek SPI (wyzszy priorytet) nie pobral bufora w polowie kopiowania. Dziala takze wtedy, gdy wysylka trwa dluzej
    // niz okres ramki (male spi_hz) i w trakcie jednej transmisji przychodza dwie ramki.
    portENTER_CRITICAL(&s_mux);
    const int f = s_busy == 0 ? 1 : 0;
    if (s_pending == f) s_pending = -1;
    portEXIT_CRITICAL(&s_mux);
    wire_frame(s_buf[f], frame);
    portENTER_CRITICAL(&s_mux);
    s_pending = f;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_sem);
}

uint32_t spi_out_frames_sent(void) { return s_sent; }
float spi_out_fps(void) { return esp_timer_get_time() - s_last_tx_us > 1500000 ? 0.0f : s_fps; }   // 0, gdy nic nie idzie
void spi_out_reconfigure(void) { s_reconf = true; }
void spi_out_set_input_delay(int ns) { s_in_delay_ns = ns; s_reconf = true; }
int spi_out_get_input_delay(void) { return s_in_delay_ns; }
