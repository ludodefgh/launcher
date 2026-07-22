#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "nvs_state.h"
#include "nav_input.h"
#include "boot_logic.h"
#include "boot_into.h"
#include "display.h"
#include "ui_menu.h"
#include "app_registry.h"

static const char *TAG = "launcher";

/* Must match LAUNCHER_CLIENT_PROTOCOL_VERSION in components/launcher_client/launcher_client.h.
 * Duplicated here (not a shared header) because guest apps only pull in the
 * launcher_client component, not the launcher's own main/ sources. */
#define LAUNCHER_SUPPORTED_CLIENT_PROTOCOL_VERSION 1

static bool s_display_ready;

static void check_client_protocol_version(void) {
    uint32_t version = 0;
    if (nvs_state_get_protocol_version(&version) != ESP_OK || version == 0) {
        return; /* never written yet (no guest app has requested the menu) -- not an error */
    }
    if (version != LAUNCHER_SUPPORTED_CLIENT_PROTOCOL_VERSION) {
        ESP_LOGW(TAG, "guest app used launcher_client protocol v%" PRIu32 ", launcher supports v%d -- "
                      "force_menu is still honored, but other behavior may have changed",
                 version, LAUNCHER_SUPPORTED_CLIENT_PROTOCOL_VERSION);
    }
}

static esp_err_t ensure_display(void) {
    if (s_display_ready) {
        return ESP_OK;
    }
    esp_err_t err = display_init();
    if (err == ESP_OK) {
        s_display_ready = true;
    }
    return err;
}

static void show_error_and_wait(const char *line1, const char *line2) {
    if (ensure_display() != ESP_OK) {
        return; /* nothing more we can do without a screen */
    }
    display_fill_screen(DISPLAY_COLOR_BLACK);
    display_draw_text(8, 90, "ERREUR", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    if (line1) {
        display_draw_text(8, 120, line1, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    }
    if (line2) {
        display_draw_text(8, 132, line2, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    }
    display_draw_text(8, 150, "RETOUR AU MENU...", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_state_init());

    const nav_input_driver_t *drv = nav_input_get_active_driver();
    ESP_ERROR_CHECK(drv->init());

    boot_check_slot_count_consistency();

    boot_decision_input_t decision_input = {0};
    bool found_last_app = false;
    ESP_ERROR_CHECK(nvs_state_get_last_app(decision_input.last_app_partition,
                                            sizeof(decision_input.last_app_partition), &found_last_app));
    decision_input.has_last_app = found_last_app;
    ESP_ERROR_CHECK(nvs_state_consume_force_menu(&decision_input.force_menu));
    decision_input.button_held = drv->is_button_held();
    check_client_protocol_version();

    boot_action_t action = boot_logic_decide(&decision_input);

    if (action == BOOT_ACTION_BOOT_DIRECT) {
        ESP_LOGI(TAG, "direct boot into '%s'", decision_input.last_app_partition);
#if CONFIG_LAUNCHER_BOOT_SPLASH_MS > 0
        if (ensure_display() == ESP_OK) {
            display_fill_screen(DISPLAY_COLOR_BLACK);
            display_draw_text(8, 100, "LAUNCHER", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 3);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_LAUNCHER_BOOT_SPLASH_MS));
        }
#endif
        esp_err_t err = boot_into(decision_input.last_app_partition);
        /* Only reached if boot_into() failed -- esp_restart() never returns on success. */
        ESP_LOGW(TAG, "direct boot failed (%s), falling back to menu", esp_err_to_name(err));
        show_error_and_wait("Programme introuvable ou", "partition non flashee.");
        action = BOOT_ACTION_SHOW_MENU;
    }

    while (action == BOOT_ACTION_SHOW_MENU) {
        ESP_ERROR_CHECK(ensure_display());
        int selected = ui_menu_run(drv);
        const char *label = kApps[selected].partition_label;

        ESP_ERROR_CHECK(nvs_state_set_last_app(label));

        esp_err_t err = boot_into(label);
        /* Only reached on failure -- stay in the menu instead of crash-looping. */
        ESP_LOGW(TAG, "boot into '%s' failed (%s)", label, esp_err_to_name(err));
        show_error_and_wait(kApps[selected].display_name, "semble vide/non flashe.");
    }
}
