#include "sync.h"
#include "pins.h"
#include "params.h"
#include "capture.h"
#include "driver/gpio.h"
#include "driver/mcpwm_cap.h"
#include "driver/pulse_cnt.h"
#include "soc/pcnt_struct.h"
#include "soc/mcpwm_struct.h"
#include "esp_intr_alloc.h"
#include "soc/gpio_struct.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "sync";

volatile uint32_t g_abs_line = 0;
volatile uint32_t g_frame_first_line = 0;
volatile uint32_t g_frame_count = 0;
volatile int64_t  g_vsync_ts = 0;
volatile uint32_t g_lines_last_frame = 0;
volatile int64_t  g_frame_period_us = 0;

static TaskHandle_t s_notify = NULL;
static intr_handle_t s_intr = NULL;
static uint32_t s_cap_to_cpu = 3;          // cykle CPU na tick licznika capture (240 MHz / 80 MHz)
static bool s_cap_ok = false;
static bool s_pcnt_ok = false;             // linie liczy sprzetowy licznik impulsow (nie gubi zboczy)
static int32_t s_pcnt_prev = 0;
#define PCNT_LIMIT 32767
volatile uint32_t g_isr_late_max_cyc = 0;   // najwieksze zmierzone opoznienie wejscia w przerwanie

// Jedno przerwanie dla obu pinow (GPIO32..39 -> rejestr status1). Bez sterownika gpio_isr_service:
// mniejsze i stalsze opoznienie, bo czas wejscia w przerwanie jest punktem odniesienia dla probkowania.
// VSync obslugiwany przed HSync: w CEA-861 oba zbocza wypadaja w tej samej chwili i ta linia jest linia 0 ramki.
static void IRAM_ATTR gpio_isr(void *arg)
{
    uint32_t c0 = esp_cpu_get_cycle_count();
    const uint32_t st = GPIO.status1.intr_st;
    GPIO.status1_w1tc.intr_st = st;
    if (s_cap_ok && (st & (1u << (PIN_HSYNC - 32)))) {
        // Sekcje krytyczne FreeRTOS na tym rdzeniu blokuja przerwania do poziomu 3, wiec wejscie bywa
        // spoznione o kilka us. Prawdziwy czas zbocza daje sprzetowy capture (kanal 0), a "teraz" w tym
        // samym liczniku - capture programowy (kanal 1). Roznica x3 = cykle CPU od zbocza.
        const uint32_t edge = MCPWM0.cap_chn[0].capn_value;
        MCPWM0.cap_chn_cfg[1].capn_sw = 1;
        const uint32_t now_cap = MCPWM0.cap_chn[1].capn_value;
        const uint32_t late = (now_cap - edge) * s_cap_to_cpu;
        if (late < 240 * 200) {            // < 200 us: sensowne; inaczej zostaw c0
            c0 = esp_cpu_get_cycle_count() - late;
            if (late > g_isr_late_max_cyc) g_isr_late_max_cyc = late;
        }
    }
    const bool hs = (st & (1u << (PIN_HSYNC - 32))) != 0;
    if (s_pcnt_ok) {
        // Sprzetowy licznik zboczy HSync: dlugie przerwanie probkujace moze przespac kilka zboczy,
        // a licznik i tak je zliczy. Licznik zeruje sie po dojsciu do PCNT_LIMIT.
        const int32_t cur = (int16_t) PCNT.cnt_unit[0].cnt_val;
        int32_t d = cur - s_pcnt_prev;
        if (d < 0) d += PCNT_LIMIT;
        s_pcnt_prev = cur;
        g_abs_line += (uint32_t) d;
    } else if (hs) {
        g_abs_line++;
    }
    if (st & (1u << (PIN_VSYNC - 32))) {
        // Przerwanie na obu zboczach: impuls VSync trwa kilka linii, przerwa miedzy impulsami ~cala ramke.
        // Zbocze po dlugiej przerwie to POCZATEK impulsu (od niego liczone sa linie w tabeli formatow),
        // zbocze po krotkiej - koniec impulsu, pomijamy. Dziala niezaleznie od polaryzacji VSync.
        static uint32_t last_edge_line = 0;
        const uint32_t since = g_abs_line - last_edge_line;
        last_edge_line = g_abs_line;
        if (since < 64) goto vsync_done;
        const int64_t now = esp_timer_get_time();
        // HSync zbiezny z poczatkiem VSync (oba bity w jednym przerwaniu) to linia 0 nowej ramki
        const uint32_t l = hs ? g_abs_line - 1 : g_abs_line;
        g_lines_last_frame = l - g_frame_first_line;
        g_frame_first_line = l + 1;
        g_frame_period_us = now - g_vsync_ts;
        g_vsync_ts = now;
        g_frame_count++;
        if (capture_on_vsync() && s_notify) {
            BaseType_t hp = pdFALSE;
            vTaskNotifyGiveFromISR(s_notify, &hp);
            portYIELD_FROM_ISR(hp);
        }
    }
vsync_done:
    if (hs) capture_on_hsync(g_abs_line - g_frame_first_line, c0);
    if (!s_pcnt_ok) {
        // Bez licznika sprzetowego: zbocze, ktore przyszlo w trakcie obslugi, doliczamy tutaj (tylko jedno).
        uint32_t again = GPIO.status1.intr_st & (1u << (PIN_HSYNC - 32));
        while (again) {
            GPIO.status1_w1tc.intr_st = again;
            g_abs_line++;
            again = GPIO.status1.intr_st & (1u << (PIN_HSYNC - 32));
        }
    }
}

// Sprzetowy licznik zboczy HSync (PCNT, jednostka 0).
static void pcnt_init(void)
{
    pcnt_unit_handle_t unit = NULL;
    pcnt_unit_config_t ucfg = {.high_limit = PCNT_LIMIT, .low_limit = -1};
    if (pcnt_new_unit(&ucfg, &unit) != ESP_OK) { ESP_LOGW(TAG, "PCNT: brak jednostki, licze linie w przerwaniu"); return; }
    pcnt_chan_config_t ccfg = {.edge_gpio_num = PIN_HSYNC, .level_gpio_num = -1};
    pcnt_channel_handle_t ch = NULL;
    if (pcnt_new_channel(unit, &ccfg, &ch) != ESP_OK) { ESP_LOGW(TAG, "PCNT: brak kanalu"); return; }
    if (g_params.hsync_neg)
        pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_HOLD, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    else
        pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
    ESP_ERROR_CHECK(pcnt_unit_enable(unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(unit));
    ESP_ERROR_CHECK(pcnt_unit_start(unit));
    // sprawdz, ze sterownik przydzielil jednostke 0 (ISR czyta rejestr bezposrednio)
    vTaskDelay(pdMS_TO_TICKS(20));
    int drv = 0;
    pcnt_unit_get_count(unit, &drv);
    const int raw = (int16_t) PCNT.cnt_unit[0].cnt_val;
    int diff = raw - drv;
    if (diff < 0) diff += PCNT_LIMIT;
    if (diff > 4000) { ESP_LOGW(TAG, "PCNT: rejestr jednostki 0 nie zgadza sie ze sterownikiem (%d vs %d)", raw, drv); return; }
    s_pcnt_prev = raw;
    s_pcnt_ok = true;
    ESP_LOGI(TAG, "PCNT: sprzetowy licznik linii HSync aktywny");
}

static void cap_init(void)
{
    mcpwm_cap_timer_handle_t tmr = NULL;
    mcpwm_capture_timer_config_t tcfg = {.group_id = 0, .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT};
    if (mcpwm_new_capture_timer(&tcfg, &tmr) != ESP_OK) { ESP_LOGW(TAG, "capture timer nieudany"); return; }
    mcpwm_cap_channel_handle_t ch = NULL;
    mcpwm_capture_channel_config_t ccfg = {
        .gpio_num = PIN_HSYNC,
        .prescale = 1,
        .flags = {.pos_edge = !g_params.hsync_neg, .neg_edge = g_params.hsync_neg, .pull_up = 0},
    };
    if (mcpwm_new_capture_channel(tmr, &ccfg, &ch) != ESP_OK) { ESP_LOGW(TAG, "capture channel nieudany"); return; }
    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(ch));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(tmr));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(tmr));
    uint32_t res = 80000000;
    mcpwm_capture_timer_get_resolution(tmr, &res);
    s_cap_to_cpu = res ? (uint32_t) (esp_clk_cpu_freq() / res) : 3;
    MCPWM0.cap_chn_cfg[1].capn_en = 1;       // kanal 1: tylko capture programowy ("teraz")
    // GPIO39 musi dalej byc wejsciem GPIO dla przerwania (sterownik capture przestawil funkcje pinu)
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_HSYNC) | (1ULL << PIN_VSYNC),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    s_cap_ok = true;
    ESP_LOGI(TAG, "capture HSync: %lu Hz, %lu cykli CPU na tick", (unsigned long) res, (unsigned long) s_cap_to_cpu);
}

static void isr_install_task(void *arg)
{
    pcnt_init();
    cap_init();
    // przerwanie GPIO trafia do rdzenia, na ktorym je wlaczono: rdzen 1 (rdzen 0 ma zadanie ramek)
    gpio_set_intr_type(PIN_HSYNC, g_params.hsync_neg ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE);
    gpio_set_intr_type(PIN_VSYNC, GPIO_INTR_ANYEDGE);   // poczatek impulsu wykrywany w przerwaniu (patrz gpio_isr)
    GPIO.status1_w1tc.intr_st = 0xFF;
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_GPIO_INTR_SOURCE, ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM, gpio_isr, NULL,
                                   &s_intr));
    ESP_ERROR_CHECK(gpio_intr_enable(PIN_HSYNC));
    ESP_ERROR_CHECK(gpio_intr_enable(PIN_VSYNC));
    ESP_LOGI(TAG, "przerwanie HSync/VSync poziom 3 na rdzeniu %d", xPortGetCoreID());
    vTaskDelete(NULL);
}

void sync_init(TaskHandle_t notify_task)
{
    s_notify = notify_task;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_HSYNC) | (1ULL << PIN_VSYNC),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     // GPIO34/39 nie maja pull-up/down
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    xTaskCreatePinnedToCore(isr_install_task, "isr_install", 3072, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "HSync GPIO%d (%s), VSync GPIO%d (%s)", PIN_HSYNC, g_params.hsync_neg ? "neg" : "pos",
             PIN_VSYNC, g_params.vsync_neg ? "neg" : "pos");
}

typedef struct { uint32_t total_min, total_max, active, start; const char *name; } fmt_t;

// Linie calkowite -> aktywne i pierwsza aktywna linia (liczone od pierwszego HSync po VSync).
// Wartosci z typowych timingow CEA/VESA; dokladny start stroi sie parametrem vstart.
static const fmt_t formats[] = {
    {2240, 2260, 2160, 82,  "2160p"},
    {1440, 1470, 1440, 41,  "1440p"},   // 2560x1440 CVT-RB: 1481 total; tolerancja szeroka
    {1115, 1135, 1080, 41,  "1080p"},
    {1060, 1070, 1024, 41,  "1280x1024"},
    {1080, 1095, 1050, 30,  "1680x1050"},
    {1000, 1010, 900,  26,  "1600x900"},
    {925,  940,  900,  28,  "1440x900"},
    {825,  835,  800,  22,  "1280x800"},
    {800,  812,  768,  35,  "1024x768"},
    {745,  755,  720,  25,  "720p"},
    {620,  630,  600,  27,  "800x600"},
    {620,  630,  576,  44,  "576p"},
    {555,  570,  540,  22,  "1080i (pole)"},
    {520,  530,  480,  36,  "480p"},
};

void sync_get_active(uint32_t hs_total, uint32_t *vstart, uint32_t *vlines, const char **name)
{
    const char *n = "auto";
    uint32_t s = 0, a = 0;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        if (hs_total >= formats[i].total_min && hs_total <= formats[i].total_max) {
            s = formats[i].start;
            a = formats[i].active;
            n = formats[i].name;
            break;
        }
    }
    if (a == 0 && hs_total > 100) {       // nieznany format: ~3,5 % od gory, 95,5 % aktywne
        s = hs_total * 35 / 1000;
        a = hs_total * 955 / 1000;
    }
    if (g_params.vstart > 0) s = g_params.vstart;
    if (g_params.vlines > 0) a = g_params.vlines;
    if (hs_total > 0 && s >= hs_total) a = 0;                 // vstart poza ramka (np. literowka w `set vstart`)
    else if (s + a > hs_total && hs_total > 0) a = hs_total - s;
    *vstart = s;
    *vlines = a;
    if (name) *name = n;
}

float sync_line_period_us(void)
{
    uint32_t lines = g_lines_last_frame;
    if (lines == 0 || g_frame_period_us <= 0) return 0.0f;
    return (float)g_frame_period_us / (float)lines;
}

float sync_frame_rate(void)
{
    if (g_frame_period_us <= 0) return 0.0f;
    if (esp_timer_get_time() - g_vsync_ts > 500000) return 0.0f;      // brak VSync od 0,5 s: sygnalu nie ma (bez starej wartosci)
    return 1000000.0f / (float)g_frame_period_us;
}
