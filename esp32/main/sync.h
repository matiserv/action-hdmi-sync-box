#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern volatile uint32_t g_abs_line;          // absolutny numer ostatniej linii (rosnie bez konca)
extern volatile uint32_t g_frame_first_line;  // absolutny numer pierwszej linii biezacej ramki
extern volatile uint32_t g_frame_count;
extern volatile int64_t  g_vsync_ts;          // czas ostatniego VSync (us, esp_timer)
extern volatile uint32_t g_lines_last_frame;  // ile HSync bylo w poprzedniej ramce (hs_total)
extern volatile int64_t  g_frame_period_us;   // okres ramki (us)
extern volatile uint32_t g_isr_late_max_cyc;  // najwieksze opoznienie wejscia w przerwanie wzgledem zbocza (cykle CPU)

// Jedno przerwanie GPIO (poziom 3, rdzen 1) dla HSync i VSync; HSync woła capture_on_hsync(), VSync budzi notify_task.
void sync_init(TaskHandle_t notify_task);
// Aktywne linie dla zmierzonego hs_total: najpierw parametry uzytkownika, potem tabela formatow, potem heurystyka.
void sync_get_active(uint32_t hs_total, uint32_t *vstart, uint32_t *vlines, const char **name);
float sync_line_period_us(void);   // sredni okres linii z ostatniej ramki
float sync_frame_rate(void);       // Hz
