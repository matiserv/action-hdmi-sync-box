#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Tryb HDR odczytany z logu MCU ukladu HDMI (UART2). Wartosci 0..4 jak w fabrycznym firmware.
enum {
    HDR_SDR = 0,
    HDR_HDR10 = 1,
    HDR_HDR10P = 2,
    HDR_DOLBY = 3,
    HDR_HLG = 4,
    HDR_INVALID = 5,     // "HDR:Invalid": fabryczny firmware traktuje to jak HDR (wartosc 2)
    HDR_UNKNOWN = 255,   // nic jeszcze nie odebrano
};

extern volatile uint8_t g_hdr_mode;

typedef struct {
    uint32_t rx_bytes;      // bajtow odebranych z UART2
    uint32_t rx_zero;       // w tym bajtow 0x00 (linia w stanie niskim)
    uint32_t tokens;        // rozpoznanych napisow "HDR:..."
    uint8_t  hdr_mode;
    char     last_text[161];
} hdmi_status_t;

void hdmi_init(void);
void hdmi_reset_pulse(void);
void hdmi_apply(void);     // predkosc i polaryzacja RX z parametrow
int hdmi_scope(uint32_t ms, uint32_t *dur_us, int max, int *first_level);   // czasy stanow linii RX
void hdmi_get_status(hdmi_status_t *st);
const char *hdmi_hdr_name(uint8_t mode);
// Wysyla surowe bajty po UART2 (fabryczny firmware nic nie nadaje; tylko do eksperymentow).
void hdmi_uart_send(const uint8_t *data, size_t len);
