# syncbox-esp32: własny firmware koprocesora wideo (ESP32) w Action/Eurodomest HDMI Sync Box

Zastępuje fabryczną aplikację `syncbox` na ESP32. Z analogowego RGB i HSync/VSync, które wystawia tor
HDMI (GSV2002 + konwerter), buduje siatkę 16×16 RGB i wysyła ją po SPI do modułu BK7238 z ESPHome
(komponent `syncbox`). Cel: tempo klatek równe źródłu (60 Hz) zamiast fabrycznych 10 do 12.

Napisane od zera na podstawie pinów z analizy partycji `config` ESP32. Konsola na UART0 daje telemetrię
i strojenie bez przekompilowywania.

Wersje 1 do 7 używały ADC w trybie DMA i odtwarzały czas każdej próbki z jej numeru. Na sprzęcie to nie
działało: po przepełnieniach bufora licznik próbek rozjeżdżał się z czasem i prawie żadna próbka nie trafiała
w aktywną linię (`stat`: 768 pustych komórek, taśma czarna). Od **v8** chwile próbkowania wymusza sam sygnał:
przerwanie HSync uruchamia pojedyncze konwersje SAR ADC dokładnie w środku kolejnych kolumn siatki.

## Budowa i wgranie (ESP-IDF 5.1 lub nowsze)

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Tryb programowania: przytrzymać górny mikroprzycisk (GPIO0), zresetować lub włączyć zasilanie, puścić.
Przed pierwszym wgraniem zachowaj zrzut `used_flash_0x0.bin`: pełny powrót to
`esptool --chip esp32 write-flash 0x0 used_flash_0x0.bin`.

Wgrywany jest własny bootloader, tabela partycji (NVS, otadata, dwie partycje aplikacji `ota_0`/`ota_1`
po 1,5 MB) i aplikacja pod `0x20000`. Kolejne wersje nie wymagają przycisku: komenda konsoli
`upload <bajty> <crc32>` przyjmuje surowy `firmware.bin` po UART0 (115200, ok. 30 s), zapisuje go do
nieaktywnej partycji OTA i restartuje (skrypt `esp32_upload.py COM4 firmware.bin`).

## Jak to działa

| Moduł | Rola |
|---|---|
| `sync.c` | jedno przerwanie GPIO (poziom 3, rdzeń 1, bez sterownika `gpio_isr_service`) dla HSync (GPIO39) i VSync (GPIO34); numer linii w ramce, liczba linii, okres ramki, tabela formatów (1080p, 720p, 2160p, ...) |
| `capture.c` | plan próbkowania: dla każdego z 16 wierszy siatki wybrane linie obrazu; w ich przerwaniu HSync kod czeka (licznik cykli CPU) do środka kolejnych kolumn i uruchamia konwersję SAR ADC1 (rejestry bezpośrednio, kontroler RTC, ok. 2 µs). Jedna linia = jeden kolor i co `stride`-ta kolumna; kolejne linie wiersza biorą resztę kolumn i kolorów, `rep` razy. VSync zamienia bank akumulatorów; zadanie liczy średnie, LUT (czerń/biel, wzmocnienie, odcięcie, gamma), odbicia i wysyła ramkę |
| `spi_out.c` | SPI2 master (SCLK 4, CS 5, MOSI 27, MISO 19), 768 B na jedno opuszczenie CS, podwójny bufor, rdzeń 1, odstęp ≥ 600 µs między ramkami; wzory testowe |
| `hdmi.c` | GPIO22 (reset GSV2002) trzymany w stanie nieaktywnym, UART2 (26/25, 115200) podsłuch i wysyłka bajtów |
| `console.c` | komendy `stat`, `grid`, `ascii`, `set`, `save`, `pattern`, `hdmi` |
| `params.c` | parametry w NVS |

## Plan próbkowania (v8)

- Konwersja SAR ADC przez kontroler RTC trwa ok. 2 µs (mierzone przy starcie, widoczne w `stat`), a kolumna
  siatki przy 1080p to 0,81 µs linii. Jedna linia nie zmieści więc 16 kolumn: bierze co `stride`-tą
  (np. co trzecią), tylko dla jednego koloru. Komplet 16 kolumn × 3 kolory to `3 × stride` linii; wiersz
  siatki ma ich przy 1080p 67, więc komplet mieści się `rep` razy (domyślnie 2, max 4) z zapasem.
- Linie próbkujące są rozłożone równo w wysokości wiersza, w odstępie co najmniej `spacing` linii, bo
  przerwanie próbkujące trwa dłużej niż jedna linia 1080p (ok. 16 µs przy T = 14,8 µs) i kolejna linia
  nie może być próbkująca.
- Przerwanie jest w IRAM i dotyka tylko DRAM/rejestrów, więc pracuje także podczas zapisu NVS (`save`).
- Opóźnienie od zbocza HSync do wejścia w przerwanie (ok. 0,7 µs) jest odjęte od czasów kolumn; resztę
  stroi `hoff`. Kolumny, na które przerwanie przyszło za późno (> pół kolumny), są pomijane i liczone
  jako "spóźnione kolumny" w `stat`; pojedyncze sztuki są normalne, tysiące na sekundę oznaczają, że coś
  innego blokuje rdzeń 1.
- Obciążenie rdzenia 1 przy 1080p i `rep 2`: 16 wierszy × 18 linii × ok. 16 µs plus 1125 krótkich
  przerwań, razem ok. 35 % czasu. `set rep 1` zmniejsza o połowę, `set rep 3` zwiększa uśrednianie.

## Procedura uruchomienia

1. **Tor SPI.** Po starcie wpisz `pattern 1`. Na taśmie powinna pojawić się płynna tęcza, a w ESPHome
   "FPS wideo" około 60. Jeśli nie, sprawdź `stat` (licznik wysłanych ramek) i log BK ("klatki: ... częściowe").
   Przy `pattern 0` i sygnale HDMI "probek w ostatniej" powinno być ok. 1500 przy `rep 2` (16×16×3×2),
   a "pustych komorek" 0.
   `pattern 2` daje pasy: lewo czerwone, góra zielona, prawo niebieskie, dół białe; na tym sprawdzisz
   `start_corner`/`direction` w ESPHome.
2. **Synchronizacja.** `pattern 0`, podaj sygnał HDMI, `stat`: VSync powinien pokazać 60 Hz (lub 50/24/30),
   "linii w ramce" np. 1125 dla 1080p, format rozpoznany. Jeśli VSync jest 0, zmień `set vsync_neg 0`
   (polaryzacja) i `set hsync_neg 0`; po zmianie `save` i `reboot`.
3. **Poziomy.** `stat` pokazuje średnie i skrajne kody ADC. Przy czarnym ekranie kody powinny odpowiadać
   `black_mv`, przy białym `white_mv`. Dostrój `set black_mv` / `set white_mv` tak, by czerń dawała 0, a biel 255.
4. **Mapowanie poziome.** Wyświetl planszę z pionowymi pasami kolorów i wpisz `ascii` lub `grid`.
   Jeśli pasy są przesunięte w poziomie, zmieniaj `set hoff` (w 0,1 µs, może być ujemne), jeśli ściśnięte
   lub rozciągnięte, `set hblank` (w 0,1 %: 127 = 12,7 % dla 1080p, 224 dla 720p). Odbicie: `flip_h`.
5. **Mapowanie pionowe.** Plansza z poziomymi pasami; `set vstart` / `set vlines` nadpisują tabelę formatów.
6. `save`.

## Największe niewiadome (do potwierdzenia na sprzęcie)

- Czy tor wideo wystawia analogowe RGB i synchronizacje bez żadnej inicjalizacji przez UART2 od ESP32.
  Jeśli po wgraniu `stat` pokazuje VSync 0 Hz, a przelot HDMI działa, najpewniej GSV2002/MCU czeka na
  komendy. Wtedy trzeba podsłuchać UART2 (GPIO26 TX ESP32) na fabrycznym firmware i odtworzyć sekwencję
  komendą `hdmi <hex>`. Komponent loguje wszystko, co przychodzi z UART2.
- Zakres i polaryzacja sygnałów RGB (dokument: 645 do 1170 mV) oraz polaryzacja HSync/VSync.
- Rzeczywista rozdzielczość pozioma: każda kolumna siatki jest próbkowana w jednej chwili (ok. 2 µs okna
  SAR) co `rep` linii wiersza; przy 720p/480p linie są dłuższe, więc `stride` maleje i kolumn na linię jest więcej.
- `hstart` zakłada, że zbocze HSync to początek impulsu synchronizacji i że impuls plus tylny ganek to ok. 69 %
  wygaszania (CEA 1080p). Dla innych timingów skoryguj `hoff`.

## Po stronie ESPHome

Nic nie trzeba zmieniać: klatka 768 B, RGB wierszami od góry, bez nagłówka, SPI mode 0, 2 MHz.
Przy 60 klatkach/s wyłącz interpolację ("Wideo: interpolacja klatek") i ustaw wygładzanie na 0.

## v9 (2026-11-10): kalibracja z pomiarów, HDR, tryb cyfrowy ADC

Zmierzone na sprzęcie benchmarkiem (`../bench`: strona testowa sterowana z PC + porównanie z siatką):

- **Poziomy:** czerń = kod 1594, biel 255 = kod ~3830, szarość 128 dokładnie w połowie. Tor jest liniowy, kanały
  niezależne (czysta czerwień daje G i B na czerni) i równe do 1 %. Domyślne `black_mv 585`, `white_mv 1400`.
  Wcześniejsze `white_mv 1040` obcinało wszystko powyżej ~55 % jasności (wypłowiałe kolory, skoki w biel).
- **VSync:** przerwanie na obu zboczach, początek impulsu rozpoznawany po długości przerwy (niezależnie od
  polaryzacji). Wcześniej łapany był koniec impulsu i okno pionowe było przesunięte o 5 linii.
- **Licznik linii:** sprzętowy PCNT na HSync; długie przerwanie próbkujące nie gubi już zboczy.
- **HDR:** MCU układu HDMI (HK32) nadaje log tekstowy po UART2 z prędkością **9600 bps** (konfiguracja fabryczna
  mówi 115200, więc fabryczny firmware najpewniej nigdy go nie odczytał). Przykład:
  `RxTimingInfo: Active:3840x2160P60Hz HDR:SDR MaxCLL:0x00,0x00 MaxFALL:0x00,0x00`. Parser bierze ostatnie
  wystąpienie `HDR:` (SDR, HDR10, HDR10+, HDR-DolbyVision, HLG, Invalid); w trybie innym niż SDR nasycenie w HSV
  jest mnożone przez `hdr_gain` (200 % jak fabryczne `sw_amb_hdr_gain = 2`). `hdr_mode`: 0 auto, 1 zawsze SDR,
  2 zawsze HDR. Komenda `hdmi` pokazuje ostatni tekst, `hdmi scope` przebieg linii RX.
- **Tryb cyfrowy ADC (`adc_mode 1`, domyślny):** kontroler DIG skanuje R,G,B bez przerwy, próbki idą do FIFO I2S0,
  a przerwanie HSync czyta FIFO przez CPU (bez DMA) przez aktywny odcinek następnej linii. Szczegóły:
  - sterownik ustawia zegar SAR na APB/16 (konwersja 3,4 µs mimo "2 MS/s"); z `dig_clkdiv 2` konwersja trwa 0,55 µs;
  - słowo FIFO to dwie próbki, okres słowa jest mierzony przy starcie (1,217 µs, czyli 1,64 MS/s);
  - puste FIFO zwraca stare słowa, więc czytamy tylko, gdy flaga `rx_take_data` (próg 0) mówi, że są dane;
  - czas próbki liczony jest z numeru słowa w linii (kotwica = najwcześniejszy odczyt), bo słowa bywają odczytane
    z opóźnieniem do ~1 µs; `dig_lat` (0,3 µs) i `dig_guard` (0,6 µs, margines przy krawędziach) z profilu `hprof`;
  - co `dig_step`-tą linię (3): ok. 6000 próbek na ramkę zamiast 958, każda komórka uśrednia obszar, a nie punkt.
- **Tryb RTC (`adc_mode 0`):** jak w v8, jedna próbka na komórkę w środku kolumny. Ostrzejszy przestrzennie
  (wzór 16×16: błąd 3 zamiast 26), ale próbkuje punktowo: ruch poziomy idzie krokami co kolumnę, a drobne
  kontrasty (napisy) migoczą.

Wyniki benchmarku w trybie cyfrowym: pełnoekranowe kolory z błędem ≤ 3/255 i rozrzutem komórek ≤ 4, skok
czerń/biel bez ramek pośrednich, płynny ruch pasa w poziomie i pionie, 0 komórek niestabilnych na statycznym obrazie.

Diagnostyka: `frames [n]` (kolejne ramki), `hprof [krok] [kanał]` (profil linii), `digrate` (tempo konwersji i
zachowanie FIFO), `digtest`, `glitch`, `hold 0|1`.

## v9.1 (2026-11-10): stabilny obraz na drobnych szczegółach

Problem: pełnoekranowe kolory były stabilne, ale na obrazie ze szczegółami (pasek zadań, napisy, wideo) kolory
pływały. Przyczyna: w trybie cyfrowym zegar przetwornika nie jest związany z obrazem, więc w każdej ramce próbki
trafiały w inne piksele. Zmiany (opisy z v9 dotyczące `dig_lat`, okresu słowa i kotwicy są nieaktualne):

- **Restart przetwornika w każdej linii próbkującej** (`dig_restart`: zatrzymanie I2S RX, wskaźnik wzorca na
  początek, reset odbiornika i FIFO, start). Po restarcie kolejność to zawsze R,G,B, a chwila próbki nr `n` to
  `koniec restartu + dig_d (635 ns) + n × 0,5 µs` z błędem ok. ±0,05 µs (zmierzone komendą `digidx` na poziomym
  gradiencie: kod ADC = położenie). Sam restart trwa zmiennie (do ok. 0,8 µs), dlatego liczy się zmierzony koniec.
- **`dig_limit 255`** (rejestr `SARADC_MAX_MEAS_NUM`): sterownik ustawia 10, przez co po każdych 9 próbkach była
  przerwa 1 µs (kanał się powtarzał, w FIFO 82 % konwersji). Z 255 próbki idą równo co 0,5 µs.
- Numer konwersji jest odtwarzany z kanału próbki (kanał rośnie cyklicznie; ten sam kanał = przeskok o 3), więc
  ewentualna przerwa nie psuje osi czasu. Konwersja nr 0 jest odrzucana (pobierana wcześniej niż reszta).
- **Wagi liniowe:** próbka zasila dwie sąsiednie kolumny proporcjonalnie do odległości od ich środków.
- **`blur` (domyślnie 1):** rozmycie siatki 16×16 jądrem 1-2-1 w poziomie i pionie (2 = 1-4-6-4-1, 0 = brak).
  Tłumi szum próbkowania bez opóźnienia.
- **`smooth` (25 %), `smooth_t1` (8), `smooth_t2` (32):** wygładzanie w czasie zależne od wielkości zmiany. Różnica
  względem stanu poniżej `t1` jest uśredniana (udział nowej ramki 25 %), od `t2` przechodzi od razu, pomiędzy
  liniowo. `smooth 100` wyłącza. (Parametr `smooth_med`, mediana z trzech ramek, usunięty w v13.1: nic nie dawał
  ponad rozmycie, a usuwał jednoklatkowe błyski.)
- Okno odczytu FIFO dłuższe o 2,6 µs za końcem linii (słowo pojawia się ok. 1,7 µs po swojej drugiej próbce),
  margines przy krawędziach `dig_guard` 0,3 µs, ok. 8700 próbek na ramkę, 0 pustych komórek.

Wyniki (`bench/detail.py`, statyczna losowa szachownica, odchylenie standardowe komórki w czasie, skala 0–255):

| wersja | pola 20 px (96×54) | pola 60 px (32×18) |
|---|---|---|
| v9 | 41 | 30 |
| v9.1 bez filtrów | 8,2 | 3,6 |
| v9.1 z `blur 1` | 4,0 | 2,1 |
| v9.1 domyślne (`blur 1`, `smooth 25`) | 1,6 | 0,8 |

Pełnoekranowe kolory: błąd średniej ≤ 2/255, rozrzut komórek ≤ 2, skok czerń/biel nadal bez ramek pośrednich.
`bench/visualize.py COM4` nagrywa siatki i tworzy stronę `bench/podglad_esp.html` (ekran obok tego, co widzi ESP32).
Nowa diagnostyka: `digidx [µs]` (średni kod każdej kolejnej próbki po restarcie + rozkłady), `digsync`.

## v9.2 (2026-11-10): ciemne sceny

Na ciemnym obrazie pojedyncze diody mrugały, bo `cut 10` był twardym progiem: składowa o wartości ok. 10 przeskakiwała
między `0A` a `00`. To samo opisują użytkownicy HyperHDR (próg minimalnej jasności daje nagły skok przy przekraczaniu).

- **Miękki próg `cut`:** poniżej progu wartość maleje kwadratowo do zera (`o*o/cut`), bez skoku.
- **`hold` (domyślnie 10 = 1,0 poziomu):** martwa strefa wyjścia. Składowa zmienia się dopiero, gdy wygładzona
  wartość odejdzie od wysyłanej o więcej niż `hold`; usuwa przeskoki o 1 między sąsiednimi poziomami. `hold 0` wyłącza.
- `bench/showimg.py <obraz>` wyświetla dowolny obraz na stronie testowej, `bench/live.py COM4` mierzy stabilność
  na tym, co jest na ekranie (zakres zmian każdej komórki, podział wg jasności).

Na ciemnym obrazie testowym: komórki obwodu zmieniały się średnio o 1,2 (max 10), teraz o 0,4 (max 2).

## v9.3 (2026-11-10): siatka 16×9

Parametr `grid_h` (domyślnie 16; `set grid_h 9`, potem `save`) ustawia liczbę wierszy siatki. Przy 9 wierszach
komórki są kwadratowe dla obrazu 16:9, a na wiersz przypada 40 linii próbkujących zamiast 22, więc szum jest
mniejszy (szachownica 96×54: odchylenie 1,2 zamiast 1,7; 32×18: 0,5 zamiast 0,7). Zmiana działa od razu.

Ramka SPI ma nadal 768 B. Dane siatki zajmują pierwsze `16 × grid_h × 3` bajtów, reszta to zera, a zaraz za danymi
jest 16-bajtowy znacznik: `SBGRID`, szerokość, wysokość, ich negacje bitowe, `5A A5 C3 3C 96 69`. Komponent
ESPHome od v12 rozpoznaje go w każdej klatce i sam przełącza układ; starszy firmware BK wymaga `grid_h 16`.
Pełny obraz `full_0x0` kasuje zapisane parametry, więc po jego wgraniu `grid_h` wraca do 16.

## v10 (2026-11-10): pole 32×18, rozmycie w ESP32, wybór punktów w BK

Zastępuje siatkę 16×16/16×9 i znacznik z v9.3. Wymaga komponentu ESPHome v13 (BK).

- **Pole do 32×18** (`grid_w`, `grid_h`; domyślnie 32×18, zmiana na żywo, np. 24×14 albo 16×9). Tryb RTC
  (`adc_mode 0`) próbkuje zawsze 16 kolumn.
- **Ramka SPI 1744 B, 4 MHz:** 16 B nagłówka (`SBG2`, szerokość, wysokość, ich negacje bitowe, licznik ramek,
  suma bajtów pola 16 bitów, `01`, `5A A5 C3 3C`), potem pole wierszami (R,G,B), reszta zera. BK czyta układ
  z nagłówka w każdej klatce i odrzuca ramki z błędną sumą. Zmierzone na ruchomej tęczy: 0 błędnych ramek przy
  2, 4 i 8 MHz; ok. 0,1 % ramek BK nie zdąża odebrać (licznik „częściowe").
- **`blur`** = liczba przejść jądra 1-2-1 w poziomie i pionie (0..12, domyślnie 3), liczone po zebraniu ramki.
- **Wygładzanie dwustopniowe:** wyjście to wolna średnia (`smooth` = 4 % nowej ramki), która uśrednia szum
  próbkowania; szybka średnia (50 %) śledzi obraz. Gdy szybka odejdzie od wyjścia o więcej niż `smooth_t1` (10),
  wyjście zaczyna ją gonić; różnica chwilowa od `smooth_t2` (32) przechodzi od razu. `hold` 15 (1,5 poziomu).
- **`dig_pre` (1,6 µs):** restart przetwornika zaczyna się wcześniej o czas trwania samej sekwencji restartu
  (1,0–1,8 µs). Bez tego kolumna 0 nie dostawała próbek czerwonego przy 32 kolumnach.
- Zapis parametrów ma w kluczu wersję układu struktury (`p11`); starszy zapis jest pomijany.

Wyniki przy 32×18, ustawienia domyślne: kolory pełnoekranowe z błędem do 2,9/255, skok czerń/biel bez ramek
pośrednich, ruchomy pas z oczekiwaną prędkością, szachownica 96×54 / 32×18: odchylenie 0,8 / 0,6. Na taśmie
(log BK, pierwsza sekcja, nieruchoma szachownica 32×18, 30–40 s): czerwony 119–123 zamiast 111–128.

## v10.5 (2026-11-10): pas przy krawędzi bez koloru z głębi obrazu

Test `bench/bleed.py` (czerwona ramka grubości jednej komórki, wnętrze zielone) pokazał, że komórki przy krawędzi
miały ok. 50 % zieleni: rozmycie w obu kierunkach wciągało kolor ze środka, a skrajne kolumny dostawały też
próbki z sąsiedniej kolumny.

- **`blur_dir 1` (domyślnie):** rozmycie tylko wzdłuż najbliższej krawędzi ekranu (góra i dół w poziomie, boki
  w pionie, przy przekątnych mieszanka). `blur` domyślnie 4 przejścia. `blur_dir 0` przywraca rozmycie w obu kierunkach.
- **Skrajne kolumny** dostają wyłącznie próbki ze swojego obszaru; ważenie między sąsiadami zostało tylko
  dla kolumn wewnętrznych.
- **`dig_guard` 0,2 µs** (było 0,3): nieużywany margines przy lewej i prawej krawędzi to teraz ok. 30 px z 1920.

Wynik: zieleń w pierwszej komórce od krawędzi 0 % (było 50 %). Koszt: szum na szachownicach 1,9 / 1,0 zamiast
0,8 / 0,6, bo rozmycie uśrednia w jednym kierunku. Przy ramce cieńszej niż komórka (60 px przy 32×18) pas siłą
rzeczy miesza ją z wnętrzem; cieńszy pas wymaga gęstszego pola, a to z kolei większej liczby próbek.

## v10.6 (2026-11-10): kalibracja czasu próbek testem ramki

`bench/bleed.py` podaje przenikanie osobno dla każdego boku. Przy czerwonej ramce grubości jednej komórki
przenikanie po lewej oznacza za małe `dig_d`, po prawej za duże: 600 ns → lewa 86 %, 635 → 36 %, 670 → 0,6 %,
720 → prawa 1,4 %, 780 → 12 %, 840 → 39 %. Domyślne `dig_d` to teraz **695 ns** (było 635 z pomiaru na gradiencie).

## v12 (2026-11-10): pole 48×27 i 2,4 raza więcej próbek

Wymaga komponentu ESPHome v14 (BK), który przyjmuje ramki o długości zależnej od pola.

- **Tryb blokowy (`adc_mode 2`, domyślny):** jedno przerwanie restartuje przetwornik raz na blok `dig_block` (4)
  kolejnych linii i czyta FIFO przez cały blok, przeliczając każdą próbkę od razu (czas → linia i kolumna →
  akumulator) w czasie, w którym i tak czekałoby na kolejne słowo. Próbkowane są 4 z każdych 5 linii zamiast co
  trzeciej: ok. 21 800 próbek na ramkę zamiast 9 000. Okres linii jest mierzony ze sprzętowych znaczników zboczy
  HSync (14,8145 µs; z licznika linii wychodziło 14,827). `adc_mode 1` zostaje jako tryb zapasowy.
- **Pole do 48×27** (`grid_w`, `grid_h`): komórka to 40 px przy 1080p, czyli 2,1 % szerokości i 3,7 % wysokości.
  Ramka SPI ma nagłówek i dokładnie tyle bajtów, ile pole (48×27: 3904 B), zegar 8 MHz.
- **Rozmycie:** `blur 5` wzdłuż krawędzi plus `blur_in 80` (% jednego przejścia w obu kierunkach; pierwotnie 3 i 50): komórka przy
  krawędzi bierze ok. 20 % koloru z sąsiedniej komórki w głąb (przy 50 było 12 %). `blur_in 0` wyłącza rozmycie w głąb.
- **`dig_d` 686 ns** dla trybu blokowego (test `bench/bleed.py`: 660 → lewa 12 %, 680 → 1,7 % / 0,2 %,
  700 → 0,2 % / 4,1 %, 720 → prawa 27 %).
- **Filtry** przepisane (rozmycie na płaskich buforach, bez liczb zmiennoprzecinkowych w pętli): 5,4 ms na ramkę
  przy 48×27. Zadanie liczące ma priorytet niższy niż konsola i wysyłka SPI, więc przeciążenie nie blokuje konsoli.
- **Martwy przetwornik:** kontroler cyfrowy potrafi się zatrzymać tak, że FIFO oddaje same zera, a zwykły restart
  procesora tego nie naprawia (stan siedzi w domenie RTC). Firmware wykrywa to przy starcie kontrolera i po 3 s
  ramek bez próbek, po czym robi pełny reset układu przez watchdog RTC; komenda `hardreset` robi to ręcznie.
  Przyczyna zatrzymania nie jest ustalona (wystąpiło podczas eksperymentów z diagnostyką i przełączaniem trybów).
- Duże bufory są na stercie (statyczny segment danych ESP32 ma ok. 180 kB), `frames` nagrywa najwyżej 4 ramki,
  mediana z trzech ramek usunięta, nowa komenda `regs`.

Wyniki przy 48×27: 0 pustych komórek, kolory pełnoekranowe z błędem do 2,9/255, skok czerń/biel bez ramek
pośrednich, ruchomy pas z oczekiwaną prędkością, szachownica 96×54 / 32×18: odchylenie 1,5 / 0,8, przenikanie
przy `blur_in 0`: 0 % góra i dół, do 2 % boki.

## v13 (2026-11-11): odcień przy drobnych zmianach, lag, czarne pasy

Zgłoszenie: przy lekkiej zmianie obrazu taśma na moment zmieniała odcień, a spokojne sceny sprawiały wrażenie
niskiego fps. Pomiar (`bench/hueshift.py`, `bench/huetrace.py`, `bench/curve.py`) wskazał trzy przyczyny.

- **Martwe strefy przetwornika.** Przy zegarze SAR potrzebnym do 2 MS/s ESP32 po kodach kończących się na 7F / FF
  oddaje ten sam kod dla kilkudziesięciu kolejnych wartości wejściowych: w zakresie 2,5 dB jasności 80..86
  czytały się jako 81 (plus kilka stref po 2 poziomy). Kanał, który trafił w strefę, stał w miejscu, gdy pozostałe
  się zmieniały. Wolniejszy zegar (`dig_clkdiv 3`) nie kończy konwersji w 0,5 µs, więc nie jest rozwiązaniem.
  Rozwiązanie: w trybie blokowym wzorzec ma 6 pozycji, R,G,B w zakresie `dig_atten` (2,5 dB) i R,G,B w zakresie
  `dig_atten2` (6 dB), gdzie strefy wypadają na innych jasnościach (30..36, 64..70, 192..198). Kody drugiego
  zakresu są przeliczane na skalę pierwszego (`black2_mv`, `white2_mv`), a próbki o kodzie do 3 od zmierzonego
  „lepkiego" kodu (listy `STK1`, `STK2` w `capture.c`, dla tego egzemplarza) dostają wagę 8 albo 1 zamiast 256.
  Sterownik ESP-IDF wymaga jednego zakresu we wzorcu, więc zakres pozycji 3..5 jest wpisywany wprost do
  `SYSCON.saradc_sar1_patt_tab`. Wynik: brak martwych stref, błąd odczytu średnio ok. 1 poziom, największy 3.
- **Decyzje osobno dla każdej składowej.** Martwa strefa wyjścia (`hold`) i przyciemnianie ciemnych kolorów (`cut`)
  działały na każdy kanał osobno. Teraz `hold` (1,0) aktualizuje wszystkie trzy składowe naraz, a `cut` skaluje
  cały kolor wspólnym czynnikiem.
- **Wygładzanie z progiem.** Zmiany poniżej progu dochodziły z udziałem 4 % na ramkę (przełączanie 120↔128 co 0,3 s
  dawało na wyjściu stałą wartość). Teraz udział rośnie płynnie: dla pojedynczej komórki od połowy `smooth_t1` do
  `smooth_t1` (10), a dodatkowo dla bloku 8×8 komórek, gdy jego średnia różnica przekracza `smooth_tc` (2,5 poziomu):
  zgodna zmiana całego obszaru to prawdziwa zmiana obrazu, a szum próbkowania w średniej z bloku się znosi.
- **Czarne pasy (`lbox 1`, `lbox_thr 12`).** Przerwanie zapisuje największy kod każdej próbkowanej linii; zadanie
  wyznacza pierwszą i ostatnią linię obrazu (powiększenie od razu, zmniejszenie po 1,5 s, pas najwyżej 1/4
  wysokości z każdej strony) i mapuje wiersze pola na sam obraz. Pasy nie trafiają do pola, więc nie ma wierszy
  mieszanych z pasem, a rozmycie nie wciąga czerni. Test `bench/letterbox.py`: pasy po 140 px wykryte jako 141 linii.
- Kalibracja: `black_mv 584`, `white_mv 1408`, `black2_mv 398`, `white2_mv 991` (dopasowanie liniowe do
  charakterystyk z `bench/curve.py`; `bench/calib.py` ustawia czerń i biel z ekranu testowego).

Wyniki v13 przy 48×27: przy przełączaniu bliskich kolorów składowe zmieniają się razem (odchylenie odcienia do 1,5
dla szarości i ciemnego pomarańczu, pojedyncze piksele jasnego pomarańczu do 4,5; wcześniej stale 2,5..2,9),
wyjście nadąża za zmianą o 8 poziomów (122..126 zamiast stałej wartości), szum na szachownicy 96×54 / 32×18:
2,1 / 1,1, kolory pełnoekranowe do 2,9/255, skok czerń/biel bez ramek pośrednich.

## v13.1 (2026-11-11): poprawki po przeglądzie kodu

Zewnętrzna recenzja kodu wskazała dwa błędy; oba potwierdzone i poprawione, razem z usterkami znalezionymi przy
własnym przeglądzie. Przy ustawieniach domyślnych (48×27, 8 MHz) żaden z nich nie wpływał na obraz.

- **Licznik wag próbek (`s_cnt`) był 16-bitowy.** Próbka ma wagę 256, więc licznik przewijał się po 255 próbkach
  jednej składowej w komórce, czyli przy polu poniżej ok. 30 komórek (4×4: szary czytał się jako biel). Teraz
  32 bity, na stercie (segment statyczny jest pełny). Test `bench/fixes131.py`: 4×4, 5×5, 8×8 czytają szary 128
  jako 127..128.
- **`spi_out_submit()` mogło nadpisać bufor w trakcie wysyłki**, gdy transmisja trwa dłużej niż okres ramki
  (`spi_hz` poniżej ok. 2 MHz). Zapis idzie teraz zawsze do bufora, którego wątek SPI nie wysyła, a ramka czekająca
  w nim jest na czas kopiowania wycofywana.
- **Bank ramki bez właściciela.** Gdy zadanie liczące ramkę spóźniło się o całą ramkę (np. długi wydruk konsoli),
  VSync przełączał przerwanie na bank, który zadanie właśnie czytało i zerowało. Teraz VSync nie zamienia banków,
  dopóki zadanie nie skończy (`s_fin_busy`); po 5 takich ramkach przerwanie przestaje sumować.
- **Konsola zatrzymywała ramki.** `printf` kręcił się w pętli na FIFO UART z priorytetem wyższym niż zadanie ramek,
  więc `frames`, `grid`, `glitch` zatrzymywały wysyłkę do BK na 1–3 s. Wyjście idzie teraz przez sterownik UART
  z buforem (czeka, oddając procesor): podczas `frames 4` do BK idzie dalej 60 ramek na sekundę.
- `spi_hz` spoza 1..20 MHz jest odrzucane (wartość typu `8M` kończyła się abortem, a zapisana pętlą restartów
  przed startem konsoli).
- Nieudany start kontrolera cyfrowego ADC: pole przechodzi na układ trybu RTC (16 kolumn), zamiast rozciągać
  16 kolumn na 48.
- Wykrywanie zawieszonego przetwornika patrzy na to, czy z FIFO przychodzą konwersje (a nie czy udało się je
  przypisać do komórek), i ponawia pełny reset tylko wtedy, gdy poprzedni pomógł, nie częściej niż co 30 s.
- `set` wpisany w trakcie przeliczania planu nie ginie; `defaults` przywraca też ustawienia UART2; linia resetu
  układu HDMI dostaje poziom przed włączeniem wyjścia; usunięte martwe parametry `smooth_med`, `adc_hz`
  (pola w strukturze zostały, więc zapisane ustawienia są ważne).

- `stat` po zaniku sygnału wideo pokazuje 0 Hz i 0 fps (wcześniej zostawały ostatnie wartości, np. 60 Hz).

Znane ograniczenie: gdy zadanie ramek loguje (zmiana pasów, przebudowa planu) dokładnie w trakcie długiego wydruku
konsoli, czeka na jego koniec.

## v14 (2026-11-11): drobne, kontrastowe detale przy krawędzi (okno przeglądarki, pasek zadań)

Zgłoszenie: przy ciemnym oknie z ikonami i paskiem zadań taśma jest niestabilna. Pomiar na wyjściu BK
(`bench/bkstab.py`, kolory sekcji z logu co sekundę przez 30 s, obraz nieruchomy): sekcje nad narożnikami okna
(ikona, przyciski, zegar) zmieniały się o 6–23 poziomy diod co sekundę, reszta stała w miejscu.

Przyczyna: na komórkę pola i ramkę przypada kilkanaście próbek, a ich położenie co ramkę jest inne. W komórce
z drobnym, kontrastowym detalem próbki trafiają raz w detal, raz w tło, więc średnia komórki skacze co ramkę
o kilkadziesiąt poziomów. Wygładzanie brało ten rozrzut za zmianę obrazu (różnica powyżej `smooth_t1` → udział 50 %,
powyżej `smooth_t2` → od razu), a blok 8×8 z takimi komórkami wyglądał na „zgodną zmianę”.

- **Szum komórki** (`s_nz`): średnia z modułu różnicy „nowa ramka − szybka średnia”, stała czasowa 16 ramek, liczona
  względem średniej zmiany całego bloku 8×8. Miganie, cięcie albo ściemnienie zmieniają blok zgodnie i szumem nie są.
- **`smooth_nz` (30 = 3,0; 0 wyłącza):** progi wygładzania są przesuwane o `smooth_nz/10` × szum komórki (skok
  chwilowy o dwukrotność tego), próg zgodnej zmiany bloku o tyle samo × szum średniej bloku.
- Komórka o szumie powyżej 4 poziomów jest dłużej uśredniana (udział nowej ramki maleje z kwadratem szumu) i ma
  szerszą martwą strefę wyjścia (o 1/8 szumu). Komórki z czystym obrazem (szum 1–2 poziomy) zachowują się jak dotąd.
- `frames` nie wstrzymuje już próbkowania: wydruk nie blokuje zadania ramek, a wstrzymanie zamrażało taśmę na czas
  testu i psuło pierwszą ramkę kolejnej serii (zawyżało pomiary rozrzutu między ramkami).

Wynik na tym samym nieruchomym oknie, ten sam firmware, `smooth_nz` 0 → 30 (suwaki BK: gamma 2,6, nasycenie 1,8,
głębokość 1): suma rozstępów 28 sekcji 89 → 18..23, największy 23 → 4..7, sekcji z rozstępem ≥ 5: 6 → 0..1.
Reakcja na zmiany (`bench/huetrace.py`): szary 120↔128 co 17 klatek nadąża jak przed zmianą; czerń↔biel co 6 klatek
w obszarze, którego blok obejmuje też nieruchomą część obrazu, dochodzi do ok. 92 % skoku (tylko przy zmianach
powtarzanych częściej niż ok. 3 razy na sekundę; pojedynczy skok przechodzi od razu).

Zapisane ustawienia: klucz NVS zmieniony na `p20` (nowe pole), więc po aktualizacji obowiązują wartości domyślne.

### v14.2: zgodność bloku — pomiar szumu wprost i skok bloku

Dwie poprawki mechanizmu „zgodnej zmiany bloku 8×8” po pomiarach na wzorze `bench/edgedetail.py` (ciemne tło,
w pasie przy krawędziach rzadkie jasne punkty 10×10 px; środek czarny) i komendą `trace`.

- **Szum średniej bloku mierzony wprost** (`s_bnz`: średnia z modułu średniej zmiany komórek bloku na ramkę, stała
  czasowa 16 ramek). W v14 był liczony z szumu komórek przy założeniu, że szumią niezależnie; rozmycie wzdłuż krawędzi
  wiąże sąsiednie komórki, więc szacunek był 2–3 razy za mały i blok z detalami nadal uchodził za „zgodną zmianę”.
  To było główne źródło resztkowej niestabilności: z całkiem wyłączoną zgodnością bloku ten sam wzór dawał sumę
  rozstępów 21–33 zamiast 79–166.
- **Skok bloku:** gdy średnia zmiana komórek bloku w jednej ramce przekracza 2 × `smooth_tc` plus margines szumu bloku,
  komórki, których własna zmiana wystaje ponad ich szum, biorą nową wartość od razu. Wcześniej zmiana mniejsza niż
  `smooth_t2` (32 poziomy) przechodziła przez dwa uśredniania i potrzebowała ok. 4 ramek do 90 %.
- **`trace [n]`:** średni kolor środka pola w n (do 240) kolejnych ramkach wyjściowych, jedna wartość na ramkę.
  `bench/latency.py` liczy z tego, po ilu ramkach wyjście dochodzi do 90 % skoku. Strona testowa liczy okres
  w klatkach własnego odświeżania (np. 240 Hz), więc test sam wyznacza skalę.

Wyniki (`smooth_nz 30`, pozostałe domyślne):

| | przed v14 | v14 | v14.2 |
|---|---|---|---|
| detale przy krawędziach: suma rozstępów 28 sekcji / największy (poziomy taśmy, 25 s) | 274 / 47 | 79 / 13 | 28–35 / 3–4 |
| skok czerń↔biel, 60↔180 co 0,5 s: ramek do 90 % | 0 | 0 | 0 |
| skok 110↔140 co 0,5 s | 4 | 4 | 0 |
| skok 120↔128 co 0,5 s | ok. 5, dochodzi do 50–60 % | jak wcześniej | 0, dochodzi do 98 % |

Ograniczenia: średnia zmiana powtarzana częściej niż ok. 3–4 razy na sekundę podnosi zmierzony szum bloku i wraca
na wolniejszą ścieżkę (110↔140 osiem razy na sekundę: 4 ramki do 90 %). Komórki z drobnym detalem reagują na małe
zmiany wolno z założenia.

## v14.3 (2026-11-12): dziennik zdarzeń części HDMI

Do szukania przyczyny chwilowych zaników obrazu. Komenda `hdmi log [clear]` wypisuje ostatnie 32 zdarzenia z czasem
od startu: każdą zmianę komunikatu wejścia zgłaszanego przez mikrokontroler układu HDMI (`Rx: Active:3840x2160P60Hz
HDR:HDR10 …` albo `Rx: NO SIGNAL`), inne linie jego logu, zanik i powrót VSync oraz zmianę liczby linii w ramce.
`bench/hdmilog.py` przelicza czas na zegarowy, `bench/hdmiwatch.py` zapisuje zdarzenia do pliku przez wiele godzin.

Przypadek, do którego to powstało: przystawka Xiaomi przez 5-metrowy kabel HDMI. Dziennik pokazał pięć zaników
w 45 minut, każdy o tym samym przebiegu (`NO SIGNAL` na wejściu, kilka krótkich powrotów, chwilowe `HDR:SDR`,
powrót do tego samego formatu), bez zmiany trybu przed zanikiem i niezależnie od temperatury boxa. Po wymianie
kabla na krótki: 0 zaników w 40 minut w tym samym trybie. Ten sam 5-metrowy kabel wpięty wprost do TV był stabilny,
a PC (obraz nieszyfrowany) szedł przez niego cały dzień — wniosek roboczy: wejście repeatera ma mniejszy zapas niż
telewizor, a przy szyfrowanym obrazie pojedynczy błąd na łączu kończy się ponownym zestawieniem połączenia.

## v14.4 (2026-11-12): łącze z modułem Wi-Fi — stan HDMI, konsola i wgrywanie firmware bez kabla

Format i protokół są opisane w `main/link.h`. W skrócie:

- **W górę (ESP32 → BK):** po polu obrazu każda ramka SPI niesie 512-bajtowy blok pomocniczy: stan wejścia HDMI
  (aktywne / brak, format, tryb HDR, licznik zaników i zmian formatu), wersję firmware i porcję wyjścia konsoli.
  Gdy nie ma obrazu, idzie ramka bez pola (W = H = 0) z samym blokiem: 5 razy na sekundę, a przy podłączonej
  konsoli co 16 ms.
- **W dół (BK → ESP32):** w tej samej transmisji, po linii MISO, moduł Wi-Fi oddaje blok z porcją wejścia konsoli.
  Robi to tylko wtedy, gdy do boxa podłączony jest klient konsoli; bez klienta SPI pracuje jak dotąd.
- **Linia MISO jest na GPIO16.** Fabryczna konfiguracja ESP32 podaje `output_spi_miso = 19`, ale na GPIO19 nic nie
  ma. Zmierzone: stan wymuszony na P17 modułu Wi-Fi pojawia się na GPIO16 (komenda `link scan` pokazuje piny
  trzymane przez inny układ).
- **Tunel:** jedna porcja naraz w każdym kierunku, powtarzana do potwierdzenia jej numeru przez drugą stronę; CRC16
  na obu blokach. Po podłączeniu klienta i po restarcie ESP32 obie strony zerują numerację.
- **Konsola:** wejście to wspólny strumień z UART0 i z tunelu; wszystko, co idzie na `stdout` (także logi), trafia
  na UART0 i do tunelu. Te same komendy działają po obu drogach, łącznie z `upload`.
- **Firmware na próbie:** obraz wgrany przez tunel startuje „na próbę”. Zostaje, jeśli po restarcie przyjdzie 20
  poprawnych bloków z modułu Wi-Fi; inaczej po 120 s (albo przy restarcie przed potwierdzeniem) wraca poprzednia
  partycja OTA.
- **Ochrona przed pętlą restartów:** czwarty start z rzędu bez 30 s spokojnej pracy przełącza na drugą partycję OTA.
- Nowe komendy: `link` (liczniki łącza, pierwsze bajty z MISO), `link dly <ns>`, `link scan`, `ver` (wersja,
  partycja, wolna sterta).

Narzędzia: `bench/esplink.py` otwiera port szeregowy albo `tcp://adres:2323`; skrypty z `bench/` i
`esp32_upload.py` przyjmują taki adres zamiast `COM4`. Hasło konsoli: zmienna `SYNCBOX_CONSOLE_PASSWORD`.

Pamięć: sterownik UART0 i bufory tunelu nie mieściły się w stercie (pierwsza wersja wpadła w pętlę restartów na
`uart_driver_install`). Bufory diagnostyczne (`grid`, `glitch`, `upload`, `hdmi scope`, wzór testowy) są teraz brane
ze sterty tylko na czas komendy; wolna sterta po starcie ok. 19,7 KB, najmniej 11,8 KB podczas `upload`.

Wyniki (BK v17, ESP32 v14.4, 4K60 HDR10 z przystawki):

| Test | Wynik |
|---|---|
| Czujniki w HA | „HDMI: wejście” = `3840x2160P60 HDR10`, „HDMI: zaniki” = 0 |
| Konsola przez Wi-Fi: `params`, `grid`, `frames 4` | komplet (65 parametrów; 27 z 27 wierszy; 108 z 108 wierszy, 36,6 KB w 4,1 s); 0 błędnych bloków |
| Obraz przy podłączonym kliencie bez komend | 59,9 kl./s, 117 zapisów taśmy/s, 0 ramek uciętych — jak bez klienta |
| Obraz przy `grid` w pętli (91 KB odpowiedzi w 12 s) | 59,0 kl./s, 111 zapisów/s, 7 ramek uciętych w 9 s |
| Wgranie firmware przez Wi-Fi (382 KB) | 19 s razem z restartem i potwierdzeniem |
| Wgranie bez potwierdzenia (klient rozłączony) | po 120 s powrót do poprzedniej partycji, układ pracuje dalej |
| Złe hasło konsoli | połączenie zamykane |

Nie sprawdzone: ochrona przed pętlą restartów (tylko skompilowana). Czujnik „Sygnał HDMI” po zmianie w BK v17
przy braku sygnału na wejściu przez 14 min pokazywał „brak” bez żadnego przeskoku.

## v14.5 (2026-11-13): konwersja HDR10, diagnostyka restartów

### Pomiar toru HDR

`bench/hdrtest.py` (Windows) otwiera na wyświetlaczu podłączonym przez box własne okno HDR10 i wpisuje do niego
dokładne 10-bitowe wartości sygnału (`bench/hdrwin.py`: łańcuch wymiany DXGI R10G10B10A2 w przestrzeni
ST 2084 / BT.2020, bez przeglądarki i odtwarzacza), a z ESP32 czyta komendą `trace`, co z nich zostało.

    python bench/hdrtest.py info                                   lista wyświetlaczy
    python bench/hdrtest.py tcp://192.168.1.50:2323 3 raw prim colors bright

Wynik z komputera (4K60 HDR10, wyjście RGB 8 bitów):

- Tor analogowy jest liniowy względem kodu sygnału: odczyt = kod / 1023 × 255, odstępstwo najwyżej 3 poziomy,
  nasycenie dopiero przy kodzie 1016–1023. Szarości są neutralne (kanały rozjechane średnio o 0,4, najwyżej 2,5).
- Przesłuch między kanałami: przy jednym kanale na kodzie 640 pozostałe pokazują do 7 poziomów, przy 896 do 18.
- Czyli w trybie HDR do ESP32 docierają wartości PQ w barwach BT.2020. Dotychczasowa obróbka (mnożnik nasycenia
  ×2 w HSV, jak w fabrycznym firmware) traktowała je jak zwykły obraz: czerwień wychodziła o 19° w stronę
  pomarańczu, zieleń o 32° w stronę żółci, ciepłe tony o 8–10° w stronę żółtozielonego, biel 203 nity jako 148/255.

### Konwersja

Przy HDR10 i HDR10+ (oraz przy `hdr_mode 2`) każda komórka jest przeliczana: PQ → nity (tablica 256 pozycji),
macierz BT.2020 → BT.709 na świetle liniowym, skala tak, żeby `hdr_white` nitów dało 1, kodowanie sRGB (tablica
4096 pozycji). Kolory jaśniejsze od `hdr_white` są ściemniane wspólnym czynnikiem, więc odcień zostaje; składowa
ujemna (barwa spoza BT.709) jest obcinana do zera. Tablice są stałe i leżą we flashu (`main/hdr_tables.h`,
generowane przez `gen_hdr_tables.py`), więc nie zajmują RAM. Dalej jak w SDR: macierz użytkownika, nasycenie
`sat`, wzmocnienie, gamma, próg `cut`.

- `hdr_conv` (domyślnie 1): 0 wraca do samego mnożnika nasycenia `hdr_gain`.
- `hdr_white` (domyślnie 203): luminancja obrazu w nitach, która daje pełne wysterowanie. Mniejsza wartość
  rozjaśnia taśmę.
- HLG i Dolby Vision mają inne kodowanie i zostają przy `hdr_gain`.
- `stat` pokazuje „HDR: aktywny, konwersja PQ -> sRGB” albo „aktywny, mnoznik nasycenia”.

| 16 kolorów sRGB wyświetlonych w HDR, biel 203 nity | mnożnik nasycenia (v14.4) | konwersja (v14.5) |
|---|---|---|
| błąd barwy średni / największy | 13,2° / 32,1° | 1,2° / 3,0° |
| biały 255 255 255 | 148 149 149 | 249 251 251 |
| szary 64 | 77 77 77 | 65 64 65 |
| czerwony 255 0 0 | 136 44 0 | 255 2 2 |
| zielony 0 255 0 | 92 146 30 | 14 255 1 |
| skóra 224 172 105 | 135 116 73 | 226 169 107 |
| szarość 100 / 203 / 400 nitów | 129 / 148 / 167 | 189 / 250 / 254 |

Pomiar dotyczy wyjścia RGB z komputera. Odtwarzacze nadające YCbCr przechodzą jeszcze przez konwersję barw w układzie
HDMI boxa, której ten pomiar nie obejmuje.

### Parametry

Nowe pola są dopisane na końcu `params_t`, a zapis w NVS ma nowy klucz `p21`. Zapis `p20` z wcześniejszego firmware
jest wczytywany jako początek struktury (kalibracja przechodzi przez aktualizację; sprawdzone: 65 parametrów bez
zmian) i nie jest kasowany, więc powrót do starszej wersji też znajduje swoje parametry.

### Restarty: błąd w ochronie przed pętlą i historia startów

Ochrona przed pętlą restartów trzymała licznik i znacznik w osobnych zmiennych `RTC_NOINIT`. Ich adresy zależały od
tego, jakie inne zmienne tego typu miała dana wersja, więc po aktualizacji nowa wersja czytała „licznik startów”
z miejsca, gdzie poprzednia trzymała znacznik, widziała ponad cztery starty i od razu wracała do starej partycji
(dwa wgrania z rzędu). Teraz cały stan jest w jednej strukturze o stałym układzie (`rtc_state_t g_rtc`: znacznik,
licznik, miejsce w programie, historia), licznik powyżej 8 jest traktowany jak śmieci, a obcy układ pamięci
(inna wersja) zeruje stan.

`ver` pokazuje przyczynę ostatniego startu i historię sześciu startów: wersja / partycja / przyczyna
(`esp_reset_reason`) / miejsce, w którym program był przed restartem (0 poza liczeniem ramki, 1 średnie i rozmycie,
3 wygładzanie i kolory, 4 to samo z konwersją HDR, 5 po kolorach). Historia przeżywa restart po błędzie i powrót do
drugiej partycji, więc widać po fakcie, dlaczego firmware na próbie się nie utrzymał.

`esp32_upload.py` potwierdza wgranie tylko wtedy, gdy ESP32 odpowiada z partycji, do której trafił obraz; wcześniej
zgłaszał „potwierdzony”, gdy odpowiedział poprzedni firmware po powrocie.

| Test (wszystko przez Wi-Fi, bez adaptera) | Wynik |
|---|---|
| Wgranie v14.5 dwa razy z rzędu (obie partycje) | oba potwierdzone, historia: same restarty programowe |
| Cztery restarty w odstępach ok. 9 s | przy czwartym starcie bez 30 s pracy przełączenie na drugą partycję — ochrona przed pętlą działa |
| Parametry po aktualizacji z v14.4 | identyczne, plus `hdr_conv` i `hdr_white` |

Nie wyjaśnione: pierwsze wgranie v14.5 (jeszcze ze starego v14.4) skończyło się restartem nowego obrazu przed
potwierdzeniem i powrotem do v14.4. Ten sam obraz wgrany później wstał bez problemu. Wtedy nie było jeszcze historii
startów; przy powtórce `ver` pokaże przyczynę.
