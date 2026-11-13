#pragma once
// Lacze z modulem Wi-Fi (BK7238) poza samym obrazem: stan czesci HDMI, konsola i wgrywanie firmware przez Wi-Fi.
//
// W gore (ESP32 -> BK, linia MOSI): po polu obrazu kazda ramka niesie blok pomocniczy AUX_BYTES:
//   0..3   'S','B','X','1'
//   4      flagi: bit0 wejscie HDMI aktywne (wg MCU ukladu HDMI), bit1 jest VSync, bit2 konsola podlaczona,
//          bit3 firmware na probie (po wgraniu przez tunel, jeszcze niepotwierdzony), bit4 potwierdzenie zerowania tunelu,
//          bit5 prosba o zerowanie tunelu (ESP32 dopiero wystartowal)
//   5      tryb HDR (jak g_hdr_mode)
//   6..7   szerokosc wejscia, 8..9 wysokosc (mlodszy bajt pierwszy), 10 odswiezanie Hz, 11 'P' / 'I'
//   12..13 licznik zanikow VSync, 14..15 licznik zmian formatu wejscia
//   16..19 czas od startu w sekundach
//   20     wersja firmware x 10 (146 = v14.6)
//   21     numer porcji tunelu w gore, 22 numer ostatniej przyjetej porcji w dol, 23 rezerwa
//   24..25 dlugosc porcji w gore, 26.. dane (do LINK_UP_PAY bajtow)
//   ostatnie 2 bajty: CRC16 (CCITT, 0xFFFF) z calej reszty bloku, starszy bajt pierwszy
// Ramka bez obrazu (W = H = 0 w naglowku) niesie sam blok pomocniczy; idzie, gdy nie ma sygnalu wideo.
//
// W dol (BK -> ESP32, linia MISO, na poczatku tej samej transmisji), tylko gdy do BK podlaczony jest klient konsoli:
//   kilka bajtow zerowych, potem 'S','B','D','1', flagi (bit0 konsola podlaczona, bit1 zerowanie tunelu),
//   numer porcji w dol, numer ostatniej przyjetej porcji w gore, dlugosc (2 B), dane (do LINK_DN_MAX), CRC16.
//
// Tunel w obu kierunkach: jedna porcja naraz, powtarzana w kolejnych ramkach, az druga strona potwierdzi jej numer.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "pins.h"

#define LINK_FW_VERSION 146                      // v14.6
#define LINK_UP_HDR     26
#define LINK_UP_PAY     (AUX_BYTES - LINK_UP_HDR - 2)
#define LINK_DN_MAX     1024
#define LINK_DN_RX      1100                     // tyle poczatkowych bajtow MISO jest odbieranych z kazdej ramki

void link_init(void);
uint16_t link_crc16(const uint8_t *d, size_t n);

// --- wolane z zadania SPI ---
void link_build_aux(uint8_t *aux);                       // wypelnia AUX_BYTES bajtow bloku pomocniczego
void link_parse_downlink(const uint8_t *rx, size_t n);   // poczatek danych odebranych po MISO
bool link_attached(void);                                // klient konsoli podlaczony po stronie BK (ostatnie 2 s)

// --- wyjscie konsoli (kopia wszystkiego, co idzie na stdout) ---
void link_out_write(const char *data, size_t n);

// --- wejscie konsoli: wspolny strumien bajtow z UART0 i z tunelu ---
bool link_in_push(const uint8_t *data, size_t n, bool from_tunnel);   // false = brak miejsca, nic nie wpisano
size_t link_in_read(uint8_t *buf, size_t max, TickType_t timeout);
size_t link_in_space(void);
bool link_last_input_from_tunnel(void);

// --- firmware "na probie": wgrany przez tunel jest cofany, jesli po restarcie lacze z BK nie zadziala ---
void link_trial_arm(void);          // przed restartem po wgraniu przez tunel
void link_trial_boot(void);         // na starcie, po nvs_flash_init
uint8_t link_trial_state(void);     // 0 nie, 1 uzbrojony, 2 na probie

typedef struct {
    uint32_t dn_ok, dn_bad, dn_none;     // bloki w dol: poprawne, z bledem CRC/dlugosci, bez znacznika
    uint32_t up_chunks, dn_chunks;       // porcje tunelu wyslane (potwierdzone) i przyjete
    uint32_t out_dropped;                // bajty wyjscia konsoli odrzucone (tunel zatkany)
    uint8_t raw[16];                     // pierwsze bajty ostatnio odebrane po MISO
    bool attached;
} link_stats_t;
void link_get_stats(link_stats_t *s);
