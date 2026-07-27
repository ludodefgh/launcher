#include <stdbool.h>
#include <string.h>
#include <stdio.h>
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

#if CONFIG_LAUNCHER_NET_WIFI_ENABLE
#include "net_wifi.h"
#endif
#if CONFIG_LAUNCHER_NET_OTA_ENABLE
#include "net_ota.h"
#endif
#if CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE
#include "net_version_check.h"
#endif
#if CONFIG_LAUNCHER_NET_REMOTE_CONTROL_ENABLE
#if CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_HTTP
#include "net_remote_http.h"
#elif CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_BLE
#include "net_remote_ble.h"
#endif
#endif

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
    display_draw_text(8, 90, "ERROR", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    if (line1) {
        display_draw_text(8, 120, line1, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    }
    if (line2) {
        display_draw_text(8, 132, line2, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    }
    display_draw_text(8, 150, "RETURNING TO MENU...", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

#if CONFIG_LAUNCHER_NET_WIFI_ENABLE || CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_BLE
#include "freertos/semphr.h"

/*
 * WiFi/BLE init plus whatever TLS/HTTP/cJSON call frames stack on top of it
 * outgrow the default "main" task stack (3584 bytes) -- a real stack
 * overflow was observed running these inline on app_main()'s own stack,
 * see issue #14. Run them on a dedicated, generously-sized worker task
 * instead and just block app_main() until done, so the call sites below
 * don't need to change their synchronous, blocking calling convention.
 */
typedef struct {
    void (*fn)(void *arg);
    void *arg;
    SemaphoreHandle_t done;
} network_task_ctx_t;

static void network_task_trampoline(void *param) {
    network_task_ctx_t *ctx = (network_task_ctx_t *)param;
    ctx->fn(ctx->arg);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static void run_on_network_task(void (*fn)(void *arg), void *arg) {
    network_task_ctx_t ctx = {.fn = fn, .arg = arg, .done = xSemaphoreCreateBinary()};
    xTaskCreate(network_task_trampoline, "launcher_net", CONFIG_LAUNCHER_NET_TASK_STACK_SIZE, &ctx,
                tskIDLE_PRIORITY + 5, NULL);
    xSemaphoreTake(ctx.done, portMAX_DELAY);
    vSemaphoreDelete(ctx.done);
}
#endif

#if CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE
static void run_version_check(void *arg) {
    (void)arg;
    net_version_check_run();
}
#endif

#if CONFIG_LAUNCHER_NET_REMOTE_CONTROL_ENABLE
#if CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_HTTP
static void run_wifi_and_http_remote(void *arg) {
    (void)arg;
    if (!net_wifi_connect()) {
        return;
    }
    net_remote_http_start();

    /* IP is logged by net_wifi.c's event handler (issue #15) and shown
     * persistently in the menu footer (ui_menu.c, issue #17) -- no need
     * for a transient splash here that would just delay the menu. */
    char ip[16];
    if (net_wifi_get_ip_string(ip, sizeof(ip))) {
        ESP_LOGI(TAG, "remote control HTTP server reachable at http://%s/", ip);
    }
}
#elif CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_BLE
static void run_ble_remote(void *arg) {
    (void)arg;
    net_remote_ble_start();
}
#endif
#endif

#if CONFIG_LAUNCHER_NET_OTA_ENABLE
static void run_ota_flow(void *arg) {
    net_ota_run_download_flow((const nav_input_driver_t *)arg);
}
#endif

void app_main(void) {
    ESP_ERROR_CHECK(nvs_state_init());

    const nav_input_driver_t *drv = nav_input_get_active_driver();
    ESP_ERROR_CHECK(drv->init());

    app_registry_init();

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
        /* fresh_selection=false: an automatic direct-boot must leave
         * otadata undisturbed for rollback crash-loop detection to work --
         * see boot_into.h, issue #27. */
        esp_err_t err = boot_into(decision_input.last_app_partition, false);
        /* Only reached if boot_into() failed -- esp_restart() never returns on success. */
        ESP_LOGW(TAG, "direct boot failed (%s), falling back to menu", esp_err_to_name(err));
        show_error_and_wait("Program not found or", "partition not flashed.");
        action = BOOT_ACTION_SHOW_MENU;
    }

    while (action == BOOT_ACTION_SHOW_MENU) {
        ESP_ERROR_CHECK(ensure_display());

#if CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE
        display_fill_screen(DISPLAY_COLOR_BLACK);
        display_draw_text(8, 100, "CHECKING FOR UPDATES...", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
        run_on_network_task(run_version_check, NULL); /* no-op / silently skipped if WiFi unavailable */
#endif

#if CONFIG_LAUNCHER_NET_REMOTE_CONTROL_ENABLE
#if CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_HTTP
        run_on_network_task(run_wifi_and_http_remote, NULL);
#elif CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_BLE
        run_on_network_task(run_ble_remote, NULL); /* independent of WiFi */
#endif
#endif

        int selected = ui_menu_run(drv);

#if CONFIG_LAUNCHER_NET_OTA_ENABLE
        if (selected == (int)app_registry_count()) {
            run_on_network_task(run_ota_flow, (void *)drv);
            continue; /* redraw the menu, possibly with a freshly written slot */
        }
#endif

        const char *label = app_registry_partition_label((size_t)selected);

        ESP_ERROR_CHECK(nvs_state_set_last_app(label));

        /* fresh_selection=true: this is a deliberate new pick from the
         * menu -- see boot_into.h, issue #27. */
        esp_err_t err = boot_into(label, true);
        /* Only reached on failure -- stay in the menu instead of crash-looping. */
        ESP_LOGW(TAG, "boot into '%s' failed (%s)", label, esp_err_to_name(err));
        char name_buf[NVS_STATE_SLOT_NAME_LEN];
        const char *display_name = app_registry_resolve_label((size_t)selected, name_buf, sizeof(name_buf));
        show_error_and_wait(display_name, "appears empty/not flashed.");
    }
}
