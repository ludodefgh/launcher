#include "net_remote_http.h"
#include "app_registry.h"
#include "boot_into.h"
#include "nvs_state.h"
#include "net_wifi.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "net_remote_http";
static httpd_handle_t s_server;

static bool pin_ok(httpd_req_t *req) {
    if (CONFIG_LAUNCHER_NET_REMOTE_PIN[0] == '\0') {
        return true; /* no PIN configured -- deterrent disabled, see Kconfig help */
    }
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char pin[64];
    if (httpd_query_key_value(query, "pin", pin, sizeof(pin)) != ESP_OK) {
        return false;
    }
    return strcmp(pin, CONFIG_LAUNCHER_NET_REMOTE_PIN) == 0;
}

static esp_err_t index_handler(httpd_req_t *req) {
    static char body[2048];
    int len = 0;
    len += snprintf(body + len, sizeof(body) - len, "<html><body><h1>launcher</h1><ul>");
    for (size_t i = 0; i < app_registry_count() && len < (int)sizeof(body) - 300; i++) {
        char name_buf[NVS_STATE_SLOT_NAME_LEN];
        const char *display_name = app_registry_resolve_label(i, name_buf, sizeof(name_buf));
        len += snprintf(
            body + len, sizeof(body) - len,
            "<li><form action=\"/boot\" method=\"get\">"
            "<input type=\"hidden\" name=\"slot\" value=\"%s\">%s "
            "%s<button type=\"submit\">Boot</button></form></li>",
            app_registry_partition_label(i), display_name,
            CONFIG_LAUNCHER_NET_REMOTE_PIN[0] != '\0'
                ? "<input type=\"password\" name=\"pin\" placeholder=\"PIN\">"
                : "");
    }
    len += snprintf(body + len, sizeof(body) - len, "</ul></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, body, len);
    return ESP_OK;
}

static esp_err_t boot_handler(httpd_req_t *req) {
    if (!pin_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid or missing PIN");
        return ESP_OK;
    }

    char query[128];
    char slot[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "slot", slot, sizeof(slot)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'slot' parameter");
        return ESP_OK;
    }

    bool known_slot = false;
    for (size_t i = 0; i < app_registry_count(); i++) {
        if (strcmp(app_registry_partition_label(i), slot) == 0) {
            known_slot = true;
            break;
        }
    }
    if (!known_slot) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown slot");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "remote boot request for '%s'", slot);
    esp_err_t err = boot_into(slot);
    /* Only reached if boot_into() failed -- esp_restart() never returns on success,
     * so the client will just see the connection drop, which is expected. */
    ESP_LOGW(TAG, "remote boot into '%s' failed: %s", slot, esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "boot_into failed, slot may be empty/invalid");
    return ESP_OK;
}

/* Persists WiFi credentials remotely (issue #32) -- PIN via the same
 * ?pin=... query convention as /boot, credentials via a POST body
 * ("ssid=...&pass=..."), read with the exact same httpd_query_key_value()
 * used for query strings since the format is identical.
 *
 * This can only ever *update* already-working credentials, never bootstrap
 * a device with none configured: net_remote_http_start() only runs after
 * net_wifi_connect() has already succeeded (see main.c), so this endpoint
 * isn't reachable at all on a device with no working WiFi yet. Bootstrapping
 * from zero needs the BLE characteristic instead (net_remote_ble.c), which
 * has no such dependency. Persists only -- takes effect on the next
 * reconnect/boot, no live reconnect attempted here (this HTTP connection is
 * itself running over the *current* WiFi connection; forcing a disconnect
 * to reconnect with the new credentials would drop the very response being
 * sent back). */
static esp_err_t wifi_handler(httpd_req_t *req) {
    if (!pin_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid or missing PIN");
        return ESP_OK;
    }

    char body[128] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_OK;
    }
    body[len] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'ssid'");
        return ESP_OK;
    }
    httpd_query_key_value(body, "pass", pass, sizeof(pass)); /* optional -- open networks have none */

    net_wifi_save_credentials(ssid, pass);
    ESP_LOGI(TAG, "remote WiFi credentials updated for SSID '%s' (effective next reconnect/boot)", ssid);
    httpd_resp_send(req, "OK -- takes effect on next reconnect/boot", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Writes a client-supplied binary straight into a chosen slot with no
 * manifest/repo/URL involved at all (issue #33) -- distinct from the
 * manifest-driven OTA flow (net_ota.c). The POST request itself is treated
 * as the confirmation (no separate confirm step, matching /boot above --
 * an HTTP API has no equivalent of the local menu's confirm dialog). */
static esp_err_t upload_handler(httpd_req_t *req) {
    if (!pin_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid or missing PIN");
        return ESP_OK;
    }

    char query[128];
    char slot[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "slot", slot, sizeof(slot)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'slot' parameter");
        return ESP_OK;
    }

    bool known_slot = false;
    for (size_t i = 0; i < app_registry_count(); i++) {
        if (strcmp(app_registry_partition_label(i), slot) == 0) {
            known_slot = true;
            break;
        }
    }
    if (!known_slot) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown slot");
        return ESP_OK;
    }
    const esp_partition_t *dest = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, slot);
    if (dest == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target partition not found");
        return ESP_OK;
    }

    size_t content_len = req->content_len;
    if (content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body / Content-Length");
        return ESP_OK;
    }
    if (content_len > dest->size) {
        ESP_LOGW(TAG, "upload for '%s' is %u bytes, exceeds %u-byte partition", slot, (unsigned)content_len,
                 (unsigned)dest->size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "binary too large for this slot");
        return ESP_OK;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(dest, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin('%s') failed: %s", slot, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_OK;
    }

    static char buf[4096];
    size_t remaining = content_len;
    size_t total = 0;
    while (remaining > 0) {
        int to_read = remaining < sizeof(buf) ? (int)remaining : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, to_read);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue; /* standard esp_http_server upload retry idiom */
        }
        if (n <= 0) {
            ESP_LOGE(TAG, "upload read error after %u bytes", (unsigned)total);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read error");
            return ESP_OK;
        }
        err = esp_ota_write(ota_handle, buf, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed after %u bytes: %s", (unsigned)total, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
            return ESP_OK;
        }
        total += (size_t)n;
        remaining -= (size_t)n;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end('%s') failed: %s (image invalid/incomplete)", slot, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_end failed -- image invalid/incomplete");
        return ESP_OK;
    }

    /* Unlike the manifest-driven flow, there's no name/version to record --
     * a raw upload has no manifest entry it came from. The slot shows up as
     * flashed but keeps its static partition-label fallback name until a
     * manifest-driven download or a rename feature (not implemented) gives
     * it a friendlier one. */
    ESP_LOGI(TAG, "remote upload wrote %u bytes into '%s'", (unsigned)total, slot);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void net_remote_http_start(void) {
    if (s_server != NULL) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return;
    }

    static const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    static const httpd_uri_t boot_uri = {.uri = "/boot", .method = HTTP_GET, .handler = boot_handler};
    static const httpd_uri_t wifi_uri = {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_handler};
    static const httpd_uri_t upload_uri = {.uri = "/upload", .method = HTTP_POST, .handler = upload_handler};
    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &boot_uri);
    httpd_register_uri_handler(s_server, &wifi_uri);
    httpd_register_uri_handler(s_server, &upload_uri);

    ESP_LOGI(TAG, "remote control HTTP server started on port %d", config.server_port);
}
