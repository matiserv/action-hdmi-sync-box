# syncbox - ESPHome dla Eurodomest / LSC HDMI Sync Box 2. gen (T1-U, BK7238)

## Instalacja w Home Assistant (dodatek ESPHome)

1. Skopiuj katalog `components/syncbox` do `/config/esphome/components/syncbox`.
2. Skopiuj `hdmi-syncbox.yaml` do `/config/esphome/` i wpisz swoje Wi-Fi, klucz API i hasło OTA.
3. Zainstaluj przez OTA. Logger musi zostać na `UART1`, bo `P0` (UART2) należy do taśmy.

## Co jest w środku

| Funkcja fabryczna | Jak zrobiona w ESPHome |
|---|---|
| Sterowanie taśmą (UART2 P0, 6,5 Mbaud 6N1, inwerter, BRG) | hub `syncbox` + `light: platform: syncbox` (światło adresowalne, 28 sekcji) |
| Zasilanie taśmy P24 | `enable_pin` (aktywne LOW), odcinane gdy światło wyłączone |
| Synchronizacja HDMI (SPI slave DMA, pole do 48x27 RGB z nagłówkiem) | wątek SPI w hubie, mapowanie na boki; włączana jak scena: efekt światła `syncbox_video` ("Synchronizacja HDMI") |
| Montaż 3/4-stronny, róg startowy, kierunek | `layout:` |
| Kalibracja odstępu od krawędzi (DP 102-110) | `number` "Kalibracja: odstęp ..." + `depth` |
| Wykrywanie czarnych pasów 21:9 / 4:3 | `letterbox_detection` (20 klatek stabilizacji, próg 30/255) |
| Wygładzanie, nasycenie, jasność wideo | `number` "Wideo: ..." |
| Gaszenie przy braku sygnału (DP 101) | `auto_off` + `signal_timeout` |
| Sceny (DP 25/52) | efekty ESPHome na liście `effects` |
| Kolor statyczny i malowanie sekcji (DP 24/51/61) | zwykłe `light.turn_on` oraz `light.addressable_set` z HA |
| Minutnik (DP 26) | `number` "Minutnik wyłączenia" |
| Przycisk P26 | krótko = włącz/wyłącz, 3 s = reset ustawień |
| Tryb muzyczny z mikrofonu P20 | jeszcze nie (wymaga osobnego próbkowania ADC) |
| Łączenie świateł / satelity (multicast do innych lamp Tuya) | `satellites:` - wysyłka E1.31 (sACN) do świateł w sieci; `text_sensor` "Kolor średni" dla zwykłych żarówek przez HA |
| Pamięć trybu po zaniku zasilania | efekt światła jest odtwarzany przez ESPHome razem ze stanem światła |
| Kalibracja kolorów obrazu | `black_level`, `white_level`, `white_balance` (R/G/B), `gamma_correct`, `saturation` + suwaki "Wideo: ..." w HA |
| Odświeżanie taśmy | `refresh_interval` (domyślnie 1 s): bufor wysyłany ponownie, gdy nic się nie zmieniło |
| Sterowanie z sieci (LedFx, Hyperion, xLights, WLED) | efekty `e131` (sACN, port 5568) i `wled` (UDP realtime, port 21324) |

## Dlaczego po włączeniu zasilania taśma świeciła na biało

Dawny select "Tryb" po restarcie odtwarzał swoją wartość z flasha, ale ESPHome nie woła wtedy `set_action`,
więc komponent startował w trybie "światło" z odtworzonym kolorem światła (domyślnie biel) i ignorował
klatki z układu wideo do czasu ręcznej zmiany trybu. Teraz synchronizacja HDMI jest efektem światła:
ESPHome zapisuje aktywny efekt razem ze stanem światła i odtwarza go po restarcie, a w trybie HDMI bez
sygnału taśma jest od razu gaszona (czerń), zamiast pokazywać kolor statyczny.

## Kalibracja kolorów (spłaszczone kolory, niebieskawa biel)

Łańcuch przetwarzania koloru sekcji: średnia komórek (`depth`) -> poziomy `black_level`/`white_level`
-> balans bieli R/G/B -> gamma -> nasycenie -> jasność wideo -> wygładzanie. Wszystko po stronie BK7238,
w HA jako suwaki "Wideo: ...":

1. Biała plansza na TV: suwakami "balans R/G/B" doprowadź taśmę do neutralnej bieli (niebieskawa biel ->
   zmniejsz B, np. 0.85).
2. Czarna plansza: podnieś "poziom czerni", aż taśma zgaśnie (sygnał wideo ma czerń ok. 16/255).
3. Zwykły film: "gamma" 1.6-2.2 pogłębia kontrast (półcienie ciemniejsze, kolory żywsze), "nasycenie"
   1.3-1.8 wzmacnia kolor, "głębokość próbkowania" 1-2 zamiast 3 daje mniej uśrednione (mniej "spłaszczone")
   kolory, bo każda sekcja patrzy na węższy pas przy krawędzi.

## Satelity (E1.31 / sACN)

Box jest nadajnikiem sACN. Każdy wpis w `satellites:` to jeden odbiorca:

```yaml
syncbox:
  satellites:
    - name: "Lampa za kanapa"
      address: 192.168.1.60   # IP odbiorcy albo "multicast" (239.255.x.y wg universe)
      universe: 1
      mode: average           # average | mirror | left | top | right | bottom | full | room (patrz v16)
      leds: 1                 # ile pikseli RGB wysłać (mirror: tyle, ile sekcji ma taśma)
      brightness: 80%
      max_fps: 30
```

- `full` - cała siatka wideo (wierszami od góry) po poziomach/balansie/gammie, po 170 px na universe w kolejnych
  universach od `universe`: pole 48×27 to 1296 px w ośmiu universach, 16×16 to 256 px w dwóch. Bez `address`
  każdy universe idzie do własnej grupy multicast. Dla matryc WLED, Hyperion, drugiego boxa.
- `average` - jeden kolor (średnia całej taśmy) powtórzony `leds` razy; `left/top/right/bottom` - średnia
  danego boku (np. lampa stojąca po lewej stronie TV dostaje kolor lewego boku); `mirror` - kopia
  wszystkich sekcji 1:1 (drugi box z ESPHome przy drugim TV albo pasek WLED tej samej długości).
- Odbiorcy: WLED (Sync Interfaces -> E1.31, DMX mode "Multi RGB"), drugi box z tym komponentem
  (efekt `e131`, `universe` taki sam), kontrolery DMX/Art-Net z bramką sACN, xLights, QLC+.
- Wysyłka idzie przy każdej ramce taśmy, ograniczona do `max_fps`; przy wyłączeniu światła lub
  przełącznika "Satelity (E1.31)" satelity dostają czerń.
- Protokołu Tuya (LinkLite multicast) do oryginalnych żarówek nie da się odtworzyć bez kluczy
  urządzeń; dla takich żarówek użyj `average_color` (text_sensor `#RRGGBB`, max 2 Hz) i automatyzacji
  w HA (`light.turn_on` z `rgb_color`).

## Interpolacja klatek a 60 fps

"Wideo: interpolacja klatek" wygładza przejścia przy wolnym źródle (fabryczny ESP32, 10–12 fps). Przy
własnym firmware ESP32 (60 fps) jest automatycznie pomijana: kolejna klatka przychodziłaby, zanim
przejście zrobi drugi krok, i taśma stałaby w miejscu (objaw: "FPS taśmy" 120, kolor średni bez zmian).

## Wejście z sieci

- `e131:` + efekt `e131` na świetle: 28 sekcji x RGB = 84 kanały od adresu 1 w `universe` 1.
  Działa z LedFx (urządzenie "E1.31"), Hyperion ("UDP E1.31"), xLights, WLED jako nadajnik.
- `wled:` + efekt `wled`: protokół WLED UDP realtime (DRGB/DNRGB, port 21324), np. LedFx jako "WLED".
- Włączenie efektu "E1.31" lub "WLED" wyłącza nadpisywanie przez obraz HDMI (efekt ma pierwszeństwo).

## v15 (2026-11-11): doświetlenie ze środka, poprawki po przeglądzie kodu

### Doświetlenie ze środka obrazu

Suwak „Wideo: doświetlenie ze środka” (0–40 %, startowo 25 %; `video.inner_mix` w YAML). Sekcja pokazuje kolor
krawędzi obrazu jak dotąd. Gdy krawędź jest dużo ciemniejsza niż obraz w głębi, sekcja dostaje brakujące światło
w kolorze tego, co jest w głębi:

    środek sekcji  = pas o szerokości sekcji, od jej prostokąta przy krawędzi do osi kadru (średnia światła)
    podłoga        = procent × (jasność środka − 16)          jasność = najjaśniejsza składowa, skala taśmy 0..255
    jeśli podłoga > jasność krawędzi:  wynik = krawędź + (podłoga − jasność krawędzi) × kolor środka / jasność środka

- Nie ma progu „scena kontrastowa”: dodatek maleje płynnie do zera, gdy krawędź jaśnieje, więc nic nie przeskakuje
  ani nie pulsuje przy scenach na granicy. Krawędź jaśniejsza niż `procent` jasności środka nie zmienia się wcale
  (wynik identyczny jak przy 0 %).
- Nic nie jest przyciemniane i kolor krawędzi nie jest wzmacniany (szum ciemnej krawędzi zostaje mały).
- Liczone po poziomach, balansie, gammie i nasyceniu, więc „25 %” znaczy to samo niezależnie od suwaków.
- Środek jest liczony osobno dla każdej sekcji, więc jasny obiekt z lewej strony ekranu doświetla lewy bok, nie prawy.
- Narasta ze stałą czasową 0,2 s, opada od razu (bez poświaty po cięciu do czerni).
- Tryb satelitów `full` wysyła dalej surowe pole; pozostałe tryby dostają kolory taśmy, czyli z doświetleniem.

Test `bench/innermix.py` (pole z ESP32 → model toru w Pythonie → porównanie z logiem sekcji BK): 14 wzorów,
różnica BK − model najwyżej 1 poziom na wszystkich 28 sekcjach. Przy ustawieniach z dnia testu (gamma 2,6,
nasycenie 1,8, czerń 14, głębokość 1, odstępy 1): czarna krawędź przy białym środku 0 → (51,52,52), przy czerwonym
→ (60,0,0); krawędź (0,0,13) przy pomarańczowym środku → (47,1,13); jasna ramka, jednolity obraz i ciemna scena
bez zmian.

### Poprawki

- **Dwa zapisy taśmy na klatkę szły tuż po sobie.** Błąd różnicy czasu w `loop()` powodował drugi zapis ok. 1 ms po
  pierwszym (FPS taśmy 120 w parach), więc pierwszy stan ditheringu świecił 1 ms, a drugi 15 ms: poziomy pośrednie
  ciemnych kolorów były przekłamane (0,5 poziomu znikało, 0,25 świeciło jak 0,5). Teraz drugi zapis jest planowany
  w połowie okresu klatki (`set_timeout` + wybudzenie pętli), krok ditheringu 1/4 poziomu.
- **Suwaki w HA nie były stosowane po restarcie.** Szablonowe `number` z `set_action` po starcie tylko publikują
  zapamiętaną wartość, więc komponent pracował na wartościach z `video:`, a HA pokazywało co innego. Suwaki działają
  teraz przez `on_value`.
- **Czarne pasy.** Pas to kolumna (wiersz), w której każda komórka jest czarna; wcześniej liczyła się średnia poniżej
  30 i ciemny pulpit zwijał kadr do jasnego fragmentu na środku (log „Kadr: x 20..26”), a taśma co chwilę
  przeskakiwała. Pasy boczne muszą być symetryczne, najwyżej 1/4 szerokości z każdej strony (4:3, 1:1); szerszy
  czarny obszar nie jest pasem. Kadr powiększa się po 3 klatkach, zmniejsza po 1,5 s. Granica trafia w pierwszą
  kolumnę obrazu mimo rozmycia pola w ESP32. Pasy górny i dolny dla ramek z nagłówkiem wycina ESP32.
  Test `bench/crop.py`: 8/8.
- Statystyki raz na sekundę są logowane po wysłaniu klatki na taśmę (wydruk trwa 7–9 ms i opóźniał jedną klatkę
  na sekundę). Po uruchomieniu warto ustawić `log_frames: false`.
- Uszkodzony nagłówek ramki nie jest już traktowany jak ramka bez nagłówka (omijało to sumę kontrolną i resetowało
  kadr); przed uznaniem sygnału nic nie idzie na taśmę; wyłączenie wykrywania pasów przywraca pełny kadr; czujnik
  sygnału liczy czas od zmiany licznika ramek; pakiet E1.31 był o bajt za długi; minutnik wyłączenia wyłączał
  od razu (skrypt czytał poprzedni stan suwaka).
- Przełącznik „Diagnostyka: log sekcji”: raz na sekundę kolory wszystkich sekcji i kadr w logu (dla testów).

Testy rozmawiają z BK przez natywne API (`bench/bkapi.py`, jedno stałe połączenie). Przy wielokrotnym otwieraniu
i zrywaniu `/events` serwera WWW BK zrestartował się w trakcie testu; przyczyna nie została ustalona, przez API
problem nie wystąpił.

## Parametry do dostrojenia po pierwszym uruchomieniu

- `video.log_frames: true` loguje co 60 klatek pierwsze bajty i liczbę klatek pełnych/częściowych.
  Jeśli klatki przychodzą, ale kolory nie pasują do obrazu, zmień `pixel_order` (RGB/GRB/BRG).
  Jeśli nie przychodzą wcale, spróbuj `spi_mode: 1..3`.
- `layout.start_corner` i `direction` ustaw tak, jak fizycznie poprowadzona jest taśma.
- `depth` i odstępy boków odpowiadają fabrycznej kalibracji "odległość od krawędzi".

## FPS

Ramka 28 sekcji to 672 bajty UART, czyli około 0,83 ms plus 300 µs resetu. Taśma nadąża
z ponad 500 ramkami na sekundę; realne tempo wyznacza źródło: układ wideo (zwykle do 60 klatek/s)
albo `update_interval` efektu. Każda odebrana klatka budzi pętlę główną od razu
(`enable_loop_soon_any_context`), bez czekania na cykl 16 ms. Czujniki "FPS wideo" i "FPS taśmy"
pokazują oba tempa.

## Protokół SPI, którego oczekuje odbiornik (dla własnego firmware ESP32)

Poniżej format podstawowy, bez nagłówka (domyślne `frame_size: 768`). Firmware ESP32 z tego repozytorium wysyła
ramki z nagłówkiem, sumą kontrolną i blokiem stanu (`frame_size: 4416`) — opis w sekcjach v13, v14 i v17 oraz
w `esp32/main/link.h`.

- ESP32 jest masterem, BK7238 slave na P14 (SCK), P15 (CS), P16 (MOSI), P17 (MISO).
- Jedna klatka = jedno opuszczenie CS: CS w dół, `frame_size` bajtów (domyślnie 768 = 16x16x3),
  CS w górę. Bez nagłówka i bez sumy kontrolnej. Kolejność bajtów w pikselu wg `pixel_order`,
  wiersze od góry, kolumny od lewej.
- MSB pierwszy, tryb SPI wg `spi_mode` (domyślnie 0).
- Po podniesieniu CS slave potrzebuje krótkiej przerwy na ponowne uzbrojenie DMA: zostaw co najmniej
  0,5 ms odstępu między klatkami. Klatki wysyłane bez przerwy będą częściowo gubione.
- Zegar SPI: fabrycznie 1 MHz (768 B w 6,1 ms). Można podnieść do 2-4 MHz; wtedy klatka trwa 1,5-3 ms.
- Tempo: odbiornik przetwarza i wysyła na taśmę w ok. 1,2 ms na klatkę, więc 60 klatek/s to komfortowy
  cel; powyżej ~120 klatek/s zaczną rosnąć liczniki "zgubione" (widać je w logu przy `log_frames: true`).
- Mniejsza siatka (np. 8x8, `frame_size: 192`, `grid_width/height: 8`) skraca klatkę 4x; mapowanie na
  boki działa dla dowolnego rozmiaru siatki do 64x64.

## v11 (2026-11-10): ciemne kolory bez skakania

Przy ciemnym obrazie kolory sekcji lądowały na poziomach diod 0–4 i przeskakiwały między sąsiednimi stopniami
(zmiana o 1 to wtedy 25–100 % jasności kanału, więc pływał też odcień). Zmiany:

- obliczenia koloru sekcji bez zaokrągleń pośrednich (średnia z komórek i tablica poziomów/gammy jako ułamki);
- **dithering w czasie** w trybie wideo: część ułamkowa jest oddawana przez przełączanie sąsiednich poziomów
  w kolejnych zapisach taśmy (cel zaokrąglany do 1/4 poziomu przy co najmniej 100 zapisach na sekundę, inaczej
  do 1/2). Wyłączenie: `dithering: false` w sekcji `light:`;
- domyślne poziomy czerni/bieli 0/255 (firmware ESP32 v9 wysyła już skalibrowane wartości 0..255).
  Uwaga: suwaki „Wideo: poziom czerni/bieli" pamiętają poprzednie wartości, trzeba je przestawić ręcznie.

## v13 (2026-11-10): pole 32x18 z nagłówkiem ramki

Firmware ESP32 v10 wysyła pole do 32 x 18 punktów (rozmyte po stronie ESP32) w ramce 1744 B:
16 B nagłówka (`SBG2`, szerokość, wysokość, ich negacje bitowe, licznik ramek, 3 x 0, `5A A5 C3 3C`), potem pole
wierszami (R,G,B), reszta zera. Komponent czyta nagłówek w każdej klatce, więc układ pola (np. 16x9, 24x14,
32x18) zmienia się parametrami `grid_w`, `grid_h` w konsoli ESP32 bez przebudowy BK. W YAML:
`frame_size: 1744`, `grid_width: 32`, `grid_height: 18` (wymiary z YAML obowiązują tylko dla ramki bez nagłówka).

Wybór punktów dla taśmy jest po stronie BK: `depth` i odstępy boków liczone są w komórkach pola, więc przy
32x18 jedna komórka to ok. 3 % szerokości i 5,6 % wysokości ekranu (przy 16x16 było 6,25 %). Tryb satelitów `full`
wysyła całe pole: 576 px w czterech universach.

## v14 (2026-11-10): ramki o długości zależnej od pola

ESP32 v11 wysyła nagłówek i dokładnie tyle bajtów, ile ma pole (48x27: 3904 B, 32x18: 1744 B, 16x9: 448 B).
Komponent przyjmuje ramkę krótszą niż `frame_size`, jeśli ma poprawny nagłówek i sumę kontrolną, więc
`frame_size` jest tylko rozmiarem bufora: `frame_size: 3904`, `grid_width: 48`, `grid_height: 27`.
Mniejsze pole można ustawić w konsoli ESP32 bez przebudowy BK. Tryb satelitów `full` przy 48x27 to 1296 px
w ośmiu universach.

## v16 (2026-11-11): satelity — obwód pokoju, wygładzanie, gamma odbiornika

Objaw: lampy z efektem `e131` ESPHome (61 diod dookoła sufitu, ring 41 diod) pokazywały kolor skokami, najmocniej
gdy zmieniał się jeden jasny element obrazu; po pierwszej poprawce zostało wrażenie „klatkowania”.

Przyczyny:

- **Podwójna gamma.** Światło ESPHome nakłada na odebrane wartości własną krzywą (`gamma_correct`, domyślnie 2,8),
  a box wysyłał wartości taśmy, czyli już po gammie wideo. Razem dawało to wykładnik ok. 7: półcienie gasły,
  a mała zmiana jasnego fragmentu zmieniała jasność lampy wielokrotnie.
- **Brak wygładzania po stronie satelity.** Satelita dostawał bufor taśmy po ditheringu (skacze o 1 poziom między
  zapisami) z czasem reakcji taśmy przy ekranie.
- **Rzadkie i nierówne pakiety.** Lampa nie interpoluje: między pakietami stoi w miejscu. Przy 25–30 pakietach
  na sekundę w nierównych odstępach przejście widać jako schodki.

Nowe opcje wpisu w `satellites:` (nie dotyczą `mode: full`):

```yaml
    - name: "Sufit, obwod pokoju"
      address: 192.168.1.61
      universe: 1
      mode: room            # kolory krawedzi ekranu rozlozone po scianach
      walls:                # sciany w kolejnosci tasmy lampy, od pierwszej diody, z liczba diod na kazdej
        - back: 14          # sciana za widzem  = dolna krawedz obrazu
        - left: 16          # lewa sciana       = lewy bok obrazu (tyl pokoju = dol obrazu)
        - front: 15         # sciana z TV       = gorna krawedz
        - right: 16         # prawa sciana      = prawy bok
      max_fps: 60           # pakiet po kazdej klatce obrazu; 30 = co druga klatka
      transition: 300ms     # stala czasowa dochodzenia do nowego koloru (0ms = bez)
      blur: 3               # rozmycie wzdluz tasmy lampy (0 = bez)
      gamma: 2.8            # gamma, ktora odbiornik sam naklada; 1.0 = wartosci wprost
```

- `mode: room`: strony liczone patrząc w stronę TV. Kierunek na każdej ścianie wynika z kolejnej ściany na liście
  (`back, left, front, right` = taśma lampy biegnie od prawego tylnego rogu w lewo; `back, right, front, left` =
  w drugą stronę), więc kolejne wpisy muszą być sąsiednimi ścianami. `leds` jest wtedy sumą ścian (najwyżej 170).
  Każda dioda to mieszanka dwóch sąsiednich sekcji taśmy TV. Krawędź ekranu bez sekcji (u nas dolna) jest płynnym
  przejściem między sekcjami po jej obu stronach, czyli ściana za widzem przechodzi od koloru prawego dolnego rogu
  obrazu do lewego dolnego.
- `blur`: każda dioda staje się średnią z `blur` sąsiadów z każdej strony, liczoną dwa razy (razem okno trójkątne
  ±2 × `blur` diod). Przy `blur: 3` i 61 diodach to ok. jedna piąta obwodu: zostaje podział lewo / góra / prawo,
  znikają drobne plamy. Gdy na liście są wszystkie cztery ściany, okno przechodzi przez początek taśmy.
- `transition`: wygładzanie wykładnicze osobne dla każdej diody satelity. Philips w Ambilight+hue daje lampom
  przejście „3”, a lampom za widzem „5” (jednostki nie są opisane; przyjęte 0,3 s i 0,5 s).
- `gamma`: box wysyła `255 · (poziom/255)^(1/gamma)`, więc po krzywej odbiornika lampa świeci proporcjonalnie do
  taśmy TV. Dla świateł ESPHome bez `gamma_correct` wpisz 2.8.
- Liczenie na liczbach całkowitych (1/256 poziomu); wagi diod i tablica gamma powstają raz, przy pierwszej wysyłce.

Kiedy idą pakiety: bufor taśmy jest odkładany przy zapisie taśmy z nowej klatki, a pakiety do wszystkich
satelitów, którym minął odstęp `max_fps` (z tolerancją 3 ms), wychodzą z `loop()` zaraz potem — już po ustawieniu
czasomierza zapisu pośredniego taśmy. Wysyłka wewnątrz `transmit()` opóźniała ten czasomierz i zapis pośredni
(dithering) wypadał: 97 zapisów taśmy na sekundę zamiast 116. Po zapisie pośrednim pakiety nie wychodzą.

Zapis pośredni taśmy TV jest teraz planowany na połowę okresu klatki liczoną od jej odebrania (wcześniej 8 ms od
końca przetwarzania, czyli ok. 11 ms od klatki) i pomijany, gdy spóźni się poza 5/8 okresu.

Diagnostyka: przełącznik „Diagnostyka: log sekcji” dodaje raz na sekundę dla satelity `room` linie `sat-we`
(sekcje taśmy TV), `sat-cel` (kolory docelowe diod lampy po rozmyciu) i `sat-wy` (wysłane bajty). `bench/satroom.py`
liczy kolory docelowe niezależnie i porównuje: przy `back:14,left:16,front:15,right:16`, `blur` 0 i 3 różnica
najwyżej 0,5 poziomu. Przy `log_frames: true` raz na sekundę jest też linia „satelity: N pakietów, obsługa śr. …”.

Koszt satelitów dla taśmy TV (`bench/satcost.py`, przełącznik „Satelity (E1.31)” włączony / wyłączony):

| pakietów na sekundę (obie lampy razem) | zapisów taśmy/s | ramek wideo/s | ramek uciętych/s |
|---|---|---|---|
| 0 (wyłączone) | 116–117 | 59,4–60,0 | 0–0,5 |
| ok. 60 | 113 | 58,8–58,9 | ok. 0,9 |
| ok. 118 (2 × 60) | 109–110 | 56,3–57,0 | 2–3 |

Obsługa jednego satelity (liczenie i `sendto`) trwa średnio 1,9 ms, najdłużej 5–6,5 ms. Ramki ucięte (odrzucone
przez sumę kontrolną) rosną proporcjonalnie do liczby pakietów, ok. 2 % pakietów, niezależnie od tego, w której
części okresu klatki pakiet wychodzi. Przyczyna nie jest ustalona; przesunięcie zapisu pośredniego taśmy jej nie
usunęło. `max_fps: 30` na obu lampach zmniejsza stratę o połowę.

## v17 (2026-11-12): stan HDMI z ESP32, konsola ESP32 po sieci

Wymaga firmware ESP32 v14.4 lub nowszego (starszy działa jak dotąd, bez nowych funkcji). Format łącza:
`esp32/main/link.h`.

```yaml
syncbox:
  video:
    frame_size: 4416          # 16 B naglowka + pole 48 x 27 x 3 + 512 B bloku pomocniczego ESP32
  hdmi_input:                 # czujnik tekstowy: "3840x2160P60 HDR10", "brak sygnału", "brak danych z ESP32"
    name: "HDMI: wejscie"
  hdmi_dropouts:              # licznik zanikow sygnalu na wejsciu od startu ESP32
    name: "HDMI: zaniki"
  esp32_console:              # serwer TCP konsoli ESP32
    port: 2323
    password: !secret console_password
```

- **Stan wejścia HDMI:** ESP32 dokleja do każdej ramki blok z formatem wejścia, trybem HDR i licznikami. Zmiana
  formatu i każdy zanik trafiają też do logu („HDMI wejście: …”, „HDMI: zanik sygnału na wejściu”).
- **Czujnik „Sygnał HDMI”:** przy ESP32 v14.4 „jest” wymaga dodatkowo, żeby ESP32 zgłaszał aktywne wejście i VSync.
  Wcześniej liczyły się same ramki, a bez sygnału konwerter wystawia przypadkową synchronizację i czujnik skakał.
  Sprawdzone 2026-11-12: bez sygnału na wejściu czujnik przez 14 min pokazywał „brak” bez żadnego przeskoku.
- **Konsola ESP32 po sieci:** jeden klient TCP naraz; pierwsza linia od klienta to hasło. Bajty od klienta idą do
  konsoli ESP32, jej wyjście wraca do klienta — tak samo jak po UART, łącznie z komendą `upload`, czyli wgrywaniem
  firmware ESP32 (`esp32_upload.py tcp://adres:2323 firmware.bin`). Bez hasła każdy w sieci lokalnej może sterować
  ESP32 i wgrać mu firmware, więc hasło należy ustawić.
- **Nadawanie do ESP32 (linia MISO, P17)** jest włączane tylko na czas połączenia klienta. Bez klienta transfer SPI
  jest wyłącznie odbiorczy, jak przed v17.
- Ramka bez pola (W = H = 0) niesie sam blok pomocniczy i nie liczy się jako klatka wideo.
- Gdy ESP32 nie potwierdzi zestawienia tunelu w 3 s (stary firmware), klient dostaje
  `ERR ESP32 nie odpowiada na laczu zwrotnym` i jest rozłączany.
- ESPHome liczy limity lwIP z deklaracji komponentów; serwer konsoli zgłasza jedno gniazdo nasłuchujące i jedno
  połączenie (`socket.consume_sockets`).

Pomiary są w `esp32/README.md`, sekcja v14.4.

## v17.1 (2026-11-13): poprawki po przeglądzie kodu

- **Satelita `full` z multicastem:** adres grupy był wyliczany raz, z pierwszego universe lampy, więc pakiety
  kolejnych universe (pole 48×27 zajmuje osiem) szły do grupy pierwszego. Teraz każdy universe ma swój adres
  `239.255.<hi>.<lo>`. Przy `address` ustawionym wprost nic się nie zmienia.
- **Satelita `full` czyta kopię pola.** Pakiety idą z końca `loop()`, już po oddaniu bufora klatki wątkowi SPI, który
  mógł w tym czasie wpisywać następną klatkę; pakiety mogły zawierać fragmenty dwóch klatek. Kopia (pole × 3 bajty)
  jest robiona tylko wtedy, gdy w konfiguracji jest satelita `full`.
- **Stan wejścia HDMI:** tekst „HDMI: wejście” nie jest publikowany z kopii stanu, której nie udało się odczytać spójnie
  (cztery kolizje z wątkiem SPI z rzędu); liczniki i flagi były już tak chronione.
- Czas obsługi satelity `full` wchodzi do statystyki „satelity: … obsługa śr. … us”.

Sprawdzone: kompilacja z satelitą `full` i bez satelitów; pakiety `full` z multicastem odebrane na komputerze
(opis pomiaru w `esp32/README.md`, sekcja v14.6).
