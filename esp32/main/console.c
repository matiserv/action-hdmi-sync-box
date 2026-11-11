// Prosta konsola na UART0: telemetria i strojenie parametrow.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "pins.h"
#include "params.h"
#include "sync.h"
#include "capture.h"
#include "spi_out.h"
#include "hdmi.h"
#include "hal/adc_types.h"

static const char *TAG = "console";

static void cmd_help(void)
{
    printf("Komendy:\n"
           "  stat                 - synchronizacja, format, FPS, pokrycie siatki, surowe kody ADC\n"
           "  grid                 - ostatnia ramka (cale pole) jako hex RRGGBB\n"
           "  ascii                - ostatnia ramka jako jasnosc (znaki)\n"
           "  glitch               - ostatni wykryty blysk: ramka przed i po, kolumny\n"
           "  digtest              - test szybkiego ADC (kontroler cyfrowy + FIFO I2S bez DMA)\n"
           "  frames [n]           - nagraj n (max 4) kolejnych ramek i wydrukuj (hex RRGGBB)\n"
           "  trace [n]            - sredni kolor srodka pola w n (max 240) kolejnych ramkach\n"
           "  params               - aktualne parametry\n"
           "  set <nazwa> <wartosc>- zmiana parametru (patrz params): black_mv, white_mv, hoff, hblank, vstart, vlines,\n"
           "                         cut, gain, gamma, flip_h, flip_v, rep, lat, sar_div, sar_cycle, vsync_neg, hsync_neg,\n"
           "                         hdr_mode (0 auto, 1 SDR, 2 HDR), hdr_gain (%%), sat (%%), m_rr..m_bb (1/1000),\n"
           "                         adc_mode (0 RTC, 1 cyfrowy, 2 cyfrowy blokowy), dig_step, dig_block, dig_d (ns), dig_phases,\n"
           "                         grid_w (do 48), grid_h (do 27), blur (0..12), blur_dir (1 = wzdluz krawedzi), blur_in (%%), lbox (1 = wykrywanie czarnych pasow), lbox_thr, hold (0,1 poziomu), smooth (%%, 100 = bez), smooth_t1, smooth_tc, smooth_t2, smooth_nz (0,1 x szum komorki; 0 = bez); od razu\n"
           "  regs | hardreset     - rejestry ADC/I2S i czasy filtrow / pelny reset ukladu (takze czesci RTC)\n"
           "  save | defaults      - zapis do NVS / przywrocenie domyslnych (bez zapisu)\n"
           "  pattern <0|1|2>      - 0 wideo, 1 tecza testowa 60 Hz, 2 pasy (lewo R, gora G, prawo B, dol W)\n"
           "  hdmi                 - stan UART2: ostatni tekst z MCU ukladu HDMI i tryb HDR\n"
           "  hdmi scope [ms]      - przebieg linii RX UART2 (czasy stanow), do ustalenia predkosci\n"
           "  hdmi <hex bajty>     - wyslij bajty po UART2 (eksperyment; fabryczny soft nic nie nadaje)\n"
           "  hdmi reset           - impuls resetu GSV2002 (GPIO22)\n"
           "  hprof [krok_us] [ch] - profil linii: kod ADC kanalu ch (0/1/2=R/G/B) co krok_us od HSync\n"
           "  chtest               - test pamieci kanalu: R,G,G,B,B,R w kolejnych liniach, ta sama chwila\n"
           "  upload <bajty> <crc32> - nowy firmware (firmware.bin) surowo po UART0 do drugiej partycji OTA, potem restart\n"
           "  reboot\n");
}

static void cmd_stat(void)
{
    capture_stats_t s;
    capture_get_stats(&s);
    printf("VSync: %.2f Hz (okres %lld us), linii w ramce: %lu, okres linii %.3f us\n", sync_frame_rate(),
           (long long)g_frame_period_us, (unsigned long)g_lines_last_frame, sync_line_period_us());
    printf("Format: %s, aktywne linie %lu..%lu (%lu), hstart %.2f us, hactive %.2f us\n",
           s.format_name ? s.format_name : "-", (unsigned long)s.vstart, (unsigned long)(s.vstart + s.vlines),
           (unsigned long)s.vlines, s.hstart_us, s.hactive_us);
    printf("Ramki przetworzone: %lu, probek w ostatniej: %lu, pustych komorek (x3 kolory): %lu/%d\n",
           (unsigned long)s.frames_done, (unsigned long)s.samples_last, (unsigned long)s.cells_empty_last, GRID_BYTES);
    printf("ADC kody R/G/B: srednia %.0f/%.0f/%.0f, min %.0f/%.0f/%.0f, max %.0f/%.0f/%.0f (black_mv=%ld ~ kod %.0f, white_mv=%ld ~ kod %.0f)\n",
           s.raw_avg[0], s.raw_avg[1], s.raw_avg[2], s.raw_min[0], s.raw_min[1], s.raw_min[2], s.raw_max[0],
           s.raw_max[1], s.raw_max[2], (long)g_params.black_mv, g_params.black_mv * 4095.0f / 1500.0f,
           (long)g_params.white_mv, g_params.white_mv * 4095.0f / 1500.0f);
    printf("Tryb ADC: %s\n", s.blk_on ? "cyfrowy blokowy (liczenie na rdzeniu 0)" : (s.dig_on ? "cyfrowy (FIFO I2S, 2 MS/s)" : "RTC (pojedyncze konwersje)"));
    if (s.blk_on) printf("Tryb blokowy: %u linii w bloku, bloki z przepelnionym FIFO %lu, bloki pominiete (spoznione wejscie) %lu\n",
                         g_params.dig_block, (unsigned long)s.blk_ovf, (unsigned long)s.blk_skipped);
    if (s.blk_on) printf("Obraz bez pasow: linie %lu..%lu z %lu..%lu\n", (unsigned long)s.lb_top, (unsigned long)s.lb_bot,
                         (unsigned long)s.lb_vstart, (unsigned long)(s.lb_vstart + s.lb_vlines - 1));
    printf("Plan: konwersja ADC %.2f us, stride %lu, %lu linii/wiersz (odstep >= %lu), %lu linii probkujacych, przebudowan %lu\n",
           s.conv_us, (unsigned long)s.stride, (unsigned long)s.passes, (unsigned long)s.spacing,
           (unsigned long)s.plan_lines, (unsigned long)s.plan_rebuilds);
    printf("Przerwania: linii probkujacych %lu, probek %lu, spoznionych kolumn %lu, najdluzsze przerwanie %.1f us, "
           "narzut wejscia %.2f..%.2f us, max opoznienie wejscia %.2f us, narzut startu konwersji %.2f us\n",
           (unsigned long)s.d_isr_lines, (unsigned long)s.d_mapped, (unsigned long)s.d_late, s.isr_max_us,
           s.isr_pre_min_us, s.isr_pre_max_us, s.isr_late_max_us, s.start_ovh_us);
    printf("SAR: clk_div %lu, sample_cycle %lu, sample_num %lu\n", (unsigned long)s.sar_div, (unsigned long)s.sar_cycle,
           (unsigned long)s.sar_num);
    printf("Odrzucone probki: start spozniony %lu, konwersja za dluga %lu\n", (unsigned long)s.d_bad_start,
           (unsigned long)s.d_bad_long);
    printf("Kolumny odrzucone: ");
    for (int i = 0; i < 16; i++) printf("%lu ", (unsigned long)s.col_bad[i]);
    printf("\nKolumny probki: ");
    for (int i = 0; i < 16; i++) printf("%lu ", (unsigned long)s.col_cnt[i]);
    printf("\nKolumny spoznione: ");
    for (int i = 0; i < 16; i++) printf("%lu ", (unsigned long)s.col_late[i]);
    putchar('\n');
    {
        hdmi_status_t h;
        hdmi_get_status(&h);
        printf("HDR: %s (UART2: %s; hdr_mode=%u), nasycenie x%.2f\n", s.hdr_active ? "aktywny" : "nieaktywny",
               hdmi_hdr_name(h.hdr_mode), g_params.hdr_mode,
               (s.hdr_active ? g_params.hdr_gain_pct : g_params.sat_pct) / 100.0f);
        printf("UART2: %lu bajtow (w tym %lu zer), napisow HDR: %lu\n", (unsigned long)h.rx_bytes,
               (unsigned long)h.rx_zero, (unsigned long)h.tokens);
    }
    printf("SPI -> BK7238: wyslanych %lu ramek, %.1f fps, zegar %lu Hz, wzor testowy %u\n",
           (unsigned long)spi_out_frames_sent(), spi_out_fps(), (unsigned long)g_params.spi_hz, g_params.pattern);
}

static void print_frame(const uint8_t *f)
{
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            const uint8_t *p = f + (y * GRID_W + x) * 3;
            printf("%02X%02X%02X ", p[0], p[1], p[2]);
        }
        putchar('\n');
    }
}

// Nagranie kolejnych ramek i wydruk przy wstrzymanym probkowaniu (przerwanie probkujace gubi znaki konsoli).
static void cmd_frames(const char *n_s)
{
    uint32_t n = n_s ? (uint32_t)atoi(n_s) : 8;
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    capture_record(n);
    for (int i = 0; i < 200 && !capture_record_done(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!capture_record_done()) { printf("brak ramek (sygnal?)\n"); return; }
    // Bez wstrzymywania probkowania: zapisane ramki sa w osobnym buforze, a wydruk nie blokuje juz zadania ramek.
    // (Wstrzymanie zamrazalo tasme na czas wydruku i psulo pierwsza ramke kolejnej serii.)
    for (uint32_t i = 0; i < n; i++) {
        printf("ramka %lu:\n", (unsigned long)i);
        print_frame(capture_record_frame(i));
    }
}

// Przebieg sredniego koloru srodka pola: jedna wartosc RRGGBB na ramke, 20 w wierszu.
static void cmd_trace(const char *n_s)
{
    uint32_t n = n_s ? (uint32_t)atoi(n_s) : 120;
    if (n < 1) n = 1;
    if (n > 240) n = 240;
    capture_trace(n);
    for (int i = 0; i < 600 && !capture_trace_done(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!capture_trace_done()) { printf("brak ramek (sygnal?)\n"); return; }
    printf("przebieg %lu:\n", (unsigned long)n);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = capture_trace_frame(i);
        printf("%02X%02X%02X%s", p[0], p[1], p[2], (i % 20 == 19 || i + 1 == n) ? "\n" : " ");
    }
}

// Ostatni tekst odebrany z MCU ukladu HDMI (UART2) i rozpoznany tryb HDR.
static void cmd_hdmi_status(void)
{
    hdmi_status_t h;
    hdmi_get_status(&h);
    printf("UART2: %lu bajtow, %lu zer, napisow HDR %lu, tryb: %s\n", (unsigned long)h.rx_bytes,
           (unsigned long)h.rx_zero, (unsigned long)h.tokens, hdmi_hdr_name(h.hdr_mode));
    printf("ostatni tekst: \"%s\"\n", h.last_text);
}

// Przebieg linii RX UART2: czasy kolejnych stanow. Z najkrotszego stanu niskiego wynika predkosc transmisji.
static void cmd_hdmi_scope(const char *ms_s)
{
    static uint32_t dur[400];
    uint32_t ms = ms_s ? (uint32_t)atoi(ms_s) : 300;
    if (ms < 10) ms = 10;
    if (ms > 800) ms = 800;       // petla pomiaru nie oddaje procesora; dluzsza zatrzymalaby ramki na czas gaszenia tasmy
    int first = 0;
    int n = hdmi_scope(ms, dur, 400, &first);
    uint32_t min_lo = 0xFFFFFFFF, min_hi = 0xFFFFFFFF, nlo = 0, nhi = 0;
    for (int i = 0; i < n - 1; i++) {                 // ostatni odcinek jest uciety
        const int lvl = (i & 1) ? !first : first;
        if (lvl) { nhi++; if (dur[i] < min_hi) min_hi = dur[i]; }
        else { nlo++; if (dur[i] < min_lo) min_lo = dur[i]; }
    }
    printf("RX GPIO%d przez %lu ms: %d odcinkow, pierwszy poziom %d; stanow niskich %lu (min %lu us), wysokich %lu (min %lu us)\n",
           PIN_HDMI_UART_RX, (unsigned long)ms, n, first, (unsigned long)nlo, (unsigned long)(nlo ? min_lo : 0),
           (unsigned long)nhi, (unsigned long)(nhi ? min_hi : 0));
    printf("odcinki [us] (L=niski, H=wysoki):");
    for (int i = 0; i < n && i < 120; i++) printf(" %c%lu", ((i & 1) ? !first : first) ? 'H' : 'L', (unsigned long)dur[i]);
    putchar('\n');
}

// Test kontrolera cyfrowego ADC z odczytem FIFO I2S0 przez CPU (bez DMA). Probkowanie wideo stoi na czas testu.
static void cmd_digtest(void)
{
    static digtest_row_t rows[8];
    int n = capture_digtest(rows, 8);
    if (n < 0) { printf("digtest: blad konfiguracji (%d)\n", n); return; }
    printf("okres_us  czas_do_1_slowa_us  int_raw_po  slowa FIFO jako pary (starsza:kod mlodsza:kod)\n");
    for (int i = 0; i < n; i++) {
        printf("%4.1f  %6.2f  %08lx ", rows[i].delay_us10 / 10.0f,
               rows[i].int_raw_before == 0xFFFFFFFF ? -1.0f : rows[i].int_raw_before / 100.0f,
               (unsigned long)rows[i].int_raw_after);
        for (int k = 0; k < 4; k++) {
            const uint32_t w = rows[i].word[k];
            printf(" (%lu:%lu %lu:%lu)", (unsigned long)((w >> 28) & 0xF), (unsigned long)((w >> 16) & 0xFFF),
                   (unsigned long)((w >> 12) & 0xF), (unsigned long)(w & 0xFFF));
        }
        putchar('\n');
    }
    printf("kanaly ADC1: R=%d G=%d B=%d\n", ADC_CH_R, ADC_CH_G, ADC_CH_B);
}

static void cmd_digsync(void)
{
    static digsync_t r;
    if (capture_digsync(&r) != 0) { printf("digsync: blad\n"); return; }
    printf("restart kontrolera cyfrowego: %lu prob, %lu bez odpowiedzi\n", (unsigned long)r.n, (unsigned long)r.timeouts);
    for (int w = 0; w < 3 && r.n; w++)
        printf("  slowo %d po restarcie: min %.2f us, srednio %.2f us, max %.2f us (rozrzut %.2f us)\n", w + 1,
               (float)r.t_min[w] / r.cyc_per_us, (float)r.t_sum[w] / r.n / r.cyc_per_us, (float)r.t_max[w] / r.cyc_per_us,
               (float)(r.t_max[w] - r.t_min[w]) / r.cyc_per_us);
    printf("  pierwsza probka = R w %lu / %lu prob, pierwsze dwie = R,G w %lu\n", (unsigned long)r.first_r, (unsigned long)r.n,
           (unsigned long)r.first_rg);
    for (int i = 0; i < 4; i++) {
        printf("  proba %d:", i);
        for (int k = 0; k < 3; k++) {
            const uint32_t w = r.sample_w[i][k];
            printf(" (%lu:%lu %lu:%lu)", (unsigned long)((w >> 28) & 0xF), (unsigned long)((w >> 16) & 0xFFF),
                   (unsigned long)((w >> 12) & 0xF), (unsigned long)(w & 0xFFF));
        }
        putchar('\n');
    }
}

static void cmd_digrate(void)
{
    static digrate_t r;
    if (capture_digrate(&r) != 0) { printf("digrate: blad\n"); return; }
    printf("flaga FIFO: przed kasowaniem %u, po kasowaniu %u, po odczycie slowa i kasowaniu %u\n", r.flag_before_clear,
           r.flag_after_clear, r.flag_after_read);
    printf("clk_div %lu, standby %lu: %lu nowych probek w %d us -> konwersja co %.2f us\n", (unsigned long)r.clk_div,
           (unsigned long)r.standby, (unsigned long)r.changes, DIGRATE_WORDS,
           r.changes ? (float)DIGRATE_WORDS / (float)r.changes : 0.0f);
    printf("kanaly kolejnych polowek (co 0,5 us): %s\n", r.seq);
    printf("odczyt sterowany flaga: %lu slow w 40 us, w nich %lu nowych probek\n", (unsigned long)r.flag_words,
           (unsigned long)r.flag_fresh);
    printf("srednie kody R/G/B: %lu/%lu/%lu\n", (unsigned long)r.mean[0], (unsigned long)r.mean[1], (unsigned long)r.mean[2]);
}

static void cmd_glitch(void)
{
    static uint8_t a[PIX_BYTES_MAX], b[PIX_BYTES_MAX];
    uint32_t ma, mb, cols[GRID_W_MAX];
    uint32_t n = capture_get_glitch(a, b, &ma, &mb, cols);
    printf("blyskow (skok sredniej > 60): %lu\n", (unsigned long)n);
    if (n == 0) return;
    printf("kolumny jasniejsze o >100: ");
    for (int x = 0; x < GRID_W; x++) printf("%lu ", (unsigned long)cols[x]);
    printf("\nramka przed (srednia %lu):\n", (unsigned long)ma);
    print_frame(a);
    printf("ramka blysku (srednia %lu):\n", (unsigned long)mb);
    print_frame(b);
}

static void cmd_grid(bool ascii)
{
    static uint8_t f[PIX_BYTES_MAX];
    capture_get_frame(f);
    static const char ramp[] = " .:-=+*#%@";
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            const uint8_t *p = f + (y * GRID_W + x) * 3;
            if (ascii) {
                int l = (p[0] + p[1] + p[2]) / 3;
                putchar(ramp[l * 9 / 255]);
                putchar(' ');
            } else {
                printf("%02X%02X%02X ", p[0], p[1], p[2]);
            }
        }
        putchar('\n');
    }
}

// Profil linii: srednia wartosc ADC (kanal G) w funkcji czasu od wejscia w przerwanie HSync.
// Pokazuje, gdzie w czasie lezy aktywny obraz i wygaszanie, oraz ile trwa konwersja przy kazdym przesunieciu.
static void cmd_hprof(const char *step_s, const char *ch_s)
{
    float step = step_s ? (float)atof(step_s) : 0.25f;
    if (step < 0.05f) step = 0.05f;
    capture_hprof_start(step, 16, ch_s ? atoi(ch_s) : 1, false);
    const hprof_t *h = capture_hprof_get();
    for (int i = 0; i < 300 && h->active; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (h->active) { printf("profil nie skonczony (brak sygnalu?)\n"); return; }
    printf("czas_od_zbocza_us  kod_ADC  konwersja_us (w trybie cyfrowym: liczba probek)\n");
    for (int i = 0; i < HPROF_STEPS; i++)
        printf("%6.2f  %5u  %5.1f\n", step * i, h->avg[i], h->dur_us10[i] / 10.0f);
}

// Tryb cyfrowy: srednia wartosc kazdej kolejnej probki po restarcie przetwornika (restart w stalej chwili po HSync).
static void cmd_digidx(const char *delay_s)
{
    const float d = delay_s ? (float)atof(delay_s) : 12.0f;
    capture_digidx_start(d);
    const hprof_t *h = capture_hprof_get();
    for (int i = 0; i < 400 && h->active; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (h->active) { printf("profil nie skonczony (brak sygnalu?)\n"); return; }
    printf("DIGIDX restart_plan_us %.2f restart_us %.3f\n", d, (float)h->tr_cyc / 240.0f);
    for (int i = 0; i < HPROF_STEPS; i++)
        if (h->cnt[i]) printf("J %2d ch %u kod %4u min %4u max %4u powt %4u n %u\n", i, h->ch[i], h->avg[i], h->mn[i], h->mx[i], h->dur_us10[i], h->cnt[i]);
    const uint16_t *hs; const int32_t *ref;
    capture_digidx_hist(&hs, &ref);
    for (int k = 0; k < 4; k++) {
        if (k < 3) printf("HIST %d ref %ld:", 14 + k, (long)ref[k]);
        else printf("HIST restart (krok 25 ns):");
        for (int b = 0; b < 64; b++) if (hs[k * 64 + b]) printf(" %d:%u", (b - 32) * (k < 3 ? 4 : 25), hs[k * 64 + b]);
        putchar('\n');
    }
}

// Aktualizacja firmware po UART0: po komendzie host wysyla dokladnie `len` bajtow obrazu aplikacji.
// Zapis do nieaktywnej partycji OTA; przy zgodnym CRC32 (zlib) ustawiamy ja jako startowa i restartujemy.
static void cmd_upload(uint32_t len, uint32_t crc_expected)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || len < 1024 || len > part->size) { printf("ERR partycja/rozmiar\n"); return; }
    esp_ota_handle_t h;
    esp_err_t e = esp_ota_begin(part, len, &h);
    if (e != ESP_OK) { printf("ERR ota_begin %s\n", esp_err_to_name(e)); return; }
    printf("READY %s %lu\n", part->label, (unsigned long)len);
    fflush(stdout);
    static uint8_t buf[4096];
    uint32_t got = 0, crc = 0;
    // resztki konca linii komendy (LF po CR) nie sa czescia obrazu: obraz zaczyna sie od 0xE9
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (uart_read_bytes(UART_NUM_0, &b, 1, pdMS_TO_TICKS(3000)) != 1) { printf("ERR brak danych\n"); esp_ota_abort(h); return; }
        if (b == 13 || b == 10) continue;
        buf[0] = b; got = 1; crc = esp_rom_crc32_le(0, buf, 1);
        if (esp_ota_write(h, buf, 1) != ESP_OK) { printf("ERR ota_write pierwszy bajt\n"); esp_ota_abort(h); return; }
        break;
    }
    while (got < len) {
        uint32_t want = len - got;
        if (want > sizeof(buf)) want = sizeof(buf);
        int n = uart_read_bytes(UART_NUM_0, buf, want, pdMS_TO_TICKS(5000));
        if (n <= 0) { printf("ERR timeout po %lu B\n", (unsigned long)got); esp_ota_abort(h); return; }
        crc = esp_rom_crc32_le(crc, buf, n);
        e = esp_ota_write(h, buf, n);
        if (e != ESP_OK) { printf("ERR ota_write %s\n", esp_err_to_name(e)); esp_ota_abort(h); return; }
        got += n;
    }
    if (crc != crc_expected) { printf("ERR crc %08lx != %08lx\n", (unsigned long)crc, (unsigned long)crc_expected); esp_ota_abort(h); return; }
    e = esp_ota_end(h);
    if (e != ESP_OK) { printf("ERR ota_end %s\n", esp_err_to_name(e)); return; }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) { printf("ERR set_boot %s\n", esp_err_to_name(e)); return; }
    printf("OK %s, restart\n", part->label);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

// Test kanalow: ta sama chwila w linii (offset_us), kolejne linie probkujace z kanalami R,G,G,B,B,R.
// Jesli "G po R" rozni sie od "G po G", kondensator probkujacy SAR pamieta poprzedni kanal.
static void cmd_chtest(const char *off_s)
{
    float off = off_s ? (float)atof(off_s) : 6.0f;
    capture_hprof_start(0.0f, 32, 1, true);
    hprof_t *h = (hprof_t *)capture_hprof_get();
    h->step_cyc = 0;
    (void)off;
    for (int i = 0; i < 300 && h->active; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (h->active) { printf("test nie skonczony (brak sygnalu?)\n"); return; }
    static const char *names[6] = {"R po R", "G po R", "G po G", "B po G", "B po B", "R po B"};
    uint32_t sum[6] = {0}, n[6] = {0};
    for (int i = 0; i < HPROF_STEPS; i++) { sum[i % 6] += h->avg[i]; n[i % 6]++; }
    printf("kanal po kanale, konwersja na poczatku linii (offset 0 od ISR = ok. 6 us po HSync w chwili probkowania):\n");
    for (int k = 0; k < 6; k++) printf("  %-7s kod %lu\n", names[k], (unsigned long)(n[k] ? sum[k] / n[k] : 0));
}

static void handle_line(char *line)
{
    char *cmd = strtok(line, " \t");
    if (!cmd) return;
    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) cmd_help();
    else if (!strcmp(cmd, "stat")) cmd_stat();
    else if (!strcmp(cmd, "grid")) cmd_grid(false);
    else if (!strcmp(cmd, "ascii")) cmd_grid(true);
    else if (!strcmp(cmd, "glitch")) cmd_glitch();
    else if (!strcmp(cmd, "digtest")) cmd_digtest();
    else if (!strcmp(cmd, "digrate")) cmd_digrate();
    else if (!strcmp(cmd, "digsync")) cmd_digsync();
    else if (!strcmp(cmd, "digidx")) cmd_digidx(strtok(NULL, " \t"));
    else if (!strcmp(cmd, "frames")) cmd_frames(strtok(NULL, " \t"));
    else if (!strcmp(cmd, "trace")) cmd_trace(strtok(NULL, " \t"));
    else if (!strcmp(cmd, "hold")) { char *v = strtok(NULL, " \t"); capture_hold(v && atoi(v)); printf("hold=%d\n", v && atoi(v)); }
    else if (!strcmp(cmd, "params")) params_print();
    else if (!strcmp(cmd, "save")) params_save();
    else if (!strcmp(cmd, "defaults")) { params_defaults(); hdmi_apply(); capture_reconfigure(); spi_out_reconfigure(); params_print(); }
    else if (!strcmp(cmd, "reboot")) esp_restart();
    else if (!strcmp(cmd, "hardreset")) capture_hard_reset("komenda hardreset");
    else if (!strcmp(cmd, "regs")) capture_regs();
    else if (!strcmp(cmd, "upload")) {
        char *l = strtok(NULL, " \t");
        char *c = strtok(NULL, " \t");
        if (!l || !c) { printf("uzycie: upload <bajty> <crc32 hex>\n"); return; }
        cmd_upload((uint32_t)strtoul(l, NULL, 0), (uint32_t)strtoul(c, NULL, 16));
    }
    else if (!strcmp(cmd, "hprof")) { char *a = strtok(NULL, " \t"); cmd_hprof(a, strtok(NULL, " \t")); }
    else if (!strcmp(cmd, "chtest")) cmd_chtest(strtok(NULL, " \t"));
    else if (!strcmp(cmd, "pattern")) {
        char *v = strtok(NULL, " \t");
        g_params.pattern = v ? (uint8_t)atoi(v) : 0;
        printf("pattern = %u\n", g_params.pattern);
    } else if (!strcmp(cmd, "set")) {
        char *n = strtok(NULL, " \t");
        char *v = strtok(NULL, " \t");
        if (!n || !v) { printf("uzycie: set <nazwa> <wartosc>\n"); return; }
        if (!strcmp(n, "spi_hz")) {
            const long hz = strtol(v, NULL, 0);
            if (hz < 1000000 || hz > 20000000) { printf("spi_hz poza zakresem 1000000..20000000, bez zmiany\n"); return; }
        }
        if (!params_set(n, v)) { printf("nieznany parametr: %s\n", n); return; }
        if (!strcmp(n, "spi_hz")) spi_out_reconfigure();
        if (!strcmp(n, "hdmi_baud") || !strcmp(n, "hdmi_rx_inv")) hdmi_apply();
        capture_reconfigure();   // LUT i plan probkowania liczone od nowa
        printf("%s = %s\n", n, v);
    } else if (!strcmp(cmd, "hdmi")) {
        char *v = strtok(NULL, " \t");
        if (!v) { cmd_hdmi_status(); return; }
        if (!strcmp(v, "scope")) { cmd_hdmi_scope(strtok(NULL, " \t")); return; }
        if (!strcmp(v, "reset")) { hdmi_reset_pulse(); return; }
        uint8_t bytes[64];
        int n = 0;
        while (v && n < 64) { bytes[n++] = (uint8_t)strtol(v, NULL, 16); v = strtok(NULL, " \t"); }
        if (n) { hdmi_uart_send(bytes, n); printf("wyslano %d B\n", n); }
    } else {
        printf("nieznana komenda: %s (help)\n", cmd);
    }
}

static void console_task(void *arg)
{
    char line[128];
    int len = 0;
    uint8_t ch;
    cmd_help();
    for (;;) {
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                line[len] = 0;
                handle_line(line);
                len = 0;
            }
            continue;
        }
        if (ch == 8 || ch == 127) { if (len) len--; continue; }
        if (len < (int)sizeof(line) - 1) line[len++] = (char)ch;
    }
}

void console_init(void)
{
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_config_t cfg = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        // Bufor TX i wyjscie stdout przez sterownik: printf czeka wtedy na miejsce w buforze, oddajac procesor, zamiast
        // krecic sie w petli na FIFO. Bez tego dlugi wydruk (`frames`, `grid`, `glitch`) zatrzymywal zadanie liczace
        // ramki (nizszy priorytet) na ponad sekunde i tasma zamierala albo gasla.
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 8192, 4096, 0, NULL, 0));   // duzy bufor RX: upload
        ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &cfg));
        uart_vfs_dev_use_driver(UART_NUM_0);
    }
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 6, NULL, 0);   // powyzej zadania liczacego ramki
    ESP_LOGI(TAG, "konsola na UART0, wpisz help");
}
