#pragma once
#include <stdint.h>
#include <stdbool.h>

void spi_out_init(void);
// Kopiuje ramke (piksele pola GRID_W x GRID_H, RGB wierszami) do bufora i zleca wysylke. Nieblokujace.
void spi_out_submit(const uint8_t *frame);
uint32_t spi_out_frames_sent(void);
float spi_out_fps(void);
// Wymuszenie nowego zegara (po "set spi_hz" z konsoli).
void spi_out_reconfigure(void);
