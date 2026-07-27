#include "net_ota.h"
#include "net_manifest.h"
#include "net_wifi.h"
#include "display.h"
#include "app_registry.h"
#include "nvs_state.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net_http_util.h"

static const char *TAG = "net_ota";

#define TITLE_Y 8
#define TITLE_SCALE 2
#define LIST_START_Y 40
#define ROW_HEIGHT 22
#define ENTRY_SCALE 2
#define MARGIN_X 8

static QueueHandle_t s_evt_queue;

static void nav_cb(nav_event_t event) {
    if (s_evt_queue != NULL) {
        xQueueSend(s_evt_queue, &event, 0);
    }
}

static void show_message(const char *line1, const char *line2) {
    display_fill_screen(DISPLAY_COLOR_BLACK);
    if (line1) {
        display_draw_text(MARGIN_X, 100, line1, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    }
    if (line2) {
        display_draw_text(MARGIN_X, 124, line2, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    }
}

static void draw_picker(const char *title, const char **labels, int count, int selected) {
    display_fill_screen(DISPLAY_COLOR_BLACK);
    display_draw_text(MARGIN_X, TITLE_Y, title, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, TITLE_SCALE);
    for (int i = 0; i < count; i++) {
        int y = LIST_START_Y + i * ROW_HEIGHT;
        bool is_selected = (i == selected);
        display_color_t bg = is_selected ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK;
        display_color_t fg = is_selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;
        display_fill_rect(0, y - 2, display_width(), ROW_HEIGHT, bg);
        char line[48]; /* matches ui_menu.c's draw_row() line buffer -- see issue #22 follow-up */
        snprintf(line, sizeof(line), "%c %s", is_selected ? '>' : ' ', labels[i]);
        display_draw_text(MARGIN_X, y, line, fg, bg, ENTRY_SCALE);
    }
}

/* Blocking picker over an arbitrary label list. Returns the selected index,
 * or -1 if the user cancelled with a long press. */
static int run_picker(const nav_input_driver_t *drv, const char *title, const char **labels, int count) {
    if (count <= 0) {
        return -1;
    }
    s_evt_queue = xQueueCreate(8, sizeof(nav_event_t));
    drv->set_callback(nav_cb);

    int selected = 0;
    draw_picker(title, labels, count, selected);

    int result = -1;
    bool done = false;
    while (!done) {
        nav_event_t evt;
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt) {
            case NAV_EVENT_UP:
                selected = (selected == 0) ? count - 1 : selected - 1;
                draw_picker(title, labels, count, selected);
                break;
            case NAV_EVENT_DOWN:
                selected = (selected + 1) % count;
                draw_picker(title, labels, count, selected);
                break;
            case NAV_EVENT_SELECT:
                result = selected;
                done = true;
                break;
            case NAV_EVENT_LONG_PRESS:
                result = -1;
                done = true;
                break;
            case NAV_EVENT_BACK:
            default:
                break;
        }
    }

    drv->set_callback(NULL);
    vQueueDelete(s_evt_queue);
    s_evt_queue = NULL;
    return result;
}

/* Blocking yes/no confirm. NAV_EVENT_SELECT = yes, NAV_EVENT_LONG_PRESS = no. */
static bool run_confirm(const nav_input_driver_t *drv, const char *line1, const char *line2) {
    show_message(line1, line2);
    display_draw_text(MARGIN_X, 150, "SELECT=YES  LONG PRESS=CANCEL", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);

    s_evt_queue = xQueueCreate(4, sizeof(nav_event_t));
    drv->set_callback(nav_cb);

    bool confirmed = false;
    bool done = false;
    while (!done) {
        nav_event_t evt;
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (evt == NAV_EVENT_SELECT) {
            confirmed = true;
            done = true;
        } else if (evt == NAV_EVENT_LONG_PRESS) {
            confirmed = false;
            done = true;
        }
    }

    drv->set_callback(NULL);
    vQueueDelete(s_evt_queue);
    s_evt_queue = NULL;
    return confirmed;
}

static void draw_progress(size_t downloaded) {
    char line[32];
    snprintf(line, sizeof(line), "%u KB...", (unsigned)(downloaded / 1024));
    display_fill_rect(MARGIN_X, 124, display_width() - 2 * MARGIN_X, 16, DISPLAY_COLOR_BLACK);
    display_draw_text(MARGIN_X, 124, line, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
}

static esp_err_t download_to_partition(const net_manifest_entry_t *entry, const esp_partition_t *dest) {
    bool is_https = strncmp(entry->url, "https://", 8) == 0;
    esp_http_client_config_t config = {
        .url = entry->url,
        .timeout_ms = 15000,
        .crt_bundle_attach = is_https ? esp_crt_bundle_attach : NULL,
        /* See the matching comment in net_manifest.c -- GitHub Releases
         * redirect targets need more than the default 512-byte buffer. */
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = net_http_open_and_follow_redirects(client, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open '%s' failed: %s", entry->url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "download '%s' returned HTTP %d", entry->url, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(dest, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin('%s') failed: %s", dest->label, esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    static char buf[4096];
    size_t total = 0;
    while (true) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            ESP_LOGE(TAG, "read error after %u bytes", (unsigned)total);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed after %u bytes: %s", (unsigned)total, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return err;
        }
        total += n;
        if (total % (16 * 1024) < (size_t)n) {
            draw_progress(total);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end('%s') failed: %s (image invalid/incomplete)", dest->label, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "downloaded %u bytes into '%s'", (unsigned)total, dest->label);
    if (entry->size != 0 && entry->size != total) {
        ESP_LOGW(TAG, "manifest declared size %u but downloaded %u bytes", (unsigned)entry->size, (unsigned)total);
    }
    return ESP_OK;
}

void net_ota_run_download_flow(const nav_input_driver_t *drv) {
    show_message("CONNECTING WIFI...", NULL);
    if (!net_wifi_connect()) {
        show_message("WIFI FAILED", "network unavailable");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    show_message("LOADING MANIFEST...", NULL);
    net_manifest_t manifest;
    if (net_manifest_fetch(&manifest) != ESP_OK || manifest.count == 0) {
        show_message("MANIFEST FAILED", "check LAUNCHER_NET_OTA_URL_BASE");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    /* Shows the manifest's own (candidate/remote) version next to each
     * entry's name, when the manifest provides one -- distinct from the
     * "currently installed" version shown below, but same " v<version>"
     * formatting for a consistent look. */
    static char app_label_bufs[NET_MANIFEST_MAX_ENTRIES][NET_MANIFEST_NAME_LEN + NET_MANIFEST_VERSION_LEN + 4];
    const char *app_labels[NET_MANIFEST_MAX_ENTRIES];
    for (size_t i = 0; i < manifest.count; i++) {
        const char *name = manifest.entries[i].name[0] != '\0' ? manifest.entries[i].name : manifest.entries[i].url;
        const char *version = manifest.entries[i].version;
        /* %.40s: name can fall back to entry->url (up to NET_MANIFEST_URL_LEN,
         * 256 bytes) -- cap explicitly so this is statically safe under
         * -Wformat-truncation and the row stays a reasonable width. */
        if (version[0] != '\0') {
            snprintf(app_label_bufs[i], sizeof(app_label_bufs[i]), "%.40s v%.15s", name, version);
        } else {
            snprintf(app_label_bufs[i], sizeof(app_label_bufs[i]), "%.40s", name);
        }
        app_labels[i] = app_label_bufs[i];
    }
    int app_idx = run_picker(drv, "CHOOSE PROGRAM", app_labels, (int)manifest.count);
    if (app_idx < 0) {
        return; /* cancelled */
    }

    /* Overwriting a slot that already has something flashed is more
     * consequential than just looking at the main menu, so this picker
     * spells out "(used)" vs "(empty)" explicitly -- see issue #22
     * comment. Uses app_registry_slot_is_flashed() (a boolean check), not
     * the image's own project_name, for the same reason as the main menu.
     * Version suffix uses the exact same app_registry_format_version_suffix()
     * as ui_menu.c's main menu, so this picker and the main menu are
     * guaranteed to show the same thing for the same slot. */
    static char slot_label_bufs[NVS_STATE_MAX_APP_SLOTS][56];
    const char *slot_labels[NVS_STATE_MAX_APP_SLOTS];
    size_t slot_count = app_registry_count();
    for (size_t i = 0; i < slot_count; i++) {
        char name_buf[NVS_STATE_SLOT_NAME_LEN];
        const char *base_name = app_registry_resolve_label(i, name_buf, sizeof(name_buf));
        char version_suffix[APP_REGISTRY_VERSION_SUFFIX_LEN];
        app_registry_format_version_suffix(i, version_suffix, sizeof(version_suffix));
        const char *status = app_registry_slot_is_flashed(i) ? " (used)" : " (empty)";
        /* %.20s: matches ui_menu.c's draw_row() cap on the same
         * app_registry_resolve_label() return, both for
         * -Wformat-truncation safety and so the two screens truncate a
         * long name the same way -- see issue #22 follow-up. */
        snprintf(slot_label_bufs[i], sizeof(slot_label_bufs[i]), "%.20s%s%s", base_name, version_suffix, status);
        slot_labels[i] = slot_label_bufs[i];
    }
    int slot_idx = run_picker(drv, "CHOOSE TARGET SLOT", slot_labels, (int)slot_count);
    if (slot_idx < 0) {
        return; /* cancelled */
    }

    const net_manifest_entry_t *entry = &manifest.entries[app_idx];
    const char *dest_label = app_registry_partition_label((size_t)slot_idx);
    char dest_name_buf[NVS_STATE_SLOT_NAME_LEN];
    const char *dest_name = app_registry_resolve_label((size_t)slot_idx, dest_name_buf, sizeof(dest_name_buf));

    char confirm_line[48];
    snprintf(confirm_line, sizeof(confirm_line), "%.20s -> %.16s", entry->name, dest_name);
    if (!run_confirm(drv, "OVERWRITE THIS SLOT?", confirm_line)) {
        return; /* cancelled */
    }

    const esp_partition_t *dest =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, dest_label);
    if (dest == NULL) {
        show_message("ERROR", "target partition not found");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    show_message("DOWNLOADING...", "0 KB...");
    esp_err_t err = download_to_partition(entry, dest);
    if (err == ESP_OK) {
        /* Record the manifest's own name for this slot now, while we still
         * have it -- lets the menu show a meaningful label afterward
         * without ever having to trust anything read out of the flashed
         * image itself (see app_registry_resolve_label(), issue #22
         * comment). Best-effort: a failure here just means the menu falls
         * back to the static app_registry.h label, not worth failing the
         * whole download over. */
        const char *chosen_name = entry->name[0] != '\0' ? entry->name : entry->url;
        esp_err_t name_err = nvs_state_set_slot_name((size_t)slot_idx, chosen_name);
        if (name_err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_state_set_slot_name(%d) failed: %s", slot_idx, esp_err_to_name(name_err));
        }
        show_message("DONE", "returning to menu...");
    } else {
        show_message("DOWNLOAD FAILED", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
}
