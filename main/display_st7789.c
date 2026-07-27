/*
 * Minimal ST7789 SPI display driver: no full framebuffer (would cost
 * ~150KB for a 320x240 RGB565 panel, too much for RAM-constrained chips
 * like the C3) -- fill/text primitives stream directly to the panel via
 * esp_lcd_panel_draw_bitmap() instead, batched in chunks of FILL_CHUNK_ROWS
 * rows (fill_area) or a whole glyph cell at a time (draw_glyph) rather than
 * one SPI transaction per pixel row -- a naive one-row-at-a-time version of
 * this file made every menu redraw visibly crawl (~3s, hundreds of tiny
 * transactions per row of text), see issue #19.
 */

#include "display.h"
#include "font5x7.h"
#include "sdkconfig.h"

#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "display";

#define DISPLAY_SPI_HOST   SPI2_HOST
#define DISPLAY_WIDTH      CONFIG_LAUNCHER_DISPLAY_WIDTH
#define DISPLAY_HEIGHT     CONFIG_LAUNCHER_DISPLAY_HEIGHT
#define DISPLAY_GPIO_BL    CONFIG_LAUNCHER_DISPLAY_GPIO_BL

/* ESP-IDF/Kconfig only emits a #define for a bool option when it is "y" --
 * when "n", the symbol is absent from sdkconfig.h entirely rather than
 * defined as 0 (see CLAUDE.md gotchas). These are read as plain C
 * expressions below (esp_lcd_panel_swap_xy/mirror args), not just inside
 * #if, so provide 0 fallbacks explicitly for whichever default to "n". */
#ifndef CONFIG_LAUNCHER_DISPLAY_SWAP_XY
#define CONFIG_LAUNCHER_DISPLAY_SWAP_XY 0
#endif
#ifndef CONFIG_LAUNCHER_DISPLAY_MIRROR_X
#define CONFIG_LAUNCHER_DISPLAY_MIRROR_X 0
#endif
#ifndef CONFIG_LAUNCHER_DISPLAY_MIRROR_Y
#define CONFIG_LAUNCHER_DISPLAY_MIRROR_Y 0
#endif

static esp_lcd_panel_handle_t s_panel;

/* Matches spi_bus_config_t.max_transfer_sz below (DISPLAY_WIDTH * 32 *
 * sizeof(uint16_t)) -- batching fill_area() into chunks of this many rows
 * per esp_lcd_panel_draw_bitmap() call, instead of one call per row, is
 * most of the fix for issue #19 (240 transactions for a full-screen clear
 * down to ~8). */
#define FILL_CHUNK_ROWS 32
/* Ping-ponged: esp_lcd_panel_draw_bitmap() hands the buffer straight to the
 * SPI driver's DMA queue and can return before the hardware has actually
 * finished reading it (see issue #21) -- reusing a single static buffer on
 * the very next call risks overwriting still-in-flight pixel data. The
 * driver drains all prior in-flight transactions at the start of its next
 * command, so alternating between two buffers guarantees one full
 * intervening call's worth of drain time before a given buffer is touched
 * again. */
static uint16_t s_line_buf[2][DISPLAY_WIDTH * FILL_CHUNK_ROWS];
static int s_line_buf_idx;

static void fill_area(int x, int y, int w, int h, display_color_t color) {
    if (w <= 0 || h <= 0 || s_panel == NULL) {
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH) { w = DISPLAY_WIDTH - x; }
    if (y + h > DISPLAY_HEIGHT) { h = DISPLAY_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    uint16_t *buf = s_line_buf[s_line_buf_idx ^= 1];

    int first_chunk_rows = h < FILL_CHUNK_ROWS ? h : FILL_CHUNK_ROWS;
    for (int i = 0; i < w * first_chunk_rows; i++) {
        buf[i] = color;
    }

    int row = 0;
    while (row < h) {
        int chunk_rows = (h - row) < FILL_CHUNK_ROWS ? (h - row) : FILL_CHUNK_ROWS;
        esp_lcd_panel_draw_bitmap(s_panel, x, y + row, x + w, y + row + chunk_rows, buf);
        row += chunk_rows;
    }
}

esp_err_t display_init(void) {
    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_LAUNCHER_DISPLAY_GPIO_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_LAUNCHER_DISPLAY_GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 32 * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = CONFIG_LAUNCHER_DISPLAY_GPIO_CS,
        .dc_gpio_num = CONFIG_LAUNCHER_DISPLAY_GPIO_DC,
        .spi_mode = 0,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_LAUNCHER_DISPLAY_GPIO_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    /* Most 2.4" ST7789 modules are natively portrait (240x320); swap_xy is
     * what actually produces the landscape (WIDTHxHEIGHT) image this driver
     * assumes -- see CONFIG_LAUNCHER_DISPLAY_SWAP_XY. mirror_x/y are
     * board-specific and can't be determined without the physical panel in
     * hand, adjust via menuconfig if the image comes out flipped. */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, CONFIG_LAUNCHER_DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, CONFIG_LAUNCHER_DISPLAY_MIRROR_X, CONFIG_LAUNCHER_DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    if (DISPLAY_GPIO_BL >= 0) {
        gpio_config_t bl_cfg = {
            .pin_bit_mask = 1ULL << DISPLAY_GPIO_BL,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&bl_cfg);
        gpio_set_level(DISPLAY_GPIO_BL, 1);
    }

    ESP_LOGI(TAG, "ST7789 ready (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return ESP_OK;
}

int display_width(void) {
    return DISPLAY_WIDTH;
}

int display_height(void) {
    return DISPLAY_HEIGHT;
}

void display_fill_screen(display_color_t color) {
    fill_area(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

void display_fill_rect(int x, int y, int w, int h, display_color_t color) {
    fill_area(x, y, w, h, color);
}

/* 6*MAX_GLYPH_SCALE wide (5 font columns + 1 spacer) x 7*MAX_GLYPH_SCALE tall.
 * Ping-ponged for the same reason as s_line_buf above (issue #21): back-to-back
 * draw_glyph() calls (one per character in display_draw_text()'s loop) would
 * otherwise overwrite a shared buffer while its previous contents might still
 * be in flight over SPI/DMA, producing garbled glyphs. */
#define MAX_GLYPH_SCALE 6
static uint16_t s_glyph_buf[2][6 * MAX_GLYPH_SCALE * 7 * MAX_GLYPH_SCALE];
static int s_glyph_buf_idx;

static void draw_glyph(int x, int y, char ch, display_color_t fg, display_color_t bg, int scale) {
    ch = (char)toupper((unsigned char)ch);
    const uint8_t *cols;
    if (ch < FONT5X7_FIRST_CHAR || ch > FONT5X7_LAST_CHAR) {
        cols = font5x7_dense[0]; /* blank */
    } else {
        cols = font5x7_dense[ch - FONT5X7_FIRST_CHAR];
    }

    if (scale > MAX_GLYPH_SCALE) {
        scale = MAX_GLYPH_SCALE; /* clamp rather than overrun s_glyph_buf */
    }
    int glyph_w = 6 * scale; /* 5 font columns + 1 blank spacer column */
    int glyph_h = 7 * scale;
    if (x < 0 || y < 0 || x + glyph_w > DISPLAY_WIDTH || y + glyph_h > DISPLAY_HEIGHT) {
        return; /* off-screen: skip rather than deal with partial-buffer clipping */
    }

    /* Build the whole glyph cell in RAM and issue a single draw_bitmap call,
     * instead of up to 35 tiny per-pixel-block transactions -- this is what
     * actually made text-heavy screens slow (see issue #19), fill_area()'s
     * own chunking helps but text was the dominant cost. */
    uint16_t *buf = s_glyph_buf[s_glyph_buf_idx ^= 1];
    for (int gy = 0; gy < glyph_h; gy++) {
        int font_row = gy / scale;
        for (int gx = 0; gx < glyph_w; gx++) {
            bool on = false;
            if (gx < 5 * scale) {
                int font_col = gx / scale;
                on = (cols[font_col] & (1 << font_row)) != 0;
            }
            buf[gy * glyph_w + gx] = on ? fg : bg;
        }
    }
    esp_lcd_panel_draw_bitmap(s_panel, x, y, x + glyph_w, y + glyph_h, buf);
}

void display_draw_text(int x, int y, const char *text, display_color_t fg, display_color_t bg, int scale) {
    if (scale <= 0) {
        scale = 1;
    }
    int cursor_x = x;
    for (const char *p = text; *p != '\0'; p++) {
        draw_glyph(cursor_x, y, *p, fg, bg, scale);
        cursor_x += 6 * scale; /* 5 glyph columns + 1 spacer column */
    }
}

int display_text_width(const char *text, int scale) {
    if (scale <= 0) {
        scale = 1;
    }
    return (int)strlen(text) * 6 * scale;
}
