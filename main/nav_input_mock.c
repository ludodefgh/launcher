/*
 * Console-driven mock nav driver: lets the menu logic be exercised over the
 * serial console without an EC11 wired up. Keys: w/k = up, s/j = down,
 * enter/space = select, b = back, l = long press.
 */

#include "nav_input.h"

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "nav_mock";

static TaskHandle_t s_task_handle;
static nav_event_cb_t s_cb;

static void nav_mock_task(void *arg) {
    ESP_LOGI(TAG, "mock nav driver active: w/k=up s/j=down enter/space=select b=back l=long-press");
    while (1) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (s_cb == NULL) {
            continue;
        }
        switch (c) {
            case 'w':
            case 'k':
                s_cb(NAV_EVENT_UP);
                break;
            case 's':
            case 'j':
                s_cb(NAV_EVENT_DOWN);
                break;
            case '\r':
            case '\n':
            case ' ':
                s_cb(NAV_EVENT_SELECT);
                break;
            case 'b':
                s_cb(NAV_EVENT_BACK);
                break;
            case 'l':
                s_cb(NAV_EVENT_LONG_PRESS);
                break;
            default:
                break;
        }
    }
}

static esp_err_t mock_init(void) {
    if (xTaskCreate(nav_mock_task, "nav_mock", 3072, NULL, tskIDLE_PRIORITY + 3, &s_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void mock_deinit(void) {
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
}

static void mock_set_callback(nav_event_cb_t cb) {
    s_cb = cb;
}

static bool mock_is_button_held(void) {
    /* No physical button to poll; console mock always reports "not held" at
     * boot (force the menu with force_menu/no-last-app instead when testing). */
    return false;
}

static const nav_input_driver_t s_mock_driver = {
    .init = mock_init,
    .deinit = mock_deinit,
    .set_callback = mock_set_callback,
    .is_button_held = mock_is_button_held,
};

#if CONFIG_LAUNCHER_NAV_DRIVER_MOCK
const nav_input_driver_t *nav_input_get_active_driver(void) {
    return &s_mock_driver;
}
#endif
