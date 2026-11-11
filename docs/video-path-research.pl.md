# Tor wideo w HDMI Sync Box (Action/Eurodomest): co mówi dokumentacja

Zebrane 2026-11-09 z sieci i z lokalnych źródeł (ESP-IDF 5.3.1, ESP32 TRM v5.9). Dotyczy pytania:
jak "rozmawiać" z układami wideo, jak przetwarzać sygnał na ESP32 i jak słać dalej do ESPHome.

## 1. Łańcuch sprzętowy (teardown 1. generacji + datasheety)

```
HDMI in ──► GSV2002 (repeater 1-in-2-out, HDCP 2.2, 4K60) ──► HDMI out do TV
                     └──► kopia HDMI ──► konwerter HDMI→VGA ──► analogowe R,G,B (0,7 Vpp wg VESA VSIS)
                                             (1. gen: Capstone CS5210)      + HSYNC, VSYNC (TTL)
                                                                                  │
                                           ESP32: ADC1 CH7/CH4/CH5 = GPIO35/32/33 (R/G/B),
                                                  HSync GPIO39, VSync GPIO34, reset GSV2002 GPIO22
                                                                                  │ SPI 1 MHz (MISO19 MOSI27 SCLK4 CS5)
                                           moduł Tuya (1. gen CBU/BK7231N, 2. gen T1-U/BK7238): SPI slave DMA ──► taśma LED
```

Źródła: teardown na elektroda.com ([news4090571](https://www.elektroda.com/news/news4090571.html)) podaje
dokładnie te piny w zrzucie konfiguracji oryginalnego ESP32 (`adc_r_gpio=35`, `adc_g_gpio=32`,
`adc_b_gpio=33`, HS 39, VS 34, `hdmi_reset_gpio=22`, SPI port 1, 1 MHz, MISO 19, MOSI 27, SCLK 4, CS 5,
`output_spi_en=1`, HDMI I2C i UART wyłączone). Drugi wątek ([topic4123137](https://www.elektroda.com/rtvforum/topic4123137.html))
nazywa konwerter HDMI→VGA: **CS5210** (Capstone). W logu ESP32: `HDMI-RX: GS2002_V1.00.00.04`.

### GSV2002 (GScoolink) – [specyfikacja](https://www.onwaytech.com/static/upload/file/20231031/1698717765932698.pdf)
- Repeater HDMI 1.4/2.0, 2 wejścia, 2 wyjścia, zawsze 1-in-2-out. Pixel clock do 600 MHz.
- Szyna TTL służy **tylko do audio** (I2S/SPDIF/TDM, kierunek konfigurowalny). Układ nie ma wyjścia
  analogowego ani TTL wideo. Konfiguracja przez TTL/I2C (rodzina GSV2001: I2C 400 kHz, strona 8-bit +
  adres 16-bit), ale fabryczne firmware ESP32 ma `hdmi_i2c`/`hdmi_uart` wyłączone, więc układ pracuje
  autonomicznie. Jedyny sygnał sterujący od ESP32: **reset (GPIO22, aktywny LOW)**, trzymany wysoko.
- Wniosek: z GSV2002 nie trzeba i nie da się nic "wyciągnąć" poza przelotem HDMI.

### CS5210 (Capstone) – [datasheet](https://file.elecfans.com/web2/M00/06/52/poYBAGDmW5KAJvWbAAfHWgYZXlU522.pdf)
- HDMI 1.4 → VGA, potrójny DAC 8-bit do 200 MHz, do 1920×1200@60. Wyjścia pin 17/18/19 = VGA_B/G/R,
  pin 11/12 = VGA_VS/VGA_HS (bufor 5 V). "Analog VGA interface compliant with VESA VSIS v1r2" – czyli
  0,7 Vpp na 75 Ω, czerń 0 V, bez synchronizacji na RGB.
- Wbudowany MCU 8051 z własnym firmware; I2C slave (CFG_SDA/CFG_SCL) **tylko do debugowania i aktualizacji
  firmware**. Układ nie wymaga konfiguracji z zewnątrz, pracuje po podaniu HDMI.
- Nie ma strap-pina RGB/YPbPr w opisie pinów (GPIO pin 16 "General GPIO"). Wyjście to RGB.

### Układy na płytce użytkownika (odczytane 2026-11-10)

| Oznaczenie | Co to jest | Rola |
|---|---|---|
| GSV 2002 | GScoolink GSV2002 | repeater HDMI 1-in-2-out |
| HK32… (ARM) | Hangshun HK32F030 (Cortex-M0, klon STM32F030) | host GSV2002: ładuje konfigurację, obsługuje HDCP/EDID/HPD; to z nim ESP32 ma UART2 (GPIO25/26) i od niego pochodzi wersja `GS2002_V1.00.00.04` |
| 25VQ32BSIG | pamięć SPI NOR 32 Mbit (4 MB) | flash ESP32-D0WD-V3 (układ bez wbudowanej pamięci) |
| MX9291 | krajowy (CN) konwerter HDMI→VGA | według dystrybutora "w pełni zastępuje i jest zgodny z CS5210, AG6201, NX3303X": odbiornik HDMI, konwersja RGB/YUV→analogowe RGB, wbudowany oscylator, LDO i MCU, I2C tylko do debugowania i aktualizacji firmware ([opis](https://www.cnblogs.com/Capstone/p/18528328)) |

PCB ma te same oznaczenia co V1 i wygląda 1:1, więc teardown 1. generacji i datasheet CS5210 (pinout
QFN32: VGA_B/G/R = 17/18/19, VGA_VS/HS = 11/12, CFG_SDA/SCL = 26/27, MODE = 31) stosują się wprost.
Różnice 2. generacji: moduł Tuya (T1-U/BK7238 zamiast CBU) i MX9291 zamiast CS5210.
Nie mylić z MAX9291 (serializer GMSL Analog Devices) ani z MS9291 MacroSilicon (też HD→VGA, DAC 10-bit,
QFN-48, datasheet tylko pod NDA).

Wniosek: żaden z układów wideo nie wymaga ani nie udostępnia sterowania z ESP32. GSV2002 obsługuje
HK32, MX9291 działa sam. ESP32 dostaje gotowe analogowe RGB + HSync/VSync.

### Kandydat rozważany wcześniej: MS9292 (MacroSilicon) – [datasheet (CN)](https://atta.szlcsc.com/upload/public/pdf/source/20250210/5683772AA15CCE3E9D8D6AF1DCE051AB.pdf)
- W wcześniejszych notatkach (Gemini) padło "MS9282", ale MS9282 to konwerter **VGA→HDMI** (ADC, nie DAC),
  co nie ma sensu w tym torze. Prawdopodobnie chodzi o **MS9292**: HD→VGA, DAC 10-bit 200 MSPS,
  wyjście 0,7 Vpp, piny ROUT/GOUT/BOUT + HSOUT/VSOUT, wbudowany MCU i EDID, 75 Ω na wyjściach w
  schemacie aplikacyjnym. Też autonomiczny (I2C SCL/SDA, RSTN).
- Do potwierdzenia: odczytać oznaczenie układu QFN obok sekcji HDMI na płytce A4E32MGS02KR_V5.

### Co z tego wynika dla pomiarów, które już mamy
- Zmierzone poziomy na ADC: wygaszanie ≈ 580 mV, biel ≈ 1040 mV; fabryczne firmware zakłada 645–1170 mV
  (`sw_adc_buffer=0`). Skoro DAC daje 0 V w czerni, na płytce jest bufor/polaryzacja podnosząca sygnał
  do zakresu ADC ESP32 (ADC ESP32 jest nieliniowy przy 0 V). Zapad do 0 V w oknie HSync na wszystkich
  trzech kanałach to zachowanie tego bufora/konwertera przy impulsie synchronizacji, nie "sync on green".
- Czerwone tło czytane jako (95 %, 65 %, 53 %) na R/G/B nie zgadza się z czystym RGB z DAC-a VESA.
  Albo TV nie daje czystej czerwieni, albo tor analogowy (bufor) miesza kanały. Rozstrzygnie pomiar na
  pełnym zielonym i pełnym niebieskim ekranie; wtedy macierz 3×3 do odwrócenia mieszania w LUT ESP32.

## 1a. Protokół UART2 ESP32 ↔ HK32 (odczytany z `used_flash_0x0.bin`, 2026-11-10)

Disasemblacja partycji `factory` (objdump Xtensa), funkcje w segmencie 0x400d0020:

| Adres | Funkcja | Co robi |
|---|---|---|
| 0x400db510 | init | `uart_driver_install(port, bufor RX 1024 B)`, `uart_param_config` (prędkość z konfiguracji = 115200, 8N1), `uart_set_pin(tx=26, rx=25)`, start zadania `gs2002_uart_task` (stos 2048, priorytet 5). Wołane tylko gdy `hdmi_uart_en = 1`. |
| 0x400db3fc | `gs2002_uart_task` | pętla: `vTaskDelay(sw_amb_task_delay_ms_thread_hdmi = 10 ms)`, `memset(buf, 0, 1024)`, `uart_read_bytes(port, buf, 1024, 0)`; gdy coś przyszło: licznik `hdmi_info_cnt++`, kopia bufora do podglądu, potem `strstr` po kolei na 6 napisach (tabela niżej). |
| 0x400db504 | getter | zwraca tryb HDR (zmienna 0x3ffd9f4c). |
| 0x400db4d8 | debug | drukuje `gs2002_status_updated @ <czas> ... <ostatni tekst z UART> ... hdmi_info_cnt = N`. |

**Protokół:** jednokierunkowy, tekstowy. HK32 wysyła swój log (ASCII, 115200 8N1), ESP32 **nic nie nadaje**
(zmienna portu jest używana wyłącznie w `uart_read_bytes`; linia TX GPIO26 jest tylko skonfigurowana).
Nie ma ramek, nagłówków ani sum kontrolnych. ESP32 szuka w odebranym tekście podciągów:

| Podciąg | Tryb HDR |
|---|---|
| `HDR:Invalid` | 2 |
| `HDR:SDR` | 0 |
| `HDR:HDR10` | 1 |
| `HDR:HDR10+` | 2 (nadpisuje 1, bo zawiera `HDR:HDR10`) |
| `HDR:HDR-DolbyVision` | 3 |
| `HDR:HLG` | 4 |

Pełnej treści komunikatów HK32 w obrazie ESP32 nie ma. Ten sam sterownik GScoolink jest jednak wkompilowany
w ESP32 dla wariantu I2C (`Gsv2k2ForRazer_V1.00.00.04_20220520`) i jego format to najpewniej to, co drukuje HK32:
`RxTimingInfo: Active:%dx%d%c%dHz HDR:%s MaxCLL:0x%02X,0x%02X MaxFALL:0x%02X,0x%02X`.

**Do czego służy tryb HDR** (funkcja 0x400d8510, wołana na każdej ramce): gdy tryb ≠ 0, każda komórka 16×16
przechodzi przez 0x400d6458(r, g, b, gain): RGB→HSV, **nasycenie × `sw_amb_hdr_gain`** (w konfiguracji = 2,
obcięcie do 100 %), HSV→RGB. W SDR ten krok jest pomijany. Czyli fabryczny soft podwaja nasycenie dla treści HDR,
bo po konwersji HDR→analog kolory są wypłowiałe.

Pozostałe pola konfiguracji HDMI: `hdmi_firmware_type = 0` (zewnętrzny MCU), I2C wyłączone, reset GPIO22
aktywny LOW, `hdmi_select_gpio` i `hdmi_power_gpio` nieużywane (0).

Uwaga z testów: w naszym firmware RX (GPIO25) dawał same bajty 0x00, czyli linia stała w stanie niskim.
Do sprawdzenia przy następnym flashowaniu: podciąganie na GPIO25 i log fabrycznego softu
(`gs2002_status_updated`), żeby zobaczyć prawdziwy tekst z HK32.

## 2. Jak to robiło fabryczne firmware ESP32 (z analizy użytkownika + teardown)

- ADC przez **I2S0 w trybie DMA** (kontroler cyfrowy SAR), 3 kanały na przemian, 64 linie × ~200 próbek na
  ramkę, dekodowanie po czasie próbki (`__udivdi3` na każdej próbce, stąd 12 fps). Patch Gemini
  (co 4. próbka) dał 30 fps.
- Synchronizacja z przerwań GPIO34/39 (pomiar okresu linii, rozpoznawanie 27 formatów).
- Wyjście: 768 B RGB (16×16) po SPI 1 MHz do modułu Tuya, który odbiera jako SPI slave z DMA
  ("spi slave dma init" w logu CBU). Nasz komponent ESPHome robi to samo (SPI slave DMA na P14–P17),
  więc transport do ESPHome jest już zgodny z oryginałem; nie trzeba niczego zmieniać po stronie BK.

## 3. Szybkie próbkowanie na ESP32: co dopuszcza dokumentacja

### Stan obecny (v8): kontroler RTC
- Pojedyncze konwersje SAR przez rejestry RTC: 4,4 µs automatu + 1,3 µs narzutu startu, okres
  ~6,5 µs z zegarem XTAL/4. Linia 1080p ma 12,96 µs aktywnych, więc **2 konwersje na linię**, 27–33 linii
  na wiersz siatki, 1 próbka na komórkę. Stąd flicker na drobnych kontrastach.
- Odpytywanie rejestrów RTC w pętli zajmuje APB i gubi znaki UART0 na rdzeniu 0 (zjawisko potwierdzone
  testem `hold 1`).

### Kontroler cyfrowy (DIG) + FIFO I2S bez DMA – wg ESP32 TRM v5.9
- Rozdz. 31.3 (SAR ADC): "there is no direct access to starting a single SAR conversion" – DIG działa
  w trybie skanowania wg tablicy wzorców (16 pozycji: kanał, rozdzielczość, tłumienie), start
  "by software or I2S". Rejestry: `APB_SARADC_CTRL_REG` (`DATA_TO_I2S`, `SAR_CLK_DIV`, długość tablicy),
  `APB_SARADC_CTRL2_REG` (`MAX_MEAS_NUM`, `SAR1_INV`), `APB_SARADC_FSM_REG` (`SAMPLE_CYCLE`).
- Rozdz. 22.4.3 (I2S FIFO): "There are two approaches to accessing the FIFO; one is to directly access the
  FIFO using a CPU, the other is to access the FIFO using a DMA controller." Rejestr `I2S_FIFO_RD_REG`
  (0x3FF4F004, RO) zwraca dane odebrane; `I2S_RX_DATA_NUM` to próg zapełnienia, przerwanie
  `I2S_RX_TAKE_DATA_INT` = "receive FIFO is almost full"; `I2S_DSCR_EN=0` wyłącza DMA. ESP-IDF 5.3 ma
  pola `fifo_rd`/`fifo_wr` w `i2s_struct.h` (w starszych wersjach brakowało – issue esp-idf #8862).
- ESP-IDF (`hal/adc_hal.c`, sekcja ESP32) pokazuje pełną sekwencję konfiguracji DIG+I2S: `i2s_ll_rx_set_sample_bit(16)`,
  mono, `rx_fifo_mod_force_en`, `i2s_ll_enable_builtin_adc_dac`, zegar RX z PLL_D2 160 MHz, start/stop
  skanowania przez `i2s_ll_rx_start/stop`. Format próbki: 16 bit = [15:12] kanał, [11:0] dane (type1).
  Limit sterownika: 2 MS/s (`SOC_ADC_SAMPLE_FREQ_THRES_HIGH`).

### Proponowany model próbkowania (do prototypu)
1. Skonfigurować DIG+I2S sterownikiem `adc_continuous` (wzorzec R,G,B przy 2 MS/s), po `start`
   wyłączyć DMA (`dscr_en=0`) i trzymać skanowanie włączone.
2. W przerwaniu HSync, w chwili kolumny: `rx_fifo_reset`, odczekać na ≥2 słowa (4 próbki ≈ 2 µs),
   czytać `I2S0.fifo_rd` 2× → R,G,B(,R) jednej kolumny. Koszt ~2,5 µs na kolumnę ze wszystkimi trzema
   kolorami zamiast 6,5 µs na jeden kolor.
3. Na linię 4–5 kolumn z pełnym RGB, cały wiersz siatki w 4 liniach (dziś 27–33), więc przy 33 liniach
   na wiersz można uśredniać 8 próbek na komórkę. Rejestry I2S są na APB "zwykłym", bez wolnej domeny RTC.
4. Ryzyka do sprawdzenia na sprzęcie: czy `rx_fifo_reset` w trakcie skanowania nie przerywa pracy DIG,
   kolejność próbek w słowie (I2S na ESP32 zamienia parzyste/nieparzyste próbki, stąd pole kanału w
   danych), pierwsza próbka po resecie, oraz czy `adc_continuous` i ręczny dostęp do FIFO nie gryzą się
   (sterownik trzyma przerwanie DMA – po `dscr_en=0` nie będzie zdarzeń).

## 4. Wysyłka do ESPHome
- Oryginał: SPI master 1 MHz, 768 B na ramkę, CS na całą ramkę; moduł Tuya = SPI slave DMA. Nasz komponent
  `syncbox` to odtwarza (`bk_spi_slave_dma_xfer`, znacznik końca ramki, 60 fps potwierdzone).
- Gotowe ESPHome dla obu generacji boxa publikuje seb5594
  ([repozytorium](https://github.com/seb5594/Action_Battleton_Eurodomest_LSC-Smart-Custom-Firmware-Collection),
  [release 3225765](https://github.com/seb5594/Action_Battleton_Eurodomest_LSC-Smart-Custom-Firmware-Collection/releases/tag/3225765-eurodomest-hdmi-sync-box-2nd-generation)) –
  tylko binaria, bez YAML, nie dotyka ESP32; nie wnosi nic do strony wideo.

## 5. Otwarte punkty
1. ~~Oznaczenie konwertera HDMI→VGA na płytce 2. generacji~~ – rozstrzygnięte: MX9291 (zamiennik CS5210).
2. Pomiar pełnego zielonego i niebieskiego ekranu → macierz kolorów.
3. Prototyp DIG+FIFO (pkt 3) – największy zysk jakości (8 próbek na komórkę, koniec problemu z UART).

## Źródła
- Teardown boxa (1. gen): https://www.elektroda.com/news/news4090571.html
- OpenBeken dla boxa, CS5210: https://www.elektroda.com/rtvforum/topic4123137.html
- GSV2002 spec: https://www.onwaytech.com/static/upload/file/20231031/1698717765932698.pdf
- CS5210 datasheet: https://file.elecfans.com/web2/M00/06/52/poYBAGDmW5KAJvWbAAfHWgYZXlU522.pdf
- MS9292 / MS9288A / MS9288C datasheety (LCSC): https://www.lcsc.com/product-detail/Video-Interface-ICs_Macrosilicon-MS9292_C2874140.html
- MS9282 (VGA→HDMI, czyli nie ten kierunek): https://www.lcsc.com/product-detail/Video-Interface-ICs_Macrosilicon-MS9282_C42374505.html
- ESP32 TRM v5.9 (rozdz. 22 I2S, 31 SAR ADC): https://documentation.espressif.com/esp32_technical_reference_manual_en.pdf
- esp-idf issue o `fifo_rd`/`fifo_wr`: https://github.com/espressif/esp-idf/issues/8862
- ESPHome dla boxa (seb5594): https://github.com/seb5594/Action_Battleton_Eurodomest_LSC-Smart-Custom-Firmware-Collection

## 6. Ustalenia z v9.1 (2026-11-10): kontroler cyfrowy ESP32 w praktyce

- Przy `sample_freq_hz = 2 MHz` konwersje idą dokładnie co 0,5 µs (30 konwersji = okres linii 1080p + 0,2 µs).
- `SARADC_MAX_MEAS_NUM = 10` (ustawienie sterownika) daje serie po 9 próbek i przerwę 1 µs, po której kanał się
  powtarza; z wartością 255 przerw praktycznie nie ma (99,2 % konwersji w FIFO).
- Restart I2S RX + `sar1_patt_p_clear` ustawia kolejność na R,G,B; pierwsza konwersja wypada ok. 0,635 µs po końcu
  sekwencji restartu, kolejne co 0,5 µs, rozrzut ok. ±0,05 µs. Czas trwania samej sekwencji bywa różny (do 0,8 µs).
- Pierwsze słowo pojawia się w FIFO ok. 2,8 µs po restarcie (czyli ok. 1,7 µs po swojej drugiej próbce).
- Faza zegara I2S nie daje się związać z HSync, więc położenia próbek różnią się między ramkami o ułamek 0,5 µs;
  resztę szumu na ostrych krawędziach usuwa rozmycie przestrzenne i wygładzanie zależne od wielkości zmiany.
