#pragma once
#include <stdint.h>
#include <stdbool.h>

// Parametry strojone z konsoli, trzymane w NVS.
typedef struct {
    uint32_t spi_hz;        // zegar SPI do BK7238 (domyslnie 2 MHz)
    uint32_t adc_hz;        // (nieuzywane od v8: probkowanie z przerwania HSync)
    int32_t  black_mv;      // poziom czerni w mV (wygaszanie ~545, czern wideo ok. 560)
    int32_t  white_mv;      // poziom bieli w mV (zmierzone na pelnym 255: kod ~3830 = 1400)
    int32_t  hoff_us10;     // przesuniecie poczatku aktywnej linii wzgledem HSync, w 0,1 us (moze byc ujemne)
    int32_t  hblank_pct10;  // udzial wygaszania poziomego w linii, w 0,1 % (np. 127 = 12,7 %)
    int32_t  vstart;        // pierwsza aktywna linia (0 = z tabeli formatow / auto)
    int32_t  vlines;        // liczba aktywnych linii (0 = z tabeli formatow / auto)
    int32_t  cut;           // odciecie czerni 0..255
    int32_t  gain_pct;      // wzmocnienie 100 = 1.0
    uint8_t  gamma;         // 1 = korekcja gamma 2.2 na wyjsciu
    uint8_t  flip_h;        // odbicie poziome siatki
    uint8_t  flip_v;        // odbicie pionowe siatki
    uint8_t  pattern;       // 0 = wideo, 1 = tecza testowa, 2 = pasy kolorow (test mapowania)
    uint8_t  vsync_neg;     // 1 = VSync aktywny niskim stanem (zbocze opadajace = poczatek ramki)
    uint8_t  hsync_neg;     // 1 = HSync aktywny niskim stanem
    uint8_t  hdmi_reset_pulse; // 1 = na starcie krotki impuls resetu GSV2002
    uint8_t  rep;           // ile razy probkowac kazda komorke (kolor x kolumna) w ramce, 1..4
    uint8_t  sar_div;       // dzielnik zegara SAR ADC (kontroler RTC), 1 = domyslny sterownika
    uint8_t  sar_cycle;     // liczba cykli probkowania SAR (0 = nie zmieniaj rejestru)
    uint8_t  rtc_xtal;      // 1 = RTC_FAST z XTAL/4 (10 MHz) zamiast RC 8 MHz: szybszy SAR ADC
    uint8_t  hdmi_rx_inv;   // 1 = odwroc polaryzacje RX UART2 (eksperyment, gdy linia czyta same zera)
    uint8_t  adc_mode;      // 0 = kontroler RTC (pojedyncze konwersje), 1 = kontroler cyfrowy + FIFO I2S (ciagle 2 MS/s)
    uint8_t  dig_step;      // tryb 1: co ktora linia jest probkowana (>= 2)
    uint8_t  dig_atten;     // tryby cyfrowe: zakres wejsciowy przetwornika (0 = 0 dB, 1 = 2,5 dB, 2 = 6 dB, 3 = 11 dB)
    uint8_t  dig_atten2;    // tryb 2: drugi zakres wejsciowy uzywany na przemian z pierwszym (255 = tylko jeden zakres)
    int32_t  black2_mv;     // poziomy czerni i bieli w drugim zakresie (te same jednostki co black_mv / white_mv)
    int32_t  white2_mv;
    uint8_t  dig_block;     // tryb 2: liczba kolejnych linii w bloku (1..8); probkowane jest K z kazdych K+1 linii
    uint8_t  dig_clkdiv;    // tryb 1: dzielnik zegara SAR (0 = domyslne 16 -> 5 MHz, konwersja ok. 2,5 us)
    uint8_t  dig_standby;   // tryb 1: czas oczekiwania automatu (0 = domyslne 100)
    uint8_t  dig_phases;    // tryb 1: liczba faz restartu (rownomierne pokrycie komorki probkami kazdego koloru)
    int32_t  dig_prof_delay_us10; // tryb 1, hprof: opoznienie restartu po wejsciu w przerwanie (test liniowosci osi czasu)
    int32_t  dig_guard_us10; // tryb 1: margines przy krawedziach aktywnej linii, w 0,1 us (probki blizej krawedzi sa odrzucane)
    int32_t  dig_d_ns;      // tryb 1: czas od restartu przetwornika do konwersji nr 0, w ns (z digidx na gradiencie)
    int32_t  dig_dt_ps;     // tryb 1: odstep konwersji w ps; 0 = z czestotliwosci probkowania (500000)
    int32_t  smooth_pct;    // wygladzanie w czasie: najmniejszy udzial zmiany na ramke, w % (100 = wylaczone)
    uint8_t  grid_w;        // liczba kolumn pola (4..48; tryb RTC zawsze 16)
    uint8_t  grid_h;        // liczba wierszy pola (4..27; tryb RTC do 18)
    uint8_t  lbox;          // tryb 2: wykrywanie czarnych pasow gora/dol i mapowanie pola na sam obraz (1 = wlaczone)
    uint8_t  lbox_thr;      // jasnosc (0..255), powyzej ktorej linia nalezy do obrazu
    uint8_t  blur_in;       // domieszka rozmycia w obu kierunkach (jedno przejscie 1-2-1), w % (0 = brak)
    uint8_t  blur_dir;      // 1 = rozmycie tylko wzdluz najblizszej krawedzi ekranu (bez mieszania w glab), 0 = w obu kierunkach
    uint8_t  blur;          // rozmycie przestrzenne pola: liczba przejsc jadra 1-2-1 (0 = brak; odchylenie sqrt(n/2) komorki)
    int32_t  hold_x10;      // martwa strefa wyjscia w 0,1 poziomu (10 = zmiana dopiero powyzej 1 poziomu; 0 = brak)
    uint8_t  rsv_med;       // nieuzywane (dawniej smooth_med); pole zostaje, zeby uklad zapisanych ustawien sie nie zmienil
    int32_t  smooth_t1;     // roznica (0..255) miedzy obrazem a wyjsciem, od ktorej wyjscie nadaza z udzialem 50 %
    int32_t  smooth_tc;     // zgodna zmiana bloku 8x8 komorek: srednia roznica (w 0,1 poziomu), od ktorej blok nadaza z udzialem 50 %
    int32_t  smooth_t2;     // od tej roznicy zmiana przechodzi od razu
    int32_t  smooth_nz;     // progi wygladzania przesuwane o (smooth_nz / 10) x zmierzony szum komorki; 0 = bez tego
    int32_t  dig_pre_us10;  // tryb 1: zapas na czas trwania restartu przetwornika, w 0,1 us
    int32_t  dig_span_ns;   // tryb 1: zakres przesuniec restartu miedzy liniami w ns; 0 = odstep probek jednego koloru
    uint8_t  dig_limit;     // tryb 1: dlugosc serii konwersji (rejestr max_meas_num); 0 = ustawienie sterownika
    int32_t  hdmi_baud;     // predkosc UART2: MCU ukladu HDMI nadaje 9600 bps (konfiguracja fabryczna mowi 115200, ale przebieg linii ma bit 104 us)
    uint8_t  hdr_mode;      // 0 = automatycznie z UART2, 1 = zawsze SDR, 2 = zawsze HDR
    int32_t  hdr_gain_pct;  // mnoznik nasycenia w trybie HDR, w % (fabrycznie sw_amb_hdr_gain = 2 -> 200)
    int32_t  sat_pct;       // mnoznik nasycenia w SDR, w % (100 = bez zmian)
    int32_t  mat[9];        // macierz kolorow 3x3 w 1/1000: wyjscie R = (m0*R + m1*G + m2*B)/1000 itd.
    int32_t  lat_us10;      // zbocze HSync -> wejscie w przerwanie + start konwersji -> chwila probkowania; zmierzone 5,6 us (hprof)
} params_t;

extern params_t g_params;

void params_load(void);
void params_save(void);
void params_defaults(void);
bool params_set(const char *name, const char *value);  // zwraca false, gdy nieznana nazwa
void params_print(void);
