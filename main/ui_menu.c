#include "ui_menu.h"
#include "app_registry.h"
#include "display.h"
#include "nvs_state.h"
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
    return (int)app_registry_count() + EXTRA_ROW_COUNT;
}

/* Redraws a single row in either its selected or unselected appearance.
 * Splitting this out from draw_menu_full() is what lets a nav event only
 * touch the (at most) two rows that actually changed instead of clearing
 * and repainting the whole screen -- see issue #19: doing a full
 * display_fill_screen() + redrawing every row on every single detent was
 * the main cause of a multi-second, visibly "typewriter" redraw. */
static void draw_row(int i, bool is_selected) {
    int y = LIST_START_Y + i * ROW_HEIGHT;
    display_color_t bg = is_selected ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK;
    display_color_t fg = is_selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;

    display_fill_rect(0, y - 2, display_width(), ROW_HEIGHT, bg);

    char line[64];
    if (i < (int)app_registry_count()) {
        const char *update_suffix = "";
#if CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE
        if (net_version_check_has_update(app_registry_partition_label(i))) {
            update_suffix = " (UPD)";
        }
#endif
        char name_buf[NVS_STATE_SLOT_NAME_LEN];
        const char *display_name = app_registry_resolve_label(i, name_buf, sizeof(name_buf));
        char version_suffix[APP_REGISTRY_VERSION_SUFFIX_LEN];
        app_registry_format_version_suffix(i, version_suffix, sizeof(version_suffix));
        const char *empty_suffix = app_registry_slot_is_flashed(i) ? "" : " (empty)";
        /* %.20s: display_name's real bound (NVS_STATE_SLOT_NAME_LEN) isn't
         * visible to the compiler through the function-return pointer --
         * cap explicitly so this is statically safe under
         * -Wformat-truncation, and so the row stays a reasonable width on
         * screen. */
        snprintf(line, sizeof(line), "%c %.20s%s%s%s", is_selected ? '>' : ' ', display_name, version_suffix,
                 empty_suffix, update_suffix);
    } else {
        snprintf(line, sizeof(line), "%c Download a program", is_selected ? '>' : ' ');
    }
    display_draw_text(MARGIN_X, y, line, fg, bg, ENTRY_SCALE);
}

#define ACTION_LAUNCH 0
#define ACTION_DELETE 1

/* Small fixed 2-option action menu shown when a real app slot (not the
 * "Download a program" row) is selected -- see issue "click a slot ->
 * launch/delete popup instead of launching directly". Deliberately not
 * sharing net_ota.c's run_picker()/run_confirm() (a generic label-list
 * picker over its own private queue, used from a different call path) --
 * this reuses ui_menu_run()'s already-active s_evt_queue/callback directly
 * instead of tearing them down and recreating them for a fixed 2-item
 * menu. */
static void draw_action_menu(int slot_i, int action) {
    display_fill_screen(DISPLAY_COLOR_BLACK);
    char name_buf[NVS_STATE_SLOT_NAME_LEN];
    const char *display_name = app_registry_resolve_label((size_t)slot_i, name_buf, sizeof(name_buf));
    char title[32];
    snprintf(title, sizeof(title), "%.20s", display_name);
    display_draw_text(MARGIN_X, TITLE_Y, title, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, TITLE_SCALE);

    static const char *const labels[2] = {"Launch", "Delete"};
    for (int i = 0; i < 2; i++) {
        int y = LIST_START_Y + i * ROW_HEIGHT;
        bool is_selected = (i == action);
        display_color_t bg = is_selected ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK;
        display_color_t fg = is_selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;
        display_fill_rect(0, y - 2, display_width(), ROW_HEIGHT, bg);
        char line[16];
        snprintf(line, sizeof(line), "%c %s", is_selected ? '>' : ' ', labels[i]);
        display_draw_text(MARGIN_X, y, line, fg, bg, ENTRY_SCALE);
    }
}

/* Blocking yes/no confirm before an actually-destructive erase -- same
 * SELECT=yes/LONG_PRESS=no convention as net_ota.c's run_confirm(). */
static bool confirm_delete(int slot_i) {
    display_fill_screen(DISPLAY_COLOR_BLACK);
    display_draw_text(MARGIN_X, 90, "DELETE THIS SLOT?", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    char name_buf[NVS_STATE_SLOT_NAME_LEN];
    const char *display_name = app_registry_resolve_label((size_t)slot_i, name_buf, sizeof(name_buf));
    char line[40];
    snprintf(line, sizeof(line), "%.20s", display_name);
    display_draw_text(MARGIN_X, 120, line, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    display_draw_text(MARGIN_X, 150, "SELECT=YES  LONG PRESS=CANCEL", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);

    while (1) {
        nav_event_t evt;
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (evt == NAV_EVENT_SELECT) {
            return true;
        }
        if (evt == NAV_EVENT_LONG_PRESS) {
            return false;
        }
    }
}

/* Returns true if the user chose to launch slot_i (caller proceeds to boot
 * it as before); false if the user deleted it or cancelled -- either way,
 * nothing should be booted, caller just redraws the main menu and keeps
 * going. */
static bool run_slot_action_menu(int slot_i) {
    int action = ACTION_LAUNCH;
    draw_action_menu(slot_i, action);

    while (1) {
        nav_event_t evt;
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt) {
            case NAV_EVENT_UP:
            case NAV_EVENT_DOWN:
                action = (action == ACTION_LAUNCH) ? ACTION_DELETE : ACTION_LAUNCH;
                draw_action_menu(slot_i, action);
                break;
            case NAV_EVENT_SELECT:
                if (action == ACTION_LAUNCH) {
                    return true;
                }
                if (confirm_delete(slot_i)) {
                    display_fill_screen(DISPLAY_COLOR_BLACK);
                    display_draw_text(MARGIN_X, 100, "ERASING...", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
                    app_registry_erase_slot((size_t)slot_i);
                }
                return false;
            case NAV_EVENT_BACK:
            case NAV_EVENT_LONG_PRESS:
                return false; /* cancelled -- back to the main menu */
            default:
                break;
        }
    }
}

static void draw_footer(void) {
#if CONFIG_LAUNCHER_NET_REMOTE_CONTROL_ENABLE && CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_HTTP
    char ip[16];
    if (net_wifi_get_ip_string(ip, sizeof(ip))) {
        char footer[24];
        snprintf(footer, sizeof(footer), "IP: %s", ip);
        display_draw_text(MARGIN_X, display_height() - 16, footer, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    }
#endif
}

/* Full paint: only needed once, when the menu is (re-)entered -- the
 * screen may have unrelated content on it (an error message, the OTA
 * flow's screens, ...) so this can't be skipped, unlike per-detent updates. */
static void draw_menu_full(int selected) {
    display_fill_screen(DISPLAY_COLOR_BLACK);
    display_draw_text(MARGIN_X, TITLE_Y, "SELECT PROGRAM", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, TITLE_SCALE);

    int rows = total_row_count();
    for (int i = 0; i < rows; i++) {
        draw_row(i, i == selected);
    }
    draw_footer();
}

int ui_menu_run(const nav_input_driver_t *drv) {
    s_evt_queue = xQueueCreate(8, sizeof(nav_event_t));
    drv->set_callback(menu_nav_cb);

    int rows = total_row_count();
    int selected = 0;
    draw_menu_full(selected);

    while (1) {
        nav_event_t evt;
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int prev_selected = selected;
        switch (evt) {
            case NAV_EVENT_UP:
                selected = (selected == 0) ? rows - 1 : selected - 1;
                draw_row(prev_selected, false);
                draw_row(selected, true);
                break;
            case NAV_EVENT_DOWN:
                selected = (selected + 1) % rows;
                draw_row(prev_selected, false);
                draw_row(selected, true);
                break;
            case NAV_EVENT_SELECT:
                if (selected < (int)app_registry_count() && !run_slot_action_menu(selected)) {
                    /* Deleted or cancelled -- nothing to boot, redraw the
                     * main menu (slot state may have changed) and keep
                     * going instead of returning. */
                    draw_menu_full(selected);
                    break;
                }
                drv->set_callback(NULL);
                vQueueDelete(s_evt_queue);
                s_evt_queue = NULL;
                return selected;
            case NAV_EVENT_BACK:
            case NAV_EVENT_LONG_PRESS:
            default:
                break;
        }
    }
}
