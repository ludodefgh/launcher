/*
 * Minimal ST7789 SPI display driver: no full framebuffer (would cost
 * ~150KB for a 320x240 RGB565 panel, too much for RAM-constrained chips
 * like the C3) -- fill/text primitives stream row-by-row directly to the
 * panel via esp_lcd_panel_draw_bitmap() instead.
 */

#include "display.h"
#include "font5x7.h"
#include "sdkconfig.h"

#include <string.h>
#include <ctype.h>

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

static esp_lcd_panel_handle_t s_panel;
static uint16_t s_line_buf[DISPLAY_WIDTH];

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
    for (int i = 0; i < w; i++) {
        s_line_buf[i] = color;
    }
    for (int row = 0; row < h; row++) {
        esp_lcd_panel_draw_bitmap(s_panel, x, y + row, x + w, y + row + 1, s_line_buf);
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

static void draw_glyph(int x, int y, char ch, display_color_t fg, display_color_t bg, int scale) {
    ch = (char)toupper((unsigned char)ch);
    const uint8_t *cols;
    if (ch < FONT5X7_FIRST_CHAR || ch > FONT5X7_LAST_CHAR) {
        cols = font5x7_dense[0]; /* blank */
    } else {
        cols = font5x7_dense[ch - FONT5X7_FIRST_CHAR];
    }

    for (int col = 0; col < 5; col++) {
        uint8_t bits = cols[col];
        for (int row = 0; row < 7; row++) {
            display_color_t px = (bits & (1 << row)) ? fg : bg;
            fill_area(x + col * scale, y + row * scale, scale, scale, px);
        }
    }
    /* 1-column blank spacer between glyphs, at native scale. */
    fill_area(x + 5 * scale, y, scale, 7 * scale, bg);
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
