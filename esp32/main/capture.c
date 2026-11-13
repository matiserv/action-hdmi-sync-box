// Przechwytywanie analogowego RGB do pola kolorow (do 48x27 komorek) - probkowanie sterowane sygnalem HSync.
//
// Poprzednia wersja (ADC w trybie DMA + odtwarzanie czasu probek z licznika) nie dawala sie
// zsynchronizowac z obrazem: po przepelnieniach bufora licznik probek rozjezdzal sie z czasem i prawie
// zadna probka nie trafiala w aktywna linie (768 pustych komorek, taśma czarna).
//
// Teraz chwile probkowania wymusza sam sygnal wideo:
//  - kazde zbocze HSync to przerwanie (poziom 3, rdzen 1, sync.c); znamy numer linii w ramce (od VSync),
//  - dla wybranych linii ("plan") przerwanie czeka na licznik cykli CPU do srodka kolejnych kolumn siatki
//    i w tych chwilach uruchamia pojedyncze konwersje SAR ADC1 (kontroler RTC, rejestry bezposrednio,
//    ok. 1,5-2,5 us na konwersje),
//  - jedna linia probkuje jeden kolor i co `stride`-ta kolumne (konwersja trwa dluzej niz szerokosc
//    kolumny); kolejne linie tego samego wiersza siatki biora pozostale kolumny i kolory. Wiersz siatki
//    to kilkadziesiat linii obrazu, wiec 16 kolumn x 3 kolory x `rep` powtorzen miesci sie z zapasem,
//  - VSync zamienia bank akumulatorow i budzi zadanie, ktore liczy srednie, przepuszcza je przez LUT
//    (czern/biel/wzmocnienie/gamma) i wysyla ramke po SPI do BK7238.
// Efekt: jedna ramka siatki na kazda ramke obrazu (60 fps przy 60 Hz), opoznienie ponizej jednej ramki.

#include "esp_system.h"
#include "hal/wdt_hal.h"
#include "esp_heap_caps.h"
#include "capture.h"
#include "pins.h"
#include "params.h"
#include "sync.h"
#include "spi_out.h"
#include "hdmi.h"
#include "link.h"
#include "hdr_tables.h"
#include <string.h>
#include <math.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_continuous.h"
#include "soc/i2s_struct.h"
#include "soc/syscon_struct.h"
#include "hal/i2s_ll.h"
#include "hal/adc_ll.h"
#include "hal/misc.h"
#include "soc/sens_struct.h"
#include "soc/sens_reg.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "soc/rtc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"

static const char *TAG = "capture";

#define PLAN_MAX   2304                       // linii w ramce (2160p ma 2250)
#define MAX_SLOTS  (RTC_H_MAX * 3 * RTC_W)      // 16 wierszy x 3 kolory x stride (<= 16) x 1 powtorzenie

typedef struct {
    uint8_t  row;     // wiersz siatki
    uint8_t  ch;      // 0/1/2 = R/G/B (indeks w akumulatorze)
    uint8_t  ch_adc;  // numer kanalu ADC1
    uint8_t  pad;
    uint16_t mask;    // bit c = kolumna c probkowana w tej linii
} slot_t;

typedef struct {
    uint16_t line[PLAN_MAX];     // indeks slotu + 1; 0 = linia bez probkowania
    slot_t   slots[MAX_SLOTS * 2];
    int32_t  col_cyc[RTC_W];    // cykle CPU od wejscia w przerwanie do srodka kolumny
    int32_t  late_cyc;           // tolerancja spoznienia (pol kolumny)
    uint32_t wrap_mask;          // bit c = kolumna c startuje w poprzedniej linii (offset + T)
    uint32_t conv_max_cyc;       // konwersja dluzsza niz to = probka odrzucona (chwila probkowania nieznana)
    // dane, z ktorych plan powstal (wykrywanie zmian)
    uint32_t hs_total, vstart, vlines;
    float    T_us, hstart_us, hactive_us;
    uint32_t stride, passes, spacing, nlines, rep, nsets;
    int32_t  lat_us10;
} plan_t;

static plan_t s_plans[2];
static plan_t *volatile s_plan = NULL;         // aktywny plan (NULL = brak sygnalu / jeszcze nie zbudowany)
static int s_plan_idx = 0;

static volatile int  s_bank = 0;               // bank, do ktorego pisze przerwanie
// true od zamiany bankow w VSync do konca finalize_frame(): zadanie czyta wtedy drugi bank. Gdy zadanie sie spozni
// o cala ramke (np. dlugi wydruk konsoli), kolejny VSync nie zamienia bankow - przerwanie dalej dodaje do tego samego,
// a zadanie dostaje potem srednia z dwoch ramek zamiast banku, do ktorego wlasnie trwa zapis.
static volatile bool s_fin_busy = false;
static volatile uint32_t s_bank_skips = 0;
static volatile uint8_t s_fin_skip = 0;        // kolejne VSync bez zamiany bankow (zadanie spoznione)
// Przetwornik "zyje", gdy w ramce przyszla z FIFO choc jedna konwersja wlasciwego kanalu - niezaleznie od tego,
// czy ustawienia (hoff, dig_guard, vstart) pozwolily ja przypisac do komorki.
static volatile bool s_adc_alive = false;
static bool s_adc_worked = false;              // przetwornik dzialal w tym uruchomieniu
static uint32_t s_acc[2][GRID_H_MAX][GRID_W_MAX][3];
// Suma wag probek komorki (probka = 256). 32 bity: przy malym polu (ponizej ok. 30 komorek) jedna komorka dostaje w ramce
// ponad 255 probek jednej skladowej i licznik 16-bitowy by sie przewinal. Na stercie, bo segment statyczny jest pelny.
static uint32_t (*s_cnt)[GRID_H_MAX][GRID_W_MAX][3];
static volatile bool s_hold = false;           // pauza probkowania
static volatile bool s_reconf = true;
static volatile uint32_t s_isr_max_cyc = 0, s_late = 0, s_isr_lines = 0, s_mapped = 0;
static volatile uint32_t s_isr_pre_min = UINT32_MAX, s_isr_pre_max = 0;   // cykle od wejscia do pierwszego czekania
static uint32_t s_col_bad[RTC_W];      // probki odrzucone: start spozniony albo konwersja za dluga
static volatile uint32_t s_bad_start = 0, s_bad_long = 0;
static uint32_t s_col_cnt[RTC_W], s_col_late[RTC_W];
static hprof_t s_hprof;

// Tryb cyfrowy (adc_mode 1): kontroler DIG skanuje R,G,B bez przerwy (2 MS/s), probki trafiaja do FIFO I2S0,
// a przerwanie HSync czyta FIFO przez caly aktywny odcinek NASTEPNEJ linii i przypisuje probki do kolumn
// wg czasu odczytu. Mlodsze 16 bitow slowa FIFO to najnowsza probka: [15:12] kanal, [11:0] kod.
static adc_continuous_handle_t s_dig = NULL;
static volatile bool s_dig_on = false;        // kontroler cyfrowy pracuje
static volatile bool s_dig_ready = false;     // tablice ponizej sa spojne
static uint8_t  s_dig_row[PLAN_MAX];          // linia -> wiersz siatki + 1 (0 = linia bez probkowania)
static uint32_t s_dig_start_cyc, s_dig_end_cyc, s_dig_h0_cyc, s_dig_inv_q16;
static int32_t  s_dig_d_cyc;                  // czas od restartu do probki nr 0 (dig_lat)
static uint16_t s_dig_phase[PLAN_MAX];        // przesuniecie chwili restartu dla linii (cykle)
void capture_hprof_start(float step_us, uint32_t per_step, int ch, bool seq_test);
static bool     s_dp_idx = false, s_dp_idx_req = false;   // profil wg numeru probki po restarcie (digidx)
static uint32_t s_dp_dup[HPROF_STEPS];        // digidx: ile razy probka nr j byla powtorzeniem poprzedniej
static int64_t  s_dp_tr_sum;                  // digidx: suma (chwila restartu - c0)
static uint32_t s_dp_delay_cyc = 0;           // profil: opoznienie restartu po wejsciu w przerwanie
static int32_t  s_dig_lo_cyc, s_dig_hi_cyc;   // dozwolony czas probki wzgledem poczatku aktywnej linii (margines przy krawedziach)
static uint8_t  s_dig_ch[16];                 // kanal ADC -> 0/1/2 (R/G/B) albo 0xFF
static volatile uint32_t s_dig_samples = 0, s_dig_lines = 0, s_dig_shifted = 0;
static int32_t g_dig_calib_conv = 0;          // liczba konwersji policzonych przy starcie kontrolera (0 = przetwornik martwy)
static void adc_dead(const char *why);
// Tryb blokowy (adc_mode 2): przerwanie restartuje przetwornik raz na blok K kolejnych linii i czyta FIFO przez
// caly blok, przeliczajac kazda probke od razu (czas -> linia i kolumna -> akumulator). Robi to w czasie, w ktorym
// i tak czekaloby na kolejne slowo z przetwornika, wiec probkowane jest K z kazdych K+1 linii zamiast co trzeciej
// (ok. 2,4 raza wiecej probek), a rdzen 0 zajmuje sie tylko filtrami.
static TaskHandle_t s_task = NULL;
static volatile bool s_blk_mode = false;
static uint8_t  s_blk_k[PLAN_MAX];             // linia startowa bloku -> liczba linii w bloku (0 = brak)
static uint8_t  s_line_row[PLAN_MAX];          // linia obrazu -> wiersz pola + 1 (0 = poza obrazem)
static volatile uint32_t s_blk_Tq8 = 0;        // okres linii w cyklach CPU * 256 (mierzony ze zboczy HSync)
static int32_t  s_blk_hs_cyc = 0;              // poczatek aktywnej czesci linii od zbocza (cykle)
// Czarne pasy (letterbox): w trybie blokowym przerwanie zapisuje najwiekszy kod ADC kazdej probkowanej linii.
// Zadanie wyznacza z tego pierwsza i ostatnia linie obrazu i mapuje wiersze pola na sam obraz, wiec pasy w ogole
// nie trafiaja do pola, a krawedz pola jest zawsze krawedzia obrazu (bez wierszy "pol na pol" z pasem).
static uint16_t s_ln_max[PLAN_MAX];            // najwiekszy kod w aktywnej czesci linii (ostatnia ramka)
static uint32_t s_lb_top = 0, s_lb_bot = 0;    // pierwsza i ostatnia linia obrazu
static uint32_t s_lb_vstart = 0, s_lb_vlines = 0;
static uint32_t s_lb_code = 4095;              // kod ADC odpowiadajacy progowi lbox_thr
static uint8_t s_dig_att1 = 1, s_dig_att2 = 2;
static volatile bool s_dig_dual = false;       // wzorzec 6 pozycji: dwa zakresy wejsciowe na przemian
// Kody, po ktorych przetwornik ma martwa strefe (zmierzone na tym egzemplarzu: szary ekran 0..255 co 2 poziomy,
// osobno w kazdym zakresie). Probki o kodzie w odleglosci do 3 od takiego kodu dostaja mala wage.
// Zakres 2,5 dB: 0x67F (jasnosc ok. 8), 0x87F (66), 0x8FF (80..86, najszersza), 0xA7F (124), 0xCFF (196).
// Zakres 6 dB:   0x4FF (30..36), 0x5FF (64..70), 0x87F (170..174), 0x8FF (192..198).
static const uint16_t STK1[] = {0x67F, 0x87F, 0x8FF, 0xA7F, 0xCFF};
static const uint16_t STK2[] = {0x4FF, 0x5FF, 0x87F, 0x8FF};
static uint8_t s_stk[2][512];                 // mapy bitowe kodow o malej wadze, osobno dla kazdego zakresu
static uint16_t s_eq2[4096];                   // kod w drugim zakresie -> rownowazny kod w pierwszym zakresie
static volatile uint32_t s_blk_ovf = 0;        // bloki, w ktorych FIFO sie przepelnilo (liczenie nie nadazalo)
static uint32_t s_fin_us[3];                   // finalize_frame: srednie, rozmycie, wygladzanie i kolor (us)
static int32_t s_dig_dt_q8 = 120 * 256;      // odstep kolejnych konwersji w cyklach CPU * 256 (mierzony przy starcie; 0,5 us)
static uint32_t s_dig_T_cyc = 0;             // okres linii w cyklach CPU
static uint16_t s_dp_mn[HPROF_STEPS], s_dp_mx[HPROF_STEPS];
static uint16_t s_dp_hist[4][64]; static int32_t s_dp_ref[3]; static int32_t s_dp_tr_ref;   // digidx: rozklad kodu probek nr 14, 15, 16 (krok 4 kody)
static int32_t s_dig_tw_cyc = 240;           // okres slowa FIFO w cyklach CPU (mierzony przy starcie; nominalnie 1 us)
static uint32_t s_dp_acc[HPROF_STEPS], s_dp_n[HPROF_STEPS], s_dp_lines, s_dp_inv_q16;   // profil linii w trybie cyfrowym
static uint32_t s_good_frames = 0;   // kolejne ramki z waznym planem
#define REC_MAX 4
static uint8_t (*s_rec)[PIX_BYTES_MAX];         // [REC_MAX], na stercie;   // nagranie kolejnych ramek (diagnostyka)
static volatile uint32_t s_rec_n = 0, s_rec_i = 0;
// Przebieg: sredni kolor srodkowej czesci pola (polowa szerokosci i wysokosci) w kolejnych ramkach wyjsciowych.
#define TRACE_MAX 240
static uint8_t s_tr[TRACE_MAX][3];
static volatile uint32_t s_tr_n = 0, s_tr_i = 0;
static uint32_t s_prev_mean = 0, s_glitch_count = 0, s_glitch_mean = 0, s_glitch_prev_mean = 0;
static uint8_t *s_glitch_frame, *s_glitch_before;   // po PIX_BYTES_MAX, na stercie
static uint32_t s_glitch_col[GRID_W_MAX];   // ile razy kolumna byla jasniejsza o > 100 od poprzedniej ramki

static uint32_t s_cyc_per_us = 240;
static float s_conv_us = 2.0f;
static float s_sar_us = 2.0f;      // sam automat SAR (start -> done)
static uint32_t s_start_ovh_cyc = 0;   // cykle od wywolania adc1_convert_timed do faktycznego startu (dostep do rejestrow RTC)
static uint8_t s_lin[4096];              // kod ADC -> 0..255 liniowo miedzy czernia a biela
static uint8_t s_out[256];               // 0..255 liniowo -> wyjscie (wzmocnienie, gamma, odciecie)
static uint8_t s_cell[GRID_H_MAX][GRID_W_MAX][3]; // ostatnia wartosc liniowa kazdej komorki (gdy w ramce brak probki)
static capture_stats_t s_stats;
static uint8_t s_frame[PIX_BYTES_MAX];
static uint8_t s_last_frame[PIX_BYTES_MAX];
static adc_oneshot_unit_handle_t s_adc_unit = NULL;

// ------------------------------------------------------------------ ADC: pojedyncza konwersja (kanal juz wybrany)
static inline uint32_t IRAM_ATTR adc1_convert(void)
{
    while (HAL_FORCE_READ_U32_REG_FIELD(SENS.sar_slave_addr1, meas_status) != 0) { }
    SENS.sar_meas_start1.meas1_start_sar = 0;
    SENS.sar_meas_start1.meas1_start_sar = 1;
    while (!SENS.sar_meas_start1.meas1_done_sar) { }
    return HAL_FORCE_READ_U32_REG_FIELD(SENS.sar_meas_start1, meas1_data_sar);
}

// Jak wyzej, ale zwraca tez chwile startu (po oczekiwaniu na wolny automat) i czas trwania w cyklach CPU:
// jesli start sie spoznil albo automat SAR mielil dluzej niz zwykle, chwila probkowania jest nieznana.
static uint32_t s_sar_wait_cyc = 0;   // tyle cykli po starcie konwersja na pewno jeszcze trwa (bez odpytywania)

static inline uint32_t IRAM_ATTR adc1_convert_timed(uint32_t *t_start, uint32_t *dur)
{
    while (HAL_FORCE_READ_U32_REG_FIELD(SENS.sar_slave_addr1, meas_status) != 0) { }
    SENS.sar_meas_start1.meas1_start_sar = 0;
    const uint32_t t0 = esp_cpu_get_cycle_count();
    SENS.sar_meas_start1.meas1_start_sar = 1;
    // Odpytywanie rejestru RTC w petli zajmuje magistrale APB i gubi znaki UART na rdzeniu 0:
    // czekaj licznikiem CPU, odpytuj dopiero pod koniec.
    while ((int32_t) (esp_cpu_get_cycle_count() - (t0 + s_sar_wait_cyc)) < 0) { }
    while (!SENS.sar_meas_start1.meas1_done_sar) { }
    *t_start = t0;
    *dur = esp_cpu_get_cycle_count() - t0;
    return HAL_FORCE_READ_U32_REG_FIELD(SENS.sar_meas_start1, meas1_data_sar);
}

// Zegar SAR wg parametrow i pomiar czasu konwersji (do planu probkowania). Wolac przy wstrzymanym probkowaniu.
static void adc_apply_clock_and_bench(void)
{
    uint32_t div = g_params.sar_div ? g_params.sar_div : 1;   // 0 daje niestabilne konwersje (do 500 us)
    // zrodlo RTC_FAST_CLK (zegar SAR): RC ~8 MHz albo XTAL/4 = 10 MHz
    rtc_clk_fast_src_set(g_params.rtc_xtal ? SOC_RTC_FAST_CLK_SRC_XTAL_D4 : SOC_RTC_FAST_CLK_SRC_RC_FAST);
    adc_ll_set_sar_clk_div(ADC_UNIT_1, div);
    if (g_params.sar_cycle) HAL_FORCE_MODIFY_U32_REG_FIELD(SENS.sar_read_ctrl, sar1_sample_cycle, g_params.sar_cycle);
    SENS.sar_meas_start1.sar1_en_pad = 1u << ADC_CH_R;
    uint32_t mn = UINT32_MAX, mx = 0, ovh_min = UINT32_MAX;
    uint64_t tot = 0;
    s_sar_wait_cyc = 0;
    const uint32_t t_loop0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < 256; i++) {
        uint32_t t_start, dur;
        const uint32_t t = esp_cpu_get_cycle_count();
        (void) adc1_convert_timed(&t_start, &dur);
        const uint32_t ovh = t_start - t;
        if (ovh < ovh_min) ovh_min = ovh;
        tot += dur;
        if (dur < mn) mn = dur;
        if (dur > mx) mx = dur;
    }
    s_start_ovh_cyc = ovh_min;
    s_sar_wait_cyc = mn > 80 ? mn - 80 : 0;   // ok. 0,3 us przed najkrotsza zmierzona konwersja
    s_sar_us = (float) (tot / 256) / (float) s_cyc_per_us;
    // koszt jednej konwersji w linii = okres konwersji wykonywanych jedna po drugiej (z czekaniem na wolny automat)
    s_conv_us = (float) ((esp_cpu_get_cycle_count() - t_loop0) / 256) / (float) s_cyc_per_us;
    s_stats.sar_div = HAL_FORCE_READ_U32_REG_FIELD(SENS.sar_read_ctrl, sar1_clk_div);
    s_stats.sar_cycle = HAL_FORCE_READ_U32_REG_FIELD(SENS.sar_read_ctrl, sar1_sample_cycle);
    s_stats.sar_num = HAL_FORCE_READ_U32_REG_FIELD(SENS.sar_read_ctrl, sar1_sample_num);
    ESP_LOGI(TAG, "ADC1 SAR bezposrednio (R=GPIO%d G=GPIO%d B=GPIO%d), 2.5 dB, RTC_FAST %s, clk_div %lu, sample_cycle %lu: "
             "okres konwersji %.2f us, automat %.2f us (min %.2f, max %.2f), narzut startu %.2f us", PIN_ADC_R, PIN_ADC_G, PIN_ADC_B,
             rtc_clk_fast_src_get() == SOC_RTC_FAST_CLK_SRC_XTAL_D4 ? "XTAL/4" : "RC8M", (unsigned long) s_stats.sar_div,
             (unsigned long) s_stats.sar_cycle, s_conv_us, s_sar_us, (float) mn / s_cyc_per_us, (float) mx / s_cyc_per_us,
             (float) ovh_min / s_cyc_per_us);
}

static void adc_init(void)
{
    // Sterownik oneshot ustawia pady (GPIO32/33/35 jako analogowe) i rejestry kanalow; potem
    // sterujemy rejestrami sami: zasilanie SAR na stale (bez FSM), kontroler RTC, 12 bitow.
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc_unit));
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_2_5, .bitwidth = ADC_BITWIDTH_12};
    const adc_channel_t chs[3] = {ADC_CH_R, ADC_CH_G, ADC_CH_B};
    for (int i = 0; i < 3; i++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_unit, chs[i], &ccfg));
        int raw = 0;
        adc_oneshot_read(s_adc_unit, chs[i], &raw);
    }
    SENS.sar_meas_wait2.force_xpd_sar = SENS_FORCE_XPD_SAR_PU;   // SAR zasilony na stale
    adc_ll_hall_disable();
    adc_ll_amp_disable();
    adc_ll_set_sar_clk_div(ADC_UNIT_1, 1);
    for (int i = 0; i < 3; i++) adc_oneshot_ll_set_atten(ADC_UNIT_1, chs[i], ADC_ATTEN_DB_2_5);
    adc_oneshot_ll_set_output_bits(ADC_UNIT_1, ADC_BITWIDTH_12);
    adc_oneshot_ll_output_invert(ADC_UNIT_1, true);
    adc_ll_set_controller(ADC_UNIT_1, ADC_LL_CTRL_RTC);

    adc_apply_clock_and_bench();
}

// ------------------------------------------------------------------ przerwania (IRAM, tylko DRAM/rejestry)
// Restart skanowania kontrolera cyfrowego: zatrzymanie I2S RX, wskaznik wzorca na poczatek (R), reset odbiornika
// i FIFO, start. Po restarcie kolejnosc i chwile probek sa powtarzalne (zmierzone: pierwsze slowo po 2,83 us,
// rozrzut ~0,15 us, zawsze R,G | B,R | G,B ...), wiec faza probkowania jest zwiazana z chwila restartu.
static inline void IRAM_ATTR dig_restart(void)
{
    I2S0.conf.rx_start = 0;
    SYSCON.saradc_ctrl.sar1_patt_p_clear = 1;
    SYSCON.saradc_ctrl.sar1_patt_p_clear = 0;
    I2S0.conf.rx_reset = 1;
    I2S0.conf.rx_reset = 0;
    I2S0.conf.rx_fifo_reset = 1;
    I2S0.conf.rx_fifo_reset = 0;
    I2S0.conf.rx_start = 1;
}

// Tryb cyfrowy: restart przetwornika w chwili t_restart i odczyt slow FIFO do t_end.
// Probka nr j (j = 2*slowo + polowka) ma czas: t_r + s_dig_d_cyc + j * (okres slowa / 2), gdzie t_r to chwila
// restartu. Dzieki temu w kazdej ramce probki trafiaja w te same punkty obrazu (stabilny wynik na szczegolach).
#define DIG_MAX_WORDS 48
static uint32_t s_dl_w[DIG_MAX_WORDS];

static inline int IRAM_ATTR dig_restart_read(uint32_t c0, uint32_t t_restart, uint32_t t_end, int32_t *t_r)
{
    while ((int32_t) (esp_cpu_get_cycle_count() - t_restart) < 0) { }
    dig_restart();
    *t_r = (int32_t) (esp_cpu_get_cycle_count() - c0);
    int k = 0;
    for (;;) {
        const uint32_t now = esp_cpu_get_cycle_count();
        if ((int32_t) (now - t_end) >= 0 || k >= DIG_MAX_WORDS) break;
        I2S0.int_clr.val = 1;                                // flaga rx_take_data wraca od razu, gdy FIFO nie jest puste
        if (!I2S0.int_raw.rx_take_data) continue;            // puste FIFO zwracaloby stare slowa z pamieci
        s_dl_w[k++] = I2S0.fifo_rd;
    }
    return k;
}

// Tryb cyfrowy: jedna linia probkujaca. Wejscie w przerwanie moze byc spoznione o kilka us, dlatego
// probkujemy aktywny odcinek nastepnej linii (c0 + T), ktory zaczyna sie na pewno po wejsciu.
// Przetwornik konwertuje co 0,5 us w kolejnosci R,G,B, ale FIFO gubi 2 z kazdych 11 konwersji (zmierzone na
// gradiencie: po 9 probkach przeskok o 3 konwersje). Numer konwersji odtwarzamy z kanalu probki: kanal rosnie
// cyklicznie o 1, wiec ten sam kanal co poprzednio oznacza przeskok o 3. Czas probki = restart + D + nr * 0,5 us.
static void IRAM_ATTR dig_line(uint32_t lin, uint32_t c0)
{
    const uint8_t r1 = s_dig_row[lin];
    if (r1 == 0) return;
    const int bank = s_bank;
    uint32_t (*acc)[3] = s_acc[bank][r1 - 1];
    uint32_t (*cnt)[3] = s_cnt[bank][r1 - 1];
    // deterministyczne przesuniecie fazy: kolejne linie probkujace startuja o krok pozniej (ten sam uklad w kazdej ramce)
    uint32_t t_restart = c0 + s_dig_start_cyc + s_dig_phase[lin];
    uint32_t t_end = c0 + s_dig_end_cyc;
    int32_t h0 = (int32_t) s_dig_h0_cyc;
    if ((int32_t) (esp_cpu_get_cycle_count() - t_restart) > -48) {     // za pozno na te linie: bierzemy kolejna (ten sam wiersz)
        t_restart += s_dig_T_cyc; t_end += s_dig_T_cyc; h0 += (int32_t) s_dig_T_cyc;
        s_dig_shifted++;
    }
    int32_t t_r;
    const int nw = dig_restart_read(c0, t_restart, t_end, &t_r);
    const int32_t base = t_r + s_dig_d_cyc - h0;            // czas konwersji 0 wzgledem poczatku aktywnej linii
    const int32_t dtq = s_dig_dt_q8;
    uint32_t last = 2, n = 0;
    int32_t conv = -1;
    for (int k = 0; k < nw; k++) {
        const uint32_t w = s_dl_w[k];
        for (int h = 0; h < 2; h++) {
            const uint32_t smp = h == 0 ? (w >> 16) : (w & 0xFFFF);      // starsza polowka = wczesniejsza probka
            const uint32_t ci = s_dig_ch[(smp >> 12) & 0xF];
            if (ci > 2) continue;
            uint32_t stp = ci + 3 - last;
            if (stp > 3) stp -= 3;
            conv += (int32_t) stp;
            last = ci;
            if (conv == 0) continue;                        // pierwsza konwersja po restarcie jest pobierana wczesniej niz reszta
            const int32_t dt = base + ((conv * dtq) >> 8);
            if (dt < s_dig_lo_cyc || dt > s_dig_hi_cyc) continue;       // przy krawedzi: probka moze pochodzic z wygaszania
            // Skrajne kolumny dostaja tylko probki ze swojego obszaru (inaczej kolor z sasiedniej kolumny, czyli
            // z glebi obrazu, przenikalby do pasa przy krawedzi). Pozostale probki zasilaja dwie sasiednie kolumny
            // wewnetrzne z wagami zaleznymi od odleglosci do ich srodkow (suma wag 256), dzieki czemu drobne
            // przesuniecie probki zmienia wynik plynnie.
            const int32_t pc = (int32_t) (((uint32_t) dt * s_dig_inv_q16) >> 8);     // polozenie w kolumnach, Q8
            const uint32_t code = smp & 0xFFF;
            const int32_t wl = GRID_W - 1;
            if (pc < 256) { acc[0][ci] += code << 8; cnt[0][ci] += 256; }
            else if (pc >= wl * 256) { acc[wl][ci] += code << 8; cnt[wl][ci] += 256; }
            else {
                const int32_t pq = pc - 128;
                int32_t cl = pq >> 8;
                uint32_t f = (uint32_t) pq & 0xFF;
                if (cl < 1) { cl = 1; f = 0; }                      // nie dokladaj do kolumny 0
                if (cl + 1 >= wl) { cl = wl - 1; f = 0; }           // ani do ostatniej
                acc[cl][ci] += code * (256 - f); cnt[cl][ci] += 256 - f;
                if (f) { acc[cl + 1][ci] += code * f; cnt[cl + 1][ci] += f; }
            }
            n++;
        }
    }
    if (conv > 0) s_adc_alive = true;
    s_dig_samples += n;
    s_mapped += n;
    s_dig_lines++;
    s_isr_lines++;
    const uint32_t dur = esp_cpu_get_cycle_count() - c0;
    if (dur > s_isr_max_cyc) s_isr_max_cyc = dur;
}

// Tryb blokowy: jedno przerwanie probkuje K kolejnych linii. Restart jak w trybie 1; czas konwersji nr n liczony od
// zbocza linii startowej to koniec restartu + D + n * 0,5 us, a numer linii i polozenie w linii wynikaja z okresu linii
// zmierzonego ze zboczy HSync. Przerwanie konczy sie ok. 2 us po zboczu linii nastepujacej po bloku, wiec kolejny blok
// zaczyna sie od razu od tej linii.
static void IRAM_ATTR __attribute__((optimize("O3"))) blk_line(uint32_t lin, uint32_t c0)
{
    static uint32_t pl_lin = 0, pl_c0 = 0;
    if (lin > pl_lin && lin - pl_lin <= 32) {          // okres linii z dwoch kolejnych wejsc (zbocza mierzone sprzetowo)
        const uint32_t tm = ((c0 - pl_c0) << 8) / (lin - pl_lin);
        const int32_t df = (int32_t) (tm - s_blk_Tq8);
        if (df > -(int32_t) (s_blk_Tq8 >> 7) && df < (int32_t) (s_blk_Tq8 >> 7)) s_blk_Tq8 += df >> 4;
    }
    pl_lin = lin; pl_c0 = c0;
    const uint32_t K = s_blk_k[lin];
    if (K == 0) return;
    const uint32_t t_restart = c0 + s_dig_start_cyc + s_dig_phase[lin];
    if ((int32_t) (esp_cpu_get_cycle_count() - t_restart) > -48) { s_dig_shifted++; return; }   // za pozno: blok pominiety
    const uint32_t t_end = c0 + s_dig_end_cyc + (K - 1) * s_dig_T_cyc;
    const int bank = s_bank;
    const int32_t Tq = (int32_t) s_blk_Tq8, dtq = s_dig_dt_q8, hs = s_blk_hs_cyc, lo = s_dig_lo_cyc, hi = s_dig_hi_cyc;
    const uint32_t inv = s_dig_inv_q16, wl = (uint32_t) (GRID_W - 1);
    const uint8_t *chmap = s_dig_ch;
    while ((int32_t) (esp_cpu_get_cycle_count() - t_restart) < 0) { }
    dig_restart();
    const int32_t t_r = (int32_t) (esp_cpu_get_cycle_count() - c0);
    I2S0.int_clr.val = 4;                              // flaga przepelnienia FIFO od zera
    int32_t tq = ((t_r + s_dig_d_cyc) << 8) - dtq;     // chwila "konwersji nr -1"
    uint32_t line = lin, last = 2, n = 0, lnmax = 0, patt = 5;
    int32_t conv = -1;
    const bool dual = s_dig_dual;
    const uint16_t *eq2 = s_eq2;
    const uint8_t (*stk)[512] = s_stk;
    uint8_t r1 = s_line_row[line];
    uint32_t (*acc)[3] = s_acc[bank][r1 ? r1 - 1 : 0];
    uint32_t (*cnt)[3] = s_cnt[bank][r1 ? r1 - 1 : 0];
    for (;;) {
        if ((int32_t) (esp_cpu_get_cycle_count() - t_end) >= 0) break;
        I2S0.int_clr.val = 1;
        if (!I2S0.int_raw.rx_take_data) continue;       // puste FIFO zwracaloby stare slowa
        const uint32_t w = I2S0.fifo_rd;
        for (int h = 0; h < 2; h++) {
            const uint32_t smp = h == 0 ? (w >> 16) : (w & 0xFFFF);      // starsza polowka = wczesniejsza probka
            const uint32_t ci = chmap[(smp >> 12) & 0xF];
            if (ci > 2) continue;
            uint32_t stp = ci + 3 - last;               // kanal rosnie cyklicznie; ten sam kanal = przeskok o 3 konwersje
            if (stp > 3) stp -= 3;
            conv += (int32_t) stp;
            last = ci;
            patt += stp;
            if (patt >= 6) patt -= 6;
            tq += (int32_t) stp * dtq;
            if (tq >= Tq) {
                if (line < PLAN_MAX) s_ln_max[line] = (uint16_t) lnmax;    // maksimum zakonczonej linii (do wykrywania pasow)
                lnmax = 0;
                do { tq -= Tq; line++; } while (tq >= Tq);
                r1 = line < PLAN_MAX ? s_line_row[line] : 0;
                if (r1) { acc = s_acc[bank][r1 - 1]; cnt = s_cnt[bank][r1 - 1]; }
            }
            if (conv == 0) continue;                    // pierwsza konwersja po restarcie ma inny czas
            const int32_t dt = (tq >> 8) - hs;
            if (dt < lo || dt > hi) continue;           // wygaszanie albo margines przy krawedzi
            const uint32_t raw = smp & 0xFFF;
            const uint32_t code = (dual && patt >= 3) ? eq2[raw] : raw;       // drugi zakres -> skala pierwszego
            // kod przy martwej strefie przetwornika: liczy sie tylko z braku innych probek; w pierwszym zakresie strefy
            // sa wezsze, wiec tam waga nieco wieksza - gdzie strefy obu zakresow sie nakladaja, decyduje pierwszy
            const uint32_t r2 = dual && patt >= 3;
            const uint32_t wgt = ((stk[r2][raw >> 3] >> (raw & 7u)) & 1u) ? (r2 ? 1u : 8u) : 256u;
            if (code > lnmax) lnmax = code;
            if (r1 == 0) continue;                      // linia poza obrazem (czarny pas)
            uint32_t col = ((uint32_t) dt * inv) >> 16;
            if (col > wl) col = wl;
            acc[col][ci] += code * wgt;                 // kazda probka w calosci do swojej kolumny
            cnt[col][ci] += wgt;
            n++;
        }
    }
    if (line < PLAN_MAX && line > lin) s_ln_max[line] = (uint16_t) lnmax;   // ostatnia linia bloku
    if (I2S0.int_raw.val & 4) s_blk_ovf++;             // FIFO przepelnione w trakcie bloku: czesc probek ma zly czas
    if (conv > 0) s_adc_alive = true;
    s_dig_samples += n;
    s_mapped += n;
    s_dig_lines += K;
    s_isr_lines += K;
    const uint32_t dur = esp_cpu_get_cycle_count() - c0;
    if (dur > s_isr_max_cyc) s_isr_max_cyc = dur;
}

// Tryb cyfrowy: profil linii. Restart w chwili wejscia + s_dp_delay_cyc; czas probki liczony BEZ stalej D
// (t_r + j * pol okresu), wiec z polozenia krawedzi wygaszania wynika D (parametr dig_lat).
static void IRAM_ATTR dig_profile(uint32_t lin, uint32_t c0, const plan_t *pl)
{
    if (lin < pl->vstart + pl->vlines / 4 || lin > pl->vstart + pl->vlines * 3 / 4 || (lin & 3)) return;
    const uint32_t want = s_hprof.ch_adc;
    int32_t t_r;
    if (s_dp_idx) {
        // Profil wg numeru probki: restart w stalej chwili po zboczu, kazda probka (wszystkie kanaly) do przedzialu
        // o swoim numerze. Na poziomym gradiencie kod ADC = polozenie, wiec wynik to dokladna chwila kazdej probki.
        const uint32_t ts = c0 + s_dp_delay_cyc;
        const int nw = dig_restart_read(c0, ts, ts + 34 * s_cyc_per_us, &t_r);
        uint32_t last = 0xFF;
        if (s_dp_tr_ref == INT32_MIN) s_dp_tr_ref = t_r;
        {
            const int32_t b = (t_r - s_dp_tr_ref) / 6 + 32;          // krok 6 cykli = 25 ns
            if (b >= 0 && b < 64) s_dp_hist[3][b]++;
        }
        for (int k = 0; k < nw && 2 * k + 1 < HPROF_STEPS; k++) {
            const uint32_t w = s_dl_w[k];
            for (int h = 0; h < 2; h++) {
                const uint32_t smp = h == 0 ? (w >> 16) : (w & 0xFFFF);
                const uint32_t ch = (smp >> 12) & 0xF;
                const int j = 2 * k + h;
                const uint16_t cd = smp & 0xFFF;
                s_dp_acc[j] += cd; s_dp_n[j]++;
                if (cd < s_dp_mn[j]) s_dp_mn[j] = cd;
                if (cd > s_dp_mx[j]) s_dp_mx[j] = cd;
                if (j >= 14 && j <= 16) {
                    // kod po odjeciu przesuniecia wynikajacego z chwili restartu (gradient: 0,723 kodu na cykl CPU)
                    const int32_t cdc = (int32_t) cd - (((t_r - s_dp_tr_ref) * 185) >> 8);
                    if (s_dp_ref[j - 14] < 0) s_dp_ref[j - 14] = cdc;
                    const int32_t b = (cdc - s_dp_ref[j - 14]) / 4 + 32;
                    if (b >= 0 && b < 64) s_dp_hist[j - 14][b]++;
                }
                if (ch == last) s_dp_dup[j]++;
                last = ch;
                s_hprof.ch[j] = (uint8_t) ch;
            }
        }
        s_dp_tr_sum += t_r;
        if (++s_dp_lines >= 2048) {
            for (int i = 0; i < HPROF_STEPS; i++) {
                s_hprof.avg[i] = s_dp_n[i] ? (uint16_t) (s_dp_acc[i] / s_dp_n[i]) : 0;
                s_hprof.dur_us10[i] = (uint16_t) (s_dp_n[i] ? s_dp_dup[i] * 1000 / s_dp_n[i] : 0);   // powtorzenia w promilach
                s_hprof.cnt[i] = (uint16_t) s_dp_n[i];
                s_hprof.mn[i] = s_dp_mn[i]; s_hprof.mx[i] = s_dp_mx[i];
            }
            s_hprof.tr_cyc = (int32_t) (s_dp_tr_sum / s_dp_lines);
            s_hprof.active = false;
        }
        return;
    }
    const int nw = dig_restart_read(c0, esp_cpu_get_cycle_count() + s_dp_delay_cyc, c0 + s_hprof.step_cyc * HPROF_STEPS, &t_r);
    uint32_t last = 2;
    int32_t conv = -1;
    for (int k = 0; k < nw; k++) {
        const uint32_t w = s_dl_w[k];
        for (int h = 0; h < 2; h++) {
            const uint32_t smp = h == 0 ? (w >> 16) : (w & 0xFFFF);
            const uint32_t ch = (smp >> 12) & 0xF;
            const uint32_t ci = s_dig_ch[ch];
            if (ci > 2) continue;
            uint32_t stp = ci + 3 - last;
            if (stp > 3) stp -= 3;
            conv += (int32_t) stp;
            last = ci;
            if (ch != want) continue;
            const int32_t t = t_r + ((conv * s_dig_dt_q8) >> 8);
            if (t < 0) continue;
            const uint32_t bin = ((uint32_t) t * s_dp_inv_q16) >> 16;
            if (bin < HPROF_STEPS) { s_dp_acc[bin] += smp & 0xFFF; s_dp_n[bin]++; }
        }
    }
    if (++s_dp_lines >= s_hprof.per_step * 64) {
        for (int i = 0; i < HPROF_STEPS; i++) {
            s_hprof.avg[i] = s_dp_n[i] ? (uint16_t) (s_dp_acc[i] / s_dp_n[i]) : 0;
            s_hprof.dur_us10[i] = (uint16_t) (s_dp_n[i] > 65535 ? 65535 : s_dp_n[i]);   // w trybie cyfrowym: liczba probek
        }
        s_hprof.active = false;
    }
}

void IRAM_ATTR capture_on_hsync(uint32_t lin, uint32_t c0)
{
    plan_t *pl = s_plan;
    if (pl == NULL || s_hold || lin >= PLAN_MAX) return;
    if (s_fin_skip > 4) return;        // zadanie stoi od kilku ramek: nie sumuj dalej (suma 32-bitowa ma ograniczona pojemnosc)
    if (s_dig_on) {
        if (!s_dig_ready) return;
        if (s_hprof.active) dig_profile(lin, c0, pl);
        else if (s_blk_mode) blk_line(lin, c0);
        else dig_line(lin, c0);
        return;
    }
    if (s_hprof.active) {                                   // profil linii zamiast normalnego planu
        if (lin < pl->vstart + pl->vlines / 4 || lin > pl->vstart + pl->vlines * 3 / 4 || (lin & 1)) return;
        const uint32_t idx = s_hprof.idx;
        if (idx >= HPROF_STEPS) { s_hprof.active = false; return; }
        const uint32_t target = c0 + s_hprof.step_cyc * idx;
        uint8_t ch = s_hprof.ch_adc;
        if (s_hprof.seq_test) {                               // test kanalow: ta sama chwila, kanal wg sekwencji
            static const uint8_t seq[6] = {ADC_CH_R, ADC_CH_G, ADC_CH_G, ADC_CH_B, ADC_CH_B, ADC_CH_R};
            ch = seq[idx % 6];
        }
        SENS.sar_meas_start1.sar1_en_pad = 1u << ch;
        while ((int32_t) (esp_cpu_get_cycle_count() - target) < 0) { }
        const uint32_t t0 = esp_cpu_get_cycle_count();
        const uint32_t v = adc1_convert();
        s_hprof.dur += esp_cpu_get_cycle_count() - t0;
        s_hprof.acc += v;
        if (++s_hprof.n >= s_hprof.per_step) {
            s_hprof.avg[idx] = (uint16_t) (s_hprof.acc / s_hprof.n);
            s_hprof.dur_us10[idx] = (uint16_t) (s_hprof.dur * 10 / s_hprof.n / s_cyc_per_us);
            s_hprof.acc = 0; s_hprof.n = 0; s_hprof.dur = 0;
            s_hprof.idx = idx + 1;
            if (idx + 1 >= HPROF_STEPS) s_hprof.active = false;
        }
        return;
    }
    const uint16_t si = pl->line[lin];
    if (si == 0) return;
    const slot_t s = pl->slots[si - 1];
    const int bank = s_bank;
    uint32_t *acc = &s_acc[bank][s.row][0][s.ch];
    uint32_t *cnt = &s_cnt[bank][s.row][0][s.ch];
    SENS.sar_meas_start1.sar1_en_pad = 1u << s.ch_adc;      // wybor kanalu na cala linie
    uint32_t mapped = 0, late = 0;
    {
        const uint32_t pre = esp_cpu_get_cycle_count() - c0;
        if (pre < s_isr_pre_min) s_isr_pre_min = pre;
        if (pre > s_isr_pre_max) s_isr_pre_max = pre;
    }
    for (int round = 0; round < 2; round++) {                   // 1: kolumny tej linii, 2: zawiniete (po nastepnym HSync)
        const uint32_t m = round ? (s.mask & pl->wrap_mask) : (s.mask & ~pl->wrap_mask);
        if (m == 0) continue;
        for (uint32_t col = 0; col < RTC_W; col++) {
            if (!((m >> col) & 1u)) continue;
            const uint32_t target = c0 + (uint32_t) pl->col_cyc[col];
            int32_t d;
            while ((d = (int32_t) (esp_cpu_get_cycle_count() - target)) < 0) { }
            if (d > pl->late_cyc) { late++; s_col_late[col]++; continue; }   // za pozno na te kolumne
            uint32_t t_start, dur;
            const uint32_t v = adc1_convert_timed(&t_start, &dur);
            if ((int32_t) (t_start - target) > pl->late_cyc + (int32_t) s_start_ovh_cyc) { s_bad_start++; s_col_bad[col]++; continue; }
            if (dur > pl->conv_max_cyc) { s_bad_long++; s_col_bad[col]++; continue; }
            acc[col * 3] += v;
            cnt[col * 3]++;
            s_col_cnt[col]++;
            mapped++;
        }
    }
    s_mapped += mapped;
    s_late += late;
    s_isr_lines++;
    const uint32_t dur = esp_cpu_get_cycle_count() - c0;
    if (dur > s_isr_max_cyc) s_isr_max_cyc = dur;
}

bool IRAM_ATTR capture_on_vsync(void)
{
    if (s_fin_busy) {                                      // poprzednia ramka jeszcze liczona: bez zamiany i bez budzenia
        s_bank_skips++;
        if (s_fin_skip < 255) s_fin_skip++;
        return false;
    }
    s_fin_busy = true;
    s_fin_skip = 0;
    s_bank ^= 1;
    return true;
}

// ------------------------------------------------------------------ LUT kod ADC -> 0..255
static void build_lut(void)
{
    const float span = (float) (g_params.white_mv - g_params.black_mv);
    for (int code = 0; code < 4096; code++) {
        const float mv = (float) code * (1500.0f / 4095.0f);   // 2.5 dB: pelna skala ok. 1,5 V
        float v = (mv - (float) g_params.black_mv) / (span > 1.0f ? span : 1.0f);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        s_lin[code] = (uint8_t) (v * 255.0f + 0.5f);
    }
    {
        // drugi zakres: te same poziomy czerni i bieli maja inne kody; przeliczamy je liniowo na skale pierwszego
        const float b1 = (float) g_params.black_mv * 4095.0f / 1500.0f, w1 = (float) g_params.white_mv * 4095.0f / 1500.0f;
        const float b2 = (float) g_params.black2_mv * 4095.0f / 1500.0f, w2 = (float) g_params.white2_mv * 4095.0f / 1500.0f;
        const float k = (w2 - b2) > 1.0f ? (w1 - b1) / (w2 - b2) : 1.0f;
        for (int code = 0; code < 4096; code++) {
            float e = b1 + ((float) code - b2) * k;
            if (e < 0) e = 0;
            if (e > 4095) e = 4095;
            s_eq2[code] = (uint16_t) (e + 0.5f);
        }
    }
    memset(s_stk, 0, sizeof(s_stk));
    for (int r = 0; r < 2; r++) {
        const uint16_t *lst = r ? STK2 : STK1;
        const int nl = r ? (int) (sizeof(STK2) / sizeof(STK2[0])) : (int) (sizeof(STK1) / sizeof(STK1[0]));
        for (int i = 0; i < nl; i++)
            for (int c = (int) lst[i] - 3; c <= (int) lst[i] + 3; c++)
                if (c >= 0 && c < 4096) s_stk[r][c >> 3] |= (uint8_t) (1u << (c & 7));
    }
    s_lb_code = 4095;
    for (int code = 0; code < 4096; code++) {
        if (s_lin[code] >= g_params.lbox_thr) { s_lb_code = (uint32_t) code; break; }
    }
    for (int i = 0; i < 256; i++) {
        float v = (float) i / 255.0f * (float) g_params.gain_pct / 100.0f;
        if (v > 1) v = 1;
        if (g_params.gamma) v = powf(v, 2.2f);
        int o = (int) (v * 255.0f + 0.5f);
        s_out[i] = (uint8_t) o;
    }
}

// Czy stosowac korekte HDR: tryb z UART2 (jak w fabrycznym firmware: kazdy inny niz SDR) albo wymuszenie.
static bool hdr_active(void)
{
    if (g_params.hdr_mode == 1) return false;
    if (g_params.hdr_mode == 2) return true;
    const uint8_t m = g_hdr_mode;
    return m != HDR_SDR && m != HDR_UNKNOWN;
}

// Konwersja PQ dotyczy sygnalow HDR10 i HDR10+ (oba uzywaja krzywej ST 2084) oraz wymuszenia hdr_mode = 2.
// HLG i Dolby Vision maja inne kodowanie: zostaja przy mnozniku nasycenia.
RTC_NOINIT_ATTR rtc_state_t g_rtc;
uint32_t g_prev_crumb;
int g_prev_reset;

static bool hdr_pq_active(void)
{
    if (!g_params.hdr_conv || !hdr_active()) return false;
    if (g_params.hdr_mode == 2) return true;
    const uint8_t m = g_hdr_mode;
    return m == HDR_HDR10 || m == HDR_HDR10P;
}

// HDR10 -> sRGB dla jednej komorki. Zmierzone testerem (bench/hdrtest.py): odczyt toru analogowego jest liniowy
// wzgledem kodu sygnalu, czyli c[] to wartosci PQ (ST 2084) w barwach BT.2020. Kolejno: PQ -> nity, macierz
// BT.2020 -> BT.709 na swietle liniowym, skala tak, zeby `hdr_white` nitow dalo 1, kodowanie sRGB.
// Kolory jasniejsze od `hdr_white` sa sciemniane wspolnym czynnikiem (odcien zostaje), a barwy spoza BT.709
// obcinane do zera na skladowej ujemnej. Tablice sa stale (flash), bez funkcji potegowych w petli.
static inline void hdr_pq_to_srgb(int32_t *c, float inv_white)
{
    const float r = HDR_PQ_NITS[c[0]], g = HDR_PQ_NITS[c[1]], b = HDR_PQ_NITS[c[2]];
    float v[3] = {
        ( 1.660491f * r - 0.587641f * g - 0.072850f * b) * inv_white,
        (-0.124550f * r + 1.132900f * g - 0.008349f * b) * inv_white,
        (-0.018151f * r - 0.100579f * g + 1.118730f * b) * inv_white,
    };
    float mx = 0;
    for (int i = 0; i < 3; i++) {
        if (v[i] < 0) v[i] = 0;
        if (v[i] > mx) mx = v[i];
    }
    const float k = mx > 1.0f ? 4095.0f / mx : 4095.0f;
    for (int i = 0; i < 3; i++) {
        uint32_t idx = (uint32_t) (v[i] * k + 0.5f);
        c[i] = HDR_ENC[idx > 4095 ? 4095 : idx];
    }
}

// Jedna komorka: (HDR10: konwersja do sRGB,) macierz kolorow, potem nasycenie w HSV (barwa i jasnosc bez zmian), jak funkcja
// 0x400d6458 fabrycznego firmware: S' = min(1, S * mnoznik).
static inline void color_cell(const uint8_t *lin, uint8_t *out, const int32_t *m, bool identity, int32_t sat_pct, bool pq,
                              float inv_white)
{
    int32_t c[3] = {lin[0], lin[1], lin[2]};
    if (pq) hdr_pq_to_srgb(c, inv_white);
    if (!identity) {
        const int32_t r = c[0], g = c[1], b = c[2];
        for (int i = 0; i < 3; i++) {
            int32_t v = (m[i * 3] * r + m[i * 3 + 1] * g + m[i * 3 + 2] * b) / 1000;
            c[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
        }
    }
    if (sat_pct != 100) {
        int32_t mx = c[0] > c[1] ? c[0] : c[1]; if (c[2] > mx) mx = c[2];
        int32_t mn = c[0] < c[1] ? c[0] : c[1]; if (c[2] < mn) mn = c[2];
        if (mx > mn) {
            // S = (mx-mn)/mx; S' = min(1, S*k) -> nowe minimum mn' = mx*(1-S'), skladowe skalowane wokol mx
            int32_t k = sat_pct;                                   // w %
            int32_t span_new = (mx - mn) * k / 100;
            if (span_new > mx) span_new = mx;
            for (int i = 0; i < 3; i++) c[i] = mx - (mx - c[i]) * span_new / (mx - mn);
        }
    }
    int32_t o[3], mo = 0;
    for (int i = 0; i < 3; i++) {
        o[i] = s_out[c[i] < 0 ? 0 : (c[i] > 255 ? 255 : c[i])];
        if (o[i] > mo) mo = o[i];
    }
    // Miekki prog ciemnych kolorow: gdy najjasniejsza skladowa jest ponizej `cut`, caly kolor jest przyciemniany
    // wspolnym czynnikiem (najjasniejsza skladowa maleje kwadratowo do zera). Proporcje skladowych, czyli odcien,
    // zostaja; wczesniej kazda skladowa byla przycinana osobno i ciemne kolory zmienialy odcien.
    const int32_t cutv = g_params.cut;
    if (cutv > 0 && mo < cutv) {
        for (int i = 0; i < 3; i++) o[i] = (o[i] * mo + cutv / 2) / cutv;
    }
    for (int i = 0; i < 3; i++) out[i] = (uint8_t) o[i];
}

// ------------------------------------------------------------------ plan probkowania
// Czy konwersje zestawu kolumn `m` w jednej linii mieszcza sie czasowo (kolejnosc jak w ISR: niezawiniete
// rosnaco, potem zawiniete rosnaco; zawiniete maja czas + T).
static bool set_ok(const plan_t *p, const float *t_col, uint16_t m, float need)
{
    float prev = -1e9f;
    for (int round = 0; round < 2; round++)
        for (int c = 0; c < RTC_W; c++) {
            if (!((m >> c) & 1u)) continue;
            if ((((p->wrap_mask >> c) & 1u) != 0) != (round == 1)) continue;
            if (t_col[c] - prev < need) return false;
            prev = t_col[c];
        }
    return true;
}

static void build_plan(plan_t *p, uint32_t hs_total, uint32_t vstart, uint32_t vlines, float T,
                       float hstart, float hactive)
{
    memset(p->line, 0, sizeof(p->line));
    p->hs_total = hs_total; p->vstart = vstart; p->vlines = vlines;
    p->T_us = T; p->hstart_us = hstart; p->hactive_us = hactive;

    const float cw = hactive / RTC_W;                                  // szerokosc kolumny (us)
    const float lat = (float) g_params.lat_us10 / 10.0f;                // od zbocza HSync do chwili probkowania
    const float conv = s_conv_us;
    const float need = conv + 0.15f;                                    // odstep startow konwersji w jednej linii
    const float min_start = 6.0f;   // wejscie w przerwanie bywa spoznione do kilku us (sekcje krytyczne, cache):
                                    // kolumny wczesniejsze probkuja w nastepnej linii (start + T), nic nie ginie

    // Czas startu konwersji dla kazdej kolumny (od wejscia w przerwanie). Za wczesnie -> start w poprzedniej
    // linii tego samego wiersza (offset + T), bo i tak probka wypada dopiero `lat` po starcie.
    float busy_us = 0;
    p->wrap_mask = 0;
    for (int c = 0; c < RTC_W; c++) {
        float off = hstart + (c + 0.5f) * cw - lat;
        if (off < min_start) { off += T; p->wrap_mask |= 1u << c; }
        p->col_cyc[c] = (int32_t) (off * (float) s_cyc_per_us);
        if (off > busy_us) busy_us = off;
    }

    // Zestawy kolumn na jedna linie: co `stride`-ta kolumna. Gdy zestaw laczy kolumne zawinieta (czas t+T)
    // z niezawinieta (t + stride*cw), odstep miedzy nimi to T - stride*cw i tez musi pomiescic konwersje.
    // czasy startu w kolejnosci wykonania: najpierw niezawiniete rosnaco, potem zawiniete rosnaco (tak robi ISR)
    float t_col[RTC_W];
    for (int c = 0; c < RTC_W; c++) t_col[c] = (float) p->col_cyc[c] / (float) s_cyc_per_us;
    // Zestawy kolumn na linie: co `stride`-ta kolumna; zestaw, ktorego konwersje nie mieszcza sie czasowo,
    // dzielimy na czesc niezawinieta i zawinieta. Wybieramy stride dajacy najmniej zestawow (najmniej linii).
    uint32_t stride0 = (uint32_t) ceilf(need / cw);
    if (stride0 < 1) stride0 = 1;
    if (stride0 > RTC_W) stride0 = RTC_W;
    uint16_t sets[2 * RTC_W];
    uint32_t nsets = 0, stride = stride0;
    uint32_t best_n = UINT32_MAX;
    for (uint32_t st = stride0; st <= RTC_W; st++) {
        uint16_t cand[2 * RTC_W];
        uint32_t n = 0;
        for (uint32_t o = 0; o < st && n < 2 * RTC_W; o++) {
            uint16_t m = 0;
            for (uint32_t c = o; c < RTC_W; c += st) m |= (uint16_t) (1u << c);
            if (set_ok(p, t_col, m, need)) { cand[n++] = m; continue; }
            const uint16_t mw = m & (uint16_t) p->wrap_mask, mn = m & (uint16_t) ~p->wrap_mask;
            if (mn) cand[n++] = mn;
            if (mw && n < 2 * RTC_W) cand[n++] = mw;
        }
        if (n < best_n) { best_n = n; stride = st; nsets = n; memcpy(sets, cand, n * sizeof(uint16_t)); }
    }
    // ile linii zajmuje przerwanie probkujace (od HSync do konca ostatniej konwersji + zapas)
    busy_us += conv + 1.0f;
    uint32_t spacing = (uint32_t) ceilf(busy_us / T);
    if (spacing < 1) spacing = 1;
    const float row_h = (float) vlines / GRID_H;
    uint32_t passes_max = (uint32_t) (row_h / (float) spacing);
    const uint32_t set = 3 * nsets;                                     // jeden komplet: 3 kolory x zestawy
    uint32_t rep = g_params.rep > 0 ? (uint32_t) g_params.rep : 1;
    if (rep > 4) rep = 4;
    uint32_t passes = set * rep;
    if (passes > passes_max) passes = passes_max;                       // niepelne pokrycie, ale cos
    if (passes < set) ESP_LOGW(TAG, "plan: %lu linii/wiersz to za malo na komplet %lu zestawow x 3 kolory - czesc kolumn bez probek",
                               (unsigned long) passes_max, (unsigned long) nsets);
    if (passes == 0) passes = 1;
    if (passes > MAX_SLOTS * 2 / GRID_H) passes = MAX_SLOTS * 2 / GRID_H;

    const uint8_t ch_adc[3] = {ADC_CH_R, ADC_CH_G, ADC_CH_B};
    uint32_t nslots = 0, nlines = 0;
    for (uint32_t r = 0; r < GRID_H; r++) {
        const float row0 = (float) vstart + r * row_h;
        for (uint32_t k = 0; k < passes; k++) {
            uint32_t lin = (uint32_t) (row0 + (k + 0.5f) * row_h / passes);
            if (lin >= PLAN_MAX || lin + 2 >= vstart + vlines) continue;   // +2: zawiniete kolumny probkuja linie dalej
            if (p->line[lin] != 0) continue;
            slot_t *sl = &p->slots[nslots];
            sl->row = (uint8_t) r;
            sl->ch = (uint8_t) (k % 3);
            sl->ch_adc = ch_adc[k % 3];
            sl->mask = sets[(k / 3) % nsets];
            p->line[lin] = (uint16_t) (nslots + 1);
            nslots++;
            nlines++;
        }
    }
    p->late_cyc = (int32_t) (0.5f * cw * (float) s_cyc_per_us);
    p->conv_max_cyc = (uint32_t) ((s_sar_us + 0.4f) * (float) s_cyc_per_us);
    p->stride = stride; p->passes = passes; p->spacing = spacing; p->nlines = nlines; p->rep = rep; p->nsets = nsets;
    p->lat_us10 = g_params.lat_us10;
}

static void adc_rtc_restore(void);

// Dwa zakresy na przemian tylko w trybie blokowym (tryb 1 nie rozroznia pozycji wzorca).
static bool dig_want_dual(void)
{
    return g_params.adc_mode == 2 && g_params.dig_atten2 < 4 && g_params.dig_atten2 != g_params.dig_atten;
}

static bool dig_start(void)
{
    adc_continuous_handle_cfg_t hcfg = {.max_store_buf_size = 1024, .conv_frame_size = 256};
    {
        const esp_err_t e = adc_continuous_new_handle(&hcfg, &s_dig);
        if (e != ESP_OK) { ESP_LOGE(TAG, "adc_continuous_new_handle: %s (wolne %u B)", esp_err_to_name(e), (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT)); s_dig = NULL; return false; }
    }
    // Zakres wejsciowy (tlumik): 0 = 0 dB, 1 = 2,5 dB (domyslnie), 2 = 6 dB, 3 = 11 dB. Martwe strefy przetwornika leza
    // przy stalych kodach, wiec przy innym zakresie wypadaja na innych jasnosciach obrazu.
    // Przetwornik ESP32 przy tak szybkim zegarze ma martwe strefy: po kodach konczacych sie na 7F / FF (np. 0x8FF)
    // kilkadziesiat kolejnych wartosci wejsciowych daje ten sam kod (przy 2,5 dB jasnosci 80..86 czytaly sie jako 81).
    // Strefy leza przy stalych kodach, wiec przy innym zakresie wypadaja na innych jasnosciach. W trybie blokowym
    // wzorzec ma 6 pozycji: R,G,B w zakresie dig_atten, potem R,G,B w zakresie dig_atten2; probki o "lepkim" kodzie
    // dostaja znikoma wage, wiec w kazdym miejscu liczy sie ten zakres, ktory akurat mierzy poprawnie.
    static const adc_atten_t ATT[4] = {ADC_ATTEN_DB_0, ADC_ATTEN_DB_2_5, ADC_ATTEN_DB_6, ADC_ATTEN_DB_12};
    const adc_atten_t att = ATT[g_params.dig_atten < 4 ? g_params.dig_atten : 1];
    const bool dual = dig_want_dual();
    const adc_atten_t att2 = dual ? ATT[g_params.dig_atten2] : att;
    adc_digi_pattern_config_t pat[6] = {
        {.atten = att, .channel = ADC_CH_R, .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12},
        {.atten = att, .channel = ADC_CH_G, .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12},
        {.atten = att, .channel = ADC_CH_B, .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12},
        {.atten = att, .channel = ADC_CH_R, .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12},
        {.atten = att, .channel = ADC_CH_G, .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12},
        {.atten = att, .channel = ADC_CH_B, .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12},
    };   // sterownik wymaga jednego zakresu w calym wzorcu; zakres pozycji 3..5 jest ustawiany nizej wprost w rejestrze
    s_dig_dual = dual;
    s_dig_att1 = g_params.dig_atten; s_dig_att2 = g_params.dig_atten2;
    adc_continuous_config_t cfg = {
        .pattern_num = dual ? 6 : 3, .adc_pattern = pat, .sample_freq_hz = 2000000,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1, .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    esp_err_t e_cfg = adc_continuous_config(s_dig, &cfg);
    esp_err_t e_st = e_cfg == ESP_OK ? adc_continuous_start(s_dig) : ESP_FAIL;
    if (e_cfg != ESP_OK || e_st != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_config: %s, start: %s (pozycji wzorca %d)", esp_err_to_name(e_cfg), esp_err_to_name(e_st), (int) cfg.pattern_num);
        adc_continuous_deinit(s_dig);
        s_dig = NULL;
        adc_rtc_restore();
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    I2S0.int_ena.val = 0;                 // bez przerwan I2S: FIFO czyta przerwanie HSync
    i2s_ll_rx_stop_link(&I2S0);
    i2s_ll_enable_dma(&I2S0, false);
    I2S0.fifo_conf.rx_data_num = 0;       // flaga rx_take_data, gdy w FIFO jest co najmniej jedno slowo
    {
        // Sterownik ustawia zegar SAR na APB/16 = 5 MHz, przez co konwersja trwa ok. 2,5 us mimo "2 MS/s".
        const uint32_t div = g_params.dig_clkdiv ? g_params.dig_clkdiv : ADC_LL_DIGI_SAR_CLK_DIV_DEFAULT;
        const uint32_t standby = g_params.dig_standby ? g_params.dig_standby : ADC_LL_FSM_STANDBY_WAIT_DEFAULT;
        adc_ll_digi_set_clk_div(div);
        adc_ll_digi_set_fsm_time(ADC_LL_FSM_RSTB_WAIT_DEFAULT, div, standby);
    }
    if (dual) {
        // Pozycje 3..5 wzorca (drugie R,G,B) z zakresem att2. Pozycja tablicy: [7:4] kanal, [3:2] szerokosc (3 = 12 bitow),
        // [1:0] zakres; cztery pozycje na rejestr, pierwsza w najstarszym bajcie.
        static const uint8_t chs[3] = {ADC_CH_R, ADC_CH_G, ADC_CH_B};
        for (int i = 3; i < 6; i++) {
            const int idx = i / 4, off = (i % 4) * 8;
            const uint32_t ent = ((uint32_t) chs[i - 3] << 4) | (3u << 2) | ((uint32_t) att2 & 3u);
            uint32_t tab = SYSCON.saradc_sar1_patt_tab[idx];
            tab &= ~(0xFF000000u >> off);
            tab |= (ent << 24) >> off;
            SYSCON.saradc_sar1_patt_tab[idx] = tab;
        }
        ESP_LOGI(TAG, "ADC: dwa zakresy na przemian, tablica wzorca %08lx %08lx", (unsigned long) SYSCON.saradc_sar1_patt_tab[0],
                 (unsigned long) SYSCON.saradc_sar1_patt_tab[1]);
    }
    memset(s_dig_ch, 0xFF, sizeof(s_dig_ch));
    s_dig_ch[ADC_CH_R] = 0; s_dig_ch[ADC_CH_G] = 1; s_dig_ch[ADC_CH_B] = 2;
    if (g_params.dig_limit) SYSCON.saradc_ctrl2.max_meas_num = g_params.dig_limit;   // dlugosc serii konwersji (domyslnie 10)
    {
        // Odstep konwersji: przez 4 ms czytamy FIFO (odczyt sterowany flaga) i liczymy konwersje z kolejnosci kanalow
        // (kanal rosnie cyklicznie o 1; ten sam kanal = przeskok o 3). Wzgledem zegara CPU odstep jest staly.
        uint32_t words = 0, last = 2;
        int32_t conv = -1, conv_first = 0;
        I2S0.int_clr.val = 0xFFFFFFFF;
        i2s_ll_rx_reset_fifo(&I2S0);
        const uint32_t tb = esp_cpu_get_cycle_count();
        uint32_t t_first = 0, t_last = 0;
        while ((esp_cpu_get_cycle_count() - tb) < 4000 * s_cyc_per_us) {
            I2S0.int_clr.val = 1;
            if (!I2S0.int_raw.rx_take_data) continue;
            const uint32_t w = I2S0.fifo_rd;
            t_last = esp_cpu_get_cycle_count();
            for (int h = 0; h < 2; h++) {
                const uint32_t ci = s_dig_ch[((h == 0 ? (w >> 16) : w) >> 12) & 0xF];
                if (ci > 2) continue;
                uint32_t stp = ci + 3 - last;
                if (stp > 3) stp -= 3;
                conv += (int32_t) stp;
                last = ci;
            }
            if (words == 0) { t_first = t_last; conv_first = conv; }
            words++;
        }
        g_dig_calib_conv = conv - conv_first;
        if (words > 100) {
            s_dig_tw_cyc = (int32_t) ((t_last - t_first) / (words - 1));
            if (conv > conv_first + 100) s_dig_dt_q8 = (int32_t) (((uint64_t) (t_last - t_first) << 8) / (uint32_t) (conv - conv_first));
        }
        ESP_LOGI(TAG, "ADC: kontroler cyfrowy, %lu slow i %ld konwersji w 4 ms -> odstep konwersji %.4f us (%.3f MS/s), w FIFO %.1f %%",
                 (unsigned long) words, (long) (conv - conv_first), (float) s_dig_dt_q8 / 256.0f / (float) s_cyc_per_us,
                 256.0f * (float) s_cyc_per_us / (float) s_dig_dt_q8, conv > conv_first ? 200.0f * (float) (words - 1) / (float) (conv - conv_first) : 0.0f);
    }
    if (g_dig_calib_conv < 1000) adc_dead("przetwornik nie oddaje probek przy starcie");
    // Odstep konwersji w pracy z restartem przy kazdej linii: dokladnie 1 / czestotliwosc probkowania (0,5 us).
    // Sprawdzone na gradiencie: 30 konwersji = okres linii + 0,2 us. Pomiar swobodny powyzej daje 0,4976 us
    // i sluzy tylko do diagnostyki. Parametr dig_dt (ps) pozwala wymusic inna wartosc.
    {
        const uint32_t ps = g_params.dig_dt_ps > 0 ? (uint32_t) g_params.dig_dt_ps : 500000u;
        s_dig_dt_q8 = (int32_t) (((uint64_t) s_cyc_per_us * ps * 256u) / 1000000u);
    }
    s_dig_ready = false;
    s_dig_on = true;
    return true;
}

static void dig_stop(void)
{
    s_dig_on = false;
    s_blk_mode = false;
    s_dig_ready = false;
    vTaskDelay(pdMS_TO_TICKS(2));
    if (s_dig) {
        adc_continuous_stop(s_dig);
        adc_continuous_deinit(s_dig);
        s_dig = NULL;
    }
    adc_rtc_restore();
    ESP_LOGI(TAG, "ADC: kontroler RTC (pojedyncze konwersje)");
}

// Kontroler cyfrowy potrafi sie zatrzymac tak, ze FIFO oddaje same zera (kanal 0, kod 0), a zwykly restart
// procesora tego nie naprawia - stan siedzi w domenie RTC. Pomaga pelny reset ukladu przez watchdog RTC (jak wlaczenie
// zasilania). Zeby nie resetowac w kolko: jesli poprzedni reset tez byl od watchdoga, tylko zglaszamy blad.
void capture_hard_reset(const char *why)
{
    ESP_LOGE(TAG, "%s -> pelny reset ukladu", why);
    link_trial_rearm();                      // firmware na probie: ten reset nie jest jego wada
    vTaskDelay(pdMS_TO_TICKS(50));
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_config_stage(&ctx, WDT_STAGE0, 15000, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void adc_dead(const char *why)
{
    static bool reported = false;
    // Po pelnym resecie ponawiamy tylko wtedy, gdy ten reset pomogl (przetwornik dzialal) i minelo 30 s - bez petli
    // resetow, gdy przyczyna jest inna.
    if (esp_reset_reason() == ESP_RST_WDT && (!s_adc_worked || esp_timer_get_time() < 30000000)) {
        if (!reported) ESP_LOGE(TAG, "%s; poprzedni reset byl juz pelny, nie ponawiam (potrzebne odlaczenie zasilania?)", why);
        reported = true;
        return;
    }
    capture_hard_reset(why);
}

// Wiersze pola rozlozone rowno na linie top..bot (sam obraz); linie poza tym zakresem nie trafiaja do pola.
static void lbox_map_rows(uint32_t top, uint32_t bot)
{
    const uint32_t nl = bot >= top ? bot - top + 1 : 1;
    for (uint32_t lin = s_lb_vstart; lin < s_lb_vstart + s_lb_vlines && lin < PLAN_MAX; lin++) {
        if (lin < top || lin > bot) { s_line_row[lin] = 0; continue; }
        const uint32_t row = (lin - top) * GRID_H / nl;
        s_line_row[lin] = (uint8_t) ((row < GRID_H ? row : GRID_H - 1) + 1);
    }
    s_lb_top = top; s_lb_bot = bot;
}

// Wykrywanie czarnych pasow, raz na ramke (tylko tryb blokowy). Obraz zaczyna sie od pierwszych dwoch kolejnych
// probkowanych linii jasniejszych niz prog. Zakres obrazu powieksza sie od razu (cos pojawilo sie w pasie),
// a zmniejsza dopiero po 1,5 s stabilnego wyniku - ciemna scena przy krawedzi nie jest pasem.
static void lbox_update(void)
{
    const uint32_t vs = s_lb_vstart, vl = s_lb_vlines;
    if (!s_blk_mode || vl < 16) return;
    const uint32_t ve = vs + vl - 1;
    if (!g_params.lbox) {
        if (s_lb_top != vs || s_lb_bot != ve) lbox_map_rows(vs, ve);
        return;
    }
    const uint32_t thr = s_lb_code, lim = vl / 4;                  // pas najwyzej 1/4 wysokosci z kazdej strony
    uint32_t first = 0, last = 0, run = 0;
    bool found = false;
    for (uint32_t l = vs; l <= ve && l < PLAN_MAX; l++) {
        if (s_blk_k[l]) continue;                                  // linia startowa bloku nie jest probkowana
        if (s_ln_max[l] > thr) { if (++run >= 2) { first = l - 1; found = true; break; } } else run = 0;
    }
    if (!found) return;                                            // cala ramka ciemna: bez zmian
    run = 0;
    for (uint32_t l = ve; l >= vs && l < PLAN_MAX; l--) {
        if (s_blk_k[l]) { if (l == vs) break; continue; }
        if (s_ln_max[l] > thr) { if (++run >= 2) { last = l + 1; break; } } else run = 0;
        if (l == vs) break;
    }
    if (last == 0) return;
    if (first < vs) first = vs;
    if (last > ve) last = ve;
    if (first > vs + lim) first = vs + lim;
    if (last < ve - lim) last = ve - lim;
    if (first > vs) first += 1;                                    // linia graniczna bywa polcieniem skalowania
    if (last < ve) last -= 1;
    static uint32_t top_cnt = 0, top_min = 0xFFFFFFFF, bot_cnt = 0, bot_max = 0;
    uint32_t top = s_lb_top, bot = s_lb_bot;
    if (first < top) { top = first; top_cnt = 0; top_min = 0xFFFFFFFF; }
    else if (first > top + 2) {
        if (first < top_min) top_min = first;
        if (++top_cnt >= 90) { top = top_min; top_cnt = 0; top_min = 0xFFFFFFFF; }
    } else { top_cnt = 0; top_min = 0xFFFFFFFF; }
    if (last > bot) { bot = last; bot_cnt = 0; bot_max = 0; }
    else if (last + 2 < bot) {
        if (last > bot_max) bot_max = last;
        if (++bot_cnt >= 90) { bot = bot_max; bot_cnt = 0; bot_max = 0; }
    } else { bot_cnt = 0; bot_max = 0; }
    if (top != s_lb_top || bot != s_lb_bot) {
        lbox_map_rows(top, bot);
        ESP_LOGI(TAG, "obraz: linie %lu..%lu z %lu..%lu (pasy: gora %lu, dol %lu linii)", (unsigned long) top, (unsigned long) bot,
                 (unsigned long) vs, (unsigned long) ve, (unsigned long) (top - vs), (unsigned long) (ve - bot));
    }
}

// Tablice trybu cyfrowego: ktore linie probkuja ktory wiersz i okno czasowe wzgledem zbocza HSync.
static void dig_build(uint32_t vstart, uint32_t vlines, float T, float hstart, float hactive)
{
    s_dig_ready = false;
    vTaskDelay(pdMS_TO_TICKS(1));
    memset(s_dig_row, 0, sizeof(s_dig_row));
    uint32_t step = g_params.dig_step < 2 ? 2 : g_params.dig_step;
    uint32_t nlines = 0;
    for (uint32_t lin = vstart; lin + 3 < vstart + vlines && lin < PLAN_MAX; lin += step) {
        const uint32_t row = (lin + 1 - vstart) * GRID_H / vlines;     // probkowana jest linia lin + 1
        if (row < GRID_H) { s_dig_row[lin] = (uint8_t) (row + 1); nlines++; }
    }
    // tryb blokowy: linia startowa bloku L (restart pod koniec tej linii), probkowane linie L+1 .. L+K
    memset(s_blk_k, 0, sizeof(s_blk_k));
    memset(s_line_row, 0, sizeof(s_line_row));
    s_lb_vstart = vstart; s_lb_vlines = vlines;
    memset(s_ln_max, 0, sizeof(s_ln_max));
    lbox_map_rows(vstart, vstart + vlines - 1);          // po przebudowie planu caly obraz; pasy zostana wykryte od nowa
    const bool blk = g_params.adc_mode == 2;
    if (blk) {
        uint32_t K = g_params.dig_block < 1 ? 1 : (g_params.dig_block > 8 ? 8 : g_params.dig_block);
        memset(s_dig_row, 0, sizeof(s_dig_row));
        nlines = 0;
        for (uint32_t L = vstart > 0 ? vstart - 1 : 0; L + K < vstart + vlines && L < PLAN_MAX; L += K + 1) {
            s_blk_k[L] = (uint8_t) K;
            nlines += K;
        }
        step = K + 1;
    }
    const float d = (float) g_params.dig_d_ns / 1000.0f;               // restart -> konwersja nr 0
    const float cyc = (float) s_cyc_per_us;
    const float guard = (float) g_params.dig_guard_us10 / 10.0f;
    const float dt = (float) s_dig_dt_q8 / 256.0f / cyc;               // odstep konwersji (us)
    uint32_t phases = g_params.dig_phases ? g_params.dig_phases : 1;
    // fazy pokrywaja odstep probek jednego koloru (1,5 us); przy seriach z przerwami (dig_limit 10) potrzeba 3 us
    const float pstep = (g_params.dig_span_ns > 0 ? (float) g_params.dig_span_ns / 1000.0f : 3.0f * dt) / (float) phases;
    {
        // kolejnosc faz przemieszana (krok wzglednie pierwszy z liczba faz), zeby sasiednie linie nie mialy sasiednich faz
        uint32_t i = 0, mul = 1;
        for (uint32_t m = phases / 2 + 1; m < phases; m++) {
            uint32_t x = m, y = phases;
            while (y) { const uint32_t t = x % y; x = y; y = t; }
            if (x == 1) { mul = m; break; }
        }
        for (uint32_t lin = 0; lin < PLAN_MAX; lin++)
            if (s_dig_row[lin] || s_blk_k[lin]) s_dig_phase[lin] = (uint16_t) ((float) ((i++ * mul) % phases) * pstep * cyc);
    }
    s_dig_d_cyc = (int32_t) (d * cyc);
    s_dig_T_cyc = (uint32_t) (T * cyc);
    s_dig_h0_cyc = (uint32_t) ((T + hstart) * cyc);                    // poczatek kolumny 0 (od zbocza linii probkujacej)
    // restart tak, zeby konwersja nr 0 wypadla tuz przed dopuszczonym obszarem (margines + zapas 0,1 us);
    // fazy przesuwaja restart pozniej, wiec bazowy restart jest wczesniejszy o caly zakres faz
    // (konwersja nr 0 jest odrzucana, wiec restart jeszcze o jeden odstep konwersji wczesniej)
    // Sama sekwencja restartu trwa 1,0-1,8 us (zmierzone), a D liczy sie od jej konca, wiec zaczynamy o `dig_pre`
    // wczesniej. Bez tego pierwsza uzyteczna probka czerwonego wypadala najwczesniej 0,9 us od lewej krawedzi
    // i przy 32 kolumnach (0,4 us na kolumne) kolumna 0 nie dostawala czerwieni.
    const float pre = (float) g_params.dig_pre_us10 / 10.0f;
    float t_rs = T + hstart + guard - 0.1f - d - (float) (phases - 1) * pstep - dt - pre;
    if (t_rs < 11.0f) t_rs = 11.0f;                                   // po spoznionym wejsciu w przerwanie (do ~10 us)
    s_dig_start_cyc = (uint32_t) (t_rs * cyc);
    // slowo trafia do FIFO ok. 1,7 us po swojej drugiej probce (zmierzone: pierwsze slowo 2,83 us po restarcie)
    s_dig_end_cyc = (uint32_t) ((T + hstart + hactive - guard + 2.6f) * cyc);
    s_dig_inv_q16 = (uint32_t) (65536.0f * GRID_W / (hactive * cyc));
    s_dig_lo_cyc = (int32_t) (guard * cyc);
    s_dig_hi_cyc = (int32_t) ((hactive - guard) * cyc);
    s_stats.plan_lines = nlines;
    s_blk_Tq8 = (uint32_t) (T * cyc * 256.0f);
    s_blk_hs_cyc = (int32_t) (hstart * cyc);
    s_blk_mode = blk;
    s_dig_ready = true;
    ESP_LOGI(TAG, "plan cyfrowy%s: %lu linii probkujacych (co %lu), restart %.2f us po zboczu (+ faza, %lu faz co %.2f us), D %.1f us",
             blk ? " blokowy" : "", (unsigned long) nlines, (unsigned long) step, t_rs, (unsigned long) phases, pstep, d);
}

// Czy trzeba przebudowac plan: zmiana formatu, okresu linii (> 0,5 %), parametrow.
static void maybe_rebuild_plan(void)
{
    const uint32_t total = g_lines_last_frame;
    const float T = sync_line_period_us();
    const int64_t fp = g_frame_period_us;
    if (total < 100 || T < 3.0f || T > 80.0f || fp < 8000 || fp > 50000 || (esp_timer_get_time() - g_vsync_ts) > 200000) {
        if (s_plan != NULL) { s_plan = NULL; ESP_LOGI(TAG, "brak sygnalu: plan wylaczony"); }
        return;
    }
    uint32_t vstart = 0, vlines = 0;
    const char *fmt = "auto";
    sync_get_active(total, &vstart, &vlines, &fmt);
    if (vlines < GRID_H) { if (s_plan != NULL) s_plan = NULL; return; }   // za malo linii obrazu: bez planu
    const float hb = (float) g_params.hblank_pct10 / 1000.0f;
    const float hstart = T * hb * 0.69f + (float) g_params.hoff_us10 / 10.0f;
    float hactive = T * (1.0f - hb);
    if (hactive < 1.0f) hactive = 1.0f;

    plan_t *cur = s_plan;
    bool need = s_reconf || cur == NULL;
    if (!need) {
        need = cur->vstart != vstart || cur->vlines != vlines ||
               fabsf(cur->T_us - T) > 0.005f * T || fabsf(cur->hstart_us - hstart) > 0.01f ||
               cur->lat_us10 != g_params.lat_us10;
    }
    if (!need) return;
    if (s_reconf) {                      // zmiana parametrow: tryb ADC, zegar SAR i pomiar konwersji od nowa
        s_reconf = false;                // kasowane przed praca: `set` wpisany w jej trakcie wywola kolejne przeliczenie
        s_hold = true;
        vTaskDelay(pdMS_TO_TICKS(2));    // trwajace przerwanie probkujace zdazy sie skonczyc
        if (s_dig_on && g_params.adc_mode >= 1 && (dig_want_dual() != s_dig_dual || g_params.dig_atten != s_dig_att1 || (s_dig_dual && g_params.dig_atten2 != s_dig_att2))) dig_stop();
        if (g_params.adc_mode >= 1 && !s_dig_on) { if (!dig_start()) ESP_LOGE(TAG, "start kontrolera cyfrowego nieudany"); }
        else if (g_params.adc_mode == 0 && s_dig_on) dig_stop();
        else if (!s_dig_on) adc_apply_clock_and_bench();
        {
            // Uklad pola wg trybu, ktory faktycznie dziala: bez kontrolera cyfrowego (adc_mode 0 albo nieudany start)
            // probkuje tryb RTC, a ten wypelnia tylko 16 kolumn.
            const uint8_t h = g_params.grid_h >= 4 && g_params.grid_h <= GRID_H_MAX ? g_params.grid_h : GRID_H_MAX;
            uint8_t w = g_params.grid_w >= 4 && g_params.grid_w <= GRID_W_MAX ? g_params.grid_w : GRID_W_MAX;
            uint8_t hh = h;
            if (!s_dig_on) { w = RTC_W; if (hh > RTC_H_MAX) hh = RTC_H_MAX; }   // tryb RTC: do 16 x 18
            if (hh != g_grid_h || w != g_grid_w) {            // inny uklad pola: filtry startuja od nowa
                g_grid_h = hh;
                g_grid_w = w;
                s_good_frames = 0;
                ESP_LOGI(TAG, "pole %d x %d", GRID_W, GRID_H);
            }
        }
        s_hold = false;
    }
    build_lut();
    plan_t *nw = &s_plans[s_plan_idx ^ 1];
    build_plan(nw, total, vstart, vlines, T, hstart, hactive);
    s_plan_idx ^= 1;
    s_plan = nw;
    if (s_dig_on) dig_build(vstart, vlines, T, hstart, hactive);
    s_stats.hs_total = total; s_stats.vstart = vstart; s_stats.vlines = vlines; s_stats.format_name = fmt;
    s_stats.line_period_us = T; s_stats.hstart_us = hstart; s_stats.hactive_us = hactive;
    s_stats.stride = nw->stride; s_stats.passes = nw->passes; s_stats.spacing = nw->spacing;
    if (!s_dig_on) s_stats.plan_lines = nw->nlines;
    s_stats.plan_rebuilds++;
    ESP_LOGI(TAG, "plan: %s, %lu linii, aktywne %lu..%lu, T=%.2f us, hstart %.2f, hactive %.2f; konwersja %.2f us, lat %.1f -> "
             "stride %lu, %lu zestawow, zawiniete 0x%04lx, %lu linii/wiersz (odstep >= %lu), %lu linii probkujacych, rep %lu",
             fmt, (unsigned long) total, (unsigned long) vstart, (unsigned long) (vstart + vlines), T, hstart, hactive,
             s_conv_us, (float) g_params.lat_us10 / 10.0f, (unsigned long) nw->stride, (unsigned long) nw->nsets,
             (unsigned long) nw->wrap_mask, (unsigned long) nw->passes, (unsigned long) nw->spacing,
             (unsigned long) nw->nlines, (unsigned long) nw->rep);
}

// ------------------------------------------------------------------ ramka: srednie -> LUT -> SPI
// Wygladzanie w czasie zalezne od wielkosci zmiany (stan w Q8). Mala roznica wzgledem stanu (szum probkowania
// drobnych szczegolow) jest usredniana, duza (zmiana sceny, blysk) przechodzi od razu - reakcja zostaje szybka.
static uint16_t s_sm[GRID_H_MAX][GRID_W_MAX][3];    // wolna srednia = wyjscie (Q8)
static uint16_t s_fast[GRID_H_MAX][GRID_W_MAX][3];  // szybka srednia (Q8)
// Szum komorki (Q8): srednia z modulu roznicy "nowa ramka - szybka srednia" (najwieksza ze skladowych), stala czasowa
// 16 ramek. Probek na komorke i ramke jest kilkanascie i trafiaja za kazdym razem w inne piksele, wiec komorka z drobnym,
// kontrastowym detalem (ikona, tekst, zegar) daje co ramke inna srednia - o kilkadziesiat poziomow. Bez tej miary
// wygladzanie bralo ten rozrzut za zmiane obrazu i przepuszczalo go na tasme.
static uint16_t s_nz[GRID_H_MAX][GRID_W_MAX];

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}
static uint8_t s_blur[GRID_H_MAX][GRID_W_MAX][3];                        // siatka po rozmyciu przestrzennym
static uint16_t (*s_th)[GRID_W_MAX][3], (*s_tv)[GRID_W_MAX][3], (*s_tt)[GRID_W_MAX][3];   // bufory rozmycia, na stercie
#define FIELD16_BYTES (sizeof(uint16_t) * GRID_H_MAX * GRID_W_MAX * 3)
static bool s_sm_valid = false;
static uint8_t s_outh[GRID_H_MAX][GRID_W_MAX][3];     // wartosc wyjsciowa trzymana, dopoki zmiana nie przekroczy martwej strefy

// Jedno przejscie jadra 1-2-1 w poziomie (brzegi powielane). Pole w skali x16, wiersz ma GRID_W_MAX komorek po 3 skladowe.
static void __attribute__((optimize("O3"))) blur_pass_h(const uint16_t *src, uint16_t *dst, int W, int H)
{
    const int stride = GRID_W_MAX * 3, n = W * 3;
    for (int y = 0; y < H; y++) {
        const uint16_t *s = src + y * stride;
        uint16_t *d = dst + y * stride;
        for (int i = 0; i < 3; i++) d[i] = (uint16_t) ((3 * s[i] + s[i + 3] + 2) >> 2);
        for (int i = 3; i < n - 3; i++) d[i] = (uint16_t) ((s[i - 3] + 2 * s[i] + s[i + 3] + 2) >> 2);
        for (int i = n - 3; i < n; i++) d[i] = (uint16_t) ((s[i - 3] + 3 * s[i] + 2) >> 2);
    }
}

// Jedno przejscie jadra 1-2-1 w pionie.
static void __attribute__((optimize("O3"))) blur_pass_v(const uint16_t *src, uint16_t *dst, int W, int H)
{
    const int stride = GRID_W_MAX * 3, n = W * 3;
    for (int y = 0; y < H; y++) {
        const uint16_t *s = src + y * stride;
        const uint16_t *su = y > 0 ? s - stride : s, *sd = y < H - 1 ? s + stride : s;
        uint16_t *d = dst + y * stride;
        for (int i = 0; i < n; i++) d[i] = (uint16_t) ((su[i] + 2 * s[i] + sd[i] + 2) >> 2);
    }
}

// Rozmycie przestrzenne pola: `blur` przejsc jadra 1-2-1 (jadro dwumianowe o odchyleniu sqrt(blur / 2) komorki).
// Szum probkowania sasiednich komorek jest niezalezny, wiec usrednianie go tlumi bez opoznienia.
// blur_dir 1 (domyslnie): rozmycie tylko WZDLUZ najblizszej krawedzi ekranu. Komorki przy gornej i dolnej krawedzi sa
// rozmywane w poziomie, przy bokach w pionie, a w poblizu przekatnych oba wyniki sa mieszane, wiec kolor z glebi
// obrazu nie przenika do pasa przy krawedzi, z ktorego korzysta tasma. blur_dir 0: rozmycie w obu kierunkach.
// blur_in: domieszka (w %) jednego przejscia w obu kierunkach, czyli lekkie rozmycie takze w glab obrazu
// (100 % = komorka przy krawedzi bierze 25 % koloru z sasiedniej komorki w glab, 50 % - ok. 12 %).
static void __attribute__((optimize("O3"))) blur_field(void)
{
    const int W = GRID_W, H = GRID_H, stride = GRID_W_MAX * 3, n = W * 3;
    const int passes = g_params.blur > 12 ? 12 : g_params.blur;
    const bool dir = g_params.blur_dir != 0;
    const uint32_t mix = g_params.blur_in > 100 ? 100 : g_params.blur_in;
    if (passes == 0 && mix == 0) { memcpy(s_blur, s_cell, sizeof(s_blur)); return; }
    uint16_t *th = &s_th[0][0][0], *tv = &s_tv[0][0][0], *tt = &s_tt[0][0][0];
    for (int y = 0; y < H; y++) {
        const uint8_t *c = &s_cell[y][0][0];
        uint16_t *d = th + y * stride;
        for (int i = 0; i < n; i++) d[i] = (uint16_t) (c[i] << 4);     // skala x16, zeby przejscia nie gubily ulamkow
    }
    uint16_t *res = th;                                // wynik rozmycia (skala x16)
    if (passes > 0) {
        if (dir) {
            // lancuch pionowy startuje z tego samego pola: tv <- th
            for (int y = 0; y < H; y++) memcpy(tv + y * stride, th + y * stride, (size_t) n * sizeof(uint16_t));
            uint16_t *a = th, *b = tt;
            for (int p = 0; p < passes; p++) { blur_pass_h(a, b, W, H); uint16_t *t = a; a = b; b = t; }
            uint16_t *hres = a, *spare = b;            // wynik poziomy w `a`, wolny bufor w `b`
            uint16_t *c = tv, *d2 = spare;
            for (int p = 0; p < passes; p++) { blur_pass_v(c, d2, W, H); uint16_t *t = c; c = d2; d2 = t; }
            uint16_t *vres = c;
            // mieszanie wg odleglosci od krawedzi; wynik do wolnego bufora
            uint16_t *out = d2;
            for (int y = 0; y < H; y++) {
                const int ey = (2 * (y < H - 1 - y ? y : H - 1 - y) + 1) * 256 / (2 * H);       // w 1/256 wysokosci ekranu
                const int band = 256 / H > 0 ? 256 / H : 1;                                     // strefa przejscia: jedna komorka
                const uint16_t *ph = hres + y * stride, *pv = vres + y * stride;
                uint16_t *po = out + y * stride;
                for (int x = 0; x < W; x++) {
                    const int ex = (2 * (x < W - 1 - x ? x : W - 1 - x) + 1) * 256 * 16 / (2 * W * 9);   // obraz 16:9
                    int wv = 128 + (ey - ex) * 128 / band;                                      // udzial rozmycia pionowego
                    wv = wv < 0 ? 0 : (wv > 256 ? 256 : wv);
                    for (int k = 0; k < 3; k++)
                        po[x * 3 + k] = (uint16_t) (((uint32_t) ph[x * 3 + k] * (uint32_t) (256 - wv) + (uint32_t) pv[x * 3 + k] * (uint32_t) wv + 128) >> 8);
                }
            }
            res = out;
        } else {
            uint16_t *a = th, *b = tt;
            for (int p = 0; p < passes; p++) {
                blur_pass_h(a, b, W, H);
                blur_pass_v(b, a, W, H);
            }
            res = a;
        }
    }
    if (mix) {
        // dwa pozostale bufory jako robocze
        uint16_t *t1 = res == th ? tv : th, *t2 = (res == tt || t1 == tt) ? (res == tv || t1 == tv ? th : tv) : tt;
        blur_pass_h(res, t1, W, H);
        blur_pass_v(t1, t2, W, H);
        for (int y = 0; y < H; y++) {
            const uint16_t *pr = res + y * stride, *pm = t2 + y * stride;
            uint8_t *o = &s_blur[y][0][0];
            for (int i = 0; i < n; i++) o[i] = (uint8_t) (((uint32_t) pr[i] * (100u - mix) + (uint32_t) pm[i] * mix + 800u) / 1600u);
        }
    } else {
        for (int y = 0; y < H; y++) {
            const uint16_t *pr = res + y * stride;
            uint8_t *o = &s_blur[y][0][0];
            for (int i = 0; i < n; i++) o[i] = (uint8_t) ((pr[i] + 8) >> 4);
        }
    }
}

static void __attribute__((optimize("O2"))) finalize_frame(int bank)
{
    const int64_t tf0 = esp_timer_get_time();
    g_crumb = 1;
    lbox_update();
    uint32_t empty = 0, samples = 0;
    uint32_t sum[3] = {0, 0, 0}, mn[3] = {4095, 4095, 4095}, mx[3] = {0, 0, 0};
    uint32_t nsum[3] = {0, 0, 0};
    const int32_t *m = g_params.mat;
    const bool identity = m[0] == 1000 && m[4] == 1000 && m[8] == 1000 && m[1] == 0 && m[2] == 0 && m[3] == 0 &&
                          m[5] == 0 && m[6] == 0 && m[7] == 0;
    const bool hdr = hdr_active();
    // po konwersji PQ obraz jest juz jak SDR: obowiazuje nasycenie `sat`, a nie mnoznik HDR
    const bool pq = hdr && hdr_pq_active();
    const int32_t sat_pct = (hdr && !pq) ? g_params.hdr_gain_pct : g_params.sat_pct;
    const int32_t hw = g_params.hdr_white < 20 ? 20 : (g_params.hdr_white > 10000 ? 10000 : g_params.hdr_white);
    const float inv_white = pq ? 1.0f / (float) hw : 0.0f;
    s_stats.hdr_active = hdr;
    s_stats.hdr_pq = pq;
    const int32_t a_min = g_params.smooth_pct >= 100 || g_params.smooth_pct <= 0 ? 256 : g_params.smooth_pct * 256 / 100;
    const int32_t t1 = (g_params.smooth_t1 < 0 ? 0 : g_params.smooth_t1) << 8;
    const int32_t db = (g_params.hold_x10 < 0 ? 0 : g_params.hold_x10) * 256 / 10;
    const int32_t t2 = g_params.smooth_t2 > g_params.smooth_t1 ? g_params.smooth_t2 << 8 : t1 + 256;
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            uint8_t *lin = s_cell[y][x];
            for (int c = 0; c < 3; c++) {
                const uint32_t n = s_cnt[bank][y][x][c];
                if (n == 0) { empty++; continue; }        // zostaw poprzednia wartosc skladowej
                uint32_t avg = s_acc[bank][y][x][c] / n;
                if (avg > 4095) avg = 4095;
                samples += n;
                sum[c] += avg; nsum[c]++;
                if (avg < mn[c]) mn[c] = avg;
                if (avg > mx[c]) mx[c] = avg;
                lin[c] = s_lin[avg];
            }
        }
    }
    const int64_t tf1 = esp_timer_get_time();
    blur_field();
    const int64_t tf2 = esp_timer_get_time();
    g_crumb = pq ? 4 : 3;
    // Zgodnosc zmian w blokach 8 x 8 komorek. Szum probkowania jest w kazdej komorce inny i w sredniej z bloku sie znosi,
    // a prawdziwa zmiana obrazu (sciemnienie, przejscie, ruch) przesuwa caly blok w te sama strone. Gdy srednia
    // roznica "szybka srednia - wyjscie" w bloku przekracza smooth_tc, komorki bloku nadazaja z pelna predkoscia,
    // nawet jesli zmiana jest mala.
    enum { CB = 8, CBH = (GRID_H_MAX + CB - 1) / CB, CBW = (GRID_W_MAX + CB - 1) / CB };
    static int32_t bsum[CBH][CBW][3];
    // Szum sredniej bloku (Q8): srednia z modulu sredniej zmiany komorek bloku miedzy ramka a szybka srednia, stala
    // czasowa 16 ramek. Mierzony wprost: rozmycie wzdluz krawedzi wiaze sasiednie komorki, wiec szum sredniej bloku
    // jest 2-3 razy wiekszy, niz wynikaloby z szumu komorek przy zalozeniu, ze sa niezalezne.
    static int32_t s_bnz[CBH][CBW];
    static int32_t binn[CBH][CBW][3];              // srednia zmiana "nowa ramka - szybka srednia" w bloku (Q8)
    static uint8_t bcoh[CBH][CBW];
    static uint8_t bstep[CBH][CBW];                // blok zmienil sie w tej ramce skokiem (srednia zmiana komorek)
    const bool sm_on = a_min < 256 && s_sm_valid;
    // smooth_nz (w 0,1): o ile wielokrotnosci zmierzonego szumu przesuwane sa progi "to jest zmiana obrazu"; 0 = wylaczone
    const int32_t nzk = g_params.smooth_nz < 0 ? 0 : (g_params.smooth_nz > 100 ? 100 : g_params.smooth_nz);
    if (sm_on) {
        memset(bsum, 0, sizeof(bsum));
        memset(binn, 0, sizeof(binn));
        for (int y = 0; y < GRID_H; y++)
            for (int x = 0; x < GRID_W; x++) {
                for (int c = 0; c < 3; c++) {
                    const int32_t v = (int32_t) s_blur[y][x][c] << 8;
                    binn[y / CB][x / CB][c] += v - (int32_t) s_fast[y][x][c];
                    const int32_t f = (int32_t) s_fast[y][x][c] + ((v - (int32_t) s_fast[y][x][c]) >> 1);
                    s_fast[y][x][c] = (uint16_t) f;
                    bsum[y / CB][x / CB][c] += f - (int32_t) s_sm[y][x][c];
                }
            }
        const int32_t tc = (g_params.smooth_tc > 0 ? g_params.smooth_tc : 1) * 256 / 10, tclo = tc >> 2;
        const int32_t a_top = a_min > 128 ? a_min : 128;
        for (int by = 0; by * CB < GRID_H; by++)
            for (int bx = 0; bx * CB < GRID_W; bx++) {
                const int bh = GRID_H - by * CB < CB ? GRID_H - by * CB : CB, bw = GRID_W - bx * CB < CB ? GRID_W - bx * CB : CB;
                int32_t bm = 0, bi = 0;
                for (int c = 0; c < 3; c++) {
                    binn[by][bx][c] /= bh * bw;
                    const int32_t ai = binn[by][bx][c] < 0 ? -binn[by][bx][c] : binn[by][bx][c];
                    if (ai > bi) bi = ai;
                    int32_t m = bsum[by][bx][c] / (bh * bw);
                    if (m < 0) m = -m;
                    if (m > bm) bm = m;
                }
                // Szum sredniej bloku sprzed tej ramki przesuwa progi. Roznica "szybka srednia - wyjscie" ma szum ok. 0,4
                // tej miary, wiec 2 x miara to ok. 5 odchylen; zmiana jednej ramki ma szum ok. 0,8 miary, wiec 5 x miara
                // to ok. 6 odchylen. Blok z drobnymi detalami (pasek zadan, ikony) nie udaje wtedy zmiany obrazu.
                const int32_t bnoise = s_bnz[by][bx];
                s_bnz[by][bx] = bnoise + (bi - bnoise) / 16;
                const int32_t boff = bnoise * 2 * nzk / 30;
                bm = bm > boff ? bm - boff : 0;
                // Skok: srednia zmiana komorek bloku w tej ramce przekracza 2 x smooth_tc plus margines szumu. To zmiana
                // obrazu, wiec komorki bloku, ktore tez sie zmienily, biora nowa wartosc od razu - bez czekania na prog
                // smooth_t2.
                bstep[by][bx] = bi >= 2 * tc + bnoise * 5 * nzk / 30;
                bcoh[by][bx] = (uint8_t) (bm <= tclo ? 0 : (bm >= tc ? a_top : a_top * (bm - tclo) / (tc - tclo)));
            }
    }
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            const int oy = g_params.flip_v ? (GRID_H - 1 - y) : y;
            const int ox = g_params.flip_h ? (GRID_W - 1 - x) : x;
            uint8_t *dst = s_frame + (oy * GRID_W + ox) * 3;
            const uint8_t *lin = s_blur[y][x];
            const uint8_t *in = lin;
            // Wygladzanie: szybka srednia (50 % nowej ramki) sledzi obraz, wyjscie podaza za nia z udzialem zaleznym
            // od tego, jak daleko od niej jest: `smooth` % przy roznicy do smooth_t1 / 4 (sam szum probkowania),
            // rosnaco liniowo do 50 % przy roznicy smooth_t1 i wiekszej. Bez progu, wiec drobne zmiany nie stoja
            // w miejscu, a odcien nie skacze. Roznica chwilowa od smooth_t2 (ciecie sceny, blysk) przechodzi od razu.
            // Udzial jest wspolny dla trzech skladowych komorki, zeby w trakcie dochodzenia nie zmieniac odcienia.
            if (a_min >= 256 || !s_sm_valid) {
                s_nz[y][x] = 0;
                for (int c = 0; c < 3; c++) s_sm[y][x][c] = s_fast[y][x][c] = (uint16_t) (in[c] << 8);
            } else {
                int32_t dmax = 0, fmax = 0, emax = 0, fd[3];
                for (int c = 0; c < 3; c++) {
                    const int32_t v = (int32_t) in[c] << 8;
                    const int32_t d = v - (int32_t) s_sm[y][x][c];
                    const int32_t ad = d < 0 ? -d : d;
                    if (ad > dmax) dmax = ad;
                    // Szum = to, czym zmiana tej komorki rozni sie od sredniej zmiany jej bloku: miganie calego obrazu,
                    // ciecie czy sciemnienie zmieniaja blok zgodnie i szumem nie sa. (Szybka srednia zawiera juz polowe
                    // tej ramki, wiec roznica wzgledem poprzedniej jest 2x wieksza.)
                    const int32_t e = (v - (int32_t) s_fast[y][x][c]) * 2 - (sm_on ? binn[y / CB][x / CB][c] : 0);
                    const int32_t ae = e < 0 ? -e : e;
                    if (ae > emax) emax = ae;
                    fd[c] = (int32_t) s_fast[y][x][c] - (int32_t) s_sm[y][x][c];      // szybka srednia policzona wyzej
                    const int32_t afd = fd[c] < 0 ? -fd[c] : fd[c];
                    if (afd > fmax) fmax = afd;
                }
                // Szum komorki sprzed tej ramki przesuwa progi: roznica "szybka srednia - wyjscie" liczy sie dopiero powyzej
                // ok. 3 odchylen szumu, skok chwilowy powyzej ok. 4. Komorka z czystym obrazem (szum 1-2 poziomy) zachowuje
                // sie jak dotad; komorka z drobnym detalem jest usredniana w czasie, dopoki nie zmieni sie naprawde.
                const int32_t nz = s_nz[y][x];
                {
                    const int32_t nn = nz + (emax - nz) / 16;
                    s_nz[y][x] = (uint16_t) (nn > 60000 ? 60000 : nn);
                }
                const int32_t off1 = nz * nzk / 10;
                fmax = fmax > off1 ? fmax - off1 : 0;
                const int32_t draw = dmax;
                dmax -= off1 * 2;
                // przy skoku bloku: komorka, ktorej wlasna zmiana wystaje ponad jej szum (zaszumiona komorka z drobnym
                // detalem nie lapie przy okazji przypadkowej probki)
                if (dmax >= t2 || (bstep[y / CB][x / CB] && draw >= (t1 >> 1) + off1)) {
                    for (int c = 0; c < 3; c++) s_sm[y][x][c] = s_fast[y][x][c] = (uint16_t) (in[c] << 8);
                } else {
                    const int32_t tlo = t1 >> 1, a_hi = a_min > 128 ? a_min : 128;   // pojedyncza komorka: od polowy smooth_t1
                    // im wiekszy szum komorki (powyzej 4 poziomow), tym dluzsze usrednianie tego, co nie jest zmiana
                    int32_t a_lo = a_min;
                    if (nzk > 0 && nz > 1024) {
                        // udzial nowej ramki maleje z kwadratem szumu (szum 12 poziomow: 1/4, 20 poziomow: 1/9), tak zeby
                        // to, co zostaje na wyjsciu, bylo podobnie male niezaleznie od tego, jak szumi komorka
                        const int32_t k = 2048 * 256 / (2048 + (nz - 1024));     // Q8, 256 przy szumie 4 poziomow
                        a_lo = a_min * k / 256 * k / 256;
                        if (a_lo < 1) a_lo = 1;
                    }
                    int32_t a = a_lo;
                    if (fmax >= t1) a = a_hi;
                    else if (fmax > tlo) a = a_lo + (a_hi - a_lo) * (fmax - tlo) / (t1 - tlo);
                    if (bcoh[y / CB][x / CB] > a) a = bcoh[y / CB][x / CB];        // zgodna zmiana calego bloku
                    for (int c = 0; c < 3; c++) s_sm[y][x][c] = (uint16_t) ((int32_t) s_sm[y][x][c] + ((fd[c] * a + 128) >> 8));
                }
            }
            // Martwa strefa: wyjscie zmienia sie dopiero, gdy wygladzona wartosc ktorejkolwiek skladowej odejdzie od
            // niego o wiecej niz `hold`; wtedy aktualizowane sa wszystkie trzy naraz (osobno dla kazdej skladowej
            // kanaly przeskakiwaly w roznych chwilach i na moment zmienial sie odcien).
            uint8_t *h = s_outh[y][x];
            {
                int32_t dqm = 0;
                for (int c = 0; c < 3; c++) {
                    const int32_t dq = (int32_t) s_sm[y][x][c] - ((int32_t) h[c] << 8);
                    const int32_t adq = dq < 0 ? -dq : dq;
                    if (adq > dqm) dqm = adq;
                }
                // zaszumiona komorka (drobny detal): martwa strefa szersza o 1/8 jej szumu (przy smooth_nz 30), zeby resztka
                // szumu po usrednieniu nie przestawiala wyjscia o pojedyncze poziomy
                const int32_t dbc = db + (int32_t) s_nz[y][x] * nzk / 240;
                if (dqm > dbc || !s_sm_valid) {
                    for (int c = 0; c < 3; c++) h[c] = (uint8_t) (((int32_t) s_sm[y][x][c] + 128) >> 8);
                }
            }
            color_cell(h, dst, m, identity, sat_pct, pq, inv_white);
        }
    }
    {
        g_crumb = 5;
        const int64_t tf3 = esp_timer_get_time();
        s_fin_us[0] = (uint32_t) (tf1 - tf0); s_fin_us[1] = (uint32_t) (tf2 - tf1); s_fin_us[2] = (uint32_t) (tf3 - tf2);
    }
    memset(s_acc[bank], 0, sizeof(s_acc[bank]));
    memset(s_cnt[bank], 0, sizeof(s_cnt[bank]));
    s_stats.frames_done++;
    s_stats.samples_last = s_dig_on ? samples >> 8 : samples;   // tryb cyfrowy: wagi po 256 na probke
    {
        static uint32_t dead = 0;
        const bool alive = s_adc_alive;
        s_adc_alive = false;
        if (alive) s_adc_worked = true;
        dead = (s_dig_on && s_dig_ready && s_plan != NULL && !s_hold && !s_hprof.active && s_stats.plan_lines > 0 && !alive)
               ? dead + 1 : 0;
        if (dead >= 180 && dead % 180 == 0) adc_dead("3 s bez probek z przetwornika");   // ponawiane co 3 s (adc_dead ma wlasne ograniczenia)
    }
    s_stats.cells_empty_last = empty;
    for (int c = 0; c < 3; c++) {
        s_stats.raw_avg[c] = nsum[c] ? (float) sum[c] / (float) nsum[c] : 0;
        s_stats.raw_min[c] = nsum[c] ? (float) mn[c] : 0;
        s_stats.raw_max[c] = (float) mx[c];
    }
    s_stats.d_mapped = s_mapped;
    s_stats.d_late = s_late;
    s_stats.d_isr_lines = s_isr_lines;
    s_stats.isr_max_us = (float) s_isr_max_cyc / (float) s_cyc_per_us;
    s_stats.isr_late_max_us = (float) g_isr_late_max_cyc / (float) s_cyc_per_us;
    s_stats.frame_rate = sync_frame_rate();
    if (s_rec_i < s_rec_n) memcpy(s_rec[s_rec_i++], s_frame, PIX_BYTES_MAX);
    if (s_tr_i < s_tr_n) {
        uint32_t sum[3] = {0, 0, 0}, n = 0;
        for (int y = GRID_H / 4; y < GRID_H - GRID_H / 4; y++)
            for (int x = GRID_W / 4; x < GRID_W - GRID_W / 4; x++, n++)
                for (int c = 0; c < 3; c++) sum[c] += s_frame[(y * GRID_W + x) * 3 + c];
        for (int c = 0; c < 3; c++) s_tr[s_tr_i][c] = (uint8_t) (n ? (sum[c] + n / 2) / n : 0);
        s_tr_i++;
    }
    {
        // srednia jasnosc ramki; skok o > 60 wzgledem poprzedniej = podejrzany blysk -> zachowaj obie ramki
        uint32_t sum = 0;
        for (int i = 0; i < GRID_BYTES; i++) sum += s_frame[i];
        const uint32_t mean = sum / GRID_BYTES;
        if (s_stats.frames_done > 10 && mean > s_prev_mean + 60) {
            s_glitch_count++;
            s_glitch_mean = mean;
            s_glitch_prev_mean = s_prev_mean;
            memcpy(s_glitch_before, s_last_frame, PIX_BYTES_MAX);
            memcpy(s_glitch_frame, s_frame, PIX_BYTES_MAX);
            for (int x = 0; x < GRID_W; x++) {
                uint32_t a = 0, b = 0;
                for (int y = 0; y < GRID_H; y++)
                    for (int c = 0; c < 3; c++) { a += s_frame[(y * GRID_W + x) * 3 + c]; b += s_last_frame[(y * GRID_W + x) * 3 + c]; }
                if (a > b + 100 * GRID_H * 3) s_glitch_col[x]++;
            }
        }
        s_prev_mean = mean;
    }
    memcpy(s_last_frame, s_frame, PIX_BYTES_MAX);
    // Wysylaj tylko przy stabilnej synchronizacji: bez planu (brak/smieciowy sygnal) nic nie idzie,
    // a po jego odzyskaniu dopiero od 5. kolejnej ramki (uklad wideo bez wejscia HDMI daje pojedyncze VSync).
    s_sm_valid = s_plan != NULL && s_good_frames >= 3;
    if (s_plan == NULL) { s_good_frames = 0; return; }
    if (s_good_frames < 5) { s_good_frames++; return; }
    if (g_params.pattern == 0) spi_out_submit(s_frame);
}

void capture_regs(void)
{
    printf("I2S0: conf %08lx conf2 %08lx clkm %08lx sample_rate %08lx fifo_conf %08lx int_raw %08lx\n",
           (unsigned long) I2S0.conf.val, (unsigned long) I2S0.conf2.val, (unsigned long) I2S0.clkm_conf.val,
           (unsigned long) I2S0.sample_rate_conf.val, (unsigned long) I2S0.fifo_conf.val, (unsigned long) I2S0.int_raw.val);
    printf("SYSCON: saradc_ctrl %08lx ctrl2 %08lx fsm %08lx patt1[0] %08lx\n", (unsigned long) SYSCON.saradc_ctrl.val,
           (unsigned long) SYSCON.saradc_ctrl2.val, (unsigned long) SYSCON.saradc_fsm.val, (unsigned long) SYSCON.saradc_sar1_patt_tab[0]);
    printf("SENS: read_ctrl %08lx meas_start1 %08lx meas_wait2 %08lx\n", (unsigned long) SENS.sar_read_ctrl.val,
           (unsigned long) SENS.sar_meas_start1.val, (unsigned long) SENS.sar_meas_wait2.val);
    printf("filtry ramki: srednie %lu us, rozmycie %lu us, wygladzanie i kolor %lu us; okres linii %.4f us\n",
           (unsigned long) s_fin_us[0], (unsigned long) s_fin_us[1], (unsigned long) s_fin_us[2],
           (float) s_blk_Tq8 / 256.0f / (float) s_cyc_per_us);
}

static void capture_task(void *arg)
{
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) == 0) {
            s_fin_busy = false;            // brak VSync: nic nie czeka na policzenie
            maybe_rebuild_plan();          // np. zanik sygnalu -> plan wylaczony
            continue;
        }
        maybe_rebuild_plan();
        finalize_frame(s_bank ^ 1);        // bank, do ktorego przerwanie juz nie pisze (s_fin_busy blokuje zamiane)
        g_crumb = 0;
        s_fin_busy = false;
    }
}

TaskHandle_t capture_init(void)
{
    s_cyc_per_us = (uint32_t) (esp_clk_cpu_freq() / 1000000);
    memset(s_frame, 0, sizeof(s_frame));
    // Duze bufory sa na stercie: statyczny segment danych ESP32 ma ok. 180 kB, a sterta korzysta tez z reszty RAM.
    s_rec = heap_caps_calloc(REC_MAX, PIX_BYTES_MAX, MALLOC_CAP_8BIT);
    s_glitch_frame = heap_caps_calloc(1, PIX_BYTES_MAX, MALLOC_CAP_8BIT);
    s_glitch_before = heap_caps_calloc(1, PIX_BYTES_MAX, MALLOC_CAP_8BIT);
    s_th = heap_caps_calloc(1, FIELD16_BYTES, MALLOC_CAP_8BIT);
    s_tv = heap_caps_calloc(1, FIELD16_BYTES, MALLOC_CAP_8BIT);
    s_tt = heap_caps_calloc(1, FIELD16_BYTES, MALLOC_CAP_8BIT);
    s_cnt = heap_caps_calloc(2, sizeof(*s_cnt), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);   // pisze do niego przerwanie
    if (!s_rec || !s_glitch_frame || !s_glitch_before || !s_th || !s_tv || !s_tt || !s_cnt) {
        ESP_LOGE(TAG, "brak pamieci na bufory (wolne %u B)", (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT));
        abort();
    }
    ESP_LOGI(TAG, "bufory na stercie przydzielone, wolne %u B", (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT));
    build_lut();
    adc_init();
    xTaskCreatePinnedToCore(capture_task, "capture", 6144, NULL, 5, &s_task, 0);   // ponizej konsoli i wysylki SPI
    return s_task;
}

void capture_digidx_hist(const uint16_t **h, const int32_t **ref) { *h = &s_dp_hist[0][0]; *ref = s_dp_ref; }

void capture_digidx_start(float restart_us)
{
    s_dp_idx_req = true;
    capture_hprof_start(restart_us, 16, 0, false);
}

void capture_hprof_start(float step_us, uint32_t per_step, int ch, bool seq_test)
{
    s_hprof.active = false;
    s_hprof.ch_adc = ch == 0 ? ADC_CH_R : (ch == 2 ? ADC_CH_B : ADC_CH_G);
    s_hprof.seq_test = seq_test;
    memset(s_hprof.avg, 0, sizeof(s_hprof.avg));
    memset(s_hprof.dur_us10, 0, sizeof(s_hprof.dur_us10));
    s_hprof.step_us = step_us;
    s_hprof.step_cyc = (uint32_t) (step_us * (float) s_cyc_per_us);
    s_hprof.per_step = per_step ? per_step : 16;
    s_hprof.idx = 0; s_hprof.acc = 0; s_hprof.n = 0; s_hprof.dur = 0;
    memset(s_dp_acc, 0, sizeof(s_dp_acc)); memset(s_dp_n, 0, sizeof(s_dp_n)); s_dp_lines = 0;
    s_dp_inv_q16 = s_hprof.step_cyc ? 65536u / s_hprof.step_cyc : 0;
    s_dp_delay_cyc = (uint32_t) (g_params.dig_prof_delay_us10) * s_cyc_per_us / 10;
    s_dp_idx = s_dp_idx_req; s_dp_idx_req = false;
    memset(s_dp_dup, 0, sizeof(s_dp_dup)); s_dp_tr_sum = 0;
    memset(s_dp_mn, 0xFF, sizeof(s_dp_mn)); memset(s_dp_mx, 0, sizeof(s_dp_mx));
    memset(s_dp_hist, 0, sizeof(s_dp_hist)); s_dp_ref[0] = s_dp_ref[1] = s_dp_ref[2] = -1; s_dp_tr_ref = INT32_MIN;
    if (s_dp_idx) s_dp_delay_cyc = (uint32_t) (step_us * (float) s_cyc_per_us);
    s_hprof.active = true;
}

const hprof_t *capture_hprof_get(void) { return &s_hprof; }

void capture_get_stats(capture_stats_t *out)
{
    *out = s_stats;
    out->dig_on = s_dig_on;
    out->blk_on = s_blk_mode;
    out->blk_ovf = s_blk_ovf;
    out->lb_top = s_lb_top; out->lb_bot = s_lb_bot; out->lb_vstart = s_lb_vstart; out->lb_vlines = s_lb_vlines;
    out->blk_skipped = s_dig_shifted;
    memcpy(out->col_cnt, s_col_cnt, sizeof(s_col_cnt));
    memcpy(out->col_late, s_col_late, sizeof(s_col_late));
    memcpy(out->col_bad, s_col_bad, sizeof(s_col_bad));
    out->d_bad_start = s_bad_start;
    out->d_bad_long = s_bad_long;
    out->conv_us = s_conv_us;
    out->d_late = s_late;
    out->d_mapped = s_mapped;
    out->d_isr_lines = s_isr_lines;
    out->isr_max_us = (float) s_isr_max_cyc / (float) s_cyc_per_us;
    out->isr_pre_min_us = s_isr_pre_min == UINT32_MAX ? 0 : (float) s_isr_pre_min / (float) s_cyc_per_us;
    out->isr_pre_max_us = (float) s_isr_pre_max / (float) s_cyc_per_us;
    out->isr_late_max_us = (float) g_isr_late_max_cyc / (float) s_cyc_per_us;
    out->start_ovh_us = (float) s_start_ovh_cyc / (float) s_cyc_per_us;
}
void capture_get_frame(uint8_t *out) { memcpy(out, s_last_frame, PIX_BYTES_MAX); }

uint32_t capture_get_glitch(uint8_t *before, uint8_t *after, uint32_t *mean_before, uint32_t *mean_after, uint32_t *cols)
{
    memcpy(before, s_glitch_before, PIX_BYTES_MAX);
    memcpy(after, s_glitch_frame, PIX_BYTES_MAX);
    *mean_before = s_glitch_prev_mean;
    *mean_after = s_glitch_mean;
    memcpy(cols, s_glitch_col, sizeof(s_glitch_col));
    return s_glitch_count;
}
void capture_reconfigure(void) { s_reconf = true; }

void capture_pause(void)
{
    s_hold = true;    // przerwanie i tak dziala z IRAM/DRAM; pauza tylko dla porzadku
    s_stats.d_resync++;
}

void capture_resume(void) { s_hold = false; }
void capture_hold(bool on) { s_hold = on; }

// Test powtarzalnosci fazy po restarcie: 256 prob, czas do 1., 2. i 3. slowa oraz kanaly pierwszych probek.
int capture_digsync(digsync_t *r)
{
    const bool temp = !s_dig_on;
    s_hold = true;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (temp && !dig_start()) { s_hold = false; return -1; }
    memset(r, 0, sizeof(*r));
    for (int w = 0; w < 3; w++) r->t_min[w] = 0xFFFFFFFF;
    for (int i = 0; i < 256; i++) {
        // losowy odstep, zeby restart wypadal w roznych fazach wolnobieznych zegarow
        const uint32_t gap = (esp_cpu_get_cycle_count() & 0x3FF) + 200;
        const uint32_t tg = esp_cpu_get_cycle_count();
        while ((esp_cpu_get_cycle_count() - tg) < gap) { }
        portDISABLE_INTERRUPTS();
        I2S0.int_clr.val = 0xFFFFFFFF;
        const uint32_t t0 = esp_cpu_get_cycle_count();
        dig_restart();
        uint32_t words[3] = {0, 0, 0}, tt[3] = {0, 0, 0};
        int k = 0;
        while (k < 3 && (esp_cpu_get_cycle_count() - t0) < 30 * s_cyc_per_us) {
            I2S0.int_clr.val = 1;
            if (!I2S0.int_raw.rx_take_data) continue;
            words[k] = I2S0.fifo_rd;
            tt[k] = esp_cpu_get_cycle_count() - t0;
            k++;
        }
        portENABLE_INTERRUPTS();
        if (k < 3) { r->timeouts++; continue; }
        for (int w = 0; w < 3; w++) {
            if (tt[w] < r->t_min[w]) r->t_min[w] = tt[w];
            if (tt[w] > r->t_max[w]) r->t_max[w] = tt[w];
            r->t_sum[w] += tt[w];
        }
        r->n++;
        const uint32_t c0 = (words[0] >> 28) & 0xF, c1 = (words[0] >> 12) & 0xF;
        if (c0 == ADC_CH_R) r->first_r++;
        if (c0 == ADC_CH_R && c1 == ADC_CH_G) r->first_rg++;
        if (i < 4) { r->sample_w[i][0] = words[0]; r->sample_w[i][1] = words[1]; r->sample_w[i][2] = words[2]; }
    }
    r->cyc_per_us = s_cyc_per_us;
    if (temp) dig_stop();
    s_reconf = true;
    s_hold = false;
    return 0;
}

// Pomiar rzeczywistego tempa konwersji kontrolera cyfrowego i zachowania flagi FIFO.
// seq: kanaly kolejnych polowek slow ('R','G','B','?'), czytanych co 1 us po pierwszym slowie.
int capture_digrate(digrate_t *r)
{
    const bool temp = !s_dig_on;
    s_hold = true;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (temp && !dig_start()) { s_hold = false; return -1; }
    memset(r, 0, sizeof(*r));
    // 1) flaga: po 6 us od resetu w FIFO jest kilka slow; kasujemy flage i patrzymy, czy wraca bez nowego odczytu
    I2S0.int_clr.val = 0xFFFFFFFF;
    uint32_t t0 = esp_cpu_get_cycle_count();
    i2s_ll_rx_reset_fifo(&I2S0);
    while ((esp_cpu_get_cycle_count() - t0) < 6 * s_cyc_per_us) { }
    r->flag_before_clear = I2S0.int_raw.rx_take_data;
    I2S0.int_clr.val = 1;
    r->flag_after_clear = I2S0.int_raw.rx_take_data;
    (void) I2S0.fifo_rd;
    I2S0.int_clr.val = 1;
    r->flag_after_read = I2S0.int_raw.rx_take_data;
    // 2) tempo: reset, czekaj na pierwsze slowo, czytaj jedno slowo na 1 us
    I2S0.int_clr.val = 0xFFFFFFFF;
    t0 = esp_cpu_get_cycle_count();
    i2s_ll_rx_reset_fifo(&I2S0);
    I2S0.int_clr.val = 0xFFFFFFFF;
    while ((esp_cpu_get_cycle_count() - t0) < 8 * s_cyc_per_us && !I2S0.int_raw.rx_take_data) { }
    const uint32_t base = esp_cpu_get_cycle_count();
    uint32_t last = 0xFF, sum[3] = {0, 0, 0}, cnt[3] = {0, 0, 0};
    int k = 0;
    for (int i = 0; i < DIGRATE_WORDS; i++) {
        while ((esp_cpu_get_cycle_count() - base) < (uint32_t) i * s_cyc_per_us + s_cyc_per_us / 5) { }
        const uint32_t w = I2S0.fifo_rd;
        for (int h = 0; h < 2; h++) {
            const uint32_t smp = h == 0 ? (w >> 16) : (w & 0xFFFF);      // starsza polowka = wczesniejsza probka
            const uint32_t ch = (smp >> 12) & 0xF;
            const uint8_t ci = s_dig_ch[ch];
            r->seq[k++] = ci == 0 ? 'R' : (ci == 1 ? 'G' : (ci == 2 ? 'B' : '?'));
            if (ch != last) { r->changes++; last = ch; if (ci < 3) { sum[ci] += smp & 0xFFF; cnt[ci]++; } }
        }
    }
    r->seq[k] = 0;
    // 3) ile slow oddaje FIFO w 40 us przy odczycie sterowanym flaga, i ile z nich niesie nowa probke
    {
        I2S0.int_clr.val = 0xFFFFFFFF;
        i2s_ll_rx_reset_fifo(&I2S0);
        const uint32_t tb = esp_cpu_get_cycle_count();
        uint32_t words = 0, fresh = 0, lastc = 0xFF;
        while ((esp_cpu_get_cycle_count() - tb) < 40 * s_cyc_per_us) {
            I2S0.int_clr.val = 1;
            if (!I2S0.int_raw.rx_take_data) continue;
            const uint32_t w = I2S0.fifo_rd;
            words++;
            const uint32_t c1 = (w >> 28) & 0xF, c2 = (w >> 12) & 0xF;
            if (c1 != lastc) fresh++;
            if (c2 != c1) fresh++;
            lastc = c2;
        }
        r->flag_words = words;
        r->flag_fresh = fresh;
    }
    for (int c = 0; c < 3; c++) r->mean[c] = cnt[c] ? sum[c] / cnt[c] : 0;
    r->clk_div = HAL_FORCE_READ_U32_REG_FIELD(SYSCON.saradc_ctrl, sar_clk_div);
    r->standby = HAL_FORCE_READ_U32_REG_FIELD(SYSCON.saradc_fsm, standby_wait);
    if (temp) dig_stop();
    s_reconf = true;
    s_hold = false;
    return 0;
}

// Przywraca rejestry kontrolera RTC po pracy kontrolera cyfrowego.
static void adc_rtc_restore(void)
{
    const adc_channel_t chs[3] = {ADC_CH_R, ADC_CH_G, ADC_CH_B};
    SENS.sar_meas_wait2.force_xpd_sar = SENS_FORCE_XPD_SAR_PU;
    adc_ll_hall_disable();
    adc_ll_amp_disable();
    for (int i = 0; i < 3; i++) adc_oneshot_ll_set_atten(ADC_UNIT_1, chs[i], ADC_ATTEN_DB_2_5);
    adc_oneshot_ll_set_output_bits(ADC_UNIT_1, ADC_BITWIDTH_12);
    adc_oneshot_ll_output_invert(ADC_UNIT_1, true);
    adc_ll_set_controller(ADC_UNIT_1, ADC_LL_CTRL_RTC);
    adc_apply_clock_and_bench();
}

// Test: skanowanie R,G,B kontrolerem cyfrowym 2 MS/s, DMA wylaczone, po resecie FIFO odczyt slow z I2S0.fifo_rd
// po roznych opoznieniach. Wynik mowi, czy i po jakim czasie w FIFO sa swieze probki (kanal + 12 bitow).
int capture_digtest(digtest_row_t *rows, int max_rows)
{
    static const uint16_t delays_us10[] = {5, 10, 15, 20, 30, 50, 100, 400};
    int n = 0;
    if (s_dig_on) return -3;              // kontroler cyfrowy juz pracuje (adc_mode 1)
    s_hold = true;
    vTaskDelay(pdMS_TO_TICKS(20));
    adc_continuous_handle_t h = NULL;
    adc_continuous_handle_cfg_t hcfg = {.max_store_buf_size = 1024, .conv_frame_size = 256};
    if (adc_continuous_new_handle(&hcfg, &h) != ESP_OK) { s_hold = false; return -1; }
    adc_digi_pattern_config_t pat[3] = {
        {.atten = ADC_ATTEN_DB_2_5, .channel = ADC_CH_R, .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12},
        {.atten = ADC_ATTEN_DB_2_5, .channel = ADC_CH_G, .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12},
        {.atten = ADC_ATTEN_DB_2_5, .channel = ADC_CH_B, .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12},
    };
    adc_continuous_config_t cfg = {
        .pattern_num = 3, .adc_pattern = pat, .sample_freq_hz = 2000000,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1, .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    int rc = -2;
    if (adc_continuous_config(h, &cfg) == ESP_OK && adc_continuous_start(h) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const uint32_t int_ena = I2S0.int_ena.val;
        I2S0.int_ena.val = 0;                 // zadnych przerwan I2S na czas testu
        i2s_ll_rx_stop_link(&I2S0);           // zatrzymaj liste DMA
        i2s_ll_enable_dma(&I2S0, false);      // I2S_DSCR_EN = 0: FIFO czyta CPU
        I2S0.fifo_conf.rx_data_num = 0;       // prog: flaga rx_take_data, gdy w FIFO jest wiecej niz 0 slow
        for (size_t t = 0; t < sizeof(delays_us10) / sizeof(delays_us10[0]) && n < max_rows; t++) {
            digtest_row_t *r = &rows[n++];
            // okres odczytu slow: 0,9 / 1,0 / 1,1 us w kolejnych probach (ktory daje czyste pary?)
            const uint32_t period = (uint32_t) (9 + (t % 3)) * s_cyc_per_us / 10;
            r->delay_us10 = (uint16_t) (9 + (t % 3));
            I2S0.int_clr.val = 0xFFFFFFFF;
            const uint32_t t0 = esp_cpu_get_cycle_count();
            i2s_ll_rx_reset_fifo(&I2S0);
            I2S0.int_clr.val = 0xFFFFFFFF;
            uint32_t tf = 0;
            while ((esp_cpu_get_cycle_count() - t0) < 8 * s_cyc_per_us) {      // max 8 us na pierwsze slowo
                if (I2S0.int_raw.rx_take_data) { tf = esp_cpu_get_cycle_count(); break; }
            }
            r->int_raw_before = tf ? (tf - t0) * 100 / s_cyc_per_us : 0xFFFFFFFF;   // czas do pierwszego slowa, 0,01 us
            const uint32_t base = tf ? tf : esp_cpu_get_cycle_count();
            for (int i = 0; i < 4; i++) {
                while ((esp_cpu_get_cycle_count() - base) < (uint32_t) i * period + s_cyc_per_us / 5) { }
                r->word[i] = I2S0.fifo_rd;
            }
            r->int_raw_after = I2S0.int_raw.val;
        }
        I2S0.int_clr.val = 0xFFFFFFFF;
        I2S0.int_ena.val = int_ena;
        adc_continuous_stop(h);
        rc = n;
    }
    adc_continuous_deinit(h);
    adc_rtc_restore();
    s_reconf = true;                          // plan i LUT od nowa (pomiar konwersji mogl sie zmienic)
    s_hold = false;
    return rc;
}

void capture_record(uint32_t n) { s_rec_i = 0; s_rec_n = n > REC_MAX ? REC_MAX : n; }
void capture_trace(uint32_t n) { s_tr_n = 0; s_tr_i = 0; s_tr_n = n > TRACE_MAX ? TRACE_MAX : n; }
uint32_t capture_trace_done(void) { return s_tr_i >= s_tr_n ? s_tr_n : 0; }
const uint8_t *capture_trace_frame(uint32_t i) { return s_tr[i < TRACE_MAX ? i : 0]; }
uint32_t capture_record_done(void) { return s_rec_i >= s_rec_n ? s_rec_n : 0; }
const uint8_t *capture_record_frame(uint32_t i) { return s_rec[i < REC_MAX ? i : 0]; }
