/*
 * EC11 rotary encoder driver (rotation + integrated push button).
 *
 * Rotation is decoded in the GPIO ISR using the classic 7-state quadrature
 * table (Ben Buxton's "rotary" algorithm) so that a single detent reliably
 * produces exactly one CW/CCW event and contact bounce is rejected instead
 * of producing spurious extra events. The button uses a one-shot esp_timer
 * armed on press to distinguish a short click (NAV_EVENT_SELECT) from a
 * long press (NAV_EVENT_LONG_PRESS) without blocking the ISR.
 */

#include "nav_input.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "nav_ec11";

#define EC11_GPIO_CLK CONFIG_LAUNCHER_EC11_GPIO_CLK
#define EC11_GPIO_DT  CONFIG_LAUNCHER_EC11_GPIO_DT
#define EC11_GPIO_SW  CONFIG_LAUNCHER_EC11_GPIO_SW

/* ESP-IDF/Kconfig only emits a #define for a bool option when it is "y" --
 * when "n" (the default here), CONFIG_LAUNCHER_EC11_INVERT is absent from
 * sdkconfig.h entirely rather than defined as 0. That's fine inside #if
 * (undefined macros evaluate to 0 there), but breaks plain C expressions
 * like `CONFIG_LAUNCHER_EC11_INVERT ? a : b` below, so provide the 0
 * fallback explicitly. */
#ifndef CONFIG_LAUNCHER_EC11_INVERT
#define CONFIG_LAUNCHER_EC11_INVERT 0
#endif

/* Ben Buxton full-step quadrature state table: state = ttable[state & 0xf][pinstate]. */
#define DIR_NONE 0x0
#define DIR_CW   0x10
#define DIR_CCW  0x20

#define R_START      0x0
#define R_CW_FINAL   0x1
#define R_CW_BEGIN   0x2
#define R_CW_NEXT    0x3
#define R_CCW_BEGIN  0x4
#define R_CCW_FINAL  0x5
#define R_CCW_NEXT   0x6

static const uint8_t ttable[7][4] = {
    {R_START,    R_CW_BEGIN,  R_CCW_BEGIN, R_START},
    {R_CW_NEXT,  R_START,     R_CW_FINAL,  R_START | DIR_CW},
    {R_CW_NEXT,  R_CW_BEGIN,  R_START,     R_START},
    {R_CW_NEXT,  R_CW_BEGIN,  R_CW_FINAL,  R_START},
    {R_CCW_NEXT, R_START,     R_CCW_BEGIN, R_START},
    {R_CCW_NEXT, R_CCW_FINAL, R_START,     R_START | DIR_CCW},
    {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},
};

typedef enum {
    EC11_EVT_ROTATE_CW,
    EC11_EVT_ROTATE_CCW,
    EC11_EVT_BUTTON_SHORT,
    EC11_EVT_BUTTON_LONG,
} ec11_internal_event_t;

static QueueHandle_t s_evt_queue;
static TaskHandle_t s_task_handle;
static esp_timer_handle_t s_long_press_timer;
static nav_event_cb_t s_cb;
static volatile uint8_t s_quad_state = R_START;
static volatile bool s_long_press_fired;

static void IRAM_ATTR rotate_isr_handler(void *arg) {
    uint8_t pinstate = (gpio_get_level(EC11_GPIO_DT) << 1) | gpio_get_level(EC11_GPIO_CLK);
    s_quad_state = ttable[s_quad_state & 0xf][pinstate];
    uint8_t dir = s_quad_state & 0x30;
    if (dir == DIR_NONE || s_evt_queue == NULL) {
        return;
    }
    ec11_internal_event_t evt = (dir == DIR_CW) ? EC11_EVT_ROTATE_CW : EC11_EVT_ROTATE_CCW;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_evt_queue, &evt, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void long_press_timer_cb(void *arg) {
    s_long_press_fired = true;
    ec11_internal_event_t evt = EC11_EVT_BUTTON_LONG;
    xQueueSend(s_evt_queue, &evt, 0);
}

static void IRAM_ATTR button_isr_handler(void *arg) {
    bool pressed = gpio_get_level(EC11_GPIO_SW) == 0; /* active low, external/internal pull-up */
    BaseType_t woken = pdFALSE;

    if (pressed) {
        s_long_press_fired = false;
        esp_timer_start_once(s_long_press_timer, (int64_t)CONFIG_LAUNCHER_LONG_PRESS_MS * 1000);
    } else {
        esp_timer_stop(s_long_press_timer);
        if (!s_long_press_fired) {
            ec11_internal_event_t evt = EC11_EVT_BUTTON_SHORT;
            xQueueSendFromISR(s_evt_queue, &evt, &woken);
        }
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void nav_ec11_task(void *arg) {
    ec11_internal_event_t evt;
    while (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY)) {
        if (s_cb == NULL) {
            continue;
        }
        switch (evt) {
            case EC11_EVT_ROTATE_CW:
                s_cb(CONFIG_LAUNCHER_EC11_INVERT ? NAV_EVENT_UP : NAV_EVENT_DOWN);
                break;
            case EC11_EVT_ROTATE_CCW:
                s_cb(CONFIG_LAUNCHER_EC11_INVERT ? NAV_EVENT_DOWN : NAV_EVENT_UP);
                break;
            case EC11_EVT_BUTTON_SHORT:
                s_cb(NAV_EVENT_SELECT);
                break;
            case EC11_EVT_BUTTON_LONG:
                s_cb(NAV_EVENT_LONG_PRESS);
                break;
        }
    }
    vTaskDelete(NULL);
}

static void configure_input_pin(gpio_num_t pin) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static esp_err_t ec11_init(void) {
    configure_input_pin(EC11_GPIO_CLK);
    configure_input_pin(EC11_GPIO_DT);
    configure_input_pin(EC11_GPIO_SW);

    s_evt_queue = xQueueCreate(16, sizeof(ec11_internal_event_t));
    if (s_evt_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &long_press_timer_cb,
        .name = "ec11_long_press",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_long_press_timer);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE = ISR service already installed elsewhere, fine to share. */
        return err;
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(EC11_GPIO_CLK, rotate_isr_handler, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(EC11_GPIO_DT, rotate_isr_handler, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(EC11_GPIO_SW, button_isr_handler, NULL));

    if (xTaskCreate(nav_ec11_task, "nav_ec11", 3072, NULL, tskIDLE_PRIORITY + 3, &s_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "EC11 driver ready (CLK=%d DT=%d SW=%d, invert=%d)",
             EC11_GPIO_CLK, EC11_GPIO_DT, EC11_GPIO_SW, CONFIG_LAUNCHER_EC11_INVERT);
    return ESP_OK;
}

static void ec11_deinit(void) {
    gpio_isr_handler_remove(EC11_GPIO_CLK);
    gpio_isr_handler_remove(EC11_GPIO_DT);
    gpio_isr_handler_remove(EC11_GPIO_SW);
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
    if (s_long_press_timer) {
        esp_timer_delete(s_long_press_timer);
        s_long_press_timer = NULL;
    }
    if (s_evt_queue) {
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
    }
}

static void ec11_set_callback(nav_event_cb_t cb) {
    s_cb = cb;
}

static bool ec11_is_button_held(void) {
    /* Active low (internal pull-up configured in ec11_init()). Must be
     * called after init(), matching the boot_logic.h step order. */
    return gpio_get_level(EC11_GPIO_SW) == 0;
}

static const nav_input_driver_t s_ec11_driver = {
    .init = ec11_init,
    .deinit = ec11_deinit,
    .set_callback = ec11_set_callback,
    .is_button_held = ec11_is_button_held,
};

#if CONFIG_LAUNCHER_NAV_DRIVER_EC11
const nav_input_driver_t *nav_input_get_active_driver(void) {
    return &s_ec11_driver;
}
#endif
