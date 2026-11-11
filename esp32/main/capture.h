#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint32_t frames_done;       // ramki przetworzone i wyslane
    uint32_t samples_last;      // probek ADC zaliczonych do ostatniej ramki
    uint32_t cells_empty_last;  // komorki siatki (x3 kolory) bez ani jednej probki
    uint32_t hs_total;          // linii w ramce
    uint32_t vstart, vlines;    // aktywny obszar pionowy
    const char *format_name;
    float line_period_us;
    float frame_rate;
    float hstart_us, hactive_us;
    float raw_avg[3];           // sredni kod ADC R/G/B z ostatniej ramki (do kalibracji black/white)
    float raw_min[3], raw_max[3];
    // plan probkowania
    float    conv_us;           // zmierzony czas jednej konwersji ADC
    uint32_t stride;            // co ktora kolumne probkuje jedna linia
    uint32_t passes;            // linii probkujacych na wiersz siatki (3 kolory x stride)
    uint32_t spacing;           // minimalny odstep miedzy liniami probkujacymi
    uint32_t plan_lines;        // linii probkujacych w ramce
    uint32_t plan_rebuilds;
    // liczniki diagnostyczne (od startu)
    uint32_t d_mapped;          // probek zaliczonych do siatki
    uint32_t d_late;            // kolumn pominietych, bo przerwanie przyszlo za pozno
    uint32_t d_isr_lines;       // linii probkujacych obslozonych
    float    isr_max_us;        // najdluzsze przerwanie probkujace
    uint32_t d_resync;          // (pauzy na zapis NVS)
    bool     hdr_active;        // w ostatniej ramce stosowano nasycenie HDR
    bool     dig_on;            // pracuje kontroler cyfrowy ADC (adc_mode 1 lub 2)
    bool     blk_on;            // tryb blokowy (adc_mode 2)
    uint32_t blk_ovf;           // tryb blokowy: bloki z przepelnionym FIFO (liczenie w przerwaniu nie nadazalo)
    uint32_t lb_top, lb_bot, lb_vstart, lb_vlines;   // wykryty zakres linii obrazu (bez czarnych pasow)
    uint32_t blk_skipped;       // linie/bloki pominiete przez spoznione wejscie w przerwanie
    uint32_t col_cnt[16];       // probek na kolumne (od startu)
    uint32_t col_late[16];      // spoznionych kolumn na kolumne
    uint32_t col_bad[16];       // odrzuconych probek (start spozniony / konwersja za dluga) na kolumne
    uint32_t d_bad_start, d_bad_long;
    uint32_t sar_div, sar_cycle, sar_num;   // aktualne rejestry SAR
    float    isr_pre_min_us, isr_pre_max_us; // narzut od wejscia w przerwanie do pierwszego czekania
    float    isr_late_max_us;   // najwieksze opoznienie wejscia w przerwanie wzgledem zbocza HSync (capture)
    float    start_ovh_us;      // narzut startu konwersji (rejestry RTC)
} capture_stats_t;

// Profil linii: co `step_us` od wejscia w przerwanie HSync jedna konwersja (kanal G), `per_step` linii na krok.
#define HPROF_STEPS 80
typedef struct { float step_us; uint32_t step_cyc; uint32_t per_step; uint8_t ch_adc; bool seq_test; volatile bool active; volatile uint32_t idx;
                 uint32_t acc, n, dur; uint16_t avg[HPROF_STEPS]; uint16_t dur_us10[HPROF_STEPS];
                 uint16_t cnt[HPROF_STEPS]; uint16_t mn[HPROF_STEPS]; uint16_t mx[HPROF_STEPS]; uint8_t ch[HPROF_STEPS]; int32_t tr_cyc; } hprof_t;
void capture_hprof_start(float step_us, uint32_t per_step, int ch, bool seq_test);  // ch 0/1/2 = R/G/B
const hprof_t *capture_hprof_get(void);
void capture_digidx_hist(const uint16_t **h, const int32_t **ref);
void capture_digidx_start(float restart_us);   // tryb cyfrowy: profil wg numeru probki po restarcie w chwili restart_us od zbocza

TaskHandle_t capture_init(void);          // konfiguruje ADC, tworzy zadanie ramek (rdzen 0), zwraca jego uchwyt
void capture_get_stats(capture_stats_t *out);
void capture_get_frame(uint8_t *out);     // ostatnia wyslana ramka (PIX_BYTES_MAX)
// ostatni wykryty blysk (skok sredniej jasnosci): ramka przed i po, srednie, liczniki kolumn; zwraca liczbe blyskow
uint32_t capture_get_glitch(uint8_t *before, uint8_t *after, uint32_t *mean_before, uint32_t *mean_after, uint32_t *cols);
void capture_regs(void);                  // zrzut rejestrow ADC/I2S i czasow filtrow
void capture_hard_reset(const char *why); // pelny reset ukladu przez watchdog RTC
void capture_reconfigure(void);           // po zmianie parametrow (LUT, plan)
void capture_pause(void);                 // wstrzymuje probkowanie (np. na czas zapisu NVS)
void capture_resume(void);
void capture_hold(bool on);                // wstrzymanie probkowania z konsoli (test)
typedef struct { uint16_t delay_us10; uint32_t int_raw_before, int_raw_after; uint32_t word[4]; } digtest_row_t;
#define DIGRATE_WORDS 48
typedef struct { uint8_t flag_before_clear, flag_after_clear, flag_after_read; uint32_t changes, mean[3], clk_div, standby, flag_words, flag_fresh;
                 char seq[2 * DIGRATE_WORDS + 1]; } digrate_t;
typedef struct { uint32_t n, timeouts, t_min[3], t_max[3], t_sum[3], first_r, first_rg, sample_w[4][3], cyc_per_us; } digsync_t;
int capture_digsync(digsync_t *r);          // powtarzalnosc fazy probkowania po restarcie kontrolera cyfrowego
int capture_digrate(digrate_t *r);          // tempo konwersji kontrolera cyfrowego, zachowanie flagi FIFO
int capture_digtest(digtest_row_t *rows, int max_rows);   // test kontrolera cyfrowego ADC + FIFO I2S bez DMA
void capture_record(uint32_t n);           // nagraj n kolejnych ramek (max 8)
uint32_t capture_record_done(void);        // 0 = jeszcze nagrywa, inaczej liczba ramek
const uint8_t *capture_record_frame(uint32_t i);
void capture_trace(uint32_t n);            // zapisz sredni kolor srodka pola w n kolejnych ramkach (max 240)
uint32_t capture_trace_done(void);         // 0 = jeszcze zapisuje, inaczej liczba ramek
const uint8_t *capture_trace_frame(uint32_t i);   // R,G,B

// Wywolywane z przerwania GPIO (sync.c), kod w IRAM.
// lin = numer linii w ramce (0 = pierwszy HSync po VSync), c0 = licznik cykli CPU w chwili wejscia do przerwania.
void capture_on_hsync(uint32_t lin, uint32_t c0);
// Nowa ramka: zamiana banku akumulatorow; zwraca true, gdy zadanie ramek ma zostac obudzone.
bool capture_on_vsync(void);
