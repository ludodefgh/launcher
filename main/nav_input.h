#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NAV_EVENT_UP,
    NAV_EVENT_DOWN,
    NAV_EVENT_SELECT,
    NAV_EVENT_BACK,
    NAV_EVENT_LONG_PRESS,
} nav_event_t;

typedef void (*nav_event_cb_t)(nav_event_t event);

typedef struct {
    esp_err_t (*init)(void);
    void (*deinit)(void);
    void (*set_callback)(nav_event_cb_t cb);
    /*
     * Deliberate extension beyond the original HAL sketch: the boot
     * sequence (see boot_logic.h) needs an instantaneous "is the select
     * button held right now" level read at startup, before the menu (and
     * its event callback) is even entered -- this is a level query, not an
     * edge/click/long-press event, so it does not fit nav_event_cb_t.
     * Drivers with no physical button (mock, a future BLE remote) can just
     * always return false. Documented in CLAUDE.md as a design decision.
     */
    bool (*is_button_held)(void);
} nav_input_driver_t;

/**
 * Returns the driver selected at compile time via Kconfig
 * (LAUNCHER_NAV_DRIVER_EC11 / LAUNCHER_NAV_DRIVER_MOCK). The launcher core
 * (boot_logic.c, ui_menu.c) only ever talks to nav_input_driver_t and never
 * touches a GPIO, encoder protocol, or touch event directly.
 */
const nav_input_driver_t *nav_input_get_active_driver(void);

#ifdef __cplusplus
}
#endif
