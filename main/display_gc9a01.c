/*
 * Minimal GC9A01 SPI display driver -- sibling of display_st7789.c for
 * round 240x240 IPS panels (the common 1.28" module). Same "no framebuffer,
 * stream fill/text primitives straight to the panel via
 * esp_lcd_panel_draw_bitmap()" approach and the same chunked-transfer /
 * ping-ponged-buffer performance fixes as display_st7789.c -- see that
 * file's header and issues #19 / #21 for the reasoning; only panel creation
 * differs here.
 *
 * ESP-IDF's esp_lcd ships st7789/nt35510/ssd1306 but not GC9A01, so this
 * pulls the espressif/esp_lcd_gc9a01 managed component (main/idf_component.yml).
 * Compiled only when CONFIG_LAUNCHER_DISPLAY_DRIVER_GC9A01=y (see
 * main/CMakeLists.txt); display_st7789.c takes over otherwise.
 */

#include "sdkconfig.h"

#if CONFIG_LAUNCHER_DISPLAY_DRIVER_GC9A01

#include "display.h"
#include "font5x7.h"

#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_log.h"

static const char *TAG = "display";

#define DISPLAY_SPI_HOST   SPI2_HOST
#define DISPLAY_WIDTH      CONFIG_LAUNCHER_DISPLAY_WIDTH
#define DISPLAY_HEIGHT     CONFIG_LAUNCHER_DISPLAY_HEIGHT

/* Kconfig only emits a #define for a bool option when it is "y"; when "n"
 * the symbol is absent entirely (not defined as 0). These are read as plain
 * C expressions below (panel swap/mirror args), so provide 0 fallbacks. See
 * CLAUDE.md gotchas and the identical block in display_st7789.c. */
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

/* See display_st7789.c for why this is chunked and ping-ponged (issues #19, #21). */
#define FILL_CHUNK_ROWS 32
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
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_gc9a01 failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* GC9A01 panels are IPS -- colour is inverted, same as the ST7789 IPS modules. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    /* 240x240 square panel: no swap_xy needed by default (unlike the 2.4"
     * portrait ST7789). mirror_x/y depend on the specific module's scan
     * direction/mounting -- adjust via menuconfig if the image is flipped. */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, CONFIG_LAUNCHER_DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, CONFIG_LAUNCHER_DISPLAY_MIRROR_X, CONFIG_LAUNCHER_DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* Compile-time guard, not just runtime: GC9A01 modules often hard-wire
     * the backlight to 3V3 (CONFIG_LAUNCHER_DISPLAY_GPIO_BL = -1), and
     * "1ULL << -1" is a -Wshift-count-negative warning even when the
     * enclosing `if` would never run it. */
#if CONFIG_LAUNCHER_DISPLAY_GPIO_BL >= 0
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_LAUNCHER_DISPLAY_GPIO_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(CONFIG_LAUNCHER_DISPLAY_GPIO_BL, 1);
#endif

    ESP_LOGI(TAG, "GC9A01 ready (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
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

/* Glyph cell: 6*MAX_GLYPH_SCALE wide x 7*MAX_GLYPH_SCALE tall. Ping-ponged
 * for the same reason as s_line_buf (issue #21). Identical to display_st7789.c. */
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
        scale = MAX_GLYPH_SCALE;
    }
    int glyph_w = 6 * scale;
    int glyph_h = 7 * scale;
    if (x < 0 || y < 0 || x + glyph_w > DISPLAY_WIDTH || y + glyph_h > DISPLAY_HEIGHT) {
        return;
    }

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
        cursor_x += 6 * scale;
    }
}

int display_text_width(const char *text, int scale) {
    if (scale <= 0) {
        scale = 1;
    }
    return (int)strlen(text) * 6 * scale;
}

#endif /* CONFIG_LAUNCHER_DISPLAY_DRIVER_GC9A01 */
