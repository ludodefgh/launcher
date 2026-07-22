#include "ui_menu.h"
#include "app_registry.h"
#include "display.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define TITLE_Y     8
#define TITLE_SCALE 2
#define LIST_START_Y 40
#define ROW_HEIGHT  22
#define ENTRY_SCALE 2
#define MARGIN_X    8

static QueueHandle_t s_evt_queue;

static void menu_nav_cb(nav_event_t event) {
    if (s_evt_queue != NULL) {
        xQueueSend(s_evt_queue, &event, 0);
    }
}

static void draw_menu(int selected) {
    display_fill_screen(DISPLAY_COLOR_BLACK);
    display_draw_text(MARGIN_X, TITLE_Y, "SELECTION PROGRAMME", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, TITLE_SCALE);

    for (size_t i = 0; i < kAppsCount; i++) {
        int y = LIST_START_Y + (int)i * ROW_HEIGHT;
        bool is_selected = ((int)i == selected);
        display_color_t bg = is_selected ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK;
        display_color_t fg = is_selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;

        display_fill_rect(0, y - 2, display_width(), ROW_HEIGHT, bg);

        char line[40];
        snprintf(line, sizeof(line), "%c %s", is_selected ? '>' : ' ', kApps[i].display_name);
        display_draw_text(MARGIN_X, y, line, fg, bg, ENTRY_SCALE);
    }
}

int ui_menu_run(const nav_input_driver_t *drv) {
    s_evt_queue = xQueueCreate(8, sizeof(nav_event_t));
    drv->set_callback(menu_nav_cb);

    int selected = 0;
    draw_menu(selected);

    while (1) {
        nav_event_t evt;
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt) {
            case NAV_EVENT_UP:
                selected = (selected == 0) ? (int)kAppsCount - 1 : selected - 1;
                draw_menu(selected);
                break;
            case NAV_EVENT_DOWN:
                selected = (selected + 1) % (int)kAppsCount;
                draw_menu(selected);
                break;
            case NAV_EVENT_SELECT:
                drv->set_callback(NULL);
                vQueueDelete(s_evt_queue);
                s_evt_queue = NULL;
                return selected;
            case NAV_EVENT_BACK:
            case NAV_EVENT_LONG_PRESS:
            default:
                break; /* unused in v1, no sub-menus */
        }
    }
}
