#pragma once
// Plytka A4E32MGS02KR (Action / Eurodomest HDMI Sync Box). Piny wg analizy partycji config ESP32.

// Analogowe RGB z toru wideo (645..1170 mV)
#define PIN_ADC_R        35   // ADC1_CH7
#define PIN_ADC_G        32   // ADC1_CH4
#define PIN_ADC_B        33   // ADC1_CH5
#define ADC_CH_R         ADC_CHANNEL_7
#define ADC_CH_G         ADC_CHANNEL_4
#define ADC_CH_B         ADC_CHANNEL_5

// Synchronizacja
#define PIN_HSYNC        39
#define PIN_VSYNC        34

// Uklad HDMI (GSV2002 / MCU posredniczace)
#define PIN_HDMI_RESET   22   // aktywny LOW
#define PIN_HDMI_UART_TX 26
#define PIN_HDMI_UART_RX 25
#define HDMI_UART_NUM    UART_NUM_2
#define HDMI_UART_BAUD   115200

// SPI master -> BK7238 (slave na P14/P15/P16/P17)
#define PIN_SPI_SCLK     4
#define PIN_SPI_CS       5
#define PIN_SPI_MOSI     27
#define PIN_SPI_MISO     19

// Dioda statusowa na plytce
#define PIN_STATUS_LED   18

// Siatka wyjsciowa
// Pole wyjsciowe: do 48 kolumn x 27 wierszy (parametry grid_w, grid_h; domyslnie 48 x 27). Tryb RTC (adc_mode 0)
// probkuje zawsze 16 kolumn. Ramka SPI: 16 B naglowka + dane pola wierszami (R,G,B); dlugosc zalezy od pola
// (48 x 27: 3904 B). Naglowek: 'S','B','G','2', szerokosc, wysokosc, ich negacje bitowe, licznik ramek, suma bajtow pola
// (16 bitow, mlodszy bajt pierwszy), 1, 5A A5 C3 3C. BK7238 czyta z niego uklad pola w kazdej klatce.
#include <stdint.h>
#define GRID_W_MAX       48
#define GRID_H_MAX       27
#define RTC_W            16      // tryb RTC: 16 kolumn
#define RTC_H_MAX        18      // i najwyzej 18 wierszy
extern uint8_t g_grid_w, g_grid_h;
#define GRID_W           ((int) g_grid_w)
#define GRID_H           ((int) g_grid_h)
#define PIX_BYTES_MAX    (GRID_W_MAX * GRID_H_MAX * 3)
#define GRID_BYTES       (GRID_W * GRID_H * 3)
#define WIRE_HDR         16
#define FRAME_BYTES      (WIRE_HDR + PIX_BYTES_MAX)
