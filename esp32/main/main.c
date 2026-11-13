// Wlasny firmware ESP32 dla Action/Eurodomest HDMI Sync Box (plyta A4E32MGS02KR).
// Zadanie: z analogowego RGB + HSync/VSync z toru wideo zbudowac pole kolorow (do 48x27) i wysylac je
// po SPI do modulu BK7238 (ESPHome, komponent syncbox) z tempem klatek zrodla (do 60 Hz).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "pins.h"
#include "params.h"
#include "sync.h"
#include "capture.h"
#include "spi_out.h"
#include "hdmi.h"
#include "link.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

void console_init(void);

static const char *TAG = "main";

// Hook bezczynnosci rdzenia 1: zwraca false, wiec zadanie bezczynne nie wykonuje waiti (usypiania CPU).
// Budzenie z waiti i doladowanie cache z flash opozniaja wejscie w przerwanie HSync o kilka us.
static bool IRAM_ATTR core1_no_sleep_hook(void) { return false; }

static void led_task(void *arg)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_STATUS_LED, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    uint32_t last = 0;
    for (;;) {
        uint32_t now = g_frame_count;
        bool video = (now != last);   // sa VSync-i -> szybkie mruganie, brak -> wolne
        last = now;
        gpio_set_level(PIN_STATUS_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(video ? 100 : 500));
        gpio_set_level(PIN_STATUS_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(video ? 100 : 500));
    }
}

static void stats_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        capture_stats_t s;
        capture_get_stats(&s);
        ESP_LOGI(TAG, "VSync %.2f Hz, linii %lu (%s), ramek %lu, SPI %.1f fps, puste komorki %lu/%d, probek w ramce %lu, "
                 "spoznione kolumny %lu, max ISR %.1f us",
                 sync_frame_rate(), (unsigned long)g_lines_last_frame, s.format_name ? s.format_name : "-",
                 (unsigned long)s.frames_done, spi_out_fps(), (unsigned long)s.cells_empty_last, GRID_BYTES,
                 (unsigned long)s.samples_last, (unsigned long)s.d_late, s.isr_max_us);
    }
}

// Stan w pamieci RTC (g_rtc): wazny tylko ze znacznikiem RTC_MAGIC i sensownym licznikiem. Po wlaczeniu zasilania,
// po firmware sprzed v14.5 (inne zmienne pod tymi adresami) albo przy smieciach - zaczynamy od zera.
static void rtc_state_boot(void)
{
    g_prev_reset = (int) esp_reset_reason();
    if (g_prev_reset == ESP_RST_POWERON || g_rtc.magic != RTC_MAGIC || g_rtc.boot_count > 8) {
        memset(&g_rtc, 0, sizeof(g_rtc));
        g_rtc.magic = RTC_MAGIC;
    }
    g_prev_crumb = g_rtc.crumb;
    g_rtc.crumb = 0;
    const esp_partition_t *run = esp_ota_get_running_partition();
    const uint32_t slot = run != NULL && run->label[0] == 'o' ? (uint32_t) (run->label[4] - '0') & 0xFu : 0xFu;
    for (int i = BOOT_HIST_N - 1; i > 0; i--) g_rtc.hist[i] = g_rtc.hist[i - 1];
    g_rtc.hist[0] = ((uint32_t) LINK_FW_VERSION << 24) | (slot << 20) | (((uint32_t) g_prev_reset & 0xFFu) << 8) |
                    (g_prev_crumb & 0xFFu);
}

// Ochrona przed petla restartow: licznik w pamieci RTC (przetrwa restart programowy i panic, nie przetrwa wylaczenia
// zasilania). Czwarty start z rzedu bez 30 s spokojnej pracy = ten obraz nie nadaje sie do uzytku, wraca druga partycja
// OTA (poprzedni firmware). Bez tego bledny obraz wgrany przez konsole albo Wi-Fi wymagal przycisku i esptool.
static void boot_ok_cb(void *arg) { g_rtc.boot_count = 0; }

static void boot_loop_guard(void)
{
    g_rtc.boot_count++;
    if (g_rtc.boot_count >= 4) {
        g_rtc.boot_count = 0;
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        if (other != NULL && esp_ota_set_boot_partition(other) == ESP_OK) {      // sprawdza tez poprawnosc obrazu
            esp_rom_printf("petla restartow: powrot do partycji %s\n", other->label);
            esp_restart();
        }
    }
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = boot_ok_cb, .name = "bootok"};
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 30000000);
}

void app_main(void)
{
    rtc_state_boot();
    boot_loop_guard();
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    params_load();
    link_init();
    link_trial_boot();                        // firmware wgrany przez Wi-Fi: na probe albo powrot do poprzedniego
    {
        const esp_partition_t *run = esp_ota_get_running_partition();
        ESP_LOGI(TAG, "syncbox ESP32 v14.6 start, partycja %s", run ? run->label : "?");
    }

    esp_register_freertos_idle_hook_for_cpu(core1_no_sleep_hook, 1);
    hdmi_init();                              // GPIO22 = 1 (uklad HDMI poza resetem), UART2 nasluch
    spi_out_init();                           // SPI master -> BK7238, zadanie na rdzeniu 0
    TaskHandle_t cap = capture_init();        // ADC + zadanie ramek (rdzen 0)
    sync_init(cap);                           // przerwanie HSync/VSync (rdzen 1) -> probkowanie w capture
    console_init();
    xTaskCreatePinnedToCore(led_task, "led", 3072, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(stats_task, "stats", 3072, NULL, 1, NULL, 0);
}
