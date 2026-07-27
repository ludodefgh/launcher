#include "ui_menu.h"
#include "app_registry.h"
#include "display.h"
#include "sdkconfig.h"

#if CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE
#include "net_version_check.h"
#endif
#if CONFIG_LAUNCHER_NET_REMOTE_CONTROL_ENABLE && CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_HTTP
#include "net_wifi.h"
#endif

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define TITLE_Y     8
#define TITLE_SCALE 2
#define LIST_START_Y 40
#define ROW_HEIGHT  22
#define ENTRY_SCALE 2
#define MARGIN_X    8

#if CONFIG_LAUNCHER_NET_OTA_ENABLE
#define EXTRA_ROW_COUNT 1
#else
#define EXTRA_ROW_COUNT 0
#endif

static QueueHandle_t s_evt_queue;

static void menu_nav_cb(nav_event_t event) {
    if (s_evt_queue != NULL) {
        xQueueSend(s_evt_queue, &event, 0);
    }
}

static int total_row_count(void) {
    return (int)kAppsCount + EXTRA_ROW_COUNT;
}

static void draw_menu(int selected) {
    display_fill_screen(DISPLAY_COLOR_BLACK);
    display_draw_text(MARGIN_X, TITLE_Y, "SELECTION PROGRAMME", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, TITLE_SCALE);

    int rows = total_row_count();
    for (int i = 0; i < rows; i++) {
        int y = LIST_START_Y + i * ROW_HEIGHT;
        bool is_selected = (i == selected);
        display_color_t bg = is_selected ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK;
        display_color_t fg = is_selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;

        display_fill_rect(0, y - 2, display_width(), ROW_HEIGHT, bg);

        char line[40];
        if (i < (int)kAppsCount) {
            const char *update_suffix = "";
#if CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE
            if (net_version_check_has_update(kApps[i].partition_label)) {
                update_suffix = " (MAJ)";
            }
#endif
            snprintf(line, sizeof(line), "%c %s%s", is_selected ? '>' : ' ', kApps[i].display_name, update_suffix);
        } else {
            snprintf(line, sizeof(line), "%c Telecharger un programme", is_selected ? '>' : ' ');
        }
        display_draw_text(MARGIN_X, y, line, fg, bg, ENTRY_SCALE);
    }

#if CONFIG_LAUNCHER_NET_REMOTE_CONTROL_ENABLE && CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_HTTP
    char ip[16];
    if (net_wifi_get_ip_string(ip, sizeof(ip))) {
        char footer[24];
        snprintf(footer, sizeof(footer), "IP: %s", ip);
        display_draw_text(MARGIN_X, display_height() - 16, footer, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    }
#endif
}

int ui_menu_run(const nav_input_driver_t *drv) {
    s_evt_queue = xQueueCreate(8, sizeof(nav_event_t));
    drv->set_callback(menu_nav_cb);

    int rows = total_row_count();
    int selected = 0;
    draw_menu(selected);

    while (1) {
        nav_event_t evt;
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt) {
            case NAV_EVENT_UP:
                selected = (selected == 0) ? rows - 1 : selected - 1;
                draw_menu(selected);
                break;
            case NAV_EVENT_DOWN:
                selected = (selected + 1) % rows;
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
