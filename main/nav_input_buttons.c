/*
 * Three-button GPIO navigation driver (up / down / select) for boards with
 * no rotary encoder.
 *
 * Buttons are momentary to GND with the ESP32's internal pull-ups enabled
 * (active low) -- a jumper wire tapped to GND works as a stand-in for a
 * real button during bring-up. Debounced by a short polling task (a level
 * must be stable for DEBOUNCE_SAMPLES reads) rather than a GPIO ISR: a
 * hand-held wire bounces heavily and a menu has no latency requirement, so
 * polling is both simpler and more robust here, and it avoids sharing the
 * GPIO ISR service. SELECT distinguishes a short click (NAV_EVENT_SELECT)
 * from a long press (NAV_EVENT_LONG_PRESS) by hold duration, matching
 * nav_input_ec11.c's button behaviour.
 *
 * SELECT also doubles as the bootloader-level crash-loop-recovery pin
 * (CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET) -- see its Kconfig help and the
 * same note on LAUNCHER_EC11_GPIO_SW.
 */

#include "sdkconfig.h"

#if CONFIG_LAUNCHER_NAV_DRIVER_BUTTONS

#include "nav_input.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nav_btn";

enum { IDX_UP = 0, IDX_DOWN = 1, IDX_SELECT = 2, BTN_COUNT = 3 };

static const gpio_num_t s_pins[BTN_COUNT] = {
    CONFIG_LAUNCHER_BUTTONS_GPIO_UP,
    CONFIG_LAUNCHER_BUTTONS_GPIO_DOWN,
    CONFIG_LAUNCHER_BUTTONS_GPIO_SELECT,
};

#define POLL_MS          5
#define DEBOUNCE_SAMPLES 3       /* level stable for 3 * 5 ms = 15 ms */
#define LONG_PRESS_MS    CONFIG_LAUNCHER_LONG_PRESS_MS

typedef struct {
    uint8_t stable;         /* debounced level: 1 = released, 0 = pressed */
    uint8_t last_raw;
    uint8_t raw_run;        /* consecutive equal raw samples */
    int64_t pressed_at_us;
    bool long_fired;
} button_state_t;

static button_state_t s_btn[BTN_COUNT];
static nav_event_cb_t s_cb;
static TaskHandle_t s_task;

static void emit(nav_event_t ev) {
    if (s_cb) {
        s_cb(ev);
    }
}

static void poll_task(void *arg) {
    for (;;) {
        const int64_t now = esp_timer_get_time();

        for (int i = 0; i < BTN_COUNT; i++) {
            button_state_t *b = &s_btn[i];
            const uint8_t raw = (uint8_t)gpio_get_level(s_pins[i]);

            if (raw == b->last_raw) {
                if (b->raw_run < DEBOUNCE_SAMPLES) {
                    b->raw_run++;
                }
            } else {
                b->last_raw = raw;
                b->raw_run = 1;
            }
            if (b->raw_run < DEBOUNCE_SAMPLES || raw == b->stable) {
                continue;
            }

            b->stable = raw;                 /* debounced edge */
            const bool pressed = (raw == 0); /* active low */

            if (pressed) {
                b->pressed_at_us = now;
                b->long_fired = false;
                if (i == IDX_UP) {
                    emit(NAV_EVENT_UP);
                } else if (i == IDX_DOWN) {
                    emit(NAV_EVENT_DOWN);
                }
                /* SELECT acts on release (short) or hold (long), below. */
            } else if (i == IDX_SELECT && !b->long_fired) {
                emit(NAV_EVENT_SELECT);
            }
        }

        button_state_t *sel = &s_btn[IDX_SELECT];
        if (sel->stable == 0 && !sel->long_fired &&
            (now - sel->pressed_at_us) >= (int64_t)LONG_PRESS_MS * 1000) {
            sel->long_fired = true;
            emit(NAV_EVENT_LONG_PRESS);
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static esp_err_t buttons_init(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        s_btn[i] = (button_state_t){ .stable = 1, .last_raw = 1, .raw_run = DEBOUNCE_SAMPLES };
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
    }

    if (xTaskCreate(poll_task, "nav_btn", 3072, NULL, tskIDLE_PRIORITY + 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "buttons ready (UP=%d DOWN=%d SELECT=%d, active low)",
             s_pins[IDX_UP], s_pins[IDX_DOWN], s_pins[IDX_SELECT]);
    return ESP_OK;
}

static void buttons_deinit(void) {
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
}

static void buttons_set_callback(nav_event_cb_t cb) {
    s_cb = cb;
}

static bool buttons_is_button_held(void) {
    /* Active low (internal pull-up configured in buttons_init()). Called
     * after init(), matching the boot_logic.h step order (same contract as
     * nav_input_ec11.c's ec11_is_button_held). */
    return gpio_get_level(s_pins[IDX_SELECT]) == 0;
}

static const nav_input_driver_t s_buttons_driver = {
    .init = buttons_init,
    .deinit = buttons_deinit,
    .set_callback = buttons_set_callback,
    .is_button_held = buttons_is_button_held,
};

const nav_input_driver_t *nav_input_get_active_driver(void) {
    return &s_buttons_driver;
}

#endif /* CONFIG_LAUNCHER_NAV_DRIVER_BUTTONS */
