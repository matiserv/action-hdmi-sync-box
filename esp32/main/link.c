// Lacze z modulem Wi-Fi poza obrazem: blok pomocniczy w ramce SPI, tunel konsoli, firmware na probie. Opis w link.h.
#include "link.h"
#include "hdmi.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "nvs.h"

static const char *TAG = "link";

// wyjscie konsoli czekajace na wyslanie do BK: jeden piszacy (zadanie drukujace, pod blokada stdout), jeden czytajacy
// (zadanie SPI); liczniki rosna bez konca, indeks = licznik % rozmiar
#define OUT_RING 4096
static uint8_t *s_out = NULL;            // ze sterty: w segmencie danych statycznych nie ma na to miejsca
static volatile uint32_t s_out_w = 0, s_out_r = 0;

static uint8_t s_up_cur[LINK_UP_PAY];      // porcja w gore czekajaca na potwierdzenie
static uint16_t s_up_len = 0;
static uint8_t s_up_seq = 0, s_dn_last = 0;
static volatile int64_t s_attach_us = -10000000;
static volatile bool s_reset_ack = false;
static volatile bool s_need_reset = true;       // od startu do pierwszego zerowania tunelu przez BK
static volatile uint8_t s_trial = 0;

static StreamBufferHandle_t s_in;
static SemaphoreHandle_t s_in_mux;
static volatile bool s_last_tunnel = false;
static TaskHandle_t s_spi_task = NULL;

static link_stats_t s_st;

uint16_t link_crc16(const uint8_t *d, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t) (*d++) << 8;
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t) ((c << 1) ^ 0x1021) : (uint16_t) (c << 1);
    }
    return c;
}

bool link_attached(void) { return esp_timer_get_time() - s_attach_us < 2000000; }

void link_out_write(const char *data, size_t n)
{
    if (s_out == NULL || !link_attached()) return;
    // zadanie SPI samo oproznia ten bufor, wiec nie moze czekac na miejsce w nim
    const bool can_wait = xTaskGetCurrentTaskHandle() != s_spi_task;
    int waited = 0;
    size_t i = 0;
    while (i < n) {
        if ((uint32_t) (s_out_w - s_out_r) >= OUT_RING) {
            if (can_wait && waited < 500 && link_attached()) {
                vTaskDelay(1);
                waited++;
                continue;
            }
            s_st.out_dropped += (uint32_t) (n - i);
            return;
        }
        s_out[s_out_w % OUT_RING] = (uint8_t) data[i++];
        s_out_w = s_out_w + 1;
    }
}

// Calosc albo nic: miejsce jest sprawdzane i dane wpisywane pod jednym zamkiem. Pisza tu dwa zadania (odbior z UART0
// i watek SPI z danymi z tunelu); przy sprawdzeniu poza zamkiem drugie moglo zajac miejsce w miedzyczasie, wpis
// wchodzil w czesci, a porcja z tunelu byla mimo to potwierdzana.
bool link_in_push(const uint8_t *data, size_t n, bool from_tunnel)
{
    bool ok = false;
    xSemaphoreTake(s_in_mux, portMAX_DELAY);
    if (xStreamBufferSpacesAvailable(s_in) >= n) {
        xStreamBufferSend(s_in, data, n, 0);
        s_last_tunnel = from_tunnel;
        ok = true;
    }
    xSemaphoreGive(s_in_mux);
    return ok;
}

size_t link_in_read(uint8_t *buf, size_t max, TickType_t timeout) { return xStreamBufferReceive(s_in, buf, max, timeout); }
size_t link_in_space(void) { return xStreamBufferSpacesAvailable(s_in); }
bool link_last_input_from_tunnel(void) { return s_last_tunnel; }

void link_parse_downlink(const uint8_t *rx, size_t n)
{
    if (s_spi_task == NULL) s_spi_task = xTaskGetCurrentTaskHandle();
    memcpy(s_st.raw, rx, n < sizeof(s_st.raw) ? n : sizeof(s_st.raw));
    const uint8_t *p = NULL;
    for (size_t i = 0; i + 11 <= n && i < 32; i++) {
        if (rx[i] == 'S' && rx[i + 1] == 'B' && rx[i + 2] == 'D' && rx[i + 3] == '1') { p = rx + i; break; }
    }
    if (p == NULL) { s_st.dn_none++; return; }
    const uint8_t flags = p[4], seq = p[5], up_ack = p[6];
    const uint16_t len = (uint16_t) (p[7] | (p[8] << 8));
    if (len > LINK_DN_MAX || (size_t) (p - rx) + 9 + len + 2 > n) { s_st.dn_bad++; return; }
    const uint16_t crc = (uint16_t) ((p[9 + len] << 8) | p[10 + len]);
    if (link_crc16(p, 9 + (size_t) len) != crc) { s_st.dn_bad++; return; }
    s_st.dn_ok++;
    if (flags & 1) s_attach_us = esp_timer_get_time();
    if (flags & 2) {                       // zerowanie tunelu: nowy klient konsoli po stronie BK
        s_out_r = s_out_w;
        s_up_len = 0;
        s_up_seq = 0;
        s_dn_last = 0;
        s_reset_ack = true;
        s_need_reset = false;
        return;
    }
    s_reset_ack = false;
    if (s_need_reset) return;                       // po restarcie: najpierw zerowanie numeracji po obu stronach
    if (s_up_len != 0 && up_ack == s_up_seq) {      // nasza porcja doszla
        s_up_len = 0;
        s_st.up_chunks++;
    }
    if (len != 0 && seq != s_dn_last && link_in_push(p + 9, len, true)) {   // brak miejsca: BK powtorzy porcje
        s_dn_last = seq;
        s_st.dn_chunks++;
    }
}

void link_build_aux(uint8_t *aux)
{
    if (s_spi_task == NULL) s_spi_task = xTaskGetCurrentTaskHandle();
    const bool att = link_attached();
    if (s_up_len == 0 && att && s_out != NULL && !s_need_reset) {
        uint32_t avail = s_out_w - s_out_r;
        if (avail > LINK_UP_PAY) avail = LINK_UP_PAY;
        if (avail != 0) {
            for (uint32_t i = 0; i < avail; i++) s_up_cur[i] = s_out[(s_out_r + i) % OUT_RING];
            s_out_r = s_out_r + avail;
            s_up_len = (uint16_t) avail;
            s_up_seq = s_up_seq == 255 ? 1 : (uint8_t) (s_up_seq + 1);
        }
    }
    hdmi_link_t h;
    hdmi_get_link(&h);
    memset(aux, 0, AUX_BYTES);
    aux[0] = 'S'; aux[1] = 'B'; aux[2] = 'X'; aux[3] = '1';
    aux[4] = (uint8_t) ((h.in_active ? 1 : 0) | (h.vsync ? 2 : 0) | (att ? 4 : 0) | (s_trial == 2 ? 8 : 0) | (s_reset_ack ? 16 : 0) | (s_need_reset ? 32 : 0));
    aux[5] = g_hdr_mode;
    aux[6] = (uint8_t) h.w; aux[7] = (uint8_t) (h.w >> 8);
    aux[8] = (uint8_t) h.h; aux[9] = (uint8_t) (h.h >> 8);
    aux[10] = h.hz;
    aux[11] = (uint8_t) h.scan;
    aux[12] = (uint8_t) h.dropouts; aux[13] = (uint8_t) (h.dropouts >> 8);
    aux[14] = (uint8_t) h.fmt_changes; aux[15] = (uint8_t) (h.fmt_changes >> 8);
    const uint32_t up = (uint32_t) (esp_timer_get_time() / 1000000);
    aux[16] = (uint8_t) up; aux[17] = (uint8_t) (up >> 8); aux[18] = (uint8_t) (up >> 16); aux[19] = (uint8_t) (up >> 24);
    aux[20] = LINK_FW_VERSION;
    aux[21] = s_up_seq;
    aux[22] = s_dn_last;
    aux[24] = (uint8_t) s_up_len; aux[25] = (uint8_t) (s_up_len >> 8);
    memcpy(aux + LINK_UP_HDR, s_up_cur, s_up_len);
    const uint16_t crc = link_crc16(aux, AUX_BYTES - 2);
    aux[AUX_BYTES - 2] = (uint8_t) (crc >> 8);
    aux[AUX_BYTES - 1] = (uint8_t) crc;
}

void link_get_stats(link_stats_t *s)
{
    *s = s_st;
    s->attached = link_attached();
}

// ---------------------------------------------------------------------------------------------------------------
// Firmware na probie. Po wgraniu przez tunel nie ma pewnosci, ze nowy obraz w ogole dogada sie z BK, a wtedy nie daloby
// sie go poprawic bez kabla. Dlatego: przed restartem zapisujemy "trial = 1"; nowy obraz przy starcie zmienia to na 2
// i czeka na poprawne bloki z BK (klient konsoli, ktory wgrywal, jest dalej podlaczony). Jesli przyjda - trial = 0.
// Jesli nie przyjda w 120 s albo uklad zrestartuje sie wczesniej (przy starcie zastajemy 2) - wraca poprzednia partycja.
static void trial_write(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("syncbox", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "trial", v);
    nvs_commit(h);
    nvs_close(h);
}

static void trial_revert(const char *why)
{
    const esp_partition_t *prev = esp_ota_get_next_update_partition(NULL);     // przy dwoch partycjach OTA: ta druga
    ESP_LOGE(TAG, "firmware na probie odrzucony (%s): powrot do partycji %s", why, prev ? prev->label : "?");
    trial_write(0);
    if (prev != NULL && esp_ota_set_boot_partition(prev) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
}

static void trial_task(void *arg)
{
    for (int i = 0; i < 1200; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_st.dn_ok >= 20) {
            trial_write(0);
            s_trial = 0;
            ESP_LOGI(TAG, "firmware na probie potwierdzony (lacze z BK dziala)");
            vTaskDelete(NULL);
        }
    }
    trial_revert("brak lacza z BK przez 120 s");
    vTaskDelete(NULL);
}

void link_trial_arm(void) { trial_write(1); }
uint8_t link_trial_state(void) { return s_trial; }

void link_trial_boot(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("syncbox", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "trial", &v);
        nvs_close(h);
    }
    if (v == 1) {
        trial_write(2);
        s_trial = 2;
        ESP_LOGW(TAG, "firmware na probie: czekam do 120 s na lacze z BK");
        xTaskCreatePinnedToCore(trial_task, "trial", 3072, NULL, 2, NULL, 0);
    } else if (v == 2) {
        trial_revert("restart przed potwierdzeniem");
    }
}

void link_init(void)
{
    s_out = malloc(OUT_RING);
    s_in = xStreamBufferCreate(2560, 1);
    s_in_mux = xSemaphoreCreateMutex();
    if (s_out == NULL || s_in == NULL || s_in_mux == NULL) {     // konsola (takze po UART) dziala tylko z tymi buforami
        ESP_LOGE(TAG, "brak pamieci na bufory konsoli (wolne %u B)", (unsigned) esp_get_free_heap_size());
        abort();
    }
}
