#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal ST7789 (SPI) text/rect display module. Deliberately not a generic
 * graphics HAL: the launcher only ever needs to draw a title, a vertical
 * list of menu entries, and a highlight -- see ui_menu.c. Pin mapping and
 * panel size come from Kconfig (CONFIG_LAUNCHER_DISPLAY_*) so they can be
 * adjusted per board without touching this file. */

typedef uint16_t display_color_t; /* RGB565 */

#define DISPLAY_COLOR_BLACK   ((display_color_t)0x0000)
#define DISPLAY_COLOR_WHITE   ((display_color_t)0xFFFF)
#define DISPLAY_COLOR_GRAY    ((display_color_t)0x39E7)

esp_err_t display_init(void);

int display_width(void);
int display_height(void);

void display_fill_screen(display_color_t color);

void display_fill_rect(int x, int y, int w, int h, display_color_t color);

/* Draws text at (x, y) top-left, scaled by an integer factor (1 = native
 * 5x7). Only space/digits/A-Z/basic punctuation are supported (see
 * font5x7.h); unsupported characters are skipped (rendered as blank cells).
 * Lowercase letters are uppercased; French accents are not supported in v1
 * and should be avoided in strings passed here (see README). */
void display_draw_text(int x, int y, const char *text, display_color_t fg, display_color_t bg, int scale);

int display_text_width(const char *text, int scale);

#ifdef __cplusplus
}
#endif
